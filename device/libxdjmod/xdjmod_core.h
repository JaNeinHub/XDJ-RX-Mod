/*
 * libxdjmod core: the parts that do not depend on Linux or on being inside
 * rbp, so they can be unit-tested on the Mac (tests/test_core.c).
 *
 * - peak metering of interleaved PCM buffers (audio thread, allocation-free)
 * - a seqlock for publishing per-second statistics from the audio thread
 * - a lock-free multi-producer event ring (drops when full, never blocks)
 * - text formatting of events and statistics (log thread only)
 */
#ifndef XDJMOD_CORE_H
#define XDJMOD_CORE_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- ALSA format codes (snd_pcm_format_t) ---------- */
enum {
	FMT_S16_LE = 2,
	FMT_S24_LE = 6,		/* 24 bit in the low 3 bytes of a 32-bit word */
	FMT_S32_LE = 10,
	FMT_FLOAT_LE = 14,
	FMT_S24_3LE = 32,	/* packed 3 bytes */
};

static inline const char *fmt_name(int fmt)
{
	switch (fmt) {
	case FMT_S16_LE: return "S16_LE";
	case FMT_S24_LE: return "S24_LE";
	case FMT_S32_LE: return "S32_LE";
	case FMT_FLOAT_LE: return "FLOAT_LE";
	case FMT_S24_3LE: return "S24_3LE";
	default: return "?";
	}
}

/* ---------- peak metering ---------- */

#define MAX_CH 8

/* Full scale is 2^31 for every format; peaks are kept as uint32 magnitudes. */
static inline uint32_t mag32(int32_t v)
{
	return v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;	/* INT32_MIN -> 2^31 */
}

/*
 * Update per-channel peaks from an interleaved buffer. Unknown formats are
 * ignored. Cost: one load, shift and compare per sample.
 */
static inline void peak_update(uint32_t *peak, const void *buf, unsigned long frames,
			       unsigned channels, int fmt)
{
	unsigned ch_used = channels < MAX_CH ? channels : MAX_CH;
	unsigned long i;
	unsigned c;

	if (!buf || channels == 0)
		return;
	switch (fmt) {
	case FMT_S16_LE: {
		const int16_t *s = (const int16_t *)buf;
		for (i = 0; i < frames; i++, s += channels)
			for (c = 0; c < ch_used; c++) {
				uint32_t m = mag32((int32_t)s[c] * 65536);
				if (m > peak[c]) peak[c] = m;
			}
		break;
	}
	case FMT_S24_LE: {
		const int32_t *s = (const int32_t *)buf;
		for (i = 0; i < frames; i++, s += channels)
			for (c = 0; c < ch_used; c++) {
				int32_t v = (int32_t)((uint32_t)s[c] << 8);	/* drop the unused top byte, keep the sign */
				uint32_t m = mag32(v);
				if (m > peak[c]) peak[c] = m;
			}
		break;
	}
	case FMT_S32_LE: {
		const int32_t *s = (const int32_t *)buf;
		for (i = 0; i < frames; i++, s += channels)
			for (c = 0; c < ch_used; c++) {
				uint32_t m = mag32(s[c]);
				if (m > peak[c]) peak[c] = m;
			}
		break;
	}
	case FMT_S24_3LE: {
		const uint8_t *s = (const uint8_t *)buf;
		for (i = 0; i < frames; i++, s += 3 * channels)
			for (c = 0; c < ch_used; c++) {
				const uint8_t *p = s + 3 * c;
				int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
				uint32_t m = mag32(v);
				if (m > peak[c]) peak[c] = m;
			}
		break;
	}
	case FMT_FLOAT_LE: {
		const float *s = (const float *)buf;
		for (i = 0; i < frames; i++, s += channels)
			for (c = 0; c < ch_used; c++) {
				float a = s[c] < 0 ? -s[c] : s[c];
				uint32_t m = a >= 1.0f ? 0x80000000u : (uint32_t)(a * 2147483648.0f);
				if (m > peak[c]) peak[c] = m;
			}
		break;
	}
	default:
		break;
	}
}

/* "-6.0", "0.0" or "-inf" (dBFS, full scale = 2^31). */
static inline void fmt_db(char *out, size_t n, uint32_t peak)
{
	if (peak == 0)
		snprintf(out, n, "-inf");
	else
		snprintf(out, n, "%.1f", 20.0 * log10((double)peak / 2147483648.0));
}

/* ---------- seqlock (one writer, one reader, writer never waits) ---------- */

static inline void seq_publish(volatile uint32_t *seq, void *dst, const void *src, size_t n)
{
	uint32_t s = __atomic_load_n(seq, __ATOMIC_RELAXED);
	__atomic_store_n(seq, s + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	memcpy(dst, src, n);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(seq, s + 2, __ATOMIC_RELAXED);
}

/* Returns 1 and the sequence number on a consistent copy, 0 if the writer kept interfering. */
static inline int seq_read(volatile uint32_t *seq, void *dst, const void *src, size_t n, uint32_t *out_seq)
{
	int tries;
	for (tries = 0; tries < 8; tries++) {
		uint32_t a = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
		if (a & 1)
			continue;
		memcpy(dst, src, n);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(seq, __ATOMIC_RELAXED) == a) {
			*out_seq = a;
			return 1;
		}
	}
	return 0;
}

/* ---------- event ring (many producers, one consumer) ----------
 *
 * Bounded queue with a sequence number per slot (D. Vyukov's MPMC design, used
 * here with one consumer). A slot belongs to exactly one producer from the
 * moment its claim succeeds, even if that producer is preempted before
 * committing. Producers never wait: a full ring drops the event and counts it.
 * ev_ring_init() must run before the first claim.
 */

#define EV_SLOTS 512		/* power of two */
#define EV_DATA 132

enum ev_type {
	EV_NOTE = 1,
	EV_OPEN,		/* a = fd, b = class, d = path */
	EV_PCM_OPEN,		/* a = slot, b = stream, c = result, e = mode, d = name */
	EV_PCM_HW,		/* a = slot, c = result, d = struct pcm_hw */
	EV_PCM_PREPARE,		/* a = slot, c = result */
	EV_PCM_CLOSE,		/* a = slot, c = result */
	EV_PCM_ERR,		/* a = slot, b = stream, c = result, e = frames requested */
	EV_I2C_WRITE,		/* a = fd, c = result, d = bytes written */
	EV_I2C_READ,		/* a = fd, c = result, d = bytes read */
	EV_I2C_ADDR,		/* a = fd, b = ioctl cmd, c = result, e = address */
	EV_I2C_RDWR,		/* a = fd, b = number of msgs, c = result, d = packed msgs */
	EV_I2C_SMBUS,		/* a = fd, b = rw<<8|command, c = result, e = size, d = data */
	EV_I2C_IOCTL,		/* a = fd, b = cmd, c = result, e = arg */
	EV_SPI_TX,		/* a = multi<<8|part, b = part_num, len = bytes, d = frame */
	EV_SPI_RX,		/* len = bytes, d = frame */
	EV_SPI_WRITE,		/* c = result, d = bytes */
	EV_SPI_IOCTL,		/* b = cmd, c = result, e = value */
	EV_HID_IN,		/* a = fd, d = report */
	EV_HID_OUT,		/* a = fd, d = report */
};

struct ev {
	uint32_t seq;		/* ring bookkeeping, see ev_claim/ev_take */
	uint16_t type;
	uint16_t len;		/* bytes used in d */
	uint32_t a, b, e;
	int32_t c;
	int32_t tid;
	uint64_t t_us;
	uint8_t d[EV_DATA];
};

struct ev_ring {
	struct ev slot[EV_SLOTS];
	uint32_t head;		/* next position to claim (producers) */
	uint32_t tail;		/* next position to read (consumer) */
	uint32_t dropped;
};

static inline void ev_ring_init(struct ev_ring *r)
{
	uint32_t i;
	for (i = 0; i < EV_SLOTS; i++)
		__atomic_store_n(&r->slot[i].seq, i, __ATOMIC_RELAXED);
	r->head = r->tail = r->dropped = 0;
	__atomic_thread_fence(__ATOMIC_RELEASE);
}

/* Claim a slot or return NULL (ring full). Never blocks. */
static inline struct ev *ev_claim(struct ev_ring *r)
{
	uint32_t pos = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
	for (;;) {
		struct ev *e = &r->slot[pos % EV_SLOTS];
		uint32_t seq = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
		int32_t dif = (int32_t)(seq - pos);
		if (dif == 0) {
			/* free for this lap: take it if nobody else advanced head */
			if (__atomic_compare_exchange_n(&r->head, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
				e->len = 0;
				e->a = e->b = e->e = 0;
				e->c = 0;
				return e;
			}
			/* pos was reloaded by the failed CAS */
		} else if (dif < 0) {
			/* still holds an event from the previous lap: full */
			__atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED);
			return NULL;
		} else {
			pos = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
		}
	}
}

static inline void ev_commit(struct ev *e)
{
	/* the claim position is seq; publish it as readable (seq + 1) */
	__atomic_store_n(&e->seq, __atomic_load_n(&e->seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

/* Copy the next committed event into *out. Returns 0 when nothing is ready. */
static inline int ev_take(struct ev_ring *r, struct ev *out)
{
	uint32_t pos = r->tail;
	struct ev *e = &r->slot[pos % EV_SLOTS];
	if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != pos + 1)
		return 0;
	memcpy(out, e, sizeof(*out));
	/* free it for the producer one lap ahead */
	__atomic_store_n(&e->seq, pos + EV_SLOTS, __ATOMIC_RELEASE);
	r->tail = pos + 1;
	return 1;
}

/* ---------- per-stream statistics ---------- */

struct pcm_acc {
	uint32_t calls;
	uint32_t frames;
	uint32_t xruns;		/* -EPIPE */
	uint32_t errors;	/* other negative results */
	uint32_t short_io;	/* fewer frames than requested */
	uint32_t int_min, int_max;	/* µs between call entries */
	uint64_t int_sum;
	uint32_t int_n;
	uint32_t in_max;	/* µs spent inside the ALSA call */
	uint64_t in_sum;
	uint32_t peak[MAX_CH];
};

struct thr_acc {		/* the thread that does PCM I/O */
	int32_t tid;
	uint32_t cycles;	/* gaps measured */
	uint64_t busy_us;	/* time between leaving one ALSA call and entering the next */
	uint32_t gap_max;
	uint64_t span_us;	/* window length */
};

struct pcm_hw {
	int32_t format;
	uint32_t channels, rate;
	uint32_t period, buffer;
};

/* ---------- text formatting (log thread) ---------- */

static inline int hex_bytes(char *out, size_t n, const uint8_t *d, unsigned len)
{
	size_t o = 0;
	unsigned i;
	for (i = 0; i < len && o + 3 < n; i++)
		o += (size_t)snprintf(out + o, n - o, "%02x", d[i]);
	if (o < n) out[o] = 0;
	return (int)o;
}

/*
 * Changed bytes of a panel frame relative to the previous logged one:
 * "off:old>new off:old>new". Returns the number of changed bytes.
 */
static inline unsigned frame_diff(char *out, size_t n, const uint8_t *prev, const uint8_t *cur, unsigned len)
{
	size_t o = 0;
	unsigned i, changed = 0;
	out[0] = 0;
	for (i = 0; i < len; i++) {
		if (prev[i] == cur[i])
			continue;
		changed++;
		if (o + 16 < n)
			o += (size_t)snprintf(out + o, n - o, "%s%u:%02x>%02x", o ? " " : "", i, prev[i], cur[i]);
	}
	return changed;
}

static inline void fmt_pcm_line(char *out, size_t n, double t, int slot, const char *name, int stream,
				const struct pcm_acc *a, unsigned channels)
{
	char peaks[MAX_CH * 8] = "";
	size_t o = 0;
	unsigned c, ch = channels < MAX_CH ? channels : MAX_CH;
	for (c = 0; c < ch; c++) {
		char db[16];
		fmt_db(db, sizeof db, a->peak[c]);
		o += (size_t)snprintf(peaks + o, sizeof peaks - o, "%s%s", c ? "," : "", db);
	}
	snprintf(out, n,
		 "%9.3f pcm%d %s %s calls=%u frames=%u int_us=%u/%u/%u in_us=%u/%u xrun=%u err=%u short=%u peak_dbfs=%s\n",
		 t, slot, name, stream ? "capture" : "playback", a->calls, a->frames,
		 a->int_n ? a->int_min : 0, a->int_n ? (unsigned)(a->int_sum / a->int_n) : 0, a->int_max,
		 a->calls ? (unsigned)(a->in_sum / a->calls) : 0, a->in_max,
		 a->xruns, a->errors, a->short_io, ch ? peaks : "-");
}


/* ---------- panel: headphone cue mode switch ----------
 * Input frames (read() on /dev/subucom_spi1.0, 100 bytes): SHIFT deck 1 = byte 18 bit 0x01,
 * CUE CH1 = byte 91 bit 0x02, CUE CH2 = byte 91 bit 0x01. rbp checks a CRC in bytes 98/99, so input
 * frames are never edited: a hidden press is replaced by the last frame passed on unchanged.
 * LED frames (SUBUCOM_IOC_MESSAGE, multi 0 part 0): CUE CH1 LED = byte 10 bit 0x20, CUE CH2 LED =
 * byte 10 bit 0x10; CRC-16/X-25 over bytes 0..19 with each 32-bit word byte-swapped, little-endian
 * in bytes 22/23 (verified on 340 recorded frames). */
#define PANEL_RX_LEN		100
#define PANEL_RX_SHIFT_BYTE	18
#define PANEL_RX_SHIFT_BIT	0x01
#define PANEL_RX_CUE_BYTE	91
#define PANEL_RX_CUE1_BIT	0x02
#define PANEL_RX_CUE2_BIT	0x01
#define PANEL_TX_MIN_LEN	24
#define PANEL_TX_CUE_BYTE	10
#define PANEL_TX_CUE1_BIT	0x20
#define PANEL_TX_CUE2_BIT	0x10
#define HP_BLINK_US		333333u	/* 3 blinks per second */

/* Cue modes, also the Utility menu's value order: NORMAL = Pioneer's blend, SPLIT = Pioneer's built-in split
 * (always split), SPLIT STEREO = PX5/A&H style (split only while a channel is cued, else master in stereo). */
enum { HP_NORMAL = 0, HP_SPLIT = 1, HP_SPLIT_STEREO = 2 };

static inline uint16_t panel_tx_crc(const uint8_t *f)
{
	uint16_t c = 0xffff;
	int i, k;
	for (i = 0; i < 20; i++) {
		c ^= f[(i & ~3) + 3 - (i & 3)];	/* bytes of each 32-bit word in reverse order */
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
	}
	return (uint16_t)~c;
}

static inline void panel_tx_seal(uint8_t *f)
{
	uint16_t c = panel_tx_crc(f);
	f[22] = (uint8_t)c;
	f[23] = (uint8_t)(c >> 8);
}

/* LED byte 10 while SHIFT is held: the active mode's CUE LED blinks, the other one is off. */
static inline uint8_t hp_led_byte(uint8_t orig, int mode, uint64_t t_us)
{
	uint8_t on = mode != HP_NORMAL ? PANEL_TX_CUE2_BIT : PANEL_TX_CUE1_BIT;
	int lit = (t_us % HP_BLINK_US) < HP_BLINK_US / 2;
	return (uint8_t)((orig & ~(PANEL_TX_CUE1_BIT | PANEL_TX_CUE2_BIT)) | (lit ? on : 0));
}

struct hp_switch {
	uint8_t pass[PANEL_RX_LEN];	/* the last frame handed to rbp unchanged */
	int pass_valid;
	uint8_t prev_cues;		/* CUE bits of the previous raw frame */
	uint8_t captured;		/* CUE bits pressed together with SHIFT, hidden until released */
	int shift;			/* SHIFT deck 1 held (raw) */
};

/* Called with every input frame rbp reads, before rbp sees it. enabled = switching may happen.
 * Returns HP_NORMAL (SHIFT + CUE CH1), HP_SPLIT (SHIFT + CUE CH2: "a split mode", the caller picks which) or -1; *hidden = 1 if the frame was replaced. */
static inline int hp_switch_rx(struct hp_switch *s, uint8_t *f, size_t n, int enabled, int *hidden)
{
	uint8_t cues, newly;
	int mode = -1;
	*hidden = 0;
	if (n != PANEL_RX_LEN) return -1;
	s->shift = (f[PANEL_RX_SHIFT_BYTE] & PANEL_RX_SHIFT_BIT) != 0;
	cues = f[PANEL_RX_CUE_BYTE] & (PANEL_RX_CUE1_BIT | PANEL_RX_CUE2_BIT);
	newly = (uint8_t)(cues & ~s->prev_cues);
	s->prev_cues = cues;
	if (enabled && s->shift && newly) {
		s->captured |= newly;
		mode = (newly & PANEL_RX_CUE2_BIT) ? HP_SPLIT : HP_NORMAL;
	}
	s->captured &= cues;	/* a released button is no longer hidden */
	if (s->captured && s->pass_valid) {
		memcpy(f, s->pass, PANEL_RX_LEN);
		*hidden = 1;
	} else {
		memcpy(s->pass, f, PANEL_RX_LEN);
		s->pass_valid = 1;
	}
	return mode;
}

/* The mode word for HeadPhone+0x2c (1 = stereo blend, 0 = split). SPLIT always splits (Pioneer); SPLIT STEREO
 * splits only while a channel is cued, like the PX5. cue_leds = byte 10 of rbp's own LED frame. */
static inline uint32_t hp_mode_word(int mode, uint8_t cue_leds)
{
	int cued = (cue_leds & (PANEL_TX_CUE1_BIT | PANEL_TX_CUE2_BIT)) != 0;
	if (mode == HP_SPLIT) return 0u;
	return (mode == HP_SPLIT_STEREO && cued) ? 0u : 1u;
}

/* Settings file (/root/rbp/xdjmod-hpmode), one key per line:
 *     mode=normal|split|split-stereo   the cue mode
 *     split=split|split-stereo         which split SHIFT + CUE CH2 selects (the last split used)
 *     meter=pre|post                   master level display before/after MASTER LEVEL
 *     master-mono=yes|no, booth-mono=yes|no
 *     fps=20|30|40|60                  screen frame rate (gui_task)
 *     next=measure                     one-shot: next start with the frame meter only (cleared at that start)
 *     waveform=blue|rgb|3band          waveform colours (wave builds; written only when not blue)
 *     scroll=original|smooth           big waveform position (written only when smooth)
 *     sync=refresh|1.5|3.0|4.5|6.0     SCREEN SYNC (wave builds; written only when not pioneer)
 * Files of 0.1-0.3 hold just "split" (= the PX5 behaviour, now SPLIT STEREO) or "normal". */
/* Screen frame rates offered in the menu, index = setting. Index 0 = Pioneer's 20 fps. The screen never
 * gets above 30 (each frame costs ~17 ms of drawing, then waits for the next 60 Hz refresh), so 40/60 were dropped;
 * settings files from 0.7 with fps=40/60 read as 30. */
#define GUI_RATES 2
static const char *const xdj_fps_name[GUI_RATES] = { "20", "30" };
/* SCREEN SYNC (WAVE 10, see sync_next): Pioneer's refresh wait, at the refresh, or that many ms before it */
#define SYNC_MODES 6
static const char *const xdj_sync_name[SYNC_MODES] = { "pioneer", "refresh", "1.5", "3.0", "4.5", "6.0" };

struct xdj_settings {
	int mode, split, meter_pre, master_mono, booth_mono, fps;
	int measure_next;	/* one-shot: the next start loads only the frame meter (0.10) */
	int wave;		/* WAVE_BLUE / WAVE_RGB / WAVE_3BAND */
	int scroll;		/* 0 Pioneer's position, 1 SMOOTH (the position at draw time) */
	int sync;		/* SCREEN SYNC: index into xdj_sync_name (0 = Pioneer's refresh wait) */
};
#define WAVE_BLUE 0
#define WAVE_RGB 1
#define WAVE_3BAND 2
static const char *const xdj_wave_name[3] = { "blue", "rgb", "3band" };

static inline void xdj_settings_default(struct xdj_settings *st)
{
	st->mode = HP_SPLIT_STEREO;
	st->split = HP_SPLIT_STEREO;
	st->meter_pre = 1;
	st->master_mono = 0;
	st->booth_mono = 0;
	st->fps = 0;
	st->measure_next = 0;
	st->wave = WAVE_BLUE;
	st->scroll = 0;
	st->sync = 0;
}

static inline int xdj_word(const char *p, const char *end, const char *w)
{
	size_t n = strlen(w);
	return (size_t)(end - p) >= n && !strncmp(p, w, n) &&
	       ((size_t)(end - p) == n || p[n] == '\n' || p[n] == '\r' || p[n] == ' ');
}

static inline int xdj_parse_cue(const char *p, const char *end)
{
	if (xdj_word(p, end, "normal")) return HP_NORMAL;
	if (xdj_word(p, end, "split-stereo")) return HP_SPLIT_STEREO;
	if (xdj_word(p, end, "split")) return HP_SPLIT;
	return -1;
}

/* Fills *st from the file content; unknown or missing keys keep their defaults. */
static inline void xdj_settings_parse(struct xdj_settings *st, const char *txt, size_t n)
{
	const char *p = txt, *end = txt + n;
	xdj_settings_default(st);
	if (xdj_word(p, end, "split")) {	/* 0.1-0.3 */
		st->mode = HP_SPLIT_STEREO;
		return;
	}
	if (xdj_word(p, end, "normal")) {
		st->mode = HP_NORMAL;
		return;
	}
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		int v;
		if (!eol) eol = end;
		if (eol - p > 5 && !strncmp(p, "mode=", 5) && (v = xdj_parse_cue(p + 5, eol)) >= 0)
			st->mode = v;
		else if (eol - p > 6 && !strncmp(p, "split=", 6) && (v = xdj_parse_cue(p + 6, eol)) > 0)
			st->split = v;
		else if (eol - p > 6 && !strncmp(p, "meter=", 6)) {
			if (xdj_word(p + 6, eol, "pre")) st->meter_pre = 1;
			if (xdj_word(p + 6, eol, "post")) st->meter_pre = 0;
		} else if (eol - p > 12 && !strncmp(p, "master-mono=", 12)) {
			if (xdj_word(p + 12, eol, "yes")) st->master_mono = 1;
			if (xdj_word(p + 12, eol, "no")) st->master_mono = 0;
		} else if (eol - p > 4 && !strncmp(p, "fps=", 4)) {
			int k;
			for (k = 0; k < GUI_RATES; k++)
				if (xdj_word(p + 4, eol, xdj_fps_name[k])) st->fps = k;
			if (xdj_word(p + 4, eol, "40") || xdj_word(p + 4, eol, "60")) st->fps = 1;	/* 0.7 */
		} else if (eol - p > 5 && !strncmp(p, "next=", 5)) {
			st->measure_next = xdj_word(p + 5, eol, "measure");
		} else if (eol - p > 11 && !strncmp(p, "booth-mono=", 11)) {
			if (xdj_word(p + 11, eol, "yes")) st->booth_mono = 1;
			if (xdj_word(p + 11, eol, "no")) st->booth_mono = 0;
		} else if (eol - p > 9 && !strncmp(p, "waveform=", 9)) {
			int k;
			for (k = 0; k < 3; k++)
				if (xdj_word(p + 9, eol, xdj_wave_name[k])) st->wave = k;
		} else if (eol - p > 7 && !strncmp(p, "scroll=", 7)) {
			if (xdj_word(p + 7, eol, "smooth")) st->scroll = 1;
			if (xdj_word(p + 7, eol, "original")) st->scroll = 0;
		} else if (eol - p > 5 && !strncmp(p, "sync=", 5)) {
			int k;
			for (k = 0; k < SYNC_MODES; k++)
				if (xdj_word(p + 5, eol, xdj_sync_name[k])) st->sync = k;
		}
		p = eol + 1;
	}
}

static inline const char *xdj_cue_name(int mode)
{
	return mode == HP_SPLIT_STEREO ? "split-stereo" : mode == HP_SPLIT ? "split" : "normal";
}

static inline int xdj_settings_format(const struct xdj_settings *st, char *out, size_t n)
{
	int sy = st->sync > 0 && st->sync < SYNC_MODES;
	return snprintf(out, n, "mode=%s\nsplit=%s\nmeter=%s\nmaster-mono=%s\nbooth-mono=%s\nfps=%s\n%s%s%s%s%s%s%s%s",
			xdj_cue_name(st->mode), xdj_cue_name(st->split), st->meter_pre ? "pre" : "post",
			st->master_mono ? "yes" : "no", st->booth_mono ? "yes" : "no",
			xdj_fps_name[st->fps >= 0 && st->fps < GUI_RATES ? st->fps : 0],
			st->wave > 0 && st->wave < 3 ? "waveform=" : "", st->wave > 0 && st->wave < 3 ? xdj_wave_name[st->wave] : "",
			st->wave > 0 && st->wave < 3 ? "\n" : "", st->scroll ? "scroll=smooth\n" : "",
			sy ? "sync=" : "", sy ? xdj_sync_name[st->sync] : "", sy ? "\n" : "",
			st->measure_next ? "next=measure\n" : "");
}

/* The stereo blend path's fixed gain: a literal pair of floats {7.4989, 7.4989} (+17.5 dB) at 0x68320,
 * loaded only by the vldr at 0x68088 (HeadPhone stereo path). The split path
 * has its own copy at 0x68070. In split mode with no cue, the master plays through the stereo path on
 * both ears, which sounds louder than the master on one ear while cued, so the library lowers this
 * gain by HP_TRIM_DEFAULT dB while SPLIT STEREO is selected and restores the exact original otherwise. */
#define HP_STEREO_GAIN_ADDR	0x68320u
#define HP_STEREO_GAIN_BITS	0x40eff755u	/* 7.4989419f */
#define HP_TRIM_DEFAULT		4		/* dB */
#define HP_TRIM_MAX		12

static inline uint32_t le32_(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float bits_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* The value for the literal: original bits in normal mode or with trim 0, else lowered by trim_db. */
static inline uint32_t hp_stereo_gain_bits(int mode, int trim_db)
{
	float g = bits_f32(HP_STEREO_GAIN_BITS);
	int i;
	if (mode != HP_SPLIT_STEREO || trim_db <= 0) return HP_STEREO_GAIN_BITS;
	if (trim_db > HP_TRIM_MAX) trim_db = HP_TRIM_MAX;
	for (i = 0; i < trim_db; i++) g *= 0.89125094f;	/* -1 dB */
	return f32_bits(g);
}

/* Only ever overwrite what is the original or one of our own values. */
static inline int hp_stereo_gain_plausible(uint32_t bits)
{
	float g = bits_f32(bits);
	return bits == HP_STEREO_GAIN_BITS ||
	       (g == g && g >= bits_f32(hp_stereo_gain_bits(HP_SPLIT_STEREO, HP_TRIM_MAX)) * 0.999f &&
	        g < bits_f32(HP_STEREO_GAIN_BITS));
}

/* Trim file content: the attenuation in whole dB, "4", "-4" or "4 dB" alike, 0..HP_TRIM_MAX. Else -1. */
static inline int hp_parse_trim(const char *txt, size_t n)
{
	size_t i = 0;
	int v = 0, digits = 0;
	while (i < n && (txt[i] == ' ' || txt[i] == '\t')) i++;
	if (i < n && (txt[i] == '-' || txt[i] == '+')) i++;
	while (i < n && txt[i] >= '0' && txt[i] <= '9' && digits < 3) {
		v = v * 10 + (txt[i++] - '0');
		digits++;
	}
	if (!digits || v > HP_TRIM_MAX || (i < n && txt[i] >= '0' && txt[i] <= '9')) return -1;
	return v;
}


/* ---- Master level display, switchable ----
 * MasterOutChannel::process measures the meter peak at 0x664a8 with vabs d17,d16 (after MASTER LEVEL); the
 * A&H variant is vabs d17,d17 (the bus before it). Rewriting that instruction while the audio thread runs isn't
 * safe on ARMv7 (only B/BL/NOP/... may be changed under a running core). So once, in the constructor before rbp
 * has any thread, 0x664a8 becomes a branch to a short sequence in the never-called
 * DjEngineIF::setOutputHeadphoneCueStereoType (0x59e84) that picks the variant from a flag word; switching
 * later only writes that flag. r0 and the flags are dead at 0x664a8 (r0 reloaded at 0x664b4, flags set at 0x664b8). */
#define MTR_SITE	0x664a8u
#define MTR_SITE_ORIG	0xf3f91720u	/* vabs.f32 d17, d16 */
#define MTR_SITE_PRE	0xf3f91721u	/* vabs.f32 d17, d17 */
#define MTR_CAVE	0x59e84u
#define MTR_CAVE_WORDS	9
static const uint32_t mtr_cave_orig[MTR_CAVE_WORDS] = {
	0xe92d40f8, 0xe1a05001, 0xe59f4098, 0xe59f0098, 0xebffa0e6, 0xe5940000, 0xe3500000, 0x0a000002, 0xe1a01005
};

/* ARM B (always) from 'from' to 'to'. */
static inline uint32_t arm_b(uint32_t from, uint32_t to)
{
	return 0xea000000u | (((to - (from + 8)) >> 2) & 0x00ffffffu);
}

/* The sequence at MTR_CAVE: flag != 0 -> pre (A&H), 0 -> post (Pioneer). */
static inline void mtr_cave_build(uint32_t *w, uint32_t flag_addr)
{
	w[0] = 0xe59f0018;	/* ldr r0, [pc, #24]  -> w[8] */
	w[1] = 0xe5900000;	/* ldr r0, [r0] */
	w[2] = 0xe3500000;	/* cmp r0, #0 */
	w[3] = 0x1a000001;	/* bne w[6] */
	w[4] = MTR_SITE_ORIG;	/* vabs.f32 d17, d16 */
	w[5] = arm_b(MTR_CAVE + 20, MTR_SITE + 4);
	w[6] = MTR_SITE_PRE;	/* vabs.f32 d17, d17 */
	w[7] = arm_b(MTR_CAVE + 28, MTR_SITE + 4);
	w[8] = flag_addr;
}

/* ---- Screen frame rate ----
 * gui_task (0xb3a68) draws a frame, measures how long it took (dt, ms) and sleeps 50 - dt: 20 fps.
 *     b3aac: cmp r4, #49      b3ab0: bls b3ad8      b3ad8: rsb r0, r4, #50      b3adc: bl dly_tsk (0x125a50)
 * Installed once in the constructor (no thread runs yet): b3aac becomes a branch to a sequence at 0x59ea8 (the
 * never-called function after the meter sequence). Since 0.10 it calls a C function gui_frame(dt) in the library
 * that returns the sleep in ms (gui_sleep_ms) and feeds the frame meter, then sleeps if that's > 0 and goes on at
 * b3ab4. b3ab4 is a bl, and Pioneer's own path reaches it after bl dly_tsk, so nothing after it relies on r0-r3,
 * r12 or lr; sp is 8-byte aligned there (push {r4, lr}; sub sp, #8). */
#define GUI_SITE	0xb3aacu
#define GUI_RESUME	0xb3ab4u
#define GUI_DLY_TSK	0x125a50u
#define GUI_CAVE	0x59ea8u
#define GUI_CAVE_WORDS	8
#define GUI_MIN_SLEEP	2
static const uint32_t gui_site_orig[2] = { 0xe3540031, 0x9a000008 };	/* cmp r4, #49; bls b3ad8 */
static const uint32_t gui_cave_orig[GUI_CAVE_WORDS] = {
	0xe8bd40f8, 0xea0028a8, 0xe59f007c, 0xeb0a8f7e, 0xe5943000, 0xe3530000, 0x0a000003, 0xe59f0068
};

static inline uint32_t arm_bl(uint32_t from, uint32_t to)
{
	return 0xeb000000u | (((to - (from + 8)) >> 2) & 0x00ffffffu);
}

static inline void gui_cave_build(uint32_t *w, uint32_t fn_addr)
{
	w[0] = 0xe1a00004;			/* mov r0, r4         dt, ms */
	w[1] = 0xe59f300c;			/* ldr r3, [pc, #12]  -> w[6] */
	w[2] = 0xe12fff33;			/* blx r3             r0 = gui_frame(dt) */
	w[3] = 0xe3500000;			/* cmp r0, #0 */
	w[4] = 0x1b000000u | (arm_bl(GUI_CAVE + 16, GUI_DLY_TSK) & 0x00ffffffu);	/* blne dly_tsk */
	w[5] = arm_b(GUI_CAVE + 20, GUI_RESUME);
	w[6] = fn_addr;
	w[7] = 0xe1a00000;			/* nop (never reached) */
}

static const uint32_t gui_period_ms[GUI_RATES] = { 50, 33 };	/* ms per frame, by setting */

/* Sleep after a frame that took dt ms. On time: the rest of the period. Late (dt >= period): at 20 fps exactly
 * Pioneer (draw the next frame at once, b3aac/b3ab0); at higher rates also at once, unless the previous frame was
 * late too: then GUI_MIN_SLEEP, so a drawing loop that can't keep up (real-time priority) still leaves core 0 to
 * the other tasks. A fixed minimum sleep (2 ms) would delay exactly the frames that are late. */
static inline uint32_t gui_sleep_ms(uint32_t dt, uint32_t period, int *prev_late)
{
	int late = dt >= period;
	int was = *prev_late;
	*prev_late = late;
	if (!late) return period - dt;
	if (period >= 50) return 0;
	return was ? GUI_MIN_SLEEP : 0;
}

/* Display refresh (vsync, 0.11): where in the refresh cycle a frame finishes. t and vs are the low 32 bits of
 * CLOCK_MONOTONIC in us (differences wrap correctly), period in us. -1 = no usable vsync (none yet, or the last one
 * is older than 3 periods, i.e. the vsync thread stalled). */
#define VS_BINS 17	/* 1 ms bins over one 60 Hz refresh (16.7 ms) */
#define FBIO_WAITFORVSYNC_ 0x40044620u	/* _IOW('F', 0x20, u32) = MXCFB_WAIT_FOR_VSYNC on i.MX */

static inline int vs_phase_bin(uint32_t t, uint32_t vs, uint32_t period)
{
	uint32_t d = t - vs, b;
	if (period < 5000 || period > 50000 || d > 3 * period) return -1;
	b = (d % period) * VS_BINS / period;
	return b < VS_BINS ? (int)b : VS_BINS - 1;
}

/* Running estimate of the refresh period from consecutive vsync timestamps; deltas outside 10..25 ms (missed or
 * late wake-ups) are ignored. Returns the new estimate (us). */
static inline uint32_t vs_period_update(uint32_t est, uint32_t delta)
{
	if (delta < 10000 || delta > 25000) return est;
	if (!est) return delta;
	return (uint32_t)(((uint64_t)est * 15 + delta + 8) / 16);
}

/* ---- SCREEN SYNC (WAVE 10) ----
 * rbp's DirectFB waits for the refresh interrupt in gui_task, then copies the finished picture into the single-page
 * screen (~2 ms). The copy overtakes the top-to-bottom readout at row ~93: the tear. With SCREEN SYNC on, the mod's
 * own thread does the refresh waits and keeps the refresh phase (vs_pll), and gui_task's wait becomes a sleep until
 * the next "refresh - lead" (sync_next). Started early enough, the copy runs while the readout is in the bottom rows
 * or the blanking, stays ahead of it and leaves no edge. Index 0 = Pioneer's own wait; 1 = at the refresh (should look
 * like Pioneer: the reference for the others). SYNC_MODES and the names are with the settings file. */
static const int32_t sync_lead_us[SYNC_MODES] = { 0, 0, 1500, 3000, 4500, 6000 };
#define SYNC_PERIOD_NOMINAL_Q8 (16800u * 256u)	/* the panel's refresh (measured: 16.80 ms), us * 256 */

static inline int32_t sync_lead(int sel)
{
	return sel > 0 && sel < SYNC_MODES ? sync_lead_us[sel] : 0;
}

static inline int64_t floor_div64(int64_t a, int64_t b)
{
	int64_t q = a / b;
	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* The first planned moment at or after t: anchor + offset + k * period (k any integer), all in us; period_q8 =
 * us * 256. Always within [t, t + period), so a sleep until it can never hang. Returns t if the period is implausible. */
static inline uint64_t sync_next(uint64_t t, uint64_t anchor, uint32_t period_q8, int32_t offset_us)
{
	int64_t base = (int64_t)anchor + offset_us, k, s;
	if (period_q8 < 5000u * 256u || period_q8 > 50000u * 256u) return t;
	k = floor_div64(((int64_t)t - base) * 256, (int64_t)period_q8);
	s = base + floor_div64(k * (int64_t)period_q8, 256);
	while (s < (int64_t)t) {
		k++;
		s = base + floor_div64(k * (int64_t)period_q8, 256);
	}
	return (uint64_t)s;
}

/* Refresh phase from the wake times of a thread that waits for every refresh. A wake comes some time after the
 * interrupt (other threads run first), never before, so the model follows the earliest wakes: the refreshes are at
 * anchor + k * period. After a start, the first VS_WIN wakes are kept: the period is the median of their intervals
 * (late wakes make one interval longer and the next shorter, missed ones are left out) and the anchor is the one
 * with the earliest phase. From then on, the anchor is the earliest-phase wake of each window of VS_WIN wakes, and
 * the period comes from the distance between two such anchors. valid once that has happened once (~2 s after a
 * start). A gap of more than 0.5 s starts over. */
#define VS_WIN 64
#define VS_LATE_US 1000
struct vs_pll {
	uint64_t anchor, last, win_t, prev_win_t, learn_t0;
	uint32_t period_q8;		/* us * 256; 0 = still collecting the first wakes */
	int32_t win_res;
	uint32_t n, win, windows, valid;
	uint32_t learn[VS_WIN];		/* the first wakes after a start, us after learn_t0 */
	uint32_t wakes, resets, res_sum, res_max, late;	/* for the log: residuals against the model, us */
};

static inline int32_t vs_pll_res(const struct vs_pll *p, uint64_t t)
{
	int64_t d = (int64_t)(t - p->anchor), k;
	if (!p->period_q8) return 0;
	k = floor_div64(d * 256 + p->period_q8 / 2, (int64_t)p->period_q8);	/* the nearest refresh */
	return (int32_t)(d - floor_div64(k * (int64_t)p->period_q8, 256));
}

static inline void vs_pll_learn(struct vs_pll *p)
{
	uint32_t iv[VS_WIN];
	unsigned i, j, m = 0, best = 0;
	int32_t rb = 0;
	for (i = 1; i < VS_WIN; i++) {
		uint32_t d = p->learn[i] - p->learn[i - 1];
		if (d < 10000 || d > 25000) continue;
		for (j = m++; j > 0 && iv[j - 1] > d; j--) iv[j] = iv[j - 1];
		iv[j] = d;
	}
	if (m < VS_WIN / 2) {	/* mostly gaps: start over with the next wake */
		p->n = 0;
		return;
	}
	p->period_q8 = m & 1 ? iv[m / 2] * 256u : (iv[m / 2 - 1] + iv[m / 2]) * 128u;
	p->anchor = p->learn_t0 + p->learn[VS_WIN - 1];
	for (i = 0; i < VS_WIN; i++) {
		int32_t r = vs_pll_res(p, p->learn_t0 + p->learn[i]);
		if (i == 0 || r < rb) {
			rb = r;
			best = i;
		}
	}
	p->anchor = p->prev_win_t = p->learn_t0 + p->learn[best];
	p->win = 0;
}

static inline void vs_pll_add(struct vs_pll *p, uint64_t t)
{
	int32_t r;
	p->wakes++;
	if (!p->n || t < p->last || t - p->last > 500000u) {
		if (p->n) p->resets++;
		p->anchor = p->last = p->learn_t0 = t;
		p->learn[0] = 0;
		p->n = 1;
		p->period_q8 = 0;
		p->win = p->windows = p->valid = 0;
		return;
	}
	p->last = t;
	if (!p->period_q8) {
		p->learn[p->n++] = (uint32_t)(t - p->learn_t0);
		if (p->n == VS_WIN) vs_pll_learn(p);
		return;
	}
	p->n++;
	r = vs_pll_res(p, t);
	if (p->valid) {
		uint32_t a = r > 0 ? (uint32_t)r : 0;
		p->res_sum += a;
		if (a > p->res_max) p->res_max = a;
		if (a > VS_LATE_US) p->late++;
	}
	if (!p->win || r < p->win_res) {
		p->win_res = r;
		p->win_t = t;
	}
	if (++p->win < VS_WIN) return;
	{
		uint64_t span = p->win_t - p->prev_win_t;
		int64_t k = floor_div64((int64_t)span * 256 + p->period_q8 / 2, (int64_t)p->period_q8);
		if (k > 0) {
			uint32_t pn = (uint32_t)(span * 256 / (uint64_t)k);
			p->period_q8 = p->valid ? (uint32_t)(((uint64_t)p->period_q8 * 3 + pn + 2) / 4) : pn;
			p->valid = 1;
		}
	}
	p->anchor = p->prev_win_t = p->win_t;
	p->windows++;
	p->win = 0;
}

/* Frame meter: gap = time between the ends of two frames (us). Single writer (gui_task); the logger reads it once
 * a minute and takes differences, so a torn read only blurs one line. */
struct gui_meter {
	uint32_t frames, late, very_late, safety, dt_sum, dt_max, gap_max_us;
	uint32_t phase[VS_BINS], no_phase;	/* frame ends per 1 ms of the refresh cycle (0.11) */
};

static inline void gui_meter_add(struct gui_meter *m, uint32_t dt, uint32_t gap_us, uint32_t period, int safety)
{
	m->frames++;
	m->dt_sum += dt;
	if (dt > m->dt_max) m->dt_max = dt;
	if (gap_us > m->gap_max_us) m->gap_max_us = gap_us;
	if (gap_us > period * 1500u) m->late++;		/* > 1.5 periods: at least one frame missed */
	if (gap_us > period * 2500u) m->very_late++;	/* > 2.5 periods */
	m->safety += safety != 0;
}

/* gui_task's CPU time per frame (WAVE 8), from its thread CPU clock (exact, unlike the 10 ms ticks in /proc).
 * "Off" = draw time - CPU time: the part of drawing a frame in which gui_task didn't run (preempted or waiting).
 * Single writer (gui_task), read once a minute like gui_meter. */
#define GUI_OFF_BINS 5
static const uint32_t gui_off_lim[GUI_OFF_BINS - 1] = { 5000, 10000, 15000, 20000 };
struct gui_cpu {
	uint32_t frames, cpu_sum, cpu_max, off_sum, off_max, late, late_cpu_sum, late_off_sum, hist[GUI_OFF_BINS];
};

static inline void gui_cpu_add(struct gui_cpu *m, uint32_t dt_ms, uint32_t cpu_us, int late)
{
	uint32_t draw = dt_ms * 1000u, off = draw > cpu_us ? draw - cpu_us : 0;
	unsigned b = 0;
	while (b < GUI_OFF_BINS - 1 && off >= gui_off_lim[b]) b++;
	m->frames++;
	m->cpu_sum += cpu_us;
	if (cpu_us > m->cpu_max) m->cpu_max = cpu_us;
	m->off_sum += off;
	if (off > m->off_max) m->off_max = off;
	m->hist[b]++;
	if (late) {
		m->late++;
		m->late_cpu_sum += cpu_us;
		m->late_off_sum += off;
	}
}

/* ---- Frame timeline (WAVE 9) ----
 * gui_task's way to the screen in one frame, from hooks on rbp's framebuffer ioctls and on DirectFB 1.4's flip path
 * (exported functions that libdirectfb and its window manager call through the PLT): when the frame starts drawing,
 * when it waits for the refresh (FBIO_WAITFORVSYNC), when the window manager composes the window into the layer
 * (blits and fills inside dfb_window_repaint / dfb_layer_region_flip_update) and when the picture is copied to the
 * screen (dfb_back_to_front_copy), with the rows written. Times are us after the end of the previous frame
 * (gui_frame). Single writer (gui_task). */
#define FT_NONE 0xffffffffu
enum { FT_DRAW, FT_POS, FT_REP, FT_FLIP, FT_WAIT, FT_COPY, FT_BLIT, FT_SFLIP, FT_IOCTL, FT_TYPES };
static const char *const ft_name[FT_TYPES] = { "draw", "pos", "repaint", "flip", "wait", "copy", "blit", "sflip",
					       "ioctl" };
#define FTC_REP 1	/* inside dfb_window_repaint */
#define FTC_FLIP 2	/* inside dfb_layer_region_flip_update */
#define FTC_COPY 4	/* inside dfb_back_to_front_copy(_rotation) */
#define FT_EVS 24
#define FB_PAN_ 0x4606u		/* FBIOPAN_DISPLAY */
#define FB_GET_VAR_ 0x4600u	/* FBIOGET_VSCREENINFO */
#define FB_PUT_VAR_ 0x4601u	/* FBIOPUT_VSCREENINFO */
#define FB_GET_FIX_ 0x4602u	/* FBIOGET_FSCREENINFO */
struct ft_ev {
	uint32_t t, dur;
	uint8_t type, ctx;
	uint16_t arg;			/* flip flags, ioctl request (low 16 bits), blits merged into this entry */
	int16_t x1, y1, x2, y2;		/* area written; x1 > x2 = unknown (a pan keeps its yoffset in y1) */
};
struct ft_span { uint32_t in, out, n; };	/* earliest start, latest end, calls */
struct ft_frame {
	int valid;
	uint32_t base;			/* low 32 bits of the absolute time (us) the times count from */
	uint32_t draw, pos, n_draw;	/* first drawing call outside the flip path, first pos_hook, drawing calls */
	uint32_t wait_in, wait_out;	/* the longest wait for the refresh */
	uint32_t n_wait, wait_flip;	/* waits, and those inside dfb_layer_region_flip_update */
	struct ft_span rep, flip, copy, blit;	/* blit: blits/fills on the flip path outside the copy */
	int16_t by1, by2, cy1, cy2;	/* rows written by those blits / by the copy */
	uint32_t flags, n_sflip, n_pan, n_ioctl, end;
	uint32_t nev, lost;
	struct ft_ev ev[FT_EVS];
};

static inline void ft_begin(struct ft_frame *f, uint32_t base)
{
	memset(f, 0, sizeof *f);
	f->valid = 1;
	f->base = base;
	f->draw = f->pos = f->wait_in = f->wait_out = f->end = FT_NONE;
	f->rep.in = f->flip.in = f->copy.in = f->blit.in = FT_NONE;
	f->by1 = f->cy1 = INT16_MAX;
	f->by2 = f->cy2 = INT16_MIN;
}

static inline int16_t ft_i16(int32_t v) { return v < INT16_MIN ? INT16_MIN : v > INT16_MAX ? INT16_MAX : (int16_t)v; }

static inline void ft_span_add(struct ft_span *s, uint32_t t, uint32_t dur)
{
	if (s->in == FT_NONE || t < s->in) s->in = t;
	if (!s->n || t + dur > s->out) s->out = t + dur;
	s->n++;
}

/* rows y1..y2 into the running union lo..hi (lo > hi = nothing yet) */
static inline void ft_rows(int16_t *lo, int16_t *hi, int32_t y1, int32_t y2)
{
	if (y1 > y2) return;
	if (ft_i16(y1) < *lo) *lo = ft_i16(y1);
	if (ft_i16(y2) > *hi) *hi = ft_i16(y2);
}

/* One call on gui_task's path, noted when it returns: t = its start (us after base), dur = its length, ctx = the
 * flip-path calls it ran inside (FTC_*), arg and area as in ft_ev. Drawing calls (FT_DRAW) aren't timed one by one:
 * only the first one's start counts. A run of blits in the same place becomes one entry. */
static inline void ft_note(struct ft_frame *f, int type, int ctx, uint32_t t, uint32_t dur, uint32_t arg, int32_t x1,
			   int32_t y1, int32_t x2, int32_t y2)
{
	struct ft_ev *e;
	switch (type) {
	case FT_DRAW:
		if (f->draw == FT_NONE) f->draw = t;
		f->n_draw++;
		return;
	case FT_POS:
		if (f->pos != FT_NONE) return;
		f->pos = t;
		break;
	case FT_WAIT:
		f->n_wait++;
		if (ctx & FTC_FLIP) f->wait_flip++;
		if (f->wait_in == FT_NONE || dur > f->wait_out - f->wait_in) {
			f->wait_in = t;
			f->wait_out = t + dur;
		}
		break;
	case FT_REP:
		ft_span_add(&f->rep, t, dur);
		break;
	case FT_FLIP:
		ft_span_add(&f->flip, t, dur);
		f->flags |= arg;
		break;
	case FT_COPY:
		ft_span_add(&f->copy, t, dur);
		ft_rows(&f->cy1, &f->cy2, y1, y2);
		break;
	case FT_BLIT:
		if (ctx & FTC_COPY) {
			ft_rows(&f->cy1, &f->cy2, y1, y2);
		} else {
			ft_span_add(&f->blit, t, dur);
			ft_rows(&f->by1, &f->by2, y1, y2);
		}
		break;
	case FT_SFLIP:
		f->n_sflip++;
		break;
	case FT_IOCTL:
		if (arg == FB_PAN_) f->n_pan++;
		else f->n_ioctl++;
		break;
	default:
		return;
	}
	e = f->nev ? &f->ev[f->nev - 1] : NULL;
	if (type == FT_BLIT && e && e->type == FT_BLIT && e->ctx == ctx) {
		uint32_t end = t + dur > e->t + e->dur ? t + dur : e->t + e->dur;
		if (t < e->t) e->t = t;
		e->dur = end - e->t;
		if (e->arg < 0xffff) e->arg++;
		if (x1 <= x2 && y1 <= y2) {
			if (e->x1 > e->x2) {
				e->x1 = ft_i16(x1), e->y1 = ft_i16(y1), e->x2 = ft_i16(x2), e->y2 = ft_i16(y2);
			} else {
				if (ft_i16(x1) < e->x1) e->x1 = ft_i16(x1);
				if (ft_i16(y1) < e->y1) e->y1 = ft_i16(y1);
				if (ft_i16(x2) > e->x2) e->x2 = ft_i16(x2);
				if (ft_i16(y2) > e->y2) e->y2 = ft_i16(y2);
			}
		}
		return;
	}
	if (f->nev >= FT_EVS) {
		f->lost++;
		return;
	}
	e = &f->ev[f->nev++];
	e->t = t;
	e->dur = dur;
	e->type = (uint8_t)type;
	e->ctx = (uint8_t)ctx;
	e->arg = (uint16_t)(type == FT_BLIT ? 1 : arg);
	e->x1 = ft_i16(x1), e->y1 = ft_i16(y1), e->x2 = ft_i16(x2), e->y2 = ft_i16(y2);
}

/* Minute figures: two classes of frames (0 on time, 1 late), the writes to the screen relative to the end of the
 * wait for the refresh, in 1 ms bins: below -2 ms, -2..14 ms, 14 ms and more. */
#define FT_HB 18
static inline unsigned ft_hbin(uint32_t t, uint32_t ref)
{
	int32_t d = (int32_t)(t - ref);
	if (d < -2000) return 0;
	d = (d + 2000) / 1000 + 1;
	return d > FT_HB - 1 ? FT_HB - 1 : (unsigned)d;
}
struct ft_wr {		/* one kind of write: 0 the copy to the screen, 1 blits/fills on the flip path */
	uint32_t n, dur_sum, dur_max;
	int32_t start_sum;
	uint32_t h_start[FT_HB], h_end[FT_HB];
	int16_t y1, y2;
};
struct ft_cls {
	uint32_t frames, no_wait, multi_wait, wait_flip, no_draw, draw_calls, flags, sflips, pans;
	uint32_t n_b, s_b, s_w, s_a, n_t, s_t, n_pos;	/* b = start..wait, w = wait, a = wait..end, t = start..end */
	int32_t s_pos;					/* first pos_hook relative to the end of the wait */
	struct ft_wr wr[2];
};
#define OC_MAX 12
struct ft_min {
	uint32_t seq;			/* first: seqlock (gui_task writes, the logger copies) */
	struct ft_cls c[2];
	uint32_t per_sum, per_n;	/* refresh period: time between wait ends / refreshes in it */
	uint32_t oc_frames[2], oc_sum[2][OC_MAX], oc_max[OC_MAX];	/* other core-0 threads' CPU per frame, us */
};
#define FT_REF_PERIOD 16683u	/* 60 Hz, only to count the refreshes between two wait ends */

static inline void ft_wr_add(struct ft_wr *w, const struct ft_span *s, int16_t y1, int16_t y2, uint32_t ref)
{
	uint32_t dur;
	if (!s->n) return;
	if (!w->n) {
		w->y1 = INT16_MAX;
		w->y2 = INT16_MIN;
	}
	w->n++;
	dur = s->out - s->in;
	w->dur_sum += dur;
	if (dur > w->dur_max) w->dur_max = dur;
	w->start_sum += (int32_t)(s->in - ref);
	w->h_start[ft_hbin(s->in, ref)]++;
	w->h_end[ft_hbin(s->out, ref)]++;
	ft_rows(&w->y1, &w->y2, y1, y2);
}

/* A finished frame into the minute figures. *prev_wo: the previous frame's wait end (absolute, low 32 bits; 0 =
 * none), kept by the caller across minutes. */
static inline void ft_add(struct ft_min *m, const struct ft_frame *f, int late, uint32_t *prev_wo)
{
	struct ft_cls *c = &m->c[late != 0];
	uint32_t wo;
	c->frames++;
	c->draw_calls += f->n_draw;
	c->flags |= f->flags;
	c->sflips += f->n_sflip;
	c->pans += f->n_pan;
	if (f->draw == FT_NONE) {
		c->no_draw++;
	} else if (f->end != FT_NONE && f->end >= f->draw) {
		c->n_t++;
		c->s_t += f->end - f->draw;
	}
	if (!f->n_wait) {
		c->no_wait++;
		*prev_wo = 0;
		return;
	}
	c->multi_wait += f->n_wait > 1;
	c->wait_flip += f->wait_flip > 0;
	if (f->draw != FT_NONE && f->draw <= f->wait_in) {
		c->n_b++;
		c->s_b += f->wait_in - f->draw;
	}
	c->s_w += f->wait_out - f->wait_in;
	if (f->end != FT_NONE && f->end >= f->wait_out) c->s_a += f->end - f->wait_out;
	if (f->pos != FT_NONE) {
		c->n_pos++;
		c->s_pos += (int32_t)(f->pos - f->wait_out);
	}
	ft_wr_add(&c->wr[0], &f->copy, f->cy1, f->cy2, f->wait_out);
	ft_wr_add(&c->wr[1], &f->blit, f->by1, f->by2, f->wait_out);
	wo = f->base + f->wait_out;
	if (*prev_wo) {
		uint32_t d = wo - *prev_wo, n = (d + FT_REF_PERIOD / 2) / FT_REF_PERIOD;
		if (n >= 1 && n <= 6) {
			m->per_sum += d;
			m->per_n += n;
		}
	}
	*prev_wo = wo ? wo : 1;
}

/* Another core-0 thread's CPU time in one frame (us), thread i of the published list. */
static inline void oc_add(struct ft_min *m, unsigned i, uint32_t d, int late)
{
	if (i >= OC_MAX) return;
	m->oc_sum[late != 0][i] += d;
	if (d > m->oc_max[i]) m->oc_max[i] = d;
}

/* One frame as text, in time order (ms after the previous frame's end):
 * "draw 2.1 (85 calls), pos 8.0, wait 20.1+15.8 in flip, ..., end 46.0". Returns the length. */
#define FT_PUT(...)                                                                                         \
	do {                                                                                                \
		if (len < n) {                                                                              \
			int w_ = snprintf(b + len, n - len, __VA_ARGS__);                                   \
			if (w_ > 0) len += (size_t)w_;                                                      \
		}                                                                                           \
	} while (0)
static inline size_t ft_format(char *b, size_t n, const struct ft_frame *f)
{
	struct ft_ev ev[FT_EVS], tmp;
	size_t len = 0;
	unsigned i, j, k = f->nev < FT_EVS ? f->nev : FT_EVS;
	if (!n) return 0;
	b[0] = 0;
	memcpy(ev, f->ev, k * sizeof ev[0]);
	for (i = 1; i < k; i++)	/* noted at their end, nested calls first: sort by start */
		for (j = i; j > 0 && ev[j].t < ev[j - 1].t; j--) {
			tmp = ev[j];
			ev[j] = ev[j - 1];
			ev[j - 1] = tmp;
		}
	if (f->draw != FT_NONE) FT_PUT("draw %.1f (%u calls)", f->draw / 1000.0, f->n_draw);
	else FT_PUT("no drawing");
	for (i = 0; i < k; i++) {
		const struct ft_ev *e = &ev[i];
		FT_PUT(", %s %.1f", e->type < FT_TYPES ? ft_name[e->type] : "?", e->t / 1000.0);
		if (e->type != FT_POS) FT_PUT("+%.1f", e->dur / 1000.0);
		if (e->type == FT_BLIT && e->arg > 1) FT_PUT(" x%u", e->arg);
		if (e->type == FT_FLIP) FT_PUT(" flags %x", e->arg);
		if (e->type == FT_IOCTL) FT_PUT(" %x", e->arg);
		if (e->type == FT_IOCTL && e->arg == FB_PAN_) FT_PUT(" y %d", e->y1);
		if (e->x1 <= e->x2 && e->y1 <= e->y2) FT_PUT(" [%d,%d-%d,%d]", e->x1, e->y1, e->x2, e->y2);
		if (e->ctx) FT_PUT(" in %s", e->ctx & FTC_COPY ? "copy" : e->ctx & FTC_FLIP ? "flip" : "repaint");
	}
	if (f->lost) FT_PUT(", %u more", f->lost);
	if (f->lost || f->n_wait > 1) {	/* from the frame's own figures, in case its events didn't all fit */
		if (f->n_wait)
			FT_PUT(", longest of %u waits %.1f+%.1f", f->n_wait, f->wait_in / 1000.0,
			       (f->wait_out - f->wait_in) / 1000.0);
		if (f->copy.n) FT_PUT(", copies %.1f-%.1f (%u)", f->copy.in / 1000.0, f->copy.out / 1000.0, f->copy.n);
		if (f->blit.n)
			FT_PUT(", flip-path blits %.1f-%.1f (%u)", f->blit.in / 1000.0, f->blit.out / 1000.0, f->blit.n);
	}
	if (f->end != FT_NONE) FT_PUT(", end %.1f", f->end / 1000.0);
	if (len >= n) len = n - 1;
	return len;
}

/* rbp's framebuffer requests (any thread): count, by gui_task, time in the call. A slot is claimed on first use. */
#define FB_REQS 8
struct fb_req { uint32_t req, n, gui, us_sum, us_max; };
static inline struct fb_req *fb_req_slot(struct fb_req *tab, uint32_t req)
{
	unsigned i;
	for (i = 0; i < FB_REQS; i++) {
		uint32_t seen = __atomic_load_n(&tab[i].req, __ATOMIC_RELAXED);
		if (seen == req) return &tab[i];
		if (!seen) {
			if (__atomic_compare_exchange_n(&tab[i].req, &seen, req, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				return &tab[i];
			if (seen == req) return &tab[i];	/* another thread claimed it for the same request */
		}
	}
	return NULL;
}
static inline void fb_req_add(struct fb_req *s, uint32_t us, int gui)
{
	uint32_t m = __atomic_load_n(&s->us_max, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
	if (gui) __atomic_fetch_add(&s->gui, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s->us_sum, us, __ATOMIC_RELAXED);
	while (us > m && !__atomic_compare_exchange_n(&s->us_max, &m, us, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

/* The panel timing from rbp's own FBIOGET/PUT_VSCREENINFO (struct fb_var_screeninfo, 160 bytes: 40 u32) and
 * FBIOGET_FSCREENINFO (struct fb_fix_screeninfo, 68 bytes on 32-bit ARM). One line takes (xres + margins + hsync)
 * pixel clocks, one refresh (yres + margins + vsync) lines. */
struct fb_timing {
	uint32_t xres, yres, xvirt, yvirt, yoffset, bpp, pixclock, left, right, upper, lower, hsync, vsync;
	uint32_t line_ns, frame_us;	/* 0 = no pixel clock */
};
static inline int fb_var_parse(const void *var, struct fb_timing *t)
{
	uint32_t v[40];
	memcpy(v, var, sizeof v);
	t->xres = v[0], t->yres = v[1], t->xvirt = v[2], t->yvirt = v[3], t->yoffset = v[5], t->bpp = v[6];
	t->pixclock = v[25], t->left = v[26], t->right = v[27], t->upper = v[28], t->lower = v[29];
	t->hsync = v[30], t->vsync = v[31];
	t->line_ns = t->frame_us = 0;
	if (!t->pixclock || !t->xres || !t->yres || t->xres > 8192 || t->yres > 8192) return 0;
	t->line_ns = (uint32_t)(((uint64_t)(t->xres + t->left + t->right + t->hsync) * t->pixclock + 500) / 1000);
	t->frame_us = (uint32_t)(((uint64_t)t->line_ns * (t->yres + t->upper + t->lower + t->vsync) + 500) / 1000);
	return 1;
}
struct fb_fix { char id[17]; uint32_t smem_len, line_length; uint16_t ypanstep; };
static inline void fb_fix_parse(const void *fix, struct fb_fix *x)
{
	const uint8_t *p = (const uint8_t *)fix;
	memcpy(x->id, p, 16);
	x->id[16] = 0;
	memcpy(&x->smem_len, p + 20, 4);
	memcpy(&x->ypanstep, p + 38, 2);
	memcpy(&x->line_length, p + 44, 4);
}

/* Splits a long text into lines of at most max chars, at the last ", " or "; " that fits (a short line rather than
 * a cut word); a hard cut only if there is none. Returns the length of the first line; *next = where the rest starts
 * (after the separator), or NULL at the end. */
static inline size_t text_chunk(const char *s, size_t max, const char **next)
{
	size_t n = strlen(s), cut, i;
	if (n <= max) {
		*next = NULL;
		return n;
	}
	cut = max;
	for (i = max; i > 0; i--)
		if ((s[i] == ',' || s[i] == ';') && s[i + 1] == ' ') {
			cut = i + 1;
			break;
		}
	*next = s + cut + (s[cut] == ' ');
	return cut;
}

/* ---- Waveform position at draw time (WAVE 7) ----
 * Ui_CycleTask (0x17814c) wakes every >= 16 ms (dly_tsk) and copies rbp's deck state into a snapshot (0xe49ac: memcpy
 * of 388 bytes to POS_DECKS, under rbp's lock). gui_task draws each big waveform from that snapshot: 0x1cb58c(deck)
 * turns the position (ms, +0x5c) into the playhead column (wv +0xc) that the column loop starts from. The snapshot
 * is 0-16+ ms old when a frame reads it, and that age changes from frame to frame, so the waveform moves in uneven
 * steps (at 30 fps: worth 16, 32 or 48 ms of track instead of 33).
 * SMOOTH: the bl memcpy at POS_SITE goes to pos_copy, which also stamps each copy's positions with the time; the two
 * calls of 0x1cb58c go to pos_hook, which runs Pioneer's function and then, from the stamped copies, replaces the
 * playhead column by the position at (now - delay). The delay is just over the longest copy interval, so the
 * position is interpolated between copies that are already there: never past a pause, cue or loop jump that the
 * next copy would show. A start (play after pause or cue) runs at the speed the deck last played at, from where
 * that speed puts it between the two copies. Cue and loop shading come per entry from that column, so they move with
 * it. */
#define POS_SITE	0xe4abcu	/* 0xe49ac: bl memcpy (the snapshot) */
#define POS_SITE_ORIG	0xebfca757u
#define POS_DECKS	0x01824000u	/* the snapshot: + 160 * deck */
#define POS_DECK_STEP	160u
#define POS_MS		0x5cu		/* u32 position, ms */
#define POS_SNAP_LEN	388u
#define COL_FN		0x1cb58cu	/* rbp: position -> columns, (deck) */
#define COL_SITE0	0x1ce788u	/* bl COL_FN, deck 0 */
#define COL_SITE0_ORIG	0xebfff37fu
#define COL_SITE1	0x1ce77cu	/* bl COL_FN, deck 1 */
#define COL_SITE1_ORIG	0xebfff382u
#define WV_BASE		0x010f0208u	/* the waveform view, + 0x20 * deck: +4 -> -> entry count, +0xc playhead column,
					 * +0x18 zoom (float, entries per column) */
#define WV_STEP		0x20u
#define POS_CAVE	0x59f00u	/* after OV_CAVE, in the never-called function */
#define POS_CAVE_WORDS	4
static const uint32_t pos_cave_orig[POS_CAVE_WORDS] = { 0xeb002d12, 0xe3a03000, 0xe5847000, 0xe5c63000 };

/* Two veneers: ldr pc, [pc, #-4] loads the word after it (and switches to Thumb if bit 0 is set). */
static inline void pos_cave_build(uint32_t *w, uint32_t copy_fn, uint32_t hook_fn)
{
	w[0] = 0xe51ff004u;
	w[1] = copy_fn;
	w[2] = 0xe51ff004u;
	w[3] = hook_fn;
}

/* rbp's playhead column (0x1cb58c): entries of 20/3 ms, clamped to the track, divided by the zoom, truncated. */
static inline uint32_t pos_col(double ms, uint32_t count, float zoom)
{
	double e = ms / (20.0 / 3.0);
	if (e < 0) e = 0;
	if (e > count) e = count;
	return (uint32_t)(e / (double)zoom);
}

/* Copies, stamped: written by Ui_CycleTask only, read by gui_task. Slot seq % POS_RING is written before seq moves
 * on, so the reader never reads a slot that's being written unless it falls POS_RING copies behind (checked). */
#define POS_RING 16
struct pos_sample { uint64_t t; uint32_t ms[2]; };
struct pos_ring { struct pos_sample s[POS_RING]; uint32_t seq; };

static inline void pos_ring_put(struct pos_ring *r, uint64_t t, uint32_t ms0, uint32_t ms1)
{
	uint32_t q = r->seq;
	r->s[q % POS_RING] = (struct pos_sample){ t, { ms0, ms1 } };
	__atomic_store_n(&r->seq, q + 1, __ATOMIC_RELEASE);
}

/* The next copy after *next into *out: 1, or 2 if older copies were skipped first (the reader fell behind, e.g. while
 * no deck had a track and nothing was drawn), or 0 if there's none. *lost counts copies overwritten while read. */
static inline int pos_ring_get(struct pos_ring *r, uint32_t *next, struct pos_sample *out, uint32_t *lost)
{
	uint32_t q = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
	int skipped = 0;
	if (q - *next > POS_RING - 2) {
		*next = q - (POS_RING - 2);
		skipped = 1;
	}
	while (*next != q) {
		*out = r->s[*next % POS_RING];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&r->seq, __ATOMIC_RELAXED) - *next <= POS_RING - 1) {
			(*next)++;
			return 1 + skipped;
		}
		(*lost)++;
		(*next)++;
		skipped = 1;
	}
	return 0;
}

/* One deck's position track, fed with the copies in order (times in us, positions in ms, speed v in ms per ms).
 * While playing smoothly an alpha-beta filter follows the copies (gains of a least-squares line over the first
 * samples, then fixed). A copy it doesn't explain starts a new segment; the segment before stays in use for display
 * times before the cut. */
#define POS_ALPHA	0.15	/* steady gains: beta = alpha^2 / (2 - alpha), critically damped */
#define POS_TOL_MS	3.0	/* a copy within POS_TOL_MS + POS_TOL_REL * the motion since the last one: smooth */
#define POS_TOL_REL	0.35
#define POS_V_MAX	4.0	/* faster than 4x between two copies: a jump (cue, hot cue, loop, search, new track) */
#define POS_DT_MAX	250000u	/* us; copies further apart start a new segment */
#define POS_EXTRA	8000u	/* us: a display time past the newest copy goes on at most this far */
enum { POS_CONT, POS_START, POS_STOP, POS_TURN, POS_JUMP, POS_FIRST };

struct pos_seg { uint64_t t0; double p0, v; };
struct pos_trk {
	int state;			/* 0 no copy yet, 1 standing, 2 moving */
	uint32_t n;			/* copies on the current line */
	uint64_t t_last;
	double ms_last;
	struct pos_seg cur, prev;	/* prev: for display times before cut */
	uint64_t cut;
	int has_prev;
	double v_play;			/* the speed it last played at (1.0 before the first play) */
	double res;			/* the last smooth copy's residual, ms (stats) */
};

static inline double pos_seg_at(const struct pos_seg *s, uint64_t t)
{
	return s->p0 + s->v * ((double)t - (double)s->t0) / 1000.0;
}

static inline int pos_feed(struct pos_trk *k, uint64_t t, uint32_t msu)
{
	double ms = msu, dt, d;
	int kind;
	if (!k->state || t <= k->t_last) {
		k->state = 1;
		k->n = 1;
		k->cur = (struct pos_seg){ t, ms, 0 };
		k->has_prev = 0;
		k->v_play = 1.0;
		k->t_last = t;
		k->ms_last = ms;
		return POS_FIRST;
	}
	dt = (double)(t - k->t_last) / 1000.0;
	d = ms - k->ms_last;
	if (t - k->t_last <= POS_DT_MAX) {
		if (k->state == 2) {
			double pred = pos_seg_at(&k->cur, t), r = ms - pred;
			if (fabs(r) <= POS_TOL_MS + POS_TOL_REL * fabs(k->cur.v) * dt) {
				double n = ++k->n, a = 2 * (2 * n - 1) / (n * (n + 1)), b = 6 / (n * (n + 1));
				double amin = POS_ALPHA, bmin = POS_ALPHA * POS_ALPHA / (2 - POS_ALPHA);
				if (a < amin) a = amin;
				if (b < bmin) b = bmin;
				k->cur.p0 = pred + a * r;
				k->cur.v += b * r / dt;
				k->cur.t0 = t;
				k->res = r;
				k->t_last = t;
				k->ms_last = ms;
				return POS_CONT;
			}
		} else if (d == 0) {
			k->cur = (struct pos_seg){ t, ms, 0 };
			k->t_last = t;
			k->ms_last = ms;
			return POS_CONT;
		}
	}
	k->prev = k->cur;
	k->has_prev = 1;
	if (k->state == 2) k->v_play = k->cur.v;
	if (k->state == 1 && d * k->v_play > 0 && fabs(k->v_play) >= 0.1 && fabs(d) <= 1.25 * fabs(k->v_play) * dt &&
	    t - k->t_last <= POS_DT_MAX) {
		/* started (play, pause off): at the speed it played at before, so it started d / v before this copy */
		uint64_t lead = (uint64_t)(d / k->v_play * 1000);
		kind = POS_START;
		k->cut = t - lead > k->t_last ? t - lead : k->t_last;
		k->cur = (struct pos_seg){ t, ms, k->v_play };
		k->state = 2;
		k->n = 8;
	} else if (fabs(d) <= POS_V_MAX * dt && t - k->t_last <= POS_DT_MAX) {
		/* moved at a playable speed: a new line through the last two copies */
		kind = d == 0 ? POS_STOP : k->state == 2 ? POS_TURN : POS_START;
		k->cut = k->t_last;
		k->cur = (struct pos_seg){ t, ms, d / dt };
		k->state = d == 0 ? 1 : 2;
		k->n = 2;
	} else {
		/* a jump: on from the new place at the same speed (a loop or hot cue keeps playing) */
		kind = POS_JUMP;
		k->cut = t;
		k->cur = (struct pos_seg){ t, ms, k->state == 2 ? k->cur.v : 0 };
	}
	k->t_last = t;
	k->ms_last = ms;
	return kind;
}

/* The position to show for display time tau (= now - delay); 0 before the first copy. */
static inline int pos_at(const struct pos_trk *k, uint64_t tau, double *x)
{
	if (!k->state) return 0;
	if (k->has_prev && tau < k->cut) {
		*x = pos_seg_at(&k->prev, tau);
		return 1;
	}
	if (tau > k->t_last + POS_EXTRA) tau = k->t_last + POS_EXTRA;
	*x = k->state == 2 ? pos_seg_at(&k->cur, tau) : k->cur.p0;
	return 1;
}

/* The delay follows the longest copy interval of the last 32-64 copies, plus 2 ms, slewed by at most 0.25 ms per
 * waveform drawn so the waveform never jumps when it changes. */
#define POS_DELAY_MIN	12000u
#define POS_DELAY_MAX	40000u
#define POS_DELAY_SLEW	250u
struct pos_delay { uint32_t d, blk_max, prev_max, blk_n; };

static inline void pos_delay_interval(struct pos_delay *p, uint32_t us)
{
	if (us > p->blk_max) p->blk_max = us;
	if (++p->blk_n == 32) {
		p->prev_max = p->blk_max;
		p->blk_max = 0;
		p->blk_n = 0;
	}
}

static inline uint32_t pos_delay_step(struct pos_delay *p)
{
	uint32_t m = (p->blk_max > p->prev_max ? p->blk_max : p->prev_max) + 2000u;
	if (m < POS_DELAY_MIN) m = POS_DELAY_MIN;
	if (m > POS_DELAY_MAX) m = POS_DELAY_MAX;
	if (!p->d) p->d = m;
	else if (m > p->d) p->d = m - p->d > POS_DELAY_SLEW ? p->d + POS_DELAY_SLEW : m;
	else p->d = p->d - m > POS_DELAY_SLEW ? p->d - POS_DELAY_SLEW : m;
	return p->d;
}

/* Minute figures, written by gui_task between two seq increments (odd while writing); the logger copies them when
 * seq is even and unchanged. Step errors: a frame's move minus speed x the time since the deck's last frame, for
 * the snapshot (what Pioneer shows) and for the position at the delay (SMOOTH), in us of track. */
#define POS_HIST 6	/* copy intervals: <= 17, 20, 25, 33, 50 ms, more */
static const uint32_t pos_hist_lim[POS_HIST - 1] = { 17000, 20000, 25000, 33000, 50000 };
struct pos_stats {
	uint32_t seq;
	uint32_t copies, lost, iv_min, iv_max, hist[POS_HIST];
	uint64_t iv_sum;
	uint32_t kinds[POS_FIRST + 1];
	uint32_t res_n, res_max;
	uint64_t res_sq;
	uint32_t frames, steps, err_p_max, err_o_max, starved, mismatch, raced, delay_max;
	uint64_t err_p_sq, err_o_sq, delay_sum;
};

static inline void pos_stats_copy(struct pos_stats *s, uint32_t iv_us)
{
	unsigned h = 0;
	s->copies++;
	s->iv_sum += iv_us;
	if (!s->iv_min || iv_us < s->iv_min) s->iv_min = iv_us;
	if (iv_us > s->iv_max) s->iv_max = iv_us;
	while (h < POS_HIST - 1 && iv_us > pos_hist_lim[h]) h++;
	s->hist[h]++;
}

static inline void pos_stats_step(struct pos_stats *s, double err_p_ms, double err_o_ms)
{
	uint32_t p = (uint32_t)(fabs(err_p_ms) * 1000 + 0.5), o = (uint32_t)(fabs(err_o_ms) * 1000 + 0.5);
	s->steps++;
	s->err_p_sq += (uint64_t)p * p;
	s->err_o_sq += (uint64_t)o * o;
	if (p > s->err_p_max) s->err_p_max = p;
	if (o > s->err_o_max) s->err_o_max = o;
}

/* ---- Utility menu ----
 * {u32 count; item[count]} at 0x4621a4 in rbp's .data, items of 52 bytes. The code reaches it only through
 * literal words: UT_TABLE at six places, UT_TABLE + 4 at one. */
#define UT_TABLE	0x4621a4u
#define UT_COUNT	16
#define UT_ITEM_LEN	52
#define UT_MAX		24
static const uint32_t ut_lit_table[6] = { 0xe9014, 0xe91c0, 0xe92fc, 0xe949c, 0xe9500, 0xe9c5c };
#define UT_LIT_ITEMS	0xe9c54u
#define UT_LABEL_MASTER_ATT	0x39e250u	/* "MASTER ATT." */
#define UT_LABEL_MIXER_MODE	0x39e238u	/* "MIXER MODE" */
#define UT_LABEL_LCD_BRIGHTNESS	0x39e350u	/* "LCD BRIGHTNESS" */
/* Pioneer's generic item functions: init (bx lr), start edit, change, slot 6 (returns 0), cancel */
#define UT_FN_NOP	0xe7f3cu
#define UT_FN_EDIT	0xe7f40u
#define UT_FN_CHANGE	0xe7f68u
#define UT_FN_ZERO	0xe7fa0u
#define UT_FN_CANCEL	0xe7fb0u

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One menu item: label, values, count, current = edit = -1, enabled, then the 7 functions. */
static inline void ut_item(uint8_t *it, uint32_t label, uint32_t values, uint32_t count, uint32_t fn_display,
			   uint32_t fn_commit)
{
	const uint32_t w[13] = { label, values, count, 0xffffffffu, 0xffffffffu, 1, UT_FN_NOP, fn_display,
				 UT_FN_EDIT, UT_FN_CHANGE, fn_commit, UT_FN_ZERO, UT_FN_CANCEL };
	int i;
	for (i = 0; i < 13; i++) put_le32(it + 4 * i, w[i]);
}

/* An item to insert after the item whose label pointer is 'after' (Pioneer's or one inserted before it). */
struct ut_ins {
	const uint8_t *item;
	uint32_t after;
	int done;
};

static inline void ut_emit(uint8_t *out, uint32_t *n, const uint8_t *it, struct ut_ins *ins, unsigned n_ins)
{
	uint32_t label = le32_(it);
	unsigned k;
	memcpy(out + (*n)++ * UT_ITEM_LEN, it, UT_ITEM_LEN);
	for (k = 0; k < n_ins; k++)
		if (!ins[k].done && ins[k].after == label) {
			ins[k].done = 1;
			ut_emit(out, n, ins[k].item, ins, n_ins);
		}
}

/* Build the new table: Pioneer's items in order with the insertions placed after their anchors; insertions whose
 * anchor isn't found go to the end. Returns the new count, 0 if it doesn't fit. */
static inline uint32_t ut_build(uint8_t *out, uint32_t max, const uint8_t *orig, uint32_t count, struct ut_ins *ins,
				unsigned n_ins)
{
	uint32_t i, n = 0;
	unsigned k;
	if (count + n_ins > max) return 0;
	for (k = 0; k < n_ins; k++) ins[k].done = 0;
	for (i = 0; i < count; i++) ut_emit(out, &n, orig + i * UT_ITEM_LEN, ins, n_ins);
	for (k = 0; k < n_ins; k++)
		if (!ins[k].done) {
			ins[k].done = 1;
			ut_emit(out, &n, ins[k].item, ins, n_ins);
		}
	return n;
}

/* The text buffers a display function fills (as Pioneer's 0xe8128 does): 512 bytes of UTF-16 at +34 cleared,
 * length in characters at +32. */
static inline void ut_text(uint8_t *buf, const uint16_t *s)
{
	uint16_t n = 0;
	while (s[n] && n < 255) n++;
	memset(buf + 34, 0, 512);
	memcpy(buf + 32, &n, 2);
	memcpy(buf + 34, s, (size_t)n * 2);
}

/* Pioneer's commit logic (0xe8bc4): adopt the edited value if valid and different. Returns 1 if it changed. */
static inline int ut_commit(uint8_t *it, int32_t *newval)
{
	uint32_t edit = le32_(it + 0x10), cur = le32_(it + 0xc);
	uint16_t count;
	memcpy(&count, it + 8, 2);
	if (count <= (uint16_t)edit || edit == cur) return 0;
	put_le32(it + 0xc, edit);
	*newval = (int32_t)edit;
	return 1;
}

/* ---- Master / booth mono (library's snd_pcm_writei hook) ----
 * rbp writes interleaved stereo S24_LE (24 bits in a 32-bit word) to hw:…,0 (master) and hw:…,2 (booth).
 * Mono = (L + R) / 2 on both channels: centred material keeps its level, nothing can clip. g0 -> g1 is the
 * mono amount across this buffer (0 = stereo, 1 = mono), so switching fades over one period (1.45 ms) instead
 * of jumping. The top byte is written the way the source has it (sign-extended or zero). */
static inline int32_t s24_sext(int32_t raw) { return (int32_t)((uint32_t)raw << 8) >> 8; }

static inline int32_t s24_like(int32_t raw, int32_t v)
{
	int ext = ((uint32_t)raw >> 24) == ((raw & 0x800000) ? 0xffu : 0u);
	return ext ? v : (int32_t)((uint32_t)v & 0xffffffu);
}

static inline void mono_mix_s24(int32_t *dst, const int32_t *src, unsigned long frames, float g0, float g1)
{
	unsigned long i;
	for (i = 0; i < frames; i++) {
		int32_t l = s24_sext(src[2 * i]), r = s24_sext(src[2 * i + 1]), m = (l + r) >> 1, ol, or_;
		if (g0 == 1.0f && g1 == 1.0f) {
			ol = or_ = m;
		} else {
			float g = g0 + (g1 - g0) * (float)(i + 1) / (float)frames;
			float dl = g * (float)(m - l), dr = g * (float)(m - r);
			ol = l + (int32_t)(dl + (dl >= 0 ? 0.5f : -0.5f));
			or_ = r + (int32_t)(dr + (dr >= 0 ? 0.5f : -0.5f));
		}
		dst[2 * i] = s24_like(src[2 * i], ol);
		dst[2 * i + 1] = s24_like(src[2 * i + 1], or_);
	}
}

/* Which output a PCM is: 1 master, 2 booth, 0 anything else (headphones, capture). */
static inline int pcm_role(const char *name, int stream)
{
	size_t n = name ? strlen(name) : 0;
	if (stream != 0 || n < 3 || strncmp(name, "hw:", 3) != 0) return 0;
	if (name[n - 2] == ',' && name[n - 1] == '0') return 1;
	if (name[n - 2] == ',' && name[n - 1] == '2') return 2;
	return 0;
}

/* ---- Health log (long tests) ---- */
struct cpu_ticks { uint64_t busy, total; };

/* /proc/stat: fills c[i] for the lines "cpu<i> user nice system idle iowait irq softirq"; returns how many. */
static inline int parse_proc_stat(const char *txt, struct cpu_ticks *c, int max)
{
	const char *p = txt;
	int n = 0;
	while (p && *p) {
		unsigned long long v[7] = { 0 };
		int i;
		if (!strncmp(p, "cpu", 3) && p[3] >= '0' && p[3] <= '9' &&
		    sscanf(p, "cpu%d %llu %llu %llu %llu %llu %llu %llu", &i, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
			   &v[6]) >= 5 && i >= 0 && i < max) {
			c[i].busy = v[0] + v[1] + v[2] + v[5] + v[6];
			c[i].total = c[i].busy + v[3] + v[4];
			if (i + 1 > n) n = i + 1;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return n;
}

static inline unsigned pct_of(uint64_t part, uint64_t whole)
{
	return whole ? (unsigned)((part * 100 + whole / 2) / whole) : 0;
}

/* "Key:   1234 kB" lines (/proc/meminfo, /proc/self/status): the number, or -1. */
static inline long kv_number(const char *txt, const char *key)
{
	const char *p = strstr(txt, key);
	size_t n = strlen(key);
	while (p && p != txt && p[-1] != '\n') p = strstr(p + 1, key);
	if (!p || p[n] != ':') return -1;
	return strtol(p + n + 1, NULL, 10);
}

/* /proc/self/stat: utime + stime (fields 14, 15), in clock ticks; 0 if unparsable. */
static inline uint64_t proc_stat_cpu(const char *txt)
{
	const char *p = strrchr(txt, ')');	/* the command name may contain spaces */
	unsigned long long ut = 0, st = 0;
	if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
	return ut + st;
}

/* A thread's /proc/<pid>/task/<tid>/stat (WAVE 8 thread survey): name, state, CPU ticks, nice, the CPU it last ran
 * on, real-time priority and policy (0 normal, 1 FIFO, 2 RR). Returns 1 if all fields up to the policy (41) were
 * there. */
struct task_stat {
	char comm[16];
	char state;
	uint64_t cpu;
	int nice, processor, rt_prio, policy;
};

static inline int parse_task_stat(const char *txt, struct task_stat *o)
{
	const char *a = strchr(txt, '('), *z = strrchr(txt, ')'), *p;
	unsigned long long ut = 0, st = 0;
	size_t n;
	int field;
	if (!a || !z || z < a || !z[1] || !z[2]) return 0;
	n = (size_t)(z - a - 1);
	if (n > sizeof o->comm - 1) n = sizeof o->comm - 1;
	memcpy(o->comm, a + 1, n);
	o->comm[n] = 0;
	o->state = z[2];
	o->nice = 0;
	o->processor = o->rt_prio = o->policy = -1;
	for (p = z + 2, field = 3; field <= 41; field++) {
		if (field == 14) ut = strtoull(p, NULL, 10);
		else if (field == 15) st = strtoull(p, NULL, 10);
		else if (field == 19) o->nice = (int)strtol(p, NULL, 10);
		else if (field == 39) o->processor = (int)strtol(p, NULL, 10);
		else if (field == 40) o->rt_prio = (int)strtol(p, NULL, 10);
		else if (field == 41) o->policy = (int)strtol(p, NULL, 10);
		if (field == 41) break;
		p = strchr(p, ' ');
		if (!p) return 0;
		p++;
	}
	o->cpu = ut + st;
	return 1;
}

/* The text value of "key:" in a /proc status file, without the whitespace around it; "" if missing. */
static inline void kv_text(const char *txt, const char *key, char *out, size_t n)
{
	size_t k = strlen(key), i = 0;
	const char *p = txt;
	out[0] = 0;
	while ((p = strstr(p, key)) && !((p == txt || p[-1] == '\n') && p[k] == ':')) p += k;
	if (!p) return;
	p += k + 1;
	while (*p == ' ' || *p == '\t') p++;
	while (p[i] && p[i] != '\n' && i < n - 1) {
		out[i] = p[i];
		i++;
	}
	out[i] = 0;
}

/* /proc/<pid>/task/<tid>/schedstat: time on the CPU, time waiting on the run queue (runnable but not running),
 * both ns, and the number of times it got the CPU. Returns 1 if all three were there. */
static inline int parse_schedstat(const char *txt, uint64_t *run_ns, uint64_t *wait_ns, uint64_t *slices)
{
	unsigned long long r, w, s;
	if (sscanf(txt, "%llu %llu %llu", &r, &w, &s) != 3) return 0;
	*run_ns = r;
	*wait_ns = w;
	*slices = s;
	return 1;
}

/* One line of /proc/interrupts: the label before ':' ("39", "IPI0", "LOC"), the counts of up to ncpu CPUs (summed),
 * and the rest as the name, spaces squeezed. Returns the number of counts read (0: header or unparsable). */
static inline int parse_irq_line(const char *line, char *label, size_t ln, uint64_t *sum, int ncpu, char *name,
				 size_t nn)
{
	const char *c = strchr(line, ':'), *p = line, *e = strchr(line, '\n');
	size_t i = 0;
	int k = 0;
	if (!e) e = line + strlen(line);
	if (!c || c > e) return 0;
	while (*p == ' ') p++;
	while (p < c && i < ln - 1) label[i++] = *p++;
	label[i] = 0;
	*sum = 0;
	for (p = c + 1; k < ncpu; k++) {
		char *q;
		unsigned long long v;
		while (*p == ' ' && p < e) p++;
		if (p >= e || *p < '0' || *p > '9') break;
		v = strtoull(p, &q, 10);
		*sum += v;
		p = q;
	}
	i = 0;
	while (p < e && i < nn - 1) {
		if (*p == ' ' && (i == 0 || name[i - 1] == ' ')) {
			p++;
			continue;
		}
		name[i++] = *p++;
	}
	while (i && name[i - 1] == ' ') i--;
	name[i] = 0;
	return k;
}

/* Display activity: hash of a sparse sample of each horizontal band of the framebuffer (RGB565). A band whose
 * hash changes between polls was redrawn; changes per second ≈ that band's frame rate. */
#define FB_BANDS 15
static inline void fb_band_hashes(const volatile uint16_t *fb, unsigned stride_px, unsigned w, unsigned h,
				  uint32_t *hash)
{
	unsigned b, x, k, bh = h / FB_BANDS;
	for (b = 0; b < FB_BANDS; b++) {
		uint32_t v = 2166136261u;
		for (k = 1; k <= 3; k += 2) {	/* rows at 1/4 and 3/4 of the band */
			const volatile uint16_t *row = fb + (size_t)(b * bh + k * bh / 4) * stride_px;
			for (x = 3; x < w; x += 16) v = (v ^ row[x]) * 16777619u;
		}
		hash[b] = v;
	}
}

/* The headphone mixer object in rbp 2.21 (mixerengine::HeadPhone):
 * word 0 = vtable 0x37a3f8, +0x2c mode word (1 = stereo blend = normal, 0 = split; read by the audio
 * thread on every period), +0x28 byte "master routed", smoothers with the same step count at
 * +0x4c/+0x7c/+0xac and current gains at +0x40/+0x70/+0xa0. A candidate must match all of it. */
#define HP_VTABLE	0x37a3f8u
#define HP_OBJ_LEN	0xc0
#define HP_MODE_OFF	0x2c

static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static inline int hp_gain_ok(const uint8_t *p)
{
	float g;
	uint32_t u = le32(p);
	memcpy(&g, &u, 4);
	return g == g && g >= -0.001f && g <= 16.0f;	/* finite, plausible */
}

static inline int hp_object_plausible(const uint8_t *o)
{
	uint32_t steps = le32(o + 0x4c);
	return le32(o) == HP_VTABLE && le32(o + HP_MODE_OFF) <= 1 && o[0x28] <= 1 &&
	       steps > 0 && steps < 100000 && le32(o + 0x7c) == steps && le32(o + 0xac) == steps &&
	       hp_gain_ok(o + 0x40) && hp_gain_ok(o + 0x70) && hp_gain_ok(o + 0xa0);
}

/* ---- Colour waveforms ----
 * Pioneer draws each column of a deck's detail waveform with 0x1caf58(deck, col, start, end, zoom): r0-r3, zoom in s0
 * (GCC's local calling convention), called only from the per-deck column loops at 0x1cdebc / 0x1cdff0 in gui_task.
 *   ctx = 0x10f0208 (the routine's first instruction loads it into r12); obj = **(ctx + 32*deck + 4) = the deck object
 *   at 0x28d78a0 + deck*0x2ee1c: +0 entry count, +0x2ee18 buffer of {height 0..46, whiteness 0..7} per entry,
 *   +0x2ee10 12 bytes per entry (byte 8: cue/loop shading). gui_task fills the buffer from PWV3 once per loaded track
 *   (0x12a040): height = (b & 31) * 46 / 31, whiteness = b >> 5, at most 540000 entries, index = PWV3 index.
 *   rbp's count is 20 more than the file's (the tag length minus 4); its last 20 entries come from the bytes
 *   after rbp's copy of PWV3, so they are random.
 *   h = height of start (1 < zoom <= 32: the maximum over [start, end)), 0 -> 1, >= 45 -> 45; colour = blue palette
 *   0x419d20[min(whiteness of start, 7)]. Rows y = 0..44 at canvas row 126*deck + 61 - y (canvas = *(ctx + 0x40),
 *   536 px per row): y < h colour, else the shading of start: none 0, bit 1 0xb300, bit 2 0x3186 (wins). The caller
 *   mirrors rows 0..61 into 62..123 afterwards.
 * Installed once in the constructor (no thread runs yet): the first instruction becomes a branch to a sequence at
 * 0x59ec8 (after the meter and frame-rate sequences, in the never-called function). If the word wave_fn is non-zero it
 * jumps there with every argument untouched, else it does the original instruction and goes on at 0x1caf5c. From +16
 * the same sequence is Pioneer's routine as a function the library can call. Switching only writes wave_fn. */
#if defined(__arm__)
#define WAVE_PCS __attribute__((pcs("aapcs-vfp")))	/* the float in s0, as rbp passes it */
#else
#define WAVE_PCS
#endif
typedef void wave_col_fn(uint32_t deck, uint32_t col, uint32_t start, uint32_t end, float zoom) WAVE_PCS;

#define WAVE_SITE	0x1caf58u
#define WAVE_SITE_ORIG	0xe59fc19cu	/* ldr r12, [pc, #0x19c]   r12 = ctx */
#define WAVE_RESUME	0x1caf5cu
#define WAVE_LIT_CTX	0x1cb0fcu	/* the routine's literals: ctx, palette */
#define WAVE_LIT_PAL	0x1cb100u
#define WAVE_CTX	0x010f0208u
#define WAVE_PAL	0x419d20u
#define WAVE_CAVE	0x59ec8u
#define WAVE_CAVE_WORDS	8
#define WAVE_ORIG	(WAVE_CAVE + 16)
static const uint32_t wave_cave_orig[WAVE_CAVE_WORDS] = {
	0xeb0a8f7f, 0xe5940000, 0xeafffff3, 0xe59f605c, 0xe5d63000, 0xe3530000, 0x1afffff7, 0xe59f2050
};
#define WAVE_OBJ0	0x28d78a0u	/* deck objects, deck 1 right after deck 0 */
#define WAVE_OBJ_STEP	0x2ee1cu
#define WAVE_BUF0	0x2500618u	/* {height, whiteness} buffers */
#define WAVE_BUF_STEP	1080000u
#define WAVE_FLAGS0	0x18a4518u	/* 12 bytes per entry */
#define WAVE_FLAGS_STEP	6480000u
#define WAVE_OFF_FLAGS	0x2ee10u
#define WAVE_OFF_BUF	0x2ee18u
#define WAVE_MAX	540000u		/* entries per deck: 1 h at 150 per second */
#define WAVE_EXTRA	32u		/* a deck may have this many entries more than the file (20 measured) */
#define WAVE_W		536		/* canvas px per row */
#define WAVE_DECK_ROWS	126
#define WAVE_MID	61		/* a deck's centre row */
#define WAVE_HALF	45		/* rows drawn upwards from it */
static const uint16_t wave_blue[8] = { 0x09f1, 0x0af7, 0x037f, 0x0d5e, 0x55be, 0x757f, 0x757f, 0xcf3f };	/* 0x419d20 */

static inline void wave_cave_build(uint32_t *w, uint32_t fn_word_addr)
{
	w[0] = 0xe59fc010;	/* ldr r12, [pc, #16]  -> w[6] */
	w[1] = 0xe59cc000;	/* ldr r12, [r12]      wave_fn */
	w[2] = 0xe35c0000;	/* cmp r12, #0 */
	w[3] = 0x112fff1c;	/* bxne r12            the library's column function, arguments untouched */
	w[4] = 0xe59fc004;	/* ldr r12, [pc, #4]   -> w[7]: Pioneer's first instruction */
	w[5] = arm_b(WAVE_CAVE + 20, WAVE_RESUME);
	w[6] = fn_word_addr;
	w[7] = WAVE_CTX;
}

static inline uint8_t pwv3_height(uint8_t b) { return (uint8_t)((b & 31) * 46 / 31); }

/* PWV5 entry (big-endian u16: red 15-13, green 12-10, blue 9-7, height 6-2) -> RGB565. 0 = black = no colour. */
static inline uint16_t pwv5_rgb565(uint16_t v)
{
	static const uint8_t c5[8] = { 0, 4, 9, 13, 18, 22, 27, 31 };	/* (x * 31 + 3) / 7 */
	static const uint8_t c6[8] = { 0, 9, 18, 27, 36, 45, 54, 63 };
	return (uint16_t)(c5[v >> 13] << 11 | c6[(v >> 10) & 7] << 5 | c5[(v >> 7) & 7]);
}

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* rekordbox analysis file (.DAT/.EXT/.2EX): "PMAI", u32 header length, u32 file length, then tags of 4 chars, u32
 * header length, u32 tag length (big-endian). Returns the tag's start with *hl and *tl, or NULL if it is missing or
 * the file is inconsistent before it (sizes outside the file). */
static inline const uint8_t *anlz_tag(const uint8_t *d, size_t n, const char *tag, uint32_t *hl, uint32_t *tl)
{
	size_t o;
	if (n < 12 || memcmp(d, "PMAI", 4) != 0) return NULL;
	o = be32(d + 4);
	if (o < 12) return NULL;
	for (; o <= n && n - o >= 12; o += *tl) {
		*hl = be32(d + o + 4);
		*tl = be32(d + o + 8);
		if (*hl < 12 || *tl < *hl || *tl > n - o) return NULL;
		if (memcmp(d + o, tag, 4) == 0) return d + o;
	}
	return NULL;
}

/* A waveform tag's header has the entry size and count at +12/+16; the entries start at its header length. Returns
 * them and *count, or NULL if the tag is missing or anything is inconsistent (other entry size, more entries than
 * bytes). */
static inline const uint8_t *anlz_entries(const uint8_t *d, size_t n, const char *tag, uint32_t size, uint32_t *count)
{
	uint32_t hl, tl, c;
	const uint8_t *p = anlz_tag(d, n, tag, &hl, &tl);
	if (!p || hl < 20 || be32(p + 12) != size) return NULL;
	c = be32(p + 16);
	if (c == 0 || c > (tl - hl) / size) return NULL;
	*count = c;
	return p + hl;
}

/* Does a deck buffer of count entries hold exactly this PWV3 data (n entries, at most WAVE_MAX), followed by at most
 * WAVE_EXTRA entries of anything (rbp reads 20 bytes past PWV3)? */
static inline int wave_matches(const uint8_t *pwv3, uint32_t n, const uint8_t *buf, uint32_t count)
{
	uint32_t i;
	if (n == 0 || count < n || count - n > WAVE_EXTRA || count > WAVE_MAX) return 0;
	for (i = 0; i < n; i++)
		if (buf[2 * i] != pwv3_height(pwv3[i]) || buf[2 * i + 1] != pwv3[i] >> 5) return 0;
	return 1;
}

/* Fingerprint of a deck buffer: its count and 16 entries spread over it (no division: the RX's core has none). */
static inline uint32_t wave_sig(uint32_t count, const uint8_t *buf)
{
	uint32_t h = 2166136261u ^ count, k;
	if (count == 0 || count > WAVE_MAX) return h;
	for (k = 1; k <= 16; k++) {
		uint32_t i = (uint32_t)(((uint64_t)count * (k * 3855u)) >> 16);	/* ≈ count * k / 17, < count */
		h = (h ^ buf[2 * i]) * 16777619u;
		h = (h ^ buf[2 * i + 1]) * 16777619u;
	}
	return h;
}

/* One column as 0x1caf58 draws it. rgb = a colour per entry (n of them; 0 = none) or NULL: Pioneer's blue. With
 * colours, a column covering several entries takes the colour of its tallest one (Pioneer: the first one's). */
static inline void wave_draw_col(uint16_t *canvas, uint32_t deck, uint32_t col, const uint8_t *buf, const uint8_t *flags,
				 uint32_t start, uint32_t end, float zoom, const uint16_t *rgb, uint32_t n)
{
	uint32_t h = buf[2 * start], top = start, i, y;
	uint16_t c = 0, *px;
	uint8_t f = flags[12 * start + 8];
	if (zoom > 1.0f && zoom <= 32.0f)	/* false for NaN, as Pioneer's vcmpe/ble/bls */
		for (i = start + 1; i < end; i++)
			if (buf[2 * i] > h) {
				h = buf[2 * i];
				top = i;
			}
	if (h == 0)
		h = 1;
	else if (h > WAVE_HALF)
		h = WAVE_HALF;
	if (rgb && top < n) c = rgb[top];
	if (!c) c = wave_blue[buf[2 * start + 1] < 7 ? buf[2 * start + 1] : 7];
	px = canvas + (WAVE_DECK_ROWS * deck + WAVE_MID) * WAVE_W + col;
	for (y = 0; y < WAVE_HALF; y++, px -= WAVE_W)
		*px = y < h ? c : !(f & 6) ? 0 : (f & 4) ? 0x3186 : 0xb300;
}

/* ---- 3-band detail waveform (wave-4) ----
 * PWV7 in the .2EX beside the .EXT: 3 bytes per entry, low, mid, high (measured against the audio), as many as
 * PWV3. Drawn the way the CDJ-2000NXS2 three_band mod draws it: each byte clamped to 0..128 and made a step of 1..31
 * (low and mid linear, high on a raised cosine), 1.5 rows per step (1..45 rows up from the centre; rbp mirrors them
 * as always). Every row gets the colour of the bands that reach it: low blue, mid amber, high white, overlaps
 * blended. The worker turns each entry into its three row counts once (band_rows); a column takes each band's
 * maximum over the entries Pioneer's rule gives it. */
static const uint8_t band_hi31[129] = {	/* ceil(31 * (0.5 - 0.5 * cos(pi * v / 128))) */
	0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
	2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
	5, 5, 6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10,
	10, 10, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 16,
	16, 16, 17, 17, 18, 18, 18, 19, 19, 19, 20, 20, 20, 21, 21, 22,
	22, 22, 23, 23, 23, 24, 24, 24, 25, 25, 25, 26, 26, 26, 26, 27,
	27, 27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 29, 30, 30, 30, 30,
	30, 30, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
	31,
};
/* Row colour by the bands that reach it (low 4 | mid 2 | high 1), overlaps blended as the NXS2 mod does. WAVE 6 drew
 * opaque layers instead (0xffff, 0xffff, 0xfd20, 0xffff); a second rekordbox screenshot matches the blend. */
static const uint16_t band_rgb[8] = { 0x0000, 0xffff, 0xfd20, 0xff9a, 0x02bc, 0xd6ff, 0xb341, 0xf75a };
/* The cue/loop shading in 3-band: dark red-brown, as Pioneer's 0xb300 is the low + mid blend's colour. */
#define BAND_LOOP 0x50c0

/* Rows per half for one PWV7 entry: out[0..2] = low, mid, high, each 1..WAVE_HALF. */
static inline void band_rows(const uint8_t *e, uint8_t *out)
{
	uint32_t k;
	for (k = 0; k < 3; k++) {
		uint32_t v = e[k] < 128 ? e[k] : 128;
		uint32_t step = k < 2 ? (31 * v + 127) >> 7 : band_hi31[v], r;
		if (step < 1) step = 1;
		r = 3 * step >> 1;
		out[k] = (uint8_t)(r > WAVE_HALF ? WAVE_HALF : r);
	}
}

/* One column in 3-band: Pioneer's entry range (wave_draw_col), canvas and shading (bit 1 BAND_LOOP); the rows of each band from b (3
 * per entry, n entries). A column that starts past the track's entries (rbp's extra ones) is Pioneer's blue one. */
static inline void wave_draw_col3(uint16_t *canvas, uint32_t deck, uint32_t col, const uint8_t *buf, const uint8_t *flags,
				  uint32_t start, uint32_t end, float zoom, const uint8_t *b, uint32_t n)
{
	uint32_t lo, mi, hi, i, y, last = start + 1;
	uint8_t f = flags[12 * start + 8];
	uint16_t *px;
	if (start >= n) {
		wave_draw_col(canvas, deck, col, buf, flags, start, end, zoom, NULL, 0);
		return;
	}
	if (zoom > 1.0f && zoom <= 32.0f && end > last)
		last = end < n ? end : n;
	lo = b[3 * start];
	mi = b[3 * start + 1];
	hi = b[3 * start + 2];
	for (i = start + 1; i < last; i++) {
		if (b[3 * i] > lo) lo = b[3 * i];
		if (b[3 * i + 1] > mi) mi = b[3 * i + 1];
		if (b[3 * i + 2] > hi) hi = b[3 * i + 2];
	}
	px = canvas + (WAVE_DECK_ROWS * deck + WAVE_MID) * WAVE_W + col;
	for (y = 0; y < WAVE_HALF; y++, px -= WAVE_W) {
		uint32_t k = (y < lo) << 2 | (y < mi) << 1 | (y < hi);
		*px = k ? band_rgb[k] : !(f & 6) ? 0 : (f & 4) ? 0x3186 : BAND_LOOP;
	}
}

/* ---- Colour overview (wave-3) ----
 * The overview strip is a 300 x 34 RGB565 surface per deck, drawn by 0x1f8518 (no arguments). Its only caller is the
 * bl at 0x1fb3e8 in the view handler 0x1f98d8 (gui_task), on every pass while the deck has an overview (flag at
 * 0x10829d8 + 4 * deck, cleared when the track goes):
 *   deck = *0x123cd50, surface = *(0x123cd50 + 0x10 + 4 * deck); lock it (0x13ca98: pixels, pitch in bytes; non-zero
 *   = failed), clear 34 rows, then for x = 0..299 the entry s = 0, 1, 2, 3, 5, 6, 7, 9, ... (every multiple of 4 but 0
 *   skipped: 400 -> 300), stopping at the first height 0: a bar of h * 34 / 26 rows (at most 34) up from the bottom
 *   row, colour 0x41cf20[min(whiteness, 6)] (+6 if 0x121da8() != 0: it returns 0); unlock (0x13cb00). The played
 *   part is another surface over it (0x1f86e0).
 *   The entries are {height, whiteness} pairs at 0x18a2ee4 + 900 * deck: gui_task copies them there when a track is
 *   loaded (0x12a620): PWAV from the .DAT (400 bytes: height = b & 31, whiteness = b >> 5), then normalises the
 *   heights (0x127ec4): f = max ? min(2500 / max, 252) : 252, h = h * f / 100.
 * Installed with the column sequence: the bl at 0x1fb3e8 goes to a sequence at 0x59ee8 that jumps to the function in
 * ov_fn if it is non-zero (lr untouched: it returns to 0x1fb3ec), else on to 0x1f8518. In RGB mode the library draws
 * the strip exactly so, with a colour per entry from PWV4 (the .EXT's colour overview: 1200 entries, three per PWAV
 * entry), when a published track's overview is byte for byte what rbp holds; anything else is Pioneer's routine. */
#define OV_SITE		0x1fb3e8u
#define OV_SITE_ORIG	0xebfff44au	/* bl 0x1f8518 */
#define OV_ORIG		0x1f8518u
#define OV_LIT		0x1f86d4u	/* the routine's literals: OV_VIEW, OV_BUF, OV_PAL - 0x160 */
#define OV_VIEW		0x0123cd50u	/* the deck being drawn; + 0x10 + 4 * deck: its surface */
#define OV_BUF		0x018a2ee4u
#define OV_DECK_BYTES	900u		/* per deck: 400 pairs, then 100 PWV2 bytes */
#define OV_PAL		0x41cf20u
#define OV_LOCK		0x13ca98u
#define OV_UNLOCK	0x13cb00u
#define OV_STUB		0x121da8u
#define OV_CAVE		0x59ee8u	/* right after the column sequence */
#define OV_CAVE_WORDS	6
#define OV_N		400		/* PWAV entries */
#define OV_P4		1200		/* PWV4 entries */
#define OV_W		300
#define OV_H		34
static const uint32_t ov_stub_orig[2] = { 0xe3a00000, 0xe12fff1e };	/* mov r0, #0; bx lr */
static const uint32_t ov_cave_orig[OV_CAVE_WORDS] = { 0xe3a03001, 0xe3a00054, 0xe5c63000, 0xe5c23000, 0xebfecea3, 0xe1a07000 };
static const uint16_t ov_blue[7] = { 0x42d3, 0x42d3, 0x42d3, 0x42d3, 0x6d1e, 0x6d1e, 0x00cc };	/* 0x41cf20 */

typedef void ov_orig_fn(void);
typedef int ov_lock_fn(uint32_t surface, void **px, uint32_t *pitch);
typedef void ov_unlock_fn(uint32_t surface);

static inline void ov_cave_build(uint32_t *w, uint32_t fn_word_addr)
{
	w[0] = 0xe59fc00c;	/* ldr r12, [pc, #12]  -> w[5] */
	w[1] = 0xe59cc000;	/* ldr r12, [r12]      ov_fn */
	w[2] = 0xe35c0000;	/* cmp r12, #0 */
	w[3] = 0x112fff1c;	/* bxne r12            the library's routine */
	w[4] = arm_b(OV_CAVE + 16, OV_ORIG);
	w[5] = fn_word_addr;
}

/* PWAV (in the .DAT): header +12 the entry count, +16 0x00010000; one byte per entry. NULL unless it has OV_N. */
static inline const uint8_t *anlz_pwav(const uint8_t *d, size_t n)
{
	uint32_t hl, tl;
	const uint8_t *p = anlz_tag(d, n, "PWAV", &hl, &tl);
	return p && hl >= 16 && be32(p + 12) == OV_N && tl - hl >= OV_N ? p + hl : NULL;
}

/* The overview entries rbp makes from a PWAV (0x12a620, 0x127ec4): OV_N {height, whiteness} pairs. */
static inline void ov_expect(const uint8_t *pwav, uint8_t *ov)
{
	uint32_t i, m = 0, f;
	for (i = 0; i < OV_N; i++)
		if ((pwav[i] & 31u) > m) m = pwav[i] & 31u;
	f = m ? 2500 / m : 252;
	if (f > 252) f = 252;
	for (i = 0; i < OV_N; i++) {
		ov[2 * i] = (uint8_t)((pwav[i] & 31u) * f / 100);
		ov[2 * i + 1] = pwav[i] >> 5;
	}
}

/* A colour per PWAV entry from PWV4 (OV_P4 entries of 6 bytes; bytes 3, 4, 5 red, green, blue as beat-link reads
 * them): the sum over its three entries, scaled so the strongest channel is full, RGB565. 0 = none (all three 0). */
static inline void ov_colours(const uint8_t *p4, uint16_t *ovc)
{
	uint32_t i, j;
	for (i = 0; i < OV_N; i++) {
		uint32_t r = 0, g = 0, b = 0, m;
		for (j = 3 * i; j < 3 * i + 3; j++) {
			r += p4[6 * j + 3];
			g += p4[6 * j + 4];
			b += p4[6 * j + 5];
		}
		m = r > g ? r : g;
		if (b > m) m = b;
		ovc[i] = m ? (uint16_t)((r * 255 / m) >> 3 << 11 | (g * 255 / m) >> 2 << 5 | (b * 255 / m) >> 3) : 0;
	}
}

/* 3-band overview from PWV6 (the .2EX: OV_P4 entries of 3 bytes, low, mid, high). Per PWAV entry the rows of low,
 * of low + mid and of all three (each 1..OV_H), stacked up from the bottom, as the CDJ-2000NXS2 three_band mod
 * computes them: weighted 1.2 low, 0.6 mid and 1.2 * 128 * C(high) (C the raised cosine), over the track's largest
 * unweighted stack (clamped to 1..128); of the 3 PWV6 entries of a PWAV entry the one with the tallest stack. */
static const uint8_t band_c8[129] = {	/* round(128 * (0.5 - 0.5 * cos(pi * h / 128))) */
	0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4,
	5, 5, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
	19, 20, 21, 22, 23, 25, 26, 27, 28, 30, 31, 32, 34, 35, 37, 38,
	40, 41, 42, 44, 45, 47, 48, 50, 52, 53, 55, 56, 58, 59, 61, 62,
	64, 66, 67, 69, 70, 72, 73, 75, 76, 78, 80, 81, 83, 84, 86, 87,
	88, 90, 91, 93, 94, 96, 97, 98, 100, 101, 102, 103, 105, 106, 107, 108,
	109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 120, 121, 122, 123,
	123, 124, 124, 125, 125, 126, 126, 126, 127, 127, 127, 128, 128, 128, 128, 128,
	128,
};

static inline void ov_bands(const uint8_t *p6, uint8_t *b3)
{
	uint32_t i, j, k, d = 1;
#define OV_CLAMP(v) ((v) < 128 ? (uint32_t)(v) : 128u)
	for (j = 0; j < OV_P4; j++) {
		uint32_t u = OV_CLAMP(p6[3 * j]) + OV_CLAMP(p6[3 * j + 1]) + band_c8[OV_CLAMP(p6[3 * j + 2])];
		if (u > d) d = u;
	}
	if (d > 128) d = 128;
	for (i = 0; i < OV_N; i++) {
		uint32_t st[3] = { 0, 0, 0 };
		for (j = 3 * i; j < 3 * i + 3; j++) {
			uint32_t l = 6 * OV_CLAMP(p6[3 * j]), m = l + 3 * OV_CLAMP(p6[3 * j + 1]);
			uint32_t f = m + 6 * band_c8[OV_CLAMP(p6[3 * j + 2])];
			if (j == 3 * i || f > st[2]) {
				st[0] = l;
				st[1] = m;
				st[2] = f;
			}
		}
		for (k = 0; k < 3; k++) {
			uint32_t r = (OV_H * st[k] + 5 * d - 1) / (5 * d);	/* ceil(OV_H * stack / D), stacks * 5 */
			b3[3 * i + k] = (uint8_t)(r < 1 ? 1 : r > OV_H ? OV_H : r);
		}
	}
#undef OV_CLAMP
}

/* The strip as 0x1f8518 draws it into the locked surface (pitch px per row). ov = the deck's pairs; rgb = a colour per
 * entry (0 = none) or NULL: Pioneer's colours. With b3 (ov_bands) the bars are the stacked bands instead. */
static inline void ov_strip(uint16_t *px, uint32_t pitch, const uint8_t *ov, const uint16_t *rgb, const uint8_t *b3)
{
	uint32_t x, y, s = 0;
	memset(px, 0, (size_t)pitch * 2 * OV_H);
	for (x = 0; x < OV_W && ov[2 * s]; x++) {
		uint32_t bar = (uint32_t)((uint64_t)(34u * ov[2 * s]) * 0x4ec4ec4fu >> 35) & 0xff, w = ov[2 * s + 1];
		uint16_t c = rgb ? rgb[s] : 0, *p = px + pitch * (OV_H - 1) + x;
		if (!c) c = ov_blue[w < 6 ? w : 6];
		if (bar > OV_H) bar = OV_H;
		if (b3)
			for (y = 0; y < OV_H; y++, p -= pitch)
				*p = y < b3[3 * s] ? band_rgb[4] : y < b3[3 * s + 1] ? band_rgb[2] : y < b3[3 * s + 2] ? band_rgb[1] : 0;
		else
			for (y = 0; y < OV_H; y++, p -= pitch) *p = y < bar ? c : 0;
		if (!(++s & 3)) s++;
	}
}

static inline void ov_draw_strip(uint16_t *px, uint32_t pitch, const uint8_t *ov, const uint16_t *rgb)
{
	ov_strip(px, pitch, ov, rgb, NULL);
}

/* A track's colours, made by the worker from its .EXT file (PWV3 kept to recognise it in a deck's buffer). Only
 * the worker changes or frees one, and only while it is on no deck and no column or overview call can still hold it. */
struct wave_track {
	uint32_t n;			/* entries, at most WAVE_MAX */
	uint32_t sig[2];		/* per deck: the buffer fingerprint it was matched with */
	uint32_t count[2];		/* per deck: the deck's entry count then (n to n + WAVE_EXTRA) */
	uint32_t key;			/* the file it came from (path and size) */
	uint32_t file;			/* its path alone (0 = unknown): a newer copy of the file replaces this one */
	uint32_t used;			/* LRU stamp */
	const uint16_t *rgb;		/* colour per entry, 0 = none */
	const uint8_t *pwv3;
	const uint8_t *ovp;		/* the overview pairs rbp makes from the .DAT (OV_N), or NULL */
	const uint16_t *ovc;		/* and a colour for each, 0 = none */
	const uint8_t *bands;		/* 3-band rows per entry (band_rows, 3 * n), or NULL */
	const uint8_t *ov3;		/* the overview's 3-band rows (ov_bands, 3 * OV_N; only with ovp), or NULL */
};

/* A track from the bytes of its .EXT file: PWV3 and PWV5 with the same number of entries (one malloc). NULL if
 * either is missing or broken, or no memory. With the .DAT's PWAV (dat may be NULL) and the .EXT's PWV4 also the
 * overview, else ovp and ovc are NULL. With the .2EX (x2 may be NULL): PWV7 with PWV3's count gives bands, PWV6 (and
 * the overview) ov3; else they are NULL. */
static inline struct wave_track *wave_track_new(const uint8_t *ext, size_t len, const uint8_t *dat, size_t dlen,
						const uint8_t *x2, size_t x2len, uint32_t key)
{
	uint32_t n3, n5, n4 = 0, n7 = 0, n6 = 0, n, i;
	const uint8_t *p3 = anlz_entries(ext, len, "PWV3", 1, &n3);
	const uint8_t *p5 = anlz_entries(ext, len, "PWV5", 2, &n5);
	const uint8_t *p4 = anlz_entries(ext, len, "PWV4", 6, &n4);
	const uint8_t *pa = dat ? anlz_pwav(dat, dlen) : NULL;
	const uint8_t *p7 = x2 ? anlz_entries(x2, x2len, "PWV7", 3, &n7) : NULL;
	const uint8_t *p6 = x2 ? anlz_entries(x2, x2len, "PWV6", 3, &n6) : NULL;
	size_t ovb;
	struct wave_track *t;
	uint16_t *rgb, *ovc;
	uint8_t *pw, *b7, *b6;
	if (!p3 || !p5 || n3 != n5) return NULL;
	if (!p4 || n4 != OV_P4) pa = NULL;
	if (n7 != n3) p7 = NULL;
	if (!pa || n6 != OV_P4) p6 = NULL;
	n = n3 < WAVE_MAX ? n3 : WAVE_MAX;
	ovb = (pa ? 4 * OV_N : 0) + (p7 ? 3 * (size_t)n : 0) + (p6 ? 3 * OV_N : 0);
	t = malloc(sizeof *t + (size_t)n * 3 + ovb);
	if (!t) return NULL;
	rgb = (uint16_t *)(t + 1);
	ovc = rgb + n;
	pw = (uint8_t *)(ovc + (pa ? OV_N : 0));
	b7 = pw + n + (pa ? 2 * OV_N : 0);
	b6 = b7 + (p7 ? 3 * (size_t)n : 0);
	for (i = 0; i < n; i++) {
		rgb[i] = pwv5_rgb565((uint16_t)(p5[2 * i] << 8 | p5[2 * i + 1]));
		pw[i] = p3[i];
		if (p7) band_rows(p7 + 3 * i, b7 + 3 * i);
	}
	memset(t, 0, sizeof *t);
	if (pa) {
		ov_colours(p4, ovc);
		ov_expect(pa, pw + n);
		t->ovc = ovc;
		t->ovp = pw + n;
	}
	if (p7) t->bands = b7;
	if (p6) {
		ov_bands(p6, b6);
		t->ov3 = b6;
	}
	t->n = n;
	t->key = key;
	t->rgb = rgb;
	t->pwv3 = pw;
	return t;
}

struct wave_env {
	uintptr_t base;			/* 0 in rbp; the tests put rbp's addresses into a buffer */
	uint32_t ctx;			/* WAVE_CTX */
	wave_col_fn *orig;		/* Pioneer's routine (WAVE_ORIG) */
	struct wave_track *pub[2];	/* set by the worker: the track on each deck, NULL = Pioneer's colours */
	uint32_t inflight;		/* column calls that may hold a track: the worker frees one only at 0 */
	uint32_t ours, stock;		/* columns drawn, read by the log */
	ov_orig_fn *ov_orig;		/* Pioneer's overview routine (OV_ORIG) */
	ov_lock_fn *ov_lock;		/* OV_LOCK */
	ov_unlock_fn *ov_unlock;	/* OV_UNLOCK */
	uint32_t ov_ours, ov_stock;	/* overviews drawn, read by the log */
};

/* The column function in RGB (bands 0) or 3-band mode. Draws with the deck's track only if every address is the
 * expected one, Pioneer's buffer still holds what the track was matched with (count and fingerprint, on every column:
 * ~0.3 ms per frame, and no frame can mix two tracks) and, in 3-band, the track has bands; anything else is
 * Pioneer's own routine. */
static inline void wave_col_any(struct wave_env *e, int bands, uint32_t deck, uint32_t col, uint32_t start, uint32_t end,
				float zoom)
{
	if (deck < 2 && col < WAVE_W && start < WAVE_MAX && end <= WAVE_MAX) {
#define WAVE_U32(a) (*(const uint32_t *)(e->base + (a)))
		uint32_t p = WAVE_U32(e->ctx + 32 * deck + 4);
		uint32_t obj = p ? WAVE_U32(p) : 0;
		if (obj == WAVE_OBJ0 + deck * WAVE_OBJ_STEP) {
			uint32_t count = WAVE_U32(obj);
			uint32_t buf = WAVE_U32(obj + WAVE_OFF_BUF);
			uint32_t flags = WAVE_U32(obj + WAVE_OFF_FLAGS);
			uint32_t canvas = WAVE_U32(e->ctx + 0x40);
#undef WAVE_U32
			if (buf == WAVE_BUF0 + deck * WAVE_BUF_STEP && flags == WAVE_FLAGS0 + deck * WAVE_FLAGS_STEP && canvas &&
			    count <= WAVE_MAX) {
				const uint8_t *b = (const uint8_t *)(e->base + buf);
				struct wave_track *t;
				__atomic_add_fetch(&e->inflight, 1, __ATOMIC_SEQ_CST);
				t = __atomic_load_n(&e->pub[deck], __ATOMIC_SEQ_CST);
				if (t && t->count[deck] == count && t->sig[deck] == wave_sig(count, b) && (!bands || t->bands)) {
					if (bands)
						wave_draw_col3((uint16_t *)(e->base + canvas), deck, col, b,
							       (const uint8_t *)(e->base + flags), start, end, zoom, t->bands, t->n);
					else
						wave_draw_col((uint16_t *)(e->base + canvas), deck, col, b,
							      (const uint8_t *)(e->base + flags), start, end, zoom, t->rgb, t->n);
					__atomic_sub_fetch(&e->inflight, 1, __ATOMIC_RELEASE);
					__atomic_add_fetch(&e->ours, 1, __ATOMIC_RELAXED);
					return;
				}
				__atomic_sub_fetch(&e->inflight, 1, __ATOMIC_RELEASE);
			}
		}
	}
	__atomic_add_fetch(&e->stock, 1, __ATOMIC_RELAXED);
	e->orig(deck, col, start, end, zoom);
}

static inline void wave_col_run(struct wave_env *e, uint32_t deck, uint32_t col, uint32_t start, uint32_t end, float zoom)
{
	wave_col_any(e, 0, deck, col, start, end, zoom);
}

static inline void wave_col_run3(struct wave_env *e, uint32_t deck, uint32_t col, uint32_t start, uint32_t end, float zoom)
{
	wave_col_any(e, 1, deck, col, start, end, zoom);
}

/* The overview routine in RGB (bands 0) or 3-band mode. Draws with a published track (the deck's, else the other
 * deck's: the same track may be on both) only if its overview is exactly what rbp holds for the deck now (800 bytes,
 * on every call) and, in 3-band, it has the 3-band overview; then with the same lock, clearing and columns as
 * Pioneer, from the track's copy. Anything else is Pioneer's routine. */
static inline void ov_any(struct wave_env *e, int bands)
{
	uint32_t d = *(const uint32_t *)(e->base + OV_VIEW);
	if (d < 2) {
		const uint8_t *buf = (const uint8_t *)(e->base + OV_BUF + OV_DECK_BYTES * d);
		struct wave_track *t = NULL;
		uint32_t k;
		__atomic_add_fetch(&e->inflight, 1, __ATOMIC_SEQ_CST);
		for (k = 0; k < 2; k++) {
			t = __atomic_load_n(&e->pub[d ^ k], __ATOMIC_SEQ_CST);
			if (t && t->ovp && (!bands || t->ov3) && memcmp(buf, t->ovp, 2 * OV_N) == 0) break;
		}
		if (k < 2) {
			uint32_t s = *(const uint32_t *)(e->base + OV_VIEW + 0x10 + 4 * d), pitch = 0;
			void *px = NULL;
			if (s && e->ov_lock(s, &px, &pitch) == 0) {
				if (px) ov_strip(px, pitch >> 1, t->ovp, bands ? NULL : t->ovc, bands ? t->ov3 : NULL);
				e->ov_unlock(s);
			}
			__atomic_sub_fetch(&e->inflight, 1, __ATOMIC_RELEASE);
			__atomic_add_fetch(&e->ov_ours, 1, __ATOMIC_RELAXED);
			return;
		}
		__atomic_sub_fetch(&e->inflight, 1, __ATOMIC_RELEASE);
	}
	__atomic_add_fetch(&e->ov_stock, 1, __ATOMIC_RELAXED);
	e->ov_orig();
}

static inline void ov_run(struct wave_env *e)
{
	ov_any(e, 0);
}

static inline void ov_run3(struct wave_env *e)
{
	ov_any(e, 1);
}

/* ---- The worker's side: a few tracks cached, matched to the decks' buffers, published. ---- */
#define WAVE_SLOTS 4
struct wave_cache {
	struct wave_track *slot[WAVE_SLOTS];
	uint32_t stamp, gen;		/* LRU clock; tracks added so far */
	uint32_t seen[2], tried[2];	/* per deck: the fingerprint and gen of the last match attempt */
	int dirty;			/* a track was taken off a deck and no quiet moment has been seen since */
	uint32_t matched, dropped;	/* counters for the log */
};

/* What deck d's buffer holds (read from rbp's memory), or 0 if it holds nothing usable. */
static inline int wave_deck(const struct wave_env *e, uint32_t d, uint32_t *count, const uint8_t **buf, uint32_t *sig)
{
	uintptr_t obj = e->base + WAVE_OBJ0 + d * WAVE_OBJ_STEP;
	uint32_t c = *(const uint32_t *)obj, b = *(const uint32_t *)(obj + WAVE_OFF_BUF);
	if (b != WAVE_BUF0 + d * WAVE_BUF_STEP || c == 0 || c > WAVE_MAX) return 0;
	*count = c;
	*buf = (const uint8_t *)(e->base + b);
	*sig = wave_sig(c, *buf);
	return 1;
}

/* May the worker change or free tracks that are on no deck? Only once a quiet moment (no column call running) has
 * been seen after the last one came off a deck. quiet(e) waits a little for one. */
static inline int wave_settled(struct wave_env *e, struct wave_cache *c, int (*quiet)(struct wave_env *))
{
	if (c->dirty && quiet(e)) c->dirty = 0;
	return !c->dirty;
}

static inline struct wave_track *wave_find(struct wave_cache *c, uint32_t key)
{
	int k;
	for (k = 0; k < WAVE_SLOTS; k++)
		if (c->slot[k] && c->slot[k]->key == key) {
			c->slot[k]->used = ++c->stamp;
			return c->slot[k];
		}
	return NULL;
}

/* Cache a new track: in place of an older copy of its file on no deck (rbp writes cues into the files), else
 * in a free slot, else in place of the least recently used one on no deck. 0 = not taken (caller frees it). */
static inline int wave_add(struct wave_env *e, struct wave_cache *c, struct wave_track *t, int (*quiet)(struct wave_env *))
{
	int k, v = -1;
	if (t->file && wave_settled(e, c, quiet))
		for (k = 0; k < WAVE_SLOTS && v < 0; k++)
			if (c->slot[k] && c->slot[k]->file == t->file && c->slot[k] != e->pub[0] && c->slot[k] != e->pub[1]) {
				free(c->slot[k]);
				c->slot[k] = NULL;
				v = k;
			}
	for (k = 0; k < WAVE_SLOTS && v < 0; k++)
		if (!c->slot[k]) v = k;
	if (v < 0) {
		if (!wave_settled(e, c, quiet)) {
			c->dropped++;
			return 0;
		}
		for (k = 0; k < WAVE_SLOTS; k++)
			if (c->slot[k] != e->pub[0] && c->slot[k] != e->pub[1] && (v < 0 || c->slot[k]->used < c->slot[v]->used))
				v = k;
		if (v < 0) {
			c->dropped++;
			return 0;
		}
		free(c->slot[v]);
	}
	t->used = ++c->stamp;
	c->slot[v] = t;
	c->gen++;
	return 1;
}

/* One look at both decks (every 50 ms). A deck's track comes off at once when its buffer changed (count or
 * fingerprint); a cached track goes on when it holds exactly the deck's PWV3 data, checked again after the
 * comparison (gui_task may refill the buffer meanwhile). Returns the decks whose track changed (bit 0/1). */
static inline int wave_step(struct wave_env *e, struct wave_cache *c, int (*quiet)(struct wave_env *))
{
	uint32_t d;
	int k, changed = 0;
	for (d = 0; d < 2; d++) {
		uint32_t count = 0, sig = 0;
		const uint8_t *buf = NULL;
		struct wave_track *t = e->pub[d];
		int ok = wave_deck(e, d, &count, &buf, &sig);
		if (t && (!ok || t->count[d] != count || t->sig[d] != sig)) {
			__atomic_store_n(&e->pub[d], (struct wave_track *)NULL, __ATOMIC_SEQ_CST);
			c->dirty = 1;
			changed |= 1 << d;
			t = NULL;
		}
		if (t || !ok || (sig == c->seen[d] && c->gen == c->tried[d]) || !wave_settled(e, c, quiet)) continue;
		c->seen[d] = sig;
		c->tried[d] = c->gen;
		for (k = 0; k < WAVE_SLOTS; k++) {
			t = c->slot[k];
			if (t && wave_matches(t->pwv3, t->n, buf, count) && wave_sig(count, buf) == sig) {
				t->sig[d] = sig;
				t->count[d] = count;
				t->used = ++c->stamp;
				__atomic_store_n(&e->pub[d], t, __ATOMIC_SEQ_CST);
				c->matched++;
				changed |= 1 << d;
				break;
			}
		}
	}
	return changed;
}


/* ---------- panel LED set and LED walk (MAP build) ----------
 * LED set: every bit of bytes 0..19 of rbp's LED frames ever seen set (or_) and always seen set (and_). Kept over
 * starts in a small file; the factory test mode's "all LEDs on" (Mode 3) and "all off" (Mode 2) make it complete.
 * LED walk: the library sends its own frames instead of rbp's: three flashes of everything (a marker for the video),
 * the base alone, then each walk bit alone on top of the base. Base = and_ (bits Pioneer always sets), walk bits =
 * or_ & ~and_ (bits Pioneer sets in some frame), so the walk never sets a bit Pioneer itself never set. A bit whose
 * LED lights while it is 0 shows as that LED going off for its step. */
#define LED_BYTES		20
#define LED_WALK_MAX		(LED_BYTES * 8)
#define LED_WALK_FLASHES	3
#define LED_WALK_FLASH_US	500000u		/* on 0.5 s, base 0.5 s, three times */
#define LED_WALK_BASE_US	2000000u	/* then the base alone */
#define LED_WALK_STEP_US	1500000u	/* per bit: on 1.2 s, base 0.3 s (160 bits: 240 s at most) */
#define LED_WALK_ON_US		1200000u

struct led_set {
	uint8_t or_[LED_BYTES], and_[LED_BYTES];
	uint32_t frames;			/* frames seen (and_ means nothing while 0) */
};

/* Seed: 173,190 recorded LED frames (bytes 8/9, 12/13, 14/15 are the three level meters). */
static const struct led_set led_seed = {
	{ 0x7f, 0x31, 0x1c, 0xfe, 0x7f, 0x71, 0x1c, 0x7e, 0xff, 0x0f, 0x30, 0xb0, 0xff, 0x0f, 0xff, 0x07, 0, 0, 0x08, 0xff },
	{ 0, 0, 0x0c, 0x08, 0, 0, 0x0c, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x84 },
	173190,
};

static inline void led_set_init(struct led_set *s)
{
	memset(s->or_, 0, LED_BYTES);
	memset(s->and_, 0xff, LED_BYTES);
	s->frames = 0;
}

/* One frame (at least LED_BYTES bytes); returns 1 if or_ or and_ changed. */
static inline int led_set_add(struct led_set *s, const uint8_t *f)
{
	int i, ch = 0;
	for (i = 0; i < LED_BYTES; i++) {
		uint8_t o = (uint8_t)(s->or_[i] | f[i]), a = (uint8_t)(s->and_[i] & f[i]);
		if (o != s->or_[i] || a != s->and_[i]) ch = 1;
		s->or_[i] = o;
		s->and_[i] = a;
	}
	s->frames++;
	return ch;
}

/* Merge another set (one with frames == 0 is empty); returns 1 if or_ or and_ changed. */
static inline int led_set_merge(struct led_set *s, const struct led_set *o)
{
	int i, ch = 0;
	if (!o->frames) return 0;
	for (i = 0; i < LED_BYTES; i++) {
		uint8_t v = (uint8_t)(s->or_[i] | o->or_[i]), a = (uint8_t)(s->and_[i] & o->and_[i]);
		if (v != s->or_[i] || a != s->and_[i]) ch = 1;
		s->or_[i] = v;
		s->and_[i] = a;
	}
	s->frames += o->frames;
	return ch;
}

/* "or=<40 hex> and=<40 hex> frames=<n>\n"; returns the length (n must be at least 110). */
static inline int led_set_format(const struct led_set *s, char *buf, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	char o[2 * LED_BYTES + 1], a[2 * LED_BYTES + 1];
	int i;
	for (i = 0; i < LED_BYTES; i++) {
		o[2 * i] = hx[s->or_[i] >> 4];
		o[2 * i + 1] = hx[s->or_[i] & 15];
		a[2 * i] = hx[s->and_[i] >> 4];
		a[2 * i + 1] = hx[s->and_[i] & 15];
	}
	o[2 * LED_BYTES] = a[2 * LED_BYTES] = 0;
	return snprintf(buf, n, "or=%s and=%s frames=%u\n", o, a, (unsigned)s->frames);
}

static inline int led_hex_byte(const char *p, uint8_t *v)
{
	int k, r = 0;
	for (k = 0; k < 2; k++) {
		char c = p[k];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0) return -1;
		r = r * 16 + d;
	}
	*v = (uint8_t)r;
	return 0;
}

/* Parse led_set_format's line; returns 0 if it is complete and sane (and_ within or_, frames > 0), else -1. */
static inline int led_set_parse(struct led_set *s, const char *txt)
{
	struct led_set t;
	const char *o = strstr(txt, "or="), *a = strstr(txt, "and="), *f = strstr(txt, "frames=");
	char *end;
	unsigned long fr;
	int i;
	if (!o || !a || !f) return -1;
	o += 3;
	a += 4;
	for (i = 0; i < LED_BYTES; i++)
		if (led_hex_byte(o + 2 * i, &t.or_[i]) || led_hex_byte(a + 2 * i, &t.and_[i]) || (t.and_[i] & ~t.or_[i]))
			return -1;
	fr = strtoul(f + 7, &end, 10);
	if (end == f + 7 || fr == 0 || fr > 0xffffffffUL) return -1;
	t.frames = (uint32_t)fr;
	*s = t;
	return 0;
}

struct led_walk {
	uint8_t base[LED_BYTES], all[LED_BYTES];	/* all = base | every walk bit (the marker flashes) */
	uint8_t byte[LED_WALK_MAX], mask[LED_WALK_MAX];
	unsigned n;
};

/* The walk for a set: bits in order byte 0..19, bit 7..0. Returns the number of bits (0: nothing to walk). */
static inline unsigned led_walk_plan(struct led_walk *w, const struct led_set *s)
{
	int i, b;
	w->n = 0;
	for (i = 0; i < LED_BYTES; i++) {
		uint8_t walk = s->frames ? (uint8_t)(s->or_[i] & ~s->and_[i]) : 0;
		w->base[i] = s->frames ? s->and_[i] : 0;
		w->all[i] = (uint8_t)(w->base[i] | walk);
		for (b = 7; b >= 0; b--)
			if (walk & (1u << b)) {
				w->byte[w->n] = (uint8_t)i;
				w->mask[w->n] = (uint8_t)(1u << b);
				w->n++;
			}
	}
	return w->n;
}

static inline uint64_t led_walk_total_us(const struct led_walk *w)
{
	return 2ull * LED_WALK_FLASHES * LED_WALK_FLASH_US + LED_WALK_BASE_US + (uint64_t)w->n * LED_WALK_STEP_US;
}

/* Bytes 0..19 to send dt_us after the walk started. *step: -1 marker flash, -2 base, else the bit's index.
 * Returns 1 while the walk runs, 0 once it is over (out is left alone then). */
static inline int led_walk_frame(const struct led_walk *w, uint64_t dt_us, uint8_t *out, int *step)
{
	uint64_t marker = 2ull * LED_WALK_FLASHES * LED_WALK_FLASH_US;
	if (dt_us >= led_walk_total_us(w)) return 0;
	memcpy(out, w->base, LED_BYTES);
	if (dt_us < marker) {
		*step = -1;
		if ((dt_us / LED_WALK_FLASH_US) % 2 == 0) memcpy(out, w->all, LED_BYTES);
		return 1;
	}
	dt_us -= marker;
	if (dt_us < LED_WALK_BASE_US) {
		*step = -2;
		return 1;
	}
	dt_us -= LED_WALK_BASE_US;
	*step = (int)(dt_us / LED_WALK_STEP_US);
	if (dt_us % LED_WALK_STEP_US < LED_WALK_ON_US) out[w->byte[*step]] |= w->mask[*step];
	return 1;
}


/* ---------- beat jump ----------
 * SHIFT + AUTO BEAT LOOP opens a subpage of the AUTO BEAT LOOP pad mode: pad 1 = back A, pad 2 = forward A, pad 3 =
 * back B, pad 4 = forward B beats (Utility BEAT JUMP A / B: 1, 2, 4, 8, 16, 32). The jump keeps the position within
 * the beat, along the track's rekordbox beat grid (PQTZ in the .DAT), and goes through rbp's own set-position
 * (0x6b274, as hot cues do). Two hooks in rbp's key handlers, each a branch at the handler's first instruction to a
 * sequence in the never-called DjEngineIF::setEqualizerRange (0x57ebc, 55 words free): it calls the library with the
 * handler's arguments (key object, event); -1 = go on with rbp's handler (the first instruction, then the rest),
 * else that value is the handler's result and rbp never sees the event. */
#define BJ_SIZES	6
static const uint8_t bj_beats[BJ_SIZES] = { 1, 2, 4, 8, 16, 32 };
static const char *const bj_name[BJ_SIZES] = { "1", "2", "4", "8", "16", "32" };
#define BJ_A_DEFAULT	0		/* 1 beat */
#define BJ_B_DEFAULT	2		/* 4 beats (the user's choice, 2026-11-12) */
#define BJ_MAX_BEATS	16384u		/* 2 h at 136 BPM */
#define BJ_RATE		44.1		/* rbp's positions are samples at 44.1 kHz (console jump: ms * 44.1) */

/* rbp: the key objects (ui::Player, one per deck), the engine and its players */
#define BJ_OBJ_ID	0x26		/* u8: 1 = deck 1, 2 = deck 2 */
#define BJ_OBJ_SHIFT	0x34		/* u8: SHIFT held (key 0x4103) */
#define BJ_OBJ_MODE	0x7c		/* i32 pad mode: 0 HOT CUE, 1 hot cue delete, 2 AUTO BEAT LOOP, 3 LOOP SPLIT */
#define BJ_OBJ_HELD	0x80		/* list head of held pads: empty when it points to itself */
#define BJ_EV_KEY	8		/* u16 key code */
#define BJ_EV_TYPE	0xb		/* low nibble: the event type */
/* Event types as rbp's pad handler (0x235450) treats them: 0 press (adds the pad to the held list +0x80), 2 release
 * (removes it), 1 ignored (returns 0). Treating 1 as the release would
 * leave a pad lit and, once the subpage was closed, swallow that pad's release: rbp's held list would never empty
 * and the deck's pad mode buttons would stop working (rbp acts on them only with no pad held). */
#define BJ_EV_PRESS	0
#define BJ_EV_OTHER	1
#define BJ_EV_RELEASE	2
#define BJ_KEY_PAD1	0x4117u
#define BJ_MODE_ABL	2
#define BJ_ENGINE	0x1079cb0u	/* PlayEngine* (0 until rbp made it) */
#define BJ_ENG_PLAYERS	0xc		/* Player *[2] */
#define BJ_GET_POS	0x4410cu	/* int (engine, player): position, or the pending seek target */
#define BJ_SET_POS	0x6b274u	/* void (Player *, int samples): clamped to [-88200, length - 1] */

/* The .DAT's beat grid: PQTZ header (24 bytes: tag, header length, tag length, 0, 0x80000, count), then per beat
 * u16 beat in bar, u16 tempo * 100, u32 time in ms (big-endian). Copies up to max times into ms; returns the count,
 * or 0 if the tag is missing, broken, shorter than 2 beats or not strictly ascending. */
static inline uint32_t anlz_pqtz(const uint8_t *d, size_t n, uint32_t *ms, uint32_t max)
{
	uint32_t hl, tl, c, i;
	const uint8_t *p = anlz_tag(d, n, "PQTZ", &hl, &tl);
	if (!p || hl < 24) return 0;
	c = be32(p + 20);
	if (c < 2 || c > (tl - hl) / 8) return 0;
	if (c > max) c = max;
	for (i = 0; i < c; i++) {
		ms[i] = be32(p + hl + 8 * i + 4);
		if (i && ms[i] <= ms[i - 1]) return 0;
	}
	return c;
}

/* Where a jump of 'beats' (signed) from position pos (samples) lands on the grid ms[0..n): the same fraction of the
 * beat, beats grid beats away (before the first beat and after the last, the grid goes on at the first/last beat's
 * length). A target before the track start goes to 0; one past the last beat is refused. Returns samples or -1. */
static inline int64_t bj_target(const uint32_t *ms, uint32_t n, int64_t pos, int beats)
{
	double t = (double)pos / BJ_RATE, x, f, tt;
	uint32_t lo = 0, hi = n - 1, i;
	if (n < 2) return -1;
	if (t < ms[0])
		x = (t - ms[0]) / (double)(ms[1] - ms[0]);
	else if (t >= ms[n - 1])
		x = (n - 1) + (t - ms[n - 1]) / (double)(ms[n - 1] - ms[n - 2]);
	else {
		while (hi - lo > 1) {	/* ms[lo] <= t < ms[hi] */
			uint32_t mid = lo + (hi - lo) / 2;
			if (ms[mid] <= t) lo = mid; else hi = mid;
		}
		x = lo + (t - ms[lo]) / (double)(ms[hi] - ms[lo]);
	}
	x += beats;
	if (x > (double)(n - 1) + 1e-3) return -1;	/* a position on a beat, rounded to samples, may be a hair past it */
	if (x > (double)(n - 1)) x = n - 1;
	if (x < 0)
		tt = ms[0] + x * (double)(ms[1] - ms[0]);
	else {
		i = (uint32_t)x;
		f = x - i;
		tt = i == n - 1 ? ms[n - 1] : ms[i] + f * (double)(ms[i + 1] - ms[i]);
	}
	if (tt <= 0) return 0;
	return (int64_t)(tt * BJ_RATE + 0.5);
}

/* Per deck: the subpage and which pads the mod took the press of (their releases are the mod's too). */
struct bj_deck {
	uint8_t sub;
	uint8_t held;
};

/* AUTO BEAT LOOP key. rbp's handler always runs (it acts on a press with no pad held: sets AUTO BEAT LOOP mode, or
 * nothing if it is already on). With SHIFT the subpage toggles (on if it wasn't showing), without SHIFT it closes. */
static inline int bj_abl(struct bj_deck *d, int type, int shift, int pads_held, int mode)
{
	if (type != BJ_EV_PRESS || pads_held) return -1;
	d->sub = shift ? !(d->sub && mode == BJ_MODE_ABL) : 0;
	return -1;
}

/* A pad (0..3). On the subpage in AUTO BEAT LOOP mode a press is the mod's (*beats = the jump), and so is every
 * later event of that pad up to and including its release, even if the subpage closes meanwhile (rbp never saw the
 * press, so it must not see the release). Anything else is rbp's; a press in another mode closes the subpage (HOT CUE
 * or LOOP SPLIT was chosen meanwhile). A press of a pad the mod still counts as held (a lost release) clears it first,
 * so a stale bit can never swallow rbp's release. Returns -1 = rbp's, 1 = taken. */
static inline int bj_pad(struct bj_deck *d, int pad, int type, int mode, int a, int b, int *beats)
{
	uint8_t bit;
	*beats = 0;
	if (pad < 0 || pad > 3) return -1;
	bit = (uint8_t)(1u << pad);
	if (type == BJ_EV_PRESS) {
		d->held = (uint8_t)(d->held & ~bit);
		if (d->sub && mode == BJ_MODE_ABL) {
			d->held |= bit;
			*beats = (pad & 1 ? 1 : -1) * (pad < 2 ? a : b);
			return 1;
		}
		if (mode != BJ_MODE_ABL) d->sub = 0;
		return -1;
	}
	if (d->held & bit) {
		if (type == BJ_EV_RELEASE) d->held = (uint8_t)(d->held & ~bit);
		return 1;
	}
	return -1;
}

/* Does deck d show the subpage (the LED flashes)? */
static inline int bj_showing(const struct bj_deck *d, int mode)
{
	return d->sub && mode == BJ_MODE_ABL;
}

/* The subpage's LEDs. While deck d shows the subpage, the mod owns these bits in rbp's LED frame;
 * otherwise rbp's own frame goes through. A 0 bit is the panel's own dim level, not dark.
 *  - AUTO BEAT LOOP: 0.5 s bright, 0.5 s dim, starting bright (the CUE blink is 3 per second). 1 s / 1 s
 *    was too slow to see at a glance whether the subpage is on.
 *  - pads: dim, the pressed pad bright while held and at least BJ_PAD_LIT_US (a quick tap shows).
 * bj_pad_led, measured on the panel: {byte, mask}; mask 0 would leave that pad to rbp. Byte 2 / byte 6
 * are the deck's pad-mode byte: pads 1-4 0x80 0x40 0x20 0x10, (0x08 ?), HOT CUE 0x04, AUTO BEAT LOOP 0x02, LOOP SPLIT 0x01. */
#define BJ_FLASH_US	500000u
#define BJ_PAD_LIT_US	150000u
static const uint8_t bj_abl_led[2][2] = { { 2, 0x02 }, { 6, 0x02 } };
static const uint8_t bj_pad_led[2][4][2] = {
	{ { 2, 0x80 }, { 2, 0x40 }, { 2, 0x20 }, { 2, 0x10 } },	/* deck 1 pads 1-4 */
	{ { 6, 0x80 }, { 6, 0x40 }, { 6, 0x20 }, { 6, 0x10 } },	/* deck 2 pads 1-4 */
};

#define BJ_LED_ON	0x01	/* the subpage shows: the mod owns the bits */
#define BJ_LED_BRIGHT	0x02	/* AUTO BEAT LOOP bright now */
#define BJ_LED_PADS	4	/* pads lit: bits 4..7 */

/* The UI thread's view of one deck. seq: the key thread counts each taken pad press (one byte per pad). */
struct bj_lights {
	uint64_t t_on;		/* when the subpage started showing (the flash phase) */
	uint64_t until[4];	/* a tapped pad stays lit until then */
	uint8_t seq[4];		/* the press counts seen so far */
	uint8_t showing;
};

/* One UI tick at t: the state byte for bj_led_apply (0 = rbp's own LEDs). */
static inline uint8_t bj_lights_step(struct bj_lights *L, int showing, uint8_t held, const volatile uint8_t *seq,
				     uint64_t t)
{
	uint8_t st, lit = 0;
	int p;
	for (p = 0; p < 4; p++) {
		uint8_t s = seq[p];
		if (s != L->seq[p]) {
			L->seq[p] = s;
			L->until[p] = t + BJ_PAD_LIT_US;
		}
		if ((held >> p & 1) || t < L->until[p]) lit |= (uint8_t)(1u << p);
	}
	if (!showing) {
		L->showing = 0;
		return 0;
	}
	if (!L->showing) {
		L->showing = 1;
		L->t_on = t;
	}
	st = BJ_LED_ON | (uint8_t)(lit << BJ_LED_PADS);
	if ((t - L->t_on) % (2ull * BJ_FLASH_US) < BJ_FLASH_US) st |= BJ_LED_BRIGHT;
	return st;
}

/* Deck d's subpage bits into a frame (bytes 0..19; the caller seals it). Returns 1 if a byte changed. */
static inline int bj_led_apply(uint8_t *f, int d, uint8_t st)
{
	int p, changed = 0;
	if (!(st & BJ_LED_ON) || d < 0 || d > 1) return 0;
	{
		uint8_t b = bj_abl_led[d][0], m = bj_abl_led[d][1], v;
		v = st & BJ_LED_BRIGHT ? (uint8_t)(f[b] | m) : (uint8_t)(f[b] & ~m);
		changed |= v != f[b];
		f[b] = v;
	}
	for (p = 0; p < 4; p++) {
		uint8_t b = bj_pad_led[d][p][0], m = bj_pad_led[d][p][1], v;
		if (!m) continue;
		v = st >> (BJ_LED_PADS + p) & 1 ? (uint8_t)(f[b] | m) : (uint8_t)(f[b] & ~m);
		changed |= v != f[b];
		f[b] = v;
	}
	return changed;
}

/* Are the pad LEDs known (bj_pad_led filled in)? */
static inline int bj_pad_leds_known(void)
{
	int d, p, n = 0;
	for (d = 0; d < 2; d++)
		for (p = 0; p < 4; p++) n += bj_pad_led[d][p][1] != 0;
	return n;
}

/* ---- On air on the jog lights ----
 * rbp's mixer engine at [OA_ENGINE] holds a vector [eng+0 .. eng+4) of channel objects (DjEngineIF::setChannelFader
 * 0x5744c, setCrossFader 0x578c8): channel id +0x14 (0 = CH1, 1 = CH2), fader +0x144 (0..1), fader gain after the CH
 * FADER CURVE +0x118, crossfader assign +0x178 (0 THRU = gain 1), crossfader value +0x180, crossfader gain after the
 * curve +0x150 (all floats but the id and assign). A channel is on air when both gains are above 0: the numbers the
 * audio uses. The jog light: deck 1 byte 1 0x01, deck 2 byte 5 0x01, lit while the jog is touched.
 * Utility JOG LIGHT: TOUCH = Pioneer's touch light, ON AIR = lit while that channel is on air, no touch feedback. */
#define OA_ENGINE	0x107a1b0u
#define OA_MAX_CH	8
#define OA_CH_ID	0x14
#define OA_CH_FADER_GAIN 0x118
#define OA_CH_XF_GAIN	0x150
#define OA_CH_FADER	0x144
#define OA_CH_XF_ASSIGN	0x178
#define OA_CH_LEN	0x184
static const uint8_t oa_jog_led[2][2] = { { 1, 0x01 }, { 5, 0x01 } };
enum { JOG_TOUCH, JOG_ONAIR, JOG_MODES };

struct oa_channel {
	float fader, fader_gain, xf_gain;
	uint32_t assign;
};

/* Plausible values (a stale or wrong pointer must never light anything)? */
static inline int oa_plausible(const struct oa_channel *c)
{
	return c->fader >= 0.0f && c->fader <= 1.0001f && c->fader_gain >= 0.0f && c->fader_gain <= 4.0f &&
	       c->xf_gain >= 0.0f && c->xf_gain <= 4.0f && c->assign <= 2;
}

static inline int oa_on_air(const struct oa_channel *c)
{
	return c->fader_gain > 0.0f && c->xf_gain > 0.0f;
}

/* The jog light state for the frame: 0 = rbp's own (TOUCH, or the mixer not readable), else OA_LED_ON | the decks
 * on air (bit 0 deck 1, bit 1 deck 2). */
#define OA_LED_ON	0x80
static inline uint8_t oa_led_state(int mode, int readable, unsigned on_air)
{
	if (mode != JOG_ONAIR || !readable) return 0;
	return (uint8_t)(OA_LED_ON | (on_air & 3));
}

/* The jog light bits into a frame (bytes 0..19; the caller seals it). Returns 1 if a byte changed. */
static inline int oa_led_apply(uint8_t *f, uint8_t st)
{
	int d, changed = 0;
	if (!(st & OA_LED_ON)) return 0;
	for (d = 0; d < 2; d++) {
		uint8_t b = oa_jog_led[d][0], m = oa_jog_led[d][1];
		uint8_t v = st >> d & 1 ? (uint8_t)(f[b] | m) : (uint8_t)(f[b] & ~m);
		changed |= v != f[b];
		f[b] = v;
	}
	return changed;
}

/* Settings file key jog-light=on-air, written only when not the default (TOUCH). */
static inline int oa_settings_parse(const char *txt, size_t n)
{
	const char *p = txt, *end = txt + n;
	int mode = JOG_TOUCH;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol) eol = end;
		if (eol - p > 10 && !strncmp(p, "jog-light=", 10)) mode = xdj_word(p + 10, eol, "on-air") ? JOG_ONAIR : JOG_TOUCH;
		p = eol + 1;
	}
	return mode;
}

static inline int oa_settings_format(int mode, char *out, size_t n)
{
	return snprintf(out, n, "%s", mode == JOG_ONAIR ? "jog-light=on-air\n" : "");
}

/* Settings file keys beatjump-a=1|2|4|8|16|32 and beatjump-b=…, written only when not the default (older builds
 * skip unknown keys, and drop them when they save). */
struct bj_settings {
	int a, b;	/* index into bj_beats */
};

static inline void bj_settings_parse(struct bj_settings *st, const char *txt, size_t n)
{
	const char *p = txt, *end = txt + n;
	st->a = BJ_A_DEFAULT;
	st->b = BJ_B_DEFAULT;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		int k, *v = NULL;
		if (!eol) eol = end;
		if (eol - p > 11 && !strncmp(p, "beatjump-a=", 11)) v = &st->a;
		if (eol - p > 11 && !strncmp(p, "beatjump-b=", 11)) v = &st->b;
		if (v)
			for (k = 0; k < BJ_SIZES; k++)
				if (xdj_word(p + 11, eol, bj_name[k])) *v = k;
		p = eol + 1;
	}
}

static inline int bj_settings_format(const struct bj_settings *st, char *out, size_t n)
{
	int a = st->a >= 0 && st->a < BJ_SIZES && st->a != BJ_A_DEFAULT;
	int b = st->b >= 0 && st->b < BJ_SIZES && st->b != BJ_B_DEFAULT;
	return snprintf(out, n, "%s%s%s%s%s%s", a ? "beatjump-a=" : "", a ? bj_name[st->a] : "", a ? "\n" : "",
			b ? "beatjump-b=" : "", b ? bj_name[st->b] : "", b ? "\n" : "");
}

/* The two hooks: sites (rbp's handlers), their first instructions, and the sequences at BJ_CAVE. */
#define BJ_ABL_SITE	0x234b74u
#define BJ_ABL_ORIG	0xe92d4070u	/* push {r4, r5, r6, lr} */
#define BJ_PAD_SITE	0x235450u
#define BJ_PAD_ORIG	0xe1d130b8u	/* ldrh r3, [r1, #8] */
#define BJ_CAVE		0x57ebcu	/* DjEngineIF::setEqualizerRange: nothing calls it or points to it */
#define BJ_SEQ_WORDS	12
#define BJ_CAVE_WORDS	(2 * BJ_SEQ_WORDS)
static const uint32_t bj_cave_orig[BJ_CAVE_WORDS] = {
	0xe92d47f0, 0xe1a05003, 0xe59d4020, 0xe1a08002, 0xe59f60b0, 0xe1a02003, 0xe59f00ac, 0xe1a07001,
	0xe1a03004, 0xebffa8d3, 0xe5960000, 0xe3500000, 0x0a000005, 0xe1a01007, 0xe1a02008, 0xe1a03005,
	0xe58d4020, 0xe8bd47f0, 0xea002ee9, 0xe59f007c, 0xeb0a9768, 0xe5963000, 0xe3530000, 0x0a000003,
};

/* One sequence at 'at' for the handler at 'site' whose first instruction is 'orig'; fn = the library's
 * int fn(void *obj, void *ev). The handler's caller gets fn's result directly when it isn't -1. */
static inline void bj_seq_build(uint32_t *w, uint32_t at, uint32_t site, uint32_t orig, uint32_t fn)
{
	w[0] = 0xe92d500f;	/* push {r0-r3, r12, lr} */
	w[1] = 0xe59fc020;	/* ldr r12, [pc, #32]  -> w[11] */
	w[2] = 0xe12fff3c;	/* blx r12             r0 = fn(obj, ev) */
	w[3] = 0xe3700001;	/* cmn r0, #1 */
	w[4] = 0x0a000002;	/* beq w[8]            -1: rbp's handler */
	w[5] = 0xe28dd004;	/* add sp, sp, #4      drop the saved r0, keep the result */
	w[6] = 0xe8bd500e;	/* pop {r1-r3, r12, lr} */
	w[7] = 0xe12fff1e;	/* bx lr               back to the handler's caller */
	w[8] = 0xe8bd500f;	/* pop {r0-r3, r12, lr} */
	w[9] = orig;		/* the handler's first instruction */
	w[10] = arm_b(at + 40, site + 4);
	w[11] = fn;
}

static inline void bj_cave_build(uint32_t *w, uint32_t abl_fn, uint32_t pad_fn)
{
	bj_seq_build(w, BJ_CAVE, BJ_ABL_SITE, BJ_ABL_ORIG, abl_fn);
	bj_seq_build(w + BJ_SEQ_WORDS, BJ_CAVE + 4 * BJ_SEQ_WORDS, BJ_PAD_SITE, BJ_PAD_ORIG, pad_fn);
}

#endif
