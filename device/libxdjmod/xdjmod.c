/*
 * libxdjmod: the XDJRX-MOD library. apl_start loads it into the XDJ-RX 2.21 main program (/root/pioneer/rbp) with
 * LD_PRELOAD (device/README.md).
 *
 * It adds the mod's UTILITY entries and what they switch: cue modes, master level display, master/booth mono,
 * display refresh, colour waveforms, beat jump, jog light; and the safe start. It wraps a few libc and ALSA calls and
 * always passes them on to the real function. Before it changes code or data in rbp's memory it compares them with
 * the bytes it expects for 2.21, and leaves them alone if they differ.
 *
 * Real-time rules: the audio thread only does arithmetic, clock reads and
 * lock-free stores. It never allocates, locks, logs or touches files. One
 * low-priority thread on core 0 formats and writes the log.
 *
 * The library is inactive (pure pass-through) in any process other than
 * /root/pioneer/rbp, and it removes LD_PRELOAD from rbp's environment so the
 * programs rbp starts do not load it.
 *
 * Environment (set by apl_start):
 *   XDJMOD_OUT       scan:<name> = write the log into the first /media/usb<N>/<partition>/<name> folder that appears
 *   XDJMOD_SWITCH    the settings file
 *   XDJMOD_STARTING  the safe-start marker; removed after a healthy first minute
 *   XDJMOD_PASSIVE   if set: interpose but do nothing (start-up probe)
 * Start-up milestones go to /dev/shm/xdjmod-boot.txt (synchronous, at most 200 lines).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "xdjmod_core.h"

#define EXPORT __attribute__((visibility("default")))
#define SYNC_GIG 4			/* index into sync_lead_us: 4.5 ms earlier */
#define XDJMOD_VERSION "event-3c"
#define RBP_PATH "/root/pioneer/rbp"

/* ---------- real functions ---------- */

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_close)(int);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_ioctl)(int, unsigned long, ...);
static FILE *(*real_fopen)(const char *, const char *);
static FILE *(*real_fopen64)(const char *, const char *);

static int (*real_pcm_open)(void **, const char *, int, int);
static int (*real_pcm_close)(void *);
static int (*real_pcm_hw_params)(void *, void *);
static int (*real_pcm_prepare)(void *);
static long (*real_pcm_writei)(void *, const void *, unsigned long);
static long (*real_pcm_readi)(void *, void *, unsigned long);
static int (*get_format)(const void *, int *);
static int (*get_channels)(const void *, unsigned *);
static int (*get_rate)(const void *, unsigned *, int *);
static int (*get_period_size)(const void *, unsigned long *, int *);
static int (*get_buffer_size)(const void *, unsigned long *);

/* Exactly the versions rbp imports (objdump -T rbp). */
static void *find(const char *name, const char *version)
{
	void *p = dlvsym(RTLD_NEXT, name, version);
	return p ? p : dlsym(RTLD_NEXT, name);
}

#define RESOLVE(var, name, version) \
	do { if (!(var)) *(void **)&(var) = find(name, version); } while (0)

static void resolve_libc(void)
{
	RESOLVE(real_open, "open", "GLIBC_2.4");
	RESOLVE(real_open64, "open64", "GLIBC_2.4");
	RESOLVE(real_close, "close", "GLIBC_2.4");
	RESOLVE(real_read, "read", "GLIBC_2.4");
	RESOLVE(real_write, "write", "GLIBC_2.4");
	RESOLVE(real_ioctl, "ioctl", "GLIBC_2.4");
	RESOLVE(real_fopen, "fopen", "GLIBC_2.4");
	RESOLVE(real_fopen64, "fopen64", "GLIBC_2.4");
}

static void resolve_alsa(void)
{
	RESOLVE(real_pcm_open, "snd_pcm_open", "ALSA_0.9");
	RESOLVE(real_pcm_close, "snd_pcm_close", "ALSA_0.9");
	RESOLVE(real_pcm_hw_params, "snd_pcm_hw_params", "ALSA_0.9");
	RESOLVE(real_pcm_prepare, "snd_pcm_prepare", "ALSA_0.9");
	RESOLVE(real_pcm_writei, "snd_pcm_writei", "ALSA_0.9");
	RESOLVE(real_pcm_readi, "snd_pcm_readi", "ALSA_0.9");
	/* the current ABI of the getters is the default version ALSA_0.9.0rc4 */
	RESOLVE(get_format, "snd_pcm_hw_params_get_format", "ALSA_0.9.0rc4");
	RESOLVE(get_channels, "snd_pcm_hw_params_get_channels", "ALSA_0.9.0rc4");
	RESOLVE(get_rate, "snd_pcm_hw_params_get_rate", "ALSA_0.9.0rc4");
	RESOLVE(get_period_size, "snd_pcm_hw_params_get_period_size", "ALSA_0.9.0rc4");
	RESOLVE(get_buffer_size, "snd_pcm_hw_params_get_buffer_size", "ALSA_0.9.0rc4");
}

/* ---------- state ---------- */

static int active;			/* set once in the constructor, inside rbp only */
static char out_dir[256];
static struct ev_ring ring;
static uint64_t t0_us;

enum { FD_NONE = 0, FD_SPI, FD_I2C, FD_HID, FD_FB };
#define MAX_FD 4096
static volatile uint8_t fd_class[MAX_FD];
static const char *const class_name[] = { "-", "panel", "i2c", "hid", "fb" };

#define MAX_PCM 8
struct pcm_slot {
	void *handle;			/* snd_pcm_t *, NULL = free */
	char name[24];
	int stream;
	struct pcm_hw hw;		/* written by hw_params, read by I/O and log */
	/* audio thread only */
	struct pcm_acc acc;
	uint64_t win_start, last_entry;
	int role;			/* pcm_role(): 1 master, 2 booth, 0 other (set at open) */
	float mono_g;			/* current mono amount, 0..1 */
	/* published once per second */
	uint32_t seq;
	struct pcm_acc snap;
	uint64_t snap_t;
};
static struct pcm_slot pcm[MAX_PCM];
static volatile uint32_t master_periods;		/* master writes, for the safe-start health check */
#define MONO_MAX_FRAMES 1024		/* rbp writes 64-frame periods */
static int32_t mono_buf[MAX_PCM][2 * MONO_MAX_FRAMES];

/* Set from XDJMOD_FRAMES (raw device frames in the log); nothing in this build reads it. */
static int log_frames;

/* ---------- helpers ---------- */

static inline uint64_t now_us(void)
{
	struct timespec ts;
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline int32_t gettid_(void) { return (int32_t)syscall(SYS_gettid); }

static struct ev *ev_new(uint16_t type, uint64_t t)
{
	struct ev *e = ev_claim(&ring);
	if (e) {
		e->type = type;
		e->t_us = t;
		e->tid = gettid_();
	}
	return e;
}

static void ev_data(struct ev *e, const void *d, size_t n)
{
	if (n > EV_DATA) n = EV_DATA;
	if (d && n) memcpy(e->d, d, n);
	e->len = (uint16_t)n;
}

static void note(const char *fmt, ...)
{
	struct ev *e = ev_new(EV_NOTE, now_us());
	va_list ap;
	if (!e) return;
	va_start(ap, fmt);
	vsnprintf((char *)e->d, EV_DATA, fmt, ap);
	va_end(ap);
	e->len = (uint16_t)strlen((char *)e->d);
	ev_commit(e);
}

/* Start-up milestones, written synchronously to RAM so they survive a hang of everything else.
 * Never called from the audio path. */
#define BOOT_FILE "/dev/shm/xdjmod-boot.txt"
static int boot_notes;

static void boot_note(const char *fmt, ...)
{
	char line[200];
	va_list ap;
	int fd, n;
	struct timespec ts;
	if (boot_notes >= 200 || !real_open || !real_write || !real_close) return;
	boot_notes++;
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
	n = snprintf(line, sizeof line, "%ld.%03ld [%d] ", (long)ts.tv_sec, ts.tv_nsec / 1000000, gettid_());
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
	line[n++] = '\n';
	fd = real_open(BOOT_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd >= 0) {
		real_write(fd, line, (size_t)n);
		real_close(fd);
	}
}

static int classify(const char *path)
{
	if (!path) return FD_NONE;
	if (!strncmp(path, "/dev/subucom_spi", 16)) return FD_SPI;
	if (!strncmp(path, "/dev/i2c-", 9)) return FD_I2C;
	if (!strncmp(path, "/dev/hidg", 9)) return FD_HID;
	if (!strncmp(path, "/dev/fb", 7)) return FD_FB;
	return FD_NONE;
}

static void opened(int fd, const char *path)
{
	int cls;
	if (!active || fd < 0 || fd >= MAX_FD) return;
	cls = classify(path);
	fd_class[fd] = (uint8_t)cls;
	if (cls != FD_NONE) {
		struct ev *e;
		boot_note("open fd=%d %s", fd, path);
		e = ev_new(EV_OPEN, now_us());
		if (e) {
			e->a = (uint32_t)fd;
			e->b = (uint32_t)cls;
			ev_data(e, path, strlen(path) + 1);
			ev_commit(e);
		}
	}
}

static inline int fd_cls(int fd)
{
	return (active && fd >= 0 && fd < MAX_FD) ? fd_class[fd] : FD_NONE;
}

/* subucom_spi.h */
struct subucom_xfer { uint8_t multi_id, reserved, part_id, part_num; uint32_t bytes; uint64_t tx_buf; };
#define IOC_TYPE(c) (((c) >> 8) & 0xff)
#define IOC_NR(c) ((c) & 0xff)
#define IOC_DIR(c) (((c) >> 30) & 3)	/* 1 = write (to the driver) */

/* ---------- headphone cue mode and settings (XDJMOD_SWITCH=<settings file>) ----------
 * Utility CUE MODE: NORMAL, SPLIT (Pioneer's split cue, always on) or SPLIT STEREO (like a PX5: split only while a
 * channel is cued; with no CUE lit the master plays in stereo). The mode is kept in the settings file with the
 * other Utility settings and applied at start. Pure logic and checks: xdjmod_core.h.
 *
 * Threads: the low-priority xdjmod_ui thread finds the mixer, writes the mode word and saves the settings. The
 * audio thread is never touched: the mode word is written through /proc/self/mem, which fails cleanly instead of
 * faulting.
 * While split is selected, the stereo path's fixed gain is lowered by hp_trim dB (the master in stereo on
 * both ears sounds louder than on one ear while cued); normal mode keeps Pioneer's exact value. */
static char mode_path[128];
static int sw_on;				/* switch configured and library active */
static volatile int hp_mode = HP_SPLIT_STEREO;	/* wanted cue mode (HP_NORMAL/HP_SPLIT/HP_SPLIT_STEREO) */
static volatile int split_pref = HP_SPLIT_STEREO;	/* the last split mode chosen (kept in the settings file) */
static volatile uint32_t meter_pre = 1;		/* read by the meter sequence in rbp: 1 = before MASTER LEVEL */
static int meter_ok, menu_ok;			/* code patches installed (constructor) */
static volatile int master_mono, booth_mono;	/* MASTER MONO / BOOTH MONO, read by the audio thread */
static volatile int fps_sel;			/* DISPLAY REFRESH setting, index into gui_period_ms */
static volatile uint32_t gui_period = 50;	/* read by the frame-rate sequence in rbp: ms per frame */
static int gui_ok;				/* frame-rate sequence installed */
static volatile int scroll_sel;			/* WAVEFORM SCROLL: 0 Pioneer's position, 1 SMOOTH (gui_task reads it) */
static volatile int sync_sel;			/* SCREEN SYNC: index into sync_lead_us, always SYNC_GIG */
static int pos_ok;				/* position hooks installed */
static volatile int mode_dirty;			/* to be saved by the UI thread */
static volatile int measure_next;		/* always 0 (a field of the settings file) */
static int measure_only;			/* always 0 (see the constructor) */
static volatile uintptr_t hp_obj;		/* the HeadPhone object, 0 = not (yet) found */
static int mem_fd = -1, mem_writable;
static pthread_mutex_t led_lock;		/* rbp's LED frames vs the UI thread's */
static uint8_t led_rbp[128];			/* rbp's latest LED frame (multi 0, part 0) */
static struct subucom_xfer led_x;		/* its transfer header */
static int led_fd = -1, led_have;
static unsigned long led_req;
static volatile uint8_t cue_leds = 0;		/* byte 10 of rbp's own LED frame: which CUE buttons are lit */
static volatile int hp_trim = HP_TRIM_DEFAULT;	/* dB less on the master in stereo while split is selected */
static int gain_off;				/* the stereo gain literal didn't look right: leave it alone */
static volatile int out_found;			/* out_dir is complete (set by the log thread) */
static volatile int wave_mode;			/* WAVEFORM COLOR: WAVE_BLUE / WAVE_RGB / WAVE_3BAND */
#define WAVE_NAME(m) ((unsigned)(m) < 3 ? xdj_wave_name[m] : "?")

static int mem_read(uintptr_t a, void *buf, size_t n)
{
	return mem_fd >= 0 && pread64(mem_fd, buf, n, (off64_t)a) == (ssize_t)n;
}

/* Write the mode word if the object still looks like the mixer. Returns 1 if it holds the wanted value. */
static int hp_apply(void)
{
	uint8_t o[HP_OBJ_LEN];
	uint32_t want = hp_mode_word(hp_mode, cue_leds);
	uintptr_t a = hp_obj;
	if (!a) return 0;
	if (!mem_read(a, o, sizeof o) || !hp_object_plausible(o)) {
		hp_obj = 0;	/* gone or changed: stop switching, look again */
		note("switch: mixer object at %p no longer valid", (void *)a);
		return 0;
	}
	if (le32(o + HP_MODE_OFF) == want) return 1;
	if (!mem_writable || pwrite64(mem_fd, &want, 4, (off64_t)(a + HP_MODE_OFF)) != 4) {
		/* /proc/self/mem not writable here: store directly. The object was read and checked just
		 * now, and the mixer lives as long as rbp. */
		if (mem_writable) note("switch: /proc/self/mem write failed (errno %d), storing directly", errno);
		mem_writable = 0;
		__atomic_store_n((volatile uint32_t *)(a + HP_MODE_OFF), want, __ATOMIC_RELEASE);
	}
	note("switch: headphones now %s", want ? "stereo blend" : "split");
	return 1;
}

/* Write one aligned word of rbp's code page (the stereo gain literal). /proc/self/mem copies the page on
 * write like a debugger's breakpoint; if that isn't allowed, make the page writable and store directly. */
static int gain_write(uintptr_t a, uint32_t v)
{
	uint32_t back;
	if (mem_writable && pwrite64(mem_fd, &v, 4, (off64_t)a) == 4 && mem_read(a, &back, 4) && back == v)
		return 1;
	if (mprotect((void *)(a & ~(uintptr_t)4095), 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		note("switch: stereo gain not writable (errno %d)", errno);
		return 0;
	}
	__atomic_store_n((volatile uint32_t *)a, v, __ATOMIC_RELEASE);
	return 1;
}

/* Set the stereo path's gain: lowered by hp_trim while split is selected and the mixer is ours, else
 * Pioneer's exact value. Both floats of the pair are checked before anything is written. */
static void gain_apply(void)
{
	uint32_t cur[2], want = hp_stereo_gain_bits(hp_obj ? hp_mode : HP_NORMAL, hp_trim);
	uintptr_t a = HP_STEREO_GAIN_ADDR;
	int i, changed = 0;
	if (gain_off) return;
	if (!mem_read(a, cur, sizeof cur) || !hp_stereo_gain_plausible(cur[0]) || !hp_stereo_gain_plausible(cur[1])) {
		gain_off = 1;
		note("switch: stereo gain at %p looks wrong (%08x %08x), not touched", (void *)a, cur[0], cur[1]);
		return;
	}
	for (i = 0; i < 2; i++) {
		if (cur[i] == want) continue;
		if (!gain_write(a + 4 * (uintptr_t)i, want)) {
			gain_off = 1;
			return;
		}
		changed = 1;
	}
	if (changed)
		note("switch: stereo gain %s", want == HP_STEREO_GAIN_BITS ? "original" : "lowered for split");
}

/* Search rbp's anonymous writable memory for the HeadPhone object. heap_only: just [heap] (40 MB),
 * else every anonymous writable mapping (about 830 MB virtual; untouched pages read as the
 * zero page). Returns the number of plausible objects; sets *hit to the last one. */
static int hp_scan(int heap_only, uintptr_t *hit)
{
	static uint8_t chunk[65536];
	static char maps[65536];
	int fd, found = 0;
	ssize_t n, total = 0;
	char *line, *save;
	fd = real_open("/proc/self/maps", O_RDONLY);
	if (fd < 0) return 0;
	while (total < (ssize_t)sizeof maps - 1 && (n = real_read(fd, maps + total, sizeof maps - 1 - (size_t)total)) > 0)
		total += n;
	real_close(fd);
	maps[total] = 0;
	for (line = strtok_r(maps, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned long lo, hi, inode = 1;
		char perms[8], path[64] = "";
		uintptr_t a;
		unsigned mb = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %lu %63s", &lo, &hi, perms, &inode, path) < 4) continue;
		/* this kernel maps writable memory rwxp (no NX), elsewhere rw-p */
		if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p' || inode != 0) continue;
		if (!strncmp(path, "[stack", 6) || (heap_only && strcmp(path, "[heap]") != 0)) continue;
		for (a = lo; a < hi; a += sizeof chunk) {
			size_t len = hi - a < sizeof chunk ? hi - a : sizeof chunk, i;
			if (!mem_read(a, chunk, len)) continue;
			for (i = 0; i + 4 <= len; i += 4) {
				uint8_t o[HP_OBJ_LEN];
				if (le32(chunk + i) != HP_VTABLE) continue;
				if (!mem_read(a + i, o, sizeof o) || !hp_object_plausible(o)) continue;
				found++;
				*hit = a + i;
			}
			if (++mb % 64 == 0) {	/* every 4 MB: leave core 0 to rbp for a moment */
				struct timespec ts = { 0, 500 * 1000 };
				nanosleep(&ts, NULL);
			}
		}
	}
	return found;
}

static void hp_find(int full)
{
	uintptr_t hit = 0;
	int found = hp_scan(!full, &hit);
	if (found == 1) {
		hp_obj = hit;
		note("switch: mixer found at %p (%s), mode %s", (void *)hit, full ? "full scan" : "heap",
		     xdj_cue_name(hp_mode));
		boot_note("switch: mixer found at %p", (void *)hit);
		hp_apply();
	} else if (found > 1) {
		note("switch: %d candidates for the mixer, switching stays off", found);
	}
}

static struct bj_settings bj_set = { BJ_A_DEFAULT, BJ_B_DEFAULT };	/* Utility BEAT JUMP A / B (rbp's UI thread) */
static volatile int jog_mode = JOG_TOUCH;	/* Utility JOG LIGHT */
#define MODE_TXT 256	/* room for the beat jump and jog light keys */

static void mode_load(void)
{
	char b[MODE_TXT];
	struct xdj_settings st;
	int fd = real_open(mode_path, O_RDONLY);
	ssize_t n = 0;
	if (fd >= 0) {
		n = real_read(fd, b, sizeof b);
		real_close(fd);
	}
	xdj_settings_parse(&st, b, n > 0 ? (size_t)n : 0);	/* no file yet: SPLIT STEREO, meter pre */
	hp_mode = st.mode;
	split_pref = st.split;
	meter_pre = (uint32_t)st.meter_pre;
	master_mono = st.master_mono;
	booth_mono = st.booth_mono;
	fps_sel = st.fps;
	gui_period = gui_period_ms[fps_sel];
	measure_next = 0;
	wave_mode = st.wave;
	scroll_sel = st.scroll;
	sync_sel = SYNC_GIG;
	bj_settings_parse(&bj_set, b, n > 0 ? (size_t)n : 0);
	jog_mode = oa_settings_parse(b, n > 0 ? (size_t)n : 0);
}

static void mode_save(void)
{
	char tmp[140], txt[MODE_TXT];
	struct xdj_settings st = { hp_mode, split_pref, meter_pre != 0, master_mono, booth_mono, fps_sel, measure_next,
				   wave_mode, scroll_sel, sync_sel };
	int fd, n = xdj_settings_format(&st, txt, sizeof txt);
	if (n > 0 && n < (int)sizeof txt) n += bj_settings_format(&bj_set, txt + n, sizeof txt - (size_t)n);
	if (n > 0 && n < (int)sizeof txt) n += oa_settings_format(jog_mode, txt + n, sizeof txt - (size_t)n);
	if (n >= (int)sizeof txt) n = (int)sizeof txt - 1;
	snprintf(tmp, sizeof tmp, "%s.tmp", mode_path);
	fd = real_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return;
	real_write(fd, txt, (size_t)n);
	fsync(fd);
	real_close(fd);
	rename(tmp, mode_path);
}

/* ---- Code patches, installed once in the constructor (xdjmod_core.h: MTR_*, UT_*) ----
 * At that point rbp's main() hasn't run and no other thread exists, so nothing executes or reads the patched
 * words while they change. Every site is compared with the expected 2.21 bytes first; on any mismatch that
 * feature is left out entirely. */
static uint32_t rd32(uint32_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }

static int code_write(uint32_t a, const uint32_t *w, unsigned n)
{
	uintptr_t lo = a & ~(uintptr_t)4095, hi = (a + 4 * n + 4095) & ~(uintptr_t)4095;
	if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
	memcpy((void *)(uintptr_t)a, w, 4 * n);
	__builtin___clear_cache((char *)(uintptr_t)a, (char *)(uintptr_t)(a + 4 * n));
	mprotect((void *)lo, hi - lo, PROT_READ | PROT_EXEC);
	return 1;
}

static void meter_install(void)
{
	uint32_t cave[MTR_CAVE_WORDS], b;
	unsigned i;
	if (rd32(MTR_SITE) != MTR_SITE_ORIG) {
		boot_note("meter: unexpected code at 0x664a8 (%08x), not installed", rd32(MTR_SITE));
		return;
	}
	for (i = 0; i < MTR_CAVE_WORDS; i++)
		if (rd32(MTR_CAVE + 4 * i) != mtr_cave_orig[i]) {
			boot_note("meter: unexpected code at %x, not installed", MTR_CAVE + 4 * i);
			return;
		}
	mtr_cave_build(cave, (uint32_t)(uintptr_t)&meter_pre);
	b = arm_b(MTR_SITE, MTR_CAVE);
	if (!code_write(MTR_CAVE, cave, MTR_CAVE_WORDS) || !code_write(MTR_SITE, &b, 1)) {
		boot_note("meter: code not writable (errno %d)", errno);
		return;
	}
	meter_ok = 1;
	boot_note("meter: switchable, %s master level", meter_pre ? "pre" : "post");
}

/* gui_task marks itself (gui_frame): its refresh waits are the ones SCREEN SYNC replaces. */
static __thread int t_gui __attribute__((tls_model("initial-exec")));

/* ---- SCREEN SYNC (xdjmod_core.h: sync_next, vs_pll) ----
 * DirectFB copies each finished picture into the single-page screen right after gui_task's refresh wait; the copy
 * overtakes the readout at row ~93 (the tear). With SCREEN SYNC on, xdjmod_sync (core 0) waits for the refreshes and
 * keeps their phase, and gui_task's wait becomes a sleep until "refresh - lead": started early enough, the copy runs
 * while the readout is in the bottom rows or the blanking and leaves no edge. The kernel holds the framebuffer lock
 * during a refresh wait, so gui_task and the thread never wait at the same time:
 *   SYNC_OFF  gui_task waits as Pioneer does; the thread doesn't wait.
 *   SYNC_REQ  set by the thread: gui_task does one more real wait, then switches to SYNC_ON.
 *   SYNC_ON   only the thread waits; gui_task sleeps. Back to SYNC_OFF only by the thread, between two of its waits
 *             (setting PIONEER, or the driver failing).
 * The thread sleeps until SYNC_WIN_US before each expected refresh and only then enters the driver, so the lock is
 * free most of the time (rbp makes no other framebuffer calls while running). */
enum { SYNC_OFF, SYNC_REQ, SYNC_ON };
#define SYNC_WIN_US 3000
static const char *const sync_label[SYNC_MODES] = {
	"pioneer", "at refresh", "1.5 ms earlier", "3.0 ms earlier", "4.5 ms earlier", "6.0 ms earlier",
};
#define SYNC_LABEL(s) ((unsigned)(s) < SYNC_MODES ? sync_label[s] : "?")
static volatile int sync_state;
static volatile int sync_started, sync_dead;	/* the thread exists (constructor) / it stopped for good */
struct sync_pub {				/* the refresh model (anchor + k * period) and the thread's counters */
	uint64_t anchor;
	uint32_t period_q8, valid;		/* period_q8 0 = no model (gui_task then uses sync_gui_real) */
	uint32_t wakes, resets, res_sum, res_max, late;
};
static volatile uint32_t sync_pub_seq;
static struct sync_pub sync_pub;		/* the thread publishes (seq), gui_task reads */
static struct vs_pll sync_pll;			/* the thread only */
static uint64_t sync_gui_real;			/* gui_task only: end of its last real wait (in SYNC_REQ) */

static void sleep_until_us(uint64_t t)
{
	struct timespec ts = { (time_t)(t / 1000000u), (long)(t % 1000000u) * 1000 };
	while (syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0 && errno == EINTR) {}
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

/* gui_task's FBIO_WAITFORVSYNC (inside DirectFB's flip), from the ioctl wrapper. */
static int sync_gui_wait(int fd, unsigned long req, void *arg)
{
	struct sync_pub m;
	uint64_t t, target;
	uint32_t q;
	int st = __atomic_load_n(&sync_state, __ATOMIC_ACQUIRE), e;
	if (st != SYNC_ON) {	/* the thread isn't waiting: Pioneer's wait */
		int r = real_ioctl(fd, req, arg);
		if (st == SYNC_REQ && r == 0) {
			int want = SYNC_REQ;
			sync_gui_real = now_us();
			__atomic_compare_exchange_n(&sync_state, &want, SYNC_ON, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		}
		return r;
	}
	t = now_us();
	if (!seq_read(&sync_pub_seq, &m, &sync_pub, sizeof m, &q) || !m.period_q8) {
		m.anchor = sync_gui_real;	/* the thread's first wake hasn't come yet */
		m.period_q8 = SYNC_PERIOD_NOMINAL_Q8;
	}
	target = sync_next(t, m.anchor, m.period_q8, -sync_lead(sync_sel));	/* within one refresh from now */
	e = errno;
	sleep_until_us(target);
	errno = e;
	return 0;
}

static void sync_publish(const struct vs_pll *p, uint64_t t)
{
	struct sync_pub m;
	if (p->period_q8) {
		m.anchor = p->anchor;
		m.period_q8 = p->period_q8;
		m.valid = p->valid;
	} else {	/* learning: the last wake and the nominal period; t = 0: no model */
		m.anchor = t;
		m.period_q8 = t ? SYNC_PERIOD_NOMINAL_Q8 : 0;
		m.valid = 0;
	}
	m.wakes = p->wakes;
	m.resets = p->resets;
	m.res_sum = p->res_sum;
	m.res_max = p->res_max;
	m.late = p->late;
	seq_publish(&sync_pub_seq, &sync_pub, &m, sizeof m);
}

/* xdjmod_sync: SCREEN SYNC is always on (sync_sel = SYNC_GIG, 4.5 ms early; no Utility entry). Real-time 30 on
 * core 0, where it does a few us of work per refresh; it would idle (100 ms sleeps) only with the setting PIONEER.
 * Stops for good on driver errors or calls that don't wait. */
static void *sync_thread(void *unused)
{
	cpu_set_t cpus;
	struct sched_param rt = { 30 }, other = { 0 };
	uint32_t zero = 0;
	uint64_t prev = 0;
	int fd, rt_on = 0, errs = 0, err_total = 0, err_last = 0, fast = 0;
	(void)unused;
	prctl(PR_SET_NAME, "xdjmod_sync", 0, 0, 0);
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	sched_setaffinity(0, sizeof cpus, &cpus);
	fd = real_open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		note("sync: /dev/fb0 not opened (errno %d), Pioneer's refresh wait", errno);
		__atomic_store_n(&sync_dead, 1, __ATOMIC_RELEASE);
		return NULL;
	}
	note("sync: thread ready, setting %s", SYNC_LABEL(sync_sel));
	for (;;) {
		int sel = sync_sel, st = __atomic_load_n(&sync_state, __ATOMIC_ACQUIRE), want;
		uint64_t t, t_in;
		if (sel <= 0 || sel >= SYNC_MODES) {	/* PIONEER */
			if (st == SYNC_ON) {
				__atomic_store_n(&sync_state, SYNC_OFF, __ATOMIC_RELEASE);
				note("sync: off, Pioneer's refresh wait");
			} else if (st == SYNC_REQ) {
				want = SYNC_REQ;
				__atomic_compare_exchange_n(&sync_state, &want, SYNC_OFF, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
				continue;	/* gui_task may have switched to SYNC_ON meanwhile: look again */
			}
			if (rt_on) {
				pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
				rt_on = 0;
			}
			prev = 0;
			sleep_ms(100);
			continue;
		}
		if (st == SYNC_OFF) {	/* learn anew */
			sync_pll.n = sync_pll.period_q8 = sync_pll.valid = 0;
			sync_publish(&sync_pll, 0);	/* no stale model from an earlier time on */
			want = SYNC_OFF;
			__atomic_compare_exchange_n(&sync_state, &want, SYNC_REQ, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
			continue;
		}
		if (st == SYNC_REQ) {	/* gui_task hands over at its next refresh wait */
			sleep_ms(5);
			continue;
		}
		if (!rt_on) {
			rt_on = pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt) == 0 ? 1 : -1;
			note("sync: on, %s (%s)", SYNC_LABEL(sel), rt_on > 0 ? "real-time 30" : "real-time refused");
		}
		if (prev) {	/* sleep until SYNC_WIN_US before the next expected refresh */
			uint64_t a = sync_pll.period_q8 ? sync_pll.anchor : prev;
			uint32_t pq = sync_pll.period_q8 ? sync_pll.period_q8 : SYNC_PERIOD_NOMINAL_Q8;
			t = now_us();
			sleep_until_us(sync_next(t + SYNC_WIN_US, a, pq, 0) - SYNC_WIN_US);
		}
		t_in = now_us();
		if (real_ioctl(fd, FBIO_WAITFORVSYNC_, &zero) != 0) {
			err_last = errno;
			err_total++;
			if (++errs >= 5 || err_total >= 50) break;
			prev = 0;
			sleep_ms(20);	/* never spin */
			continue;
		}
		t = now_us();
		errs = 0;
		if (t - t_in < 200) {
			if (++fast >= 20) break;	/* returns without waiting: stop rather than spin at real-time */
		} else {
			fast = 0;
		}
		prev = t;
		vs_pll_add(&sync_pll, t);
		sync_publish(&sync_pll, t);
	}
	__atomic_store_n(&sync_state, SYNC_OFF, __ATOMIC_RELEASE);
	__atomic_store_n(&sync_dead, 1, __ATOMIC_RELEASE);
	if (rt_on) pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
	note("sync: stopped (%d errors, last errno %d, %d fast returns), Pioneer's refresh wait from now on", err_total,
	     err_last, fast);
	real_close(fd);
	return NULL;
}

/* Called by gui_task (real-time, core 0) after every frame, from the sequence at GUI_CAVE: returns the sleep in ms
 * (0 = draw the next frame at once). No locks, no logging. */
static uint32_t gui_frame(uint32_t dt)
{
	static int prev_late;
	t_gui = 1;	/* for the ioctl wrapper: this thread's refresh waits are SCREEN SYNC's */
	return gui_sleep_ms(dt, gui_period, &prev_late);
}

static void gui_install(void)
{
	uint32_t cave[GUI_CAVE_WORDS], b;
	unsigned i;
	if (rd32(GUI_SITE) != gui_site_orig[0] || rd32(GUI_SITE + 4) != gui_site_orig[1]) {
		boot_note("display: unexpected code at %x, not installed", GUI_SITE);
		return;
	}
	for (i = 0; i < GUI_CAVE_WORDS; i++)
		if (rd32(GUI_CAVE + 4 * i) != gui_cave_orig[i]) {
			boot_note("display: unexpected code at %x, not installed", GUI_CAVE + 4 * i);
			return;
		}
	gui_cave_build(cave, (uint32_t)(uintptr_t)gui_frame);	/* bit 0 set if Thumb: blx switches */
	b = arm_b(GUI_SITE, GUI_CAVE);
	if (!code_write(GUI_CAVE, cave, GUI_CAVE_WORDS) || !code_write(GUI_SITE, &b, 1)) {
		boot_note("display: code not writable (errno %d)", errno);
		return;
	}
	gui_ok = 1;
	boot_note("display: frame rate switchable, %s fps", xdj_fps_name[fps_sel]);
}

/* ---- Waveform position at draw time (xdjmod_core.h: POS_*, COL_*) ----
 * pos_copy runs in Ui_CycleTask, pos_hook in gui_task. The tracks run in both settings; only SMOOTH writes the
 * column. */
static struct pos_ring pos_ring;
static struct pos_trk pos_trk[2];		/* gui_task only, as are the next four */
static struct pos_delay pos_delay;
static uint32_t pos_next;
static uint64_t pos_last_copy;

static void *pos_copy(void *dst, const void *src, size_t n)
{
	memcpy(dst, src, n);
	if ((uintptr_t)dst == POS_DECKS && n == POS_SNAP_LEN)
		pos_ring_put(&pos_ring, now_us(), rd32(POS_DECKS + POS_MS), rd32(POS_DECKS + POS_DECK_STEP + POS_MS));
	return dst;
}

/* gui_task: take the new snapshot copies from the ring: the copy interval (for the delay), each deck's positions. */
static void pos_take(void)
{
	struct pos_sample s;
	uint32_t lost = 0;
	int r, k;
	while ((r = pos_ring_get(&pos_ring, &pos_next, &s, &lost))) {
		if (r == 1 && pos_last_copy && s.t > pos_last_copy)
			pos_delay_interval(&pos_delay, s.t - pos_last_copy > 0xffffffffu ? 0xffffffffu :
							(uint32_t)(s.t - pos_last_copy));
		pos_last_copy = s.t;
		for (k = 0; k < 2; k++) pos_feed(&pos_trk[k], s.t, s.ms[k]);
	}
}

static void pos_hook(uint32_t deck)
{
	uint32_t wv = WV_BASE + WV_STEP * deck, count, mine;
	uint64_t t;
	double x;
	float zoom;
	((void (*)(uint32_t))(uintptr_t)COL_FN)(deck);
	if (deck > 1) return;
	t = now_us();
	pos_take();
	t -= pos_delay_step(&pos_delay);	/* the display time: now - delay (slewed every waveform) */
	if (!scroll_sel) return;
	count = rd32(rd32(rd32(wv + 4)));
	zoom = *(volatile const float *)(uintptr_t)(wv + 0x18);
	if (zoom > 0 && pos_at(&pos_trk[deck], t, &x)) {
		mine = pos_col(x, count, zoom);
		if (mine != rd32(wv + 0xc)) *(volatile uint32_t *)(uintptr_t)(wv + 0xc) = mine;
	}
}

static void pos_install(void)
{
	uint32_t cave[POS_CAVE_WORDS], b[3];
	unsigned i;
	if (!gui_ok) return;	/* needs the frame-rate sequence */
	if (rd32(POS_SITE) != POS_SITE_ORIG || rd32(COL_SITE0) != COL_SITE0_ORIG || rd32(COL_SITE1) != COL_SITE1_ORIG) {
		boot_note("scroll: unexpected code at %x/%x/%x, not installed", POS_SITE, COL_SITE0, COL_SITE1);
		return;
	}
	for (i = 0; i < POS_CAVE_WORDS; i++)
		if (rd32(POS_CAVE + 4 * i) != pos_cave_orig[i]) {
			boot_note("scroll: unexpected code at %x, not installed", POS_CAVE + 4 * i);
			return;
		}
	pos_cave_build(cave, (uint32_t)(uintptr_t)pos_copy, (uint32_t)(uintptr_t)pos_hook);
	b[0] = arm_bl(POS_SITE, POS_CAVE);
	b[1] = arm_bl(COL_SITE0, POS_CAVE + 8);
	b[2] = arm_bl(COL_SITE1, POS_CAVE + 8);
	if (!code_write(POS_CAVE, cave, POS_CAVE_WORDS) || !code_write(POS_SITE, &b[0], 1) ||
	    !code_write(COL_SITE0, &b[1], 1) || !code_write(COL_SITE1, &b[2], 1)) {
		boot_note("scroll: code not writable (errno %d)", errno);
		return;
	}
	pos_ok = 1;
	boot_note("scroll: switchable, %s", scroll_sel ? "smooth" : "original");
}

/* ---- Colour waveforms (xdjmod_core.h: WAVE_*, OV_*) ----
 * The sequence at WAVE_CAVE sends every column of the detail waveforms to wave_col (RGB) or wave_col3 (3-band)
 * while wave_fn holds its address, else straight on to Pioneer's routine (BLUE); the one at OV_CAVE does the same for
 * the overview strips with ov_fn and ov_draw / ov_draw3. The worker thread reads each ANLZ????.EXT that rbp opens (and
 * the .DAT and .2EX beside it), keeps a few tracks' colours and puts a track on a deck once the deck's buffer holds
 * exactly its PWV3 data. */
static volatile uint32_t wave_fn;		/* read by the sequence in rbp for every column */
static volatile uint32_t ov_fn;			/* read by the sequence in rbp for every overview drawn */
static int wave_ok, ov_ok;			/* sequences installed (constructor) */
static struct wave_env wave_env;
static struct wave_cache wave_cache;		/* worker only */
static pthread_mutex_t wave_lock;		/* the path queue */
#define WAVE_QUEUE 8
static char wave_q[WAVE_QUEUE][192];
static unsigned wave_qhead, wave_qtail, wave_qlost;	/* wave_qlost: counted, not reported */

/* Called by gui_task for each column in RGB / 3-band mode, with rbp's arguments (zoom in s0). */
static WAVE_PCS void wave_col(uint32_t deck, uint32_t col, uint32_t start, uint32_t end, float zoom)
{
	wave_col_run(&wave_env, deck, col, start, end, zoom);
}

static WAVE_PCS void wave_col3(uint32_t deck, uint32_t col, uint32_t start, uint32_t end, float zoom)
{
	wave_col_run3(&wave_env, deck, col, start, end, zoom);
}

/* Called by gui_task instead of 0x1f8518 in RGB / 3-band mode. */
static void ov_draw(void)
{
	ov_run(&wave_env);
}

static void ov_draw3(void)
{
	ov_run3(&wave_env);
}

static void wave_set(int mode)
{
	wave_mode = mode;
	__atomic_store_n(&wave_fn, mode == WAVE_RGB ? (uint32_t)(uintptr_t)wave_col :
				   mode == WAVE_3BAND ? (uint32_t)(uintptr_t)wave_col3 : 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&ov_fn, mode == WAVE_RGB ? (uint32_t)(uintptr_t)ov_draw :
				 mode == WAVE_3BAND ? (uint32_t)(uintptr_t)ov_draw3 : 0u, __ATOMIC_RELEASE);
}

/* rbp opened a file (any thread): queue it if it is a track's analysis file with the detail waveforms. */
static void wave_opened(const char *path)
{
	const char *b;
	size_t n;
	if (!wave_ok || !path) return;
	n = strlen(path);
	if (n < 12 || n >= sizeof wave_q[0]) return;
	b = path + n - 12;	/* ANLZnnnn.EXT */
	if ((b != path && b[-1] != '/') || strncasecmp(b, "ANLZ", 4) != 0 || strcasecmp(b + 8, ".EXT") != 0) return;
	pthread_mutex_lock(&wave_lock);
	if (wave_qtail != wave_qhead && !strcmp(wave_q[(wave_qtail - 1) % WAVE_QUEUE], path))
		;	/* opened again right away */
	else if (wave_qtail - wave_qhead < WAVE_QUEUE)
		memcpy(wave_q[wave_qtail++ % WAVE_QUEUE], path, n + 1);
	else
		wave_qlost++;
	pthread_mutex_unlock(&wave_lock);
}

/* No column call holds a track (waits up to ~10 ms; a column takes microseconds). */
static int wave_quiet(struct wave_env *e)
{
	int i;
	for (i = 0; i < 50; i++) {
		struct timespec ts = { 0, 200 * 1000 };
		if (!__atomic_load_n(&e->inflight, __ATOMIC_SEQ_CST)) return 1;
		nanosleep(&ts, NULL);
	}
	return 0;
}

/* The whole of an open analysis file (28 bytes to 4 MB: tracks up to ~2 h), malloc'd; NULL if not. */
static uint8_t *wave_file(int fd, size_t *len)
{
	off_t size = lseek(fd, 0, SEEK_END);
	uint8_t *d = NULL;
	size_t got = 0;
	if (size >= 28 && size <= (4 << 20) && lseek(fd, 0, SEEK_SET) == 0) d = malloc((size_t)size);
	while (d && got < (size_t)size) {
		ssize_t r = real_read(fd, d + got, (size_t)size - got);
		if (r <= 0) break;
		got += (size_t)r;
	}
	*len = got;
	return d;
}

/* The file beside ANLZnnnn.EXT (wave_opened) with extension ext (upper case; lower case if the path's is), or NULL. */
static uint8_t *wave_beside(const char *path, const char *ext, size_t *len)
{
	char p[sizeof wave_q[0]];
	size_t n = strlen(path), k;
	uint8_t *d;
	int fd;
	memcpy(p, path, n + 1);
	for (k = 0; k < 3; k++)
		p[n - 3 + k] = path[n - 3] == 'E' ? ext[k] : (char)(ext[k] | 0x20);	/* digits stay */
	fd = real_open(p, O_RDONLY);
	if (fd < 0) return NULL;
	d = wave_file(fd, len);
	real_close(fd);
	return d;
}

/* The tracks read so far (path and entries), newest last. rbp keeps the tracks it loaded and loads one again without
 * opening its file (a track loaded with the track search buttons would stay blue after it left our cache),
 * so a deck that holds a track no longer cached has its file read again (wave_again). Worker only. */
#define WAVE_KNOWN 32
static struct { char path[sizeof wave_q[0]]; uint32_t n; } wave_known[WAVE_KNOWN];
static unsigned wave_nknown, wave_reread;	/* wave_reread: counted, not reported */
static uint32_t wave_looked[2];	/* per deck: the buffer fingerprint last looked up */

static void wave_remember(const char *path, uint32_t n)
{
	unsigned i;
	for (i = 0; i < WAVE_KNOWN && strcmp(wave_known[i].path, path) != 0; i++)
		;
	if (i == WAVE_KNOWN) i = wave_nknown++ % WAVE_KNOWN;
	snprintf(wave_known[i].path, sizeof wave_known[i].path, "%s", path);
	wave_known[i].n = n;
}

/* ---- beat jump (xdjmod_core.h bj_*) ----
 * The grid: the worker parses each track's PQTZ when it reads the .DAT (bj_grid_add, a few cached by file key) and
 * copies the grid of the track on each deck into bj_ms[deck] (bj_grid_publish), under bj_lock. rbp's key thread
 * reads it there when a pad jumps; it never touches the worker's cache, whose tracks may be freed. */
#define BJ_CACHED 8
static struct { uint32_t key, n, used; uint32_t *ms; } bj_gc[BJ_CACHED];	/* worker only */
static uint32_t bj_gc_stamp, bj_tmp[BJ_MAX_BEATS];				/* worker only */
static pthread_mutex_t bj_lock;
static uint32_t bj_ms[2][BJ_MAX_BEATS], bj_n[2];	/* (bj_lock) */
static uint32_t bj_pub_key[2];				/* worker only: the file key published per deck */
static struct bj_deck bj_dk[2];				/* rbp's key thread */
static uint8_t *volatile bj_obj[2];			/* the decks' key objects, as the hooks saw them */
static int bj_ok;
static volatile uint8_t bj_press_seq[2][4];		/* rbp's key thread counts the pad presses it took */
static struct bj_lights bj_lt[2];			/* UI thread only, read before it takes led_lock */
static uint8_t bj_led_sent[2];				/* the subpage LED state in the frames now (led_lock) */

static void bj_grid_add(uint32_t key, const uint8_t *dat, size_t len)
{
	uint32_t n = anlz_pqtz(dat, len, bj_tmp, BJ_MAX_BEATS), *m;
	int k, v = -1;
	if (!n) {
		note("beatjump: no beat grid in the .DAT");
		return;
	}
	for (k = 0; k < BJ_CACHED && v < 0; k++)
		if (bj_gc[k].ms && bj_gc[k].key == key) v = k;
	for (k = 0; k < BJ_CACHED && v < 0; k++)
		if (!bj_gc[k].ms) v = k;
	if (v < 0) {	/* the least recently used one that is on no deck */
		for (k = 0; k < BJ_CACHED; k++)
			if (bj_gc[k].key != bj_pub_key[0] && bj_gc[k].key != bj_pub_key[1] && (v < 0 || bj_gc[k].used < bj_gc[v].used))
				v = k;
		if (v < 0) return;
	}
	if (!(m = malloc((size_t)n * sizeof *m))) return;
	memcpy(m, bj_tmp, (size_t)n * sizeof *m);
	free(bj_gc[v].ms);
	bj_gc[v].ms = m;
	bj_gc[v].n = n;
	bj_gc[v].key = key;
	bj_gc[v].used = ++bj_gc_stamp;
}

/* The grid of the track now on deck d (its file key, 0 = none) goes to bj_ms[d]; checked by the worker every 50 ms. */
static void bj_grid_publish(uint32_t d, uint32_t key)
{
	int k, v = -1;
	if (key == bj_pub_key[d]) return;
	for (k = 0; k < BJ_CACHED && key; k++)
		if (bj_gc[k].ms && bj_gc[k].key == key) v = k;
	if (key && v < 0) return;	/* no grid (yet): try again next time */
	pthread_mutex_lock(&bj_lock);
	if (v >= 0) memcpy(bj_ms[d], bj_gc[v].ms, (size_t)bj_gc[v].n * sizeof bj_ms[d][0]);
	bj_n[d] = v >= 0 ? bj_gc[v].n : 0;
	pthread_mutex_unlock(&bj_lock);
	bj_pub_key[d] = key;
	if (v >= 0) {
		bj_gc[v].used = ++bj_gc_stamp;
		note("beatjump: deck %u grid, %u beats", d + 1, bj_gc[v].n);
	} else {
		note("beatjump: deck %u no grid", d + 1);
	}
}

static int bj_deck_of(const uint8_t *obj)
{
	uint8_t id = obj[BJ_OBJ_ID];
	return id == 1 ? 0 : id == 2 ? 1 : -1;
}

static int bj_mode(const uint8_t *obj) { return *(volatile const int32_t *)(obj + BJ_OBJ_MODE); }

static int bj_pads_held(const uint8_t *obj)
{
	return *(volatile const uint32_t *)(obj + BJ_OBJ_HELD) != (uint32_t)(uintptr_t)(obj + BJ_OBJ_HELD);
}

/* UI thread (its own state, no lock): deck d's subpage LED state at t (0 = rbp's own LEDs). The key thread's flags
 * are single bytes, read as they are: at worst one 10 ms tick late. rbp's pad mode is read through /proc/self/mem and
 * only while the subpage flag is set: this thread must never fault on rbp's object, whatever rbp does with it. */
static uint8_t bj_led_now(int d, uint64_t t)
{
	uint8_t *obj = bj_obj[d];
	int32_t mode = -1;
	int showing = bj_ok && obj && *(volatile uint8_t *)&bj_dk[d].sub &&
		      mem_read((uintptr_t)obj + BJ_OBJ_MODE, &mode, 4) && bj_showing(&bj_dk[d], mode);
	return bj_lights_step(&bj_lt[d], showing, *(volatile uint8_t *)&bj_dk[d].held, bj_press_seq[d], t);
}

/* ---- On air on the jog lights (xdjmod_core.h oa_*) ----
 * The UI thread reads rbp's mixer channels every tick while JOG LIGHT is ON AIR, through /proc/self/mem (a stale
 * pointer gives a failed read, never a fault) and only plausible values count. */
static uint8_t oa_sent;				/* the jog light state in the frames now (led_lock) */
static uint32_t oa_ch[2];			/* UI thread: the channel objects of CH1 / CH2, as last found */
static int oa_readable = -1;			/* UI thread: last result, for the notes */
static uint64_t oa_retry;			/* UI thread: when to look for the channels again */

static int oa_find(void)
{
	uint32_t eng, v[2], p[OA_MAX_CH], n, i, id;
	oa_ch[0] = oa_ch[1] = 0;
	if (!mem_read(OA_ENGINE, &eng, 4) || !eng || !mem_read(eng, v, 8) || v[1] <= v[0] || (v[1] - v[0]) & 3) return 0;
	n = (v[1] - v[0]) / 4;
	if (n > OA_MAX_CH || !mem_read(v[0], p, 4 * n)) return 0;
	for (i = 0; i < n; i++)
		if (p[i] && mem_read(p[i] + OA_CH_ID, &id, 4) && id < 2 && !oa_ch[id]) oa_ch[id] = p[i];
	return oa_ch[0] && oa_ch[1];
}

static int oa_read(int d, struct oa_channel *c)
{
	uint8_t o[OA_CH_LEN];
	uint32_t id;
	if (!oa_ch[d] || !mem_read(oa_ch[d], o, sizeof o)) return 0;
	memcpy(&id, o + OA_CH_ID, 4);
	memcpy(&c->fader, o + OA_CH_FADER, 4);
	memcpy(&c->fader_gain, o + OA_CH_FADER_GAIN, 4);
	memcpy(&c->xf_gain, o + OA_CH_XF_GAIN, 4);
	memcpy(&c->assign, o + OA_CH_XF_ASSIGN, 4);
	return id == (uint32_t)d && oa_plausible(c);
}

/* UI thread (its own state, no lock): the jog light state at this tick (0 = rbp's own). */
static uint8_t oa_led_now(uint64_t t)
{
	struct oa_channel c[2];
	unsigned on = 0;
	int d, ok;
	if (jog_mode != JOG_ONAIR) return 0;
	ok = oa_read(0, &c[0]) && oa_read(1, &c[1]);
	if (!ok && t >= oa_retry) {	/* not there (yet) or changed: look again, at most once a second */
		oa_retry = t + 1000000;
		ok = oa_find() && oa_read(0, &c[0]) && oa_read(1, &c[1]);
	}
	if (ok != oa_readable) {
		oa_readable = ok;
		note(ok ? "onair: mixer channels at %x, %x" : "onair: mixer not readable (%x, %x), jog lights stay rbp's",
		     oa_ch[0], oa_ch[1]);
	}
	if (!ok) return 0;
	for (d = 0; d < 2; d++)
		if (oa_on_air(&c[d])) on |= 1u << d;
	return oa_led_state(JOG_ONAIR, 1, on);
}

/* The jump, on rbp's key thread (as a hot cue): position (or the pending target of the last jump), the grid, rbp's
 * set-position. */
static void bj_jump(int d, int beats)
{
	uint32_t eng = rd32(BJ_ENGINE), players, pl;
	int64_t to = -1;
	int pos;
	uint32_t n;
	if (!eng) return;
	players = rd32(eng + BJ_ENG_PLAYERS);
	pl = players ? rd32(players + 4 * (uint32_t)d) : 0;
	if (!pl) return;
	pos = ((int (*)(uint32_t, uint32_t))(uintptr_t)BJ_GET_POS)(eng, (uint32_t)d);
	pthread_mutex_lock(&bj_lock);
	n = bj_n[d];
	if (n >= 2) to = bj_target(bj_ms[d], n, pos, beats);
	pthread_mutex_unlock(&bj_lock);
	if (to < 0 || to > 0x7fffffff) {
		return;
	}
	((void (*)(uint32_t, int))(uintptr_t)BJ_SET_POS)(pl, (int)to);
}

/* rbp's AUTO BEAT LOOP key handler calls this first (through the sequence at BJ_CAVE). rbp's handler always runs. */
static int bj_abl_fn(uint8_t *obj, const uint8_t *ev)
{
	int d = bj_deck_of(obj), was;
	if (d < 0) return -1;
	bj_obj[d] = obj;
	was = bj_dk[d].sub;
	bj_abl(&bj_dk[d], ev[BJ_EV_TYPE] & 15, obj[BJ_OBJ_SHIFT] != 0, bj_pads_held(obj), bj_mode(obj));
	if (bj_dk[d].sub != was) note("beatjump: deck %d subpage %s", d + 1, bj_dk[d].sub ? "on" : "off");
	return -1;
}

/* rbp's pad key handler calls this first: -1 = rbp's, 1 = the mod took it (a jump on the subpage). */
static int bj_pad_fn(uint8_t *obj, const uint8_t *ev)
{
	int d = bj_deck_of(obj), beats, r, was;
	uint32_t key = (uint32_t)ev[BJ_EV_KEY] | (uint32_t)ev[BJ_EV_KEY + 1] << 8;
	if (d < 0 || key - BJ_KEY_PAD1 > 3) return -1;
	bj_obj[d] = obj;
	was = bj_dk[d].sub;
	r = bj_pad(&bj_dk[d], (int)(key - BJ_KEY_PAD1), ev[BJ_EV_TYPE] & 15, bj_mode(obj), bj_beats[bj_set.a % BJ_SIZES],
		   bj_beats[bj_set.b % BJ_SIZES], &beats);
	if (bj_dk[d].sub != was) note("beatjump: deck %d subpage off (pad mode %d)", d + 1, bj_mode(obj));
	if (beats) {
		bj_press_seq[d][key - BJ_KEY_PAD1]++;	/* the pad lights (UI thread), even if the jump can't happen */
		bj_jump(d, beats);
	}
	return r;
}

/* In the constructor (no rbp thread yet), after the waveform worker's sequence (the grid needs it): both sites and the
 * cave checked against 2.21, the cave written, then the two branches. */
static void bj_install(void)
{
	uint32_t w[BJ_CAVE_WORDS], b;
	unsigned i;
	pthread_mutexattr_t ma;
	if (!wave_ok) {
		boot_note("beatjump: no waveform worker (no grid), not installed");
		return;
	}
	if (rd32(BJ_ABL_SITE) != BJ_ABL_ORIG || rd32(BJ_PAD_SITE) != BJ_PAD_ORIG) {
		boot_note("beatjump: unexpected key handlers, not installed");
		return;
	}
	for (i = 0; i < BJ_CAVE_WORDS; i++)
		if (rd32(BJ_CAVE + 4 * i) != bj_cave_orig[i]) {
			boot_note("beatjump: unexpected word at %x, not installed", BJ_CAVE + 4 * i);
			return;
		}
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);	/* rbp's key thread vs the worker (nice 19) */
	pthread_mutex_init(&bj_lock, &ma);
	pthread_mutexattr_destroy(&ma);
	bj_cave_build(w, (uint32_t)(uintptr_t)bj_abl_fn, (uint32_t)(uintptr_t)bj_pad_fn);
	if (!code_write(BJ_CAVE, w, BJ_CAVE_WORDS)) {
		boot_note("beatjump: code not writable (errno %d), not installed", errno);
		return;
	}
	b = arm_b(BJ_ABL_SITE, BJ_CAVE);
	if (!code_write(BJ_ABL_SITE, &b, 1)) {
		boot_note("beatjump: code not writable (errno %d), not installed", errno);
		return;
	}
	b = arm_b(BJ_PAD_SITE, BJ_CAVE + 4 * BJ_SEQ_WORDS);
	if (!code_write(BJ_PAD_SITE, &b, 1)) {	/* undo the first branch: both or neither */
		b = BJ_ABL_ORIG;
		code_write(BJ_ABL_SITE, &b, 1);
		boot_note("beatjump: code not writable (errno %d), not installed", errno);
		return;
	}
	bj_ok = 1;
	boot_note("beatjump: hooks in (A %s, B %s beats), AUTO BEAT LOOP LED flash, pad LEDs %s", bj_name[bj_set.a % BJ_SIZES],
		  bj_name[bj_set.b % BJ_SIZES], bj_pad_leds_known() == 8 ? "known" : "not known yet (rbp's)");
}

/* Read one track's .EXT, the .DAT beside it (PWAV, for the overview) and the .2EX (3-band), and cache its colours.
 * The same path and size is read only once while cached; a changed file replaces its older copy. 1 = read and
 * cached. */
static int wave_read(const char *path)
{
	int fd = real_open(path, O_RDONLY);
	uint32_t key = 2166136261u, file, n;
	const char *c;
	struct wave_track *t;
	uint8_t *d, *a, *x;
	size_t got = 0, alen = 0, xlen = 0;
	int dat, x2;
	if (fd < 0) return 0;
	for (c = path; *c; c++) key = (key ^ (uint8_t)*c) * 16777619u;
	file = key ? key : 1;
	key ^= (uint32_t)lseek(fd, 0, SEEK_END);
	d = wave_find(&wave_cache, key) ? NULL : wave_file(fd, &got);
	real_close(fd);
	if (!d) return 0;
	a = wave_beside(path, "DAT", &alen);
	x = wave_beside(path, "2EX", &xlen);
	t = wave_track_new(d, got, a, alen, x, xlen, key);
	if (a && bj_ok) bj_grid_add(key, a, alen);
	dat = a != NULL;
	x2 = x != NULL;
	free(d);
	free(a);
	free(x);
	if (!t) {
		note("wave: no colour waveform in %s", path);
		return 0;
	}
	t->file = file;
	n = t->n;
	note("wave: %u entries from %s, overview %s, 3-band %s%s", n, path,
	     t->ovp ? "too" : dat ? "missing" : "missing (no .DAT)", t->bands ? "too" : x2 ? "missing" : "missing (no .2EX)",
	     t->bands && !t->ov3 ? " (overview not)" : "");
	if (!wave_add(&wave_env, &wave_cache, t, wave_quiet)) {
		free(t);
		note("wave: cache busy, %s left out", path);
		return 0;
	}
	wave_remember(path, n);
	return 1;
}

/* A deck without our colours whose buffer is new: read again the files read before with its number of entries (rbp
 * has up to WAVE_EXTRA more). Ones still cached are not read; the next wave_step matches the deck. */
static void wave_again(void)
{
	char path[sizeof wave_q[0]];
	const uint8_t *buf;
	uint32_t d, i, count, sig;
	for (d = 0; d < 2; d++) {
		if (wave_env.pub[d] || !wave_deck(&wave_env, d, &count, &buf, &sig) || sig == wave_looked[d] ||
		    !wave_settled(&wave_env, &wave_cache, wave_quiet))
			continue;	/* unsettled: looked up later, when a track can be replaced */
		wave_looked[d] = sig;
		for (i = 0; i < WAVE_KNOWN; i++)
			if (wave_known[i].n && count >= wave_known[i].n && count - wave_known[i].n <= WAVE_EXTRA) {
				memcpy(path, wave_known[i].path, sizeof path);
				if (wave_read(path)) {
					wave_reread++;
					note("wave: deck %u: read again (rbp loaded it without opening it)", d + 1);
				}
			}
	}
}

static void *wave_thread(void *unused)
{
	cpu_set_t cpus;
	struct wave_track *was[2] = { NULL, NULL };
	(void)unused;
	prctl(PR_SET_NAME, "xdjmod_wave", 0, 0, 0);
	setpriority(PRIO_PROCESS, (id_t)gettid_(), 19);
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	sched_setaffinity(0, sizeof cpus, &cpus);
	for (;;) {
		struct timespec ts = { 0, 50 * 1000 * 1000 };
		char path[sizeof wave_q[0]];
		uint32_t d;
		nanosleep(&ts, NULL);
		for (;;) {
			int have = 0;
			pthread_mutex_lock(&wave_lock);
			if (wave_qhead != wave_qtail) {
				memcpy(path, wave_q[wave_qhead++ % WAVE_QUEUE], sizeof path);
				have = 1;
			}
			pthread_mutex_unlock(&wave_lock);
			if (!have) break;
			wave_read(path);
		}
		if (wave_step(&wave_env, &wave_cache, wave_quiet))
			for (d = 0; d < 2; d++)
				if (wave_env.pub[d] != was[d]) {
					was[d] = wave_env.pub[d];
					if (was[d])
						note("wave: deck %u in colour (%u entries)", d + 1, was[d]->n);
					else
						note("wave: deck %u in Pioneer's colours", d + 1);
				}
		if (bj_ok)
			for (d = 0; d < 2; d++) bj_grid_publish(d, wave_env.pub[d] ? wave_env.pub[d]->key : 0);
		wave_again();
	}
	return NULL;
}

/* The overview sequence, after the column sequence is in (the worker and wave_env are shared). Checks that rbp's
 * routine is the one ov_draw_strip copies: its call site, literals, colours and the stub it asks. */
static void ov_install(void)
{
	uint32_t cave[OV_CAVE_WORDS], b;
	unsigned i;
	if (rd32(OV_SITE) != OV_SITE_ORIG || rd32(OV_LIT) != OV_VIEW || rd32(OV_LIT + 4) != OV_BUF ||
	    rd32(OV_LIT + 8) != OV_PAL - 0x160 || rd32(OV_STUB) != ov_stub_orig[0] || rd32(OV_STUB + 4) != ov_stub_orig[1] ||
	    memcmp((const void *)(uintptr_t)OV_PAL, ov_blue, sizeof ov_blue) != 0) {
		boot_note("overview: unexpected code at %x (%08x), not installed", OV_SITE, rd32(OV_SITE));
		return;
	}
	for (i = 0; i < OV_CAVE_WORDS; i++)
		if (rd32(OV_CAVE + 4 * i) != ov_cave_orig[i]) {
			boot_note("overview: unexpected code at %x, not installed", OV_CAVE + 4 * i);
			return;
		}
	wave_env.ov_orig = (ov_orig_fn *)(uintptr_t)OV_ORIG;
	wave_env.ov_lock = (ov_lock_fn *)(uintptr_t)OV_LOCK;
	wave_env.ov_unlock = (ov_unlock_fn *)(uintptr_t)OV_UNLOCK;
	ov_cave_build(cave, (uint32_t)(uintptr_t)&ov_fn);
	b = arm_bl(OV_SITE, OV_CAVE);
	if (!code_write(OV_CAVE, cave, OV_CAVE_WORDS) || !code_write(OV_SITE, &b, 1)) {
		boot_note("overview: code not writable (errno %d)", errno);
		return;
	}
	ov_ok = 1;
	boot_note("overview: switchable");
}

static void wave_install(void)
{
	uint32_t cave[WAVE_CAVE_WORDS], b;
	pthread_mutexattr_t ma;
	unsigned i;
	if (rd32(WAVE_SITE) != WAVE_SITE_ORIG || rd32(WAVE_LIT_CTX) != WAVE_CTX || rd32(WAVE_LIT_PAL) != WAVE_PAL) {
		boot_note("waveform: unexpected code at %x (%08x), not installed", WAVE_SITE, rd32(WAVE_SITE));
		return;
	}
	for (i = 0; i < WAVE_CAVE_WORDS; i++)
		if (rd32(WAVE_CAVE + 4 * i) != wave_cave_orig[i]) {
			boot_note("waveform: unexpected code at %x, not installed", WAVE_CAVE + 4 * i);
			return;
		}
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);	/* rbp's threads queue paths */
	pthread_mutex_init(&wave_lock, &ma);
	pthread_mutexattr_destroy(&ma);
	wave_env.base = 0;
	wave_env.ctx = WAVE_CTX;
	wave_env.orig = (wave_col_fn *)(uintptr_t)WAVE_ORIG;
	wave_set(wave_mode);
	wave_cave_build(cave, (uint32_t)(uintptr_t)&wave_fn);
	b = arm_b(WAVE_SITE, WAVE_CAVE);
	if (!code_write(WAVE_CAVE, cave, WAVE_CAVE_WORDS) || !code_write(WAVE_SITE, &b, 1)) {
		boot_note("waveform: code not writable (errno %d)", errno);
		return;
	}
	wave_ok = 1;
	boot_note("waveform: switchable, %s", WAVE_NAME(wave_mode));
	ov_install();
}

/* Utility menu items. Labels are drawn as given (rbp's Utility labels are English in every language). */
static const uint16_t L_METER[] = u"MASTER LEVEL DISPLAY";
static const uint16_t V_PRE[] = u"PRE MASTER LEVEL";	/* "POST MASTER CONTROL" is too long for the screen */
static const uint16_t V_POST[] = u"POST MASTER LEVEL";
static const uint16_t *const meter_vals[2] = { V_PRE, V_POST };
static const uint16_t L_CUE[] = u"CUE MODE";
static const uint16_t V_NORMAL[] = u"NORMAL";
static const uint16_t V_SPLIT[] = u"SPLIT";
static const uint16_t V_SPLIT_ST[] = u"SPLIT STEREO";
static const uint16_t *const cue_vals[3] = { V_NORMAL, V_SPLIT, V_SPLIT_ST };
static const uint16_t L_MMONO[] = u"MASTER MONO";
static const uint16_t L_BMONO[] = u"BOOTH MONO";
static const uint16_t V_NO[] = u"NO";
static const uint16_t V_YES[] = u"YES";
static const uint16_t *const yes_no[2] = { V_NO, V_YES };
static const uint16_t L_FPS[] = u"DISPLAY REFRESH";
static const uint16_t V_FPS20[] = u"20 FPS (ORIGINAL)";
static const uint16_t V_FPS30[] = u"30 FPS";
static const uint16_t *const fps_vals[GUI_RATES] = { V_FPS20, V_FPS30 };
static const uint16_t L_SCROLL[] = u"WAVEFORM SCROLL";
static const uint16_t V_ORIGINAL[] = u"ORIGINAL";
static const uint16_t V_SMOOTH[] = u"SMOOTH";
static const uint16_t *const scroll_vals[2] = { V_ORIGINAL, V_SMOOTH };
static const uint16_t L_WAVE[] = u"WAVEFORM COLOR";
static const uint16_t V_BLUE[] = u"BLUE (ORIGINAL)";
static const uint16_t V_RGB[] = u"RGB";
static const uint16_t V_3BAND[] = u"3-BAND";
static const uint16_t *const wave_vals[3] = { V_BLUE, V_RGB, V_3BAND };
static const uint16_t L_BJA[] = u"BEAT JUMP A";
static const uint16_t L_BJB[] = u"BEAT JUMP B";
static const uint16_t V_BJ1[] = u"1 BEAT";
static const uint16_t V_BJ2[] = u"2 BEATS";
static const uint16_t V_BJ4[] = u"4 BEATS";
static const uint16_t V_BJ8[] = u"8 BEATS";
static const uint16_t V_BJ16[] = u"16 BEATS";
static const uint16_t V_BJ32[] = u"32 BEATS";
static const uint16_t *const bj_vals[BJ_SIZES] = { V_BJ1, V_BJ2, V_BJ4, V_BJ8, V_BJ16, V_BJ32 };
static const uint16_t L_JOG[] = u"JOG LIGHT";
static const uint16_t V_JOG_TOUCH[] = u"TOUCH";
static const uint16_t V_JOG_ONAIR[] = u"ON AIR";
static const uint16_t *const jog_vals[JOG_MODES] = { V_JOG_TOUCH, V_JOG_ONAIR };
#define UT_LABEL_ON_AIR_DISPLAY	0x39e4c0u	/* Pioneer's "ON AIR DISPLAY" (menu item 4) */
#define UT_CAP (UT_MAX + 3)	/* room for Pioneer's 16 items and ours */
static struct { uint32_t count; uint8_t items[UT_CAP * UT_ITEM_LEN]; } __attribute__((aligned(8))) ut_table;

/* Called by rbp's UI: like Pioneer's display functions, show the edited value while editing, else the live
 * setting. out = two text buffers: label, value. */
static int ut_show(uint8_t *it, uint8_t **out, const uint16_t *label, const uint16_t *const *vals, int count,
		   int live)
{
	int32_t v = (int32_t)le32(it + 0x10);
	if (v == -1) {
		v = live;
		put_le32(it + 0xc, (uint32_t)v);
	}
	if (v < 0 || v >= count) v = 0;
	ut_text(out[0], label);
	ut_text(out[1], vals[v]);
	return 0;
}

static int ut_meter_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_METER, meter_vals, 2, meter_pre ? 0 : 1);
}

static int ut_cue_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_CUE, cue_vals, 3, hp_mode);
}

static int ut_mmono_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_MMONO, yes_no, 2, master_mono);
}

static int ut_bmono_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_BMONO, yes_no, 2, booth_mono);
}

static int ut_fps_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_FPS, fps_vals, GUI_RATES, fps_sel);
}

static int ut_scroll_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_SCROLL, scroll_vals, 2, scroll_sel);
}

static int ut_scroll_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	scroll_sel = v;	/* one aligned word: the next waveform uses it */
	mode_dirty = 1;
	note("menu: waveform scroll %s", v ? "smooth" : "original");
	return 1;
}

static int ut_wave_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_WAVE, wave_vals, 3, wave_mode);
}

static int ut_wave_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	wave_set(v);	/* one aligned word: the next column uses it */
	mode_dirty = 1;
	note("menu: waveform color %s", WAVE_NAME(v));
	return 1;
}

static int ut_bja_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_BJA, bj_vals, BJ_SIZES, bj_set.a);
}

static int ut_bjb_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_BJB, bj_vals, BJ_SIZES, bj_set.b);
}

static int ut_bja_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	bj_set.a = v;	/* one aligned word: the next jump uses it */
	mode_dirty = 1;
	note("menu: beat jump A %s", bj_name[v % BJ_SIZES]);
	return 1;
}

static int ut_bjb_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	bj_set.b = v;
	mode_dirty = 1;
	note("menu: beat jump B %s", bj_name[v % BJ_SIZES]);
	return 1;
}

static int ut_jog_display(uint8_t *it, uint8_t **out)
{
	return ut_show(it, out, L_JOG, jog_vals, JOG_MODES, jog_mode);
}

static int ut_jog_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	jog_mode = v;	/* one aligned word: the UI thread's next tick uses it */
	mode_dirty = 1;
	note("menu: jog light %s", v == JOG_ONAIR ? "on air" : "touch");
	return 1;
}

static int ut_fps_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	fps_sel = v;
	gui_period = gui_period_ms[v];	/* one aligned word: the next frame uses it */
	mode_dirty = 1;
	note("menu: display refresh %s fps", xdj_fps_name[v]);
	return 1;
}

static int ut_mmono_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	master_mono = v;	/* the audio thread fades to it over the next period */
	mode_dirty = 1;
	note("menu: master mono %s", v ? "yes" : "no");
	return 1;
}

static int ut_bmono_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	booth_mono = v;
	mode_dirty = 1;
	note("menu: booth mono %s", v ? "yes" : "no");
	return 1;
}

static int ut_meter_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	meter_pre = v == 0;	/* one aligned word: the meter sequence picks it up on the next sample */
	mode_dirty = 1;
	note("menu: master level display %s", meter_pre ? "pre" : "post");
	return 1;
}

static int ut_cue_commit(uint8_t *it)
{
	int32_t v;
	if (!ut_commit(it, &v)) return 0;
	hp_mode = v;
	if (v != HP_NORMAL) split_pref = v;
	mode_dirty = 1;
	note("menu: cue mode %s", xdj_cue_name(v));
	return 1;
}

static void menu_install(void)
{
	uint8_t a[UT_ITEM_LEN], b[UT_ITEM_LEN], c[UT_ITEM_LEN], d[UT_ITEM_LEN], e[UT_ITEM_LEN];
	uint8_t h[UT_ITEM_LEN];
	uint8_t g[UT_ITEM_LEN];
	uint8_t ja[UT_ITEM_LEN], jb[UT_ITEM_LEN], jl[UT_ITEM_LEN];
	struct ut_ins ins[13];
	uint32_t after_fps = (uint32_t)(uintptr_t)L_FPS;
	uint32_t n, w;
	unsigned i, k = 0;
	if (rd32(UT_TABLE) != UT_COUNT || rd32(UT_LIT_ITEMS) != UT_TABLE + 4) {
		boot_note("menu: unexpected Utility table, not installed");
		return;
	}
	for (i = 0; i < 6; i++)
		if (rd32(ut_lit_table[i]) != UT_TABLE) {
			boot_note("menu: unexpected literal at %x, not installed", ut_lit_table[i]);
			return;
		}
	ut_item(a, (uint32_t)(uintptr_t)L_METER, (uint32_t)(uintptr_t)meter_vals, 2,
		(uint32_t)(uintptr_t)ut_meter_display, (uint32_t)(uintptr_t)ut_meter_commit);
	ut_item(b, (uint32_t)(uintptr_t)L_CUE, (uint32_t)(uintptr_t)cue_vals, 3,
		(uint32_t)(uintptr_t)ut_cue_display, (uint32_t)(uintptr_t)ut_cue_commit);
	ut_item(c, (uint32_t)(uintptr_t)L_MMONO, (uint32_t)(uintptr_t)yes_no, 2,
		(uint32_t)(uintptr_t)ut_mmono_display, (uint32_t)(uintptr_t)ut_mmono_commit);
	ut_item(d, (uint32_t)(uintptr_t)L_BMONO, (uint32_t)(uintptr_t)yes_no, 2,
		(uint32_t)(uintptr_t)ut_bmono_display, (uint32_t)(uintptr_t)ut_bmono_commit);
	/* MASTER ATT., MASTER LEVEL DISPLAY, MASTER MONO, BOOTH MONO, MIXER MODE, CUE MODE */
	if (meter_ok) ins[k++] = (struct ut_ins){ a, UT_LABEL_MASTER_ATT, 0 };
	ins[k++] = (struct ut_ins){ c, meter_ok ? (uint32_t)(uintptr_t)L_METER : UT_LABEL_MASTER_ATT, 0 };
	ins[k++] = (struct ut_ins){ d, (uint32_t)(uintptr_t)L_MMONO, 0 };
	ins[k++] = (struct ut_ins){ b, UT_LABEL_MIXER_MODE, 0 };
	if (gui_ok) {
		ut_item(e, (uint32_t)(uintptr_t)L_FPS, (uint32_t)(uintptr_t)fps_vals, GUI_RATES,
			(uint32_t)(uintptr_t)ut_fps_display, (uint32_t)(uintptr_t)ut_fps_commit);
		ins[k++] = (struct ut_ins){ e, UT_LABEL_LCD_BRIGHTNESS, 0 };
	}
	if (wave_ok) {	/* after DISPLAY REFRESH, or after LCD BRIGHTNESS without it */
		ut_item(g, (uint32_t)(uintptr_t)L_WAVE, (uint32_t)(uintptr_t)wave_vals, 3,
			(uint32_t)(uintptr_t)ut_wave_display, (uint32_t)(uintptr_t)ut_wave_commit);
		ins[k++] = (struct ut_ins){ g, gui_ok ? (uint32_t)(uintptr_t)L_FPS : UT_LABEL_LCD_BRIGHTNESS, 0 };
		after_fps = (uint32_t)(uintptr_t)L_WAVE;
	}
	if (pos_ok) {	/* pos_ok implies gui_ok: after WAVEFORM COLOR, or after DISPLAY REFRESH */
		ut_item(h, (uint32_t)(uintptr_t)L_SCROLL, (uint32_t)(uintptr_t)scroll_vals, 2,
			(uint32_t)(uintptr_t)ut_scroll_display, (uint32_t)(uintptr_t)ut_scroll_commit);
		ins[k++] = (struct ut_ins){ h, after_fps, 0 };
	}
	if (bj_ok) {	/* after CUE MODE: BEAT JUMP A, BEAT JUMP B */
		ut_item(ja, (uint32_t)(uintptr_t)L_BJA, (uint32_t)(uintptr_t)bj_vals, BJ_SIZES,
			(uint32_t)(uintptr_t)ut_bja_display, (uint32_t)(uintptr_t)ut_bja_commit);
		ut_item(jb, (uint32_t)(uintptr_t)L_BJB, (uint32_t)(uintptr_t)bj_vals, BJ_SIZES,
			(uint32_t)(uintptr_t)ut_bjb_display, (uint32_t)(uintptr_t)ut_bjb_commit);
		ins[k++] = (struct ut_ins){ ja, (uint32_t)(uintptr_t)L_CUE, 0 };
		ins[k++] = (struct ut_ins){ jb, (uint32_t)(uintptr_t)L_BJA, 0 };
	}
	/* JOG LIGHT right after Pioneer's ON AIR DISPLAY (reads rbp's mixer: no hook needed) */
	ut_item(jl, (uint32_t)(uintptr_t)L_JOG, (uint32_t)(uintptr_t)jog_vals, JOG_MODES,
		(uint32_t)(uintptr_t)ut_jog_display, (uint32_t)(uintptr_t)ut_jog_commit);
	ins[k++] = (struct ut_ins){ jl, UT_LABEL_ON_AIR_DISPLAY, 0 };
	n = ut_build(ut_table.items, UT_CAP, (const uint8_t *)(uintptr_t)(UT_TABLE + 4), UT_COUNT, ins, k);
	if (!n) return;
	ut_table.count = n;
	w = (uint32_t)(uintptr_t)&ut_table;
	for (i = 0; i < 6; i++)
		if (!code_write(ut_lit_table[i], &w, 1)) break;
	if (i == 6) {
		w += 4;
		if (code_write(UT_LIT_ITEMS, &w, 1)) {
			menu_ok = 1;
			boot_note("menu: Utility has %u items", n);
			return;
		}
	}
	/* partly written: point everything back at Pioneer's table */
	w = UT_TABLE;
	for (i = 0; i < 6; i++) code_write(ut_lit_table[i], &w, 1);
	w = UT_TABLE + 4;
	code_write(UT_LIT_ITEMS, &w, 1);
	boot_note("menu: code not writable (errno %d), not installed", errno);
}

/* Do the mod's LED bits apply (a beat jump subpage shows, or JOG LIGHT is ON AIR)? Caller holds led_lock. */
static int led_bj_on(void)
{
	return bj_led_sent[0] || bj_led_sent[1] || oa_sent;
}

/* Send rbp's latest LED frame, with the mod's bits (beat jump subpage, jog light) where those are on. Caller holds
 * led_lock. */
static int led_send(int fd, unsigned long req, const struct subucom_xfer *x, const uint8_t *frame)
{
	uint8_t f[128];
	struct subucom_xfer y = *x;
	unsigned len = x->bytes < sizeof f ? x->bytes : sizeof f;
	memcpy(f, frame, len);
	if (led_bj_on()) {
		if (len >= LED_BYTES) {
			bj_led_apply(f, 0, bj_led_sent[0]);
			bj_led_apply(f, 1, bj_led_sent[1]);
			oa_led_apply(f, oa_sent);
		}
		panel_tx_seal(f);
	}
	y.tx_buf = (uint64_t)(uintptr_t)f;
	return real_ioctl(fd, req, &y);	/* the driver copies the frame during the call */
}

static int is_led_msg(unsigned long req, const void *arg)
{
	const struct subucom_xfer *x = arg;
	return IOC_TYPE(req) == 'p' && IOC_NR(req) == 0 && IOC_DIR(req) == 1 && x && x->tx_buf &&
	       x->multi_id == 0 && x->part_id == 0 && x->bytes >= PANEL_TX_MIN_LEN && x->bytes <= sizeof led_rbp;
}

/* rbp sends an LED frame: remember it, and send it with the mod's bits (beat jump subpage, jog light) while those
 * are on. */
static int led_ioctl(int fd, unsigned long req, void *arg)
{
	const struct subucom_xfer *x = arg;
	int r;
	pthread_mutex_lock(&led_lock);
	memcpy(led_rbp, (const void *)(uintptr_t)x->tx_buf, x->bytes);
	led_x = *x;
	led_fd = fd;
	led_req = req;
	led_have = 1;
	cue_leds = led_rbp[PANEL_TX_CUE_BYTE];
	r = led_bj_on() ? led_send(fd, req, x, led_rbp) : real_ioctl(fd, req, arg);
	pthread_mutex_unlock(&led_lock);
	return r;
}

static void led_update(uint64_t t)
{
	uint8_t bj_now[2] = { bj_led_now(0, t), bj_led_now(1, t) }, oa = oa_led_now(t);
	pthread_mutex_lock(&led_lock);
	if (led_have) {
		int send = 0, d;
		for (d = 0; d < 2; d++)
			if (bj_now[d] != bj_led_sent[d]) {
				bj_led_sent[d] = bj_now[d];	/* 0: that deck's bits back to rbp's own */
				send = 1;
			}
		if (oa != oa_sent) {
			oa_sent = oa;			/* 0: the jog lights back to rbp's touch light */
			send = 1;
		}
		if (send) led_send(led_fd, led_req, &led_x, led_rbp);
	}
	pthread_mutex_unlock(&led_lock);
}

static void *ui_thread(void *unused)
{
	cpu_set_t cpus;
	uint64_t next_scan = 0, next_check = 0;
	uint32_t last_word = 2, last_gain = HP_STEREO_GAIN_BITS;
	unsigned scans = 0;
	(void)unused;
	prctl(PR_SET_NAME, "xdjmod_ui", 0, 0, 0);
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	sched_setaffinity(0, sizeof cpus, &cpus);	/* never on the audio core */
	boot_note("ui thread running, mode %s", xdj_cue_name(hp_mode));
	for (;;) {
		struct timespec ts = { 0, 10 * 1000 * 1000 };
		uint64_t t;
		nanosleep(&ts, NULL);
		t = now_us();
		/* first the heap every 2 s (10x), then full scans every 5 s (10x) */
		if (!hp_obj && t - t0_us > 5000000 && t >= next_scan && scans < 20) {
			scans++;
			hp_find(scans > 10);
			next_scan = t + (scans > 10 ? 5000000 : 2000000);
			if (!hp_obj && scans == 20) note("switch: mixer not found, switching stays off");
		}
		/* the stereo gain first, so the master never comes back at full level when the last cue goes off */
		{
			uint32_t g = hp_stereo_gain_bits(hp_obj ? hp_mode : HP_NORMAL, hp_trim);
			if (g != last_gain || (hp_obj && t >= next_check)) {
				gain_apply();
				last_gain = g;
			}
		}
		/* apply at once when the mode or the cue state changes (split only while cued), and
		 * re-assert once a second in case anything reset it */
		if (hp_obj) {
			uint32_t w = hp_mode_word(hp_mode, cue_leds);
			if (t >= next_check || mode_dirty || w != last_word) {
				hp_apply();
				last_word = w;
				next_check = t + 1000000;
			}
		}
		if (mode_dirty) {
			mode_dirty = 0;
			mode_save();
		}
		led_update(t);
	}
	return NULL;
}

/* ---------- libc wrappers ---------- */

EXPORT int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	int fd;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);	/* mode_t is promoted to int */
		va_end(ap);
	}
	resolve_libc();
	fd = real_open(path, flags, mode);
	opened(fd, path);
	if (fd >= 0) wave_opened(path);
	return fd;
}

EXPORT int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	int fd;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);	/* mode_t is promoted to int */
		va_end(ap);
	}
	resolve_libc();
	fd = real_open64(path, flags, mode);
	opened(fd, path);
	if (fd >= 0) wave_opened(path);
	return fd;
}

EXPORT FILE *fopen(const char *path, const char *mode)
{
	FILE *f;
	resolve_libc();
	f = real_fopen(path, mode);
	if (f) wave_opened(path);
	return f;
}

EXPORT FILE *fopen64(const char *path, const char *mode)
{
	FILE *f;
	resolve_libc();
	f = real_fopen64(path, mode);
	if (f) wave_opened(path);
	return f;
}

EXPORT int close(int fd)
{
	resolve_libc();
	if (fd >= 0 && fd < MAX_FD)
		fd_class[fd] = FD_NONE;	/* before the fd number can be reused */
	return real_close(fd);
}

EXPORT int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	int cls;
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	resolve_libc();
	cls = fd_cls(fd);
	if (cls == FD_FB && t_gui && req == FBIO_WAITFORVSYNC_) return sync_gui_wait(fd, req, arg);
	if (cls == FD_SPI && sw_on && is_led_msg(req, arg)) return led_ioctl(fd, req, arg);
	return real_ioctl(fd, req, arg);
}

/* ---------- ALSA wrappers ---------- */

static struct pcm_slot *slot_of(const void *h)
{
	int i;
	if (!h) return NULL;
	for (i = 0; i < MAX_PCM; i++)
		if (pcm[i].handle == h)
			return &pcm[i];
	return NULL;
}

EXPORT int snd_pcm_open(void **pcmp, const char *name, int stream, int mode)
{
	int r, i, idx = MAX_PCM;	/* MAX_PCM = not tracked */
	resolve_alsa();
	r = real_pcm_open(pcmp, name, stream, mode);
	if (!active) return r;
	boot_note("snd_pcm_open %s stream=%d -> %d", name ? name : "?", stream, r);
	for (i = 0; r == 0 && pcmp && *pcmp && i < MAX_PCM; i++) {
		void *expected = NULL;
		struct pcm_slot *s = &pcm[i];
		/* claim first; no I/O can reach the slot before this call returns */
		if (!__atomic_compare_exchange_n(&s->handle, &expected, *pcmp, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			continue;
		memset(&s->acc, 0, sizeof s->acc);
		memset(&s->hw, 0, sizeof s->hw);
		s->win_start = s->last_entry = 0;
		snprintf(s->name, sizeof s->name, "%s", name ? name : "?");
		s->stream = stream;
		s->role = pcm_role(name, stream);
		s->mono_g = 0;
		idx = i;
		break;
	}
	{
		struct ev *e = ev_new(EV_PCM_OPEN, now_us());
		if (e) {
			e->a = (uint32_t)idx;
			e->b = (uint32_t)stream;
			e->c = r;
			e->e = (uint32_t)mode;
			ev_data(e, name ? name : "?", strlen(name ? name : "?") + 1);
			ev_commit(e);
		}
	}
	return r;
}

EXPORT int snd_pcm_close(void *h)
{
	struct pcm_slot *s;
	int r;
	resolve_alsa();
	s = active ? slot_of(h) : NULL;
	r = real_pcm_close(h);
	if (s) {
		struct ev *e = ev_new(EV_PCM_CLOSE, now_us());
		if (e) {
			e->a = (uint32_t)(s - pcm);
			e->c = r;
			ev_commit(e);
		}
		__atomic_store_n(&s->handle, NULL, __ATOMIC_RELEASE);
	}
	return r;
}

EXPORT int snd_pcm_hw_params(void *h, void *params)
{
	struct pcm_slot *s;
	int r;
	resolve_alsa();
	r = real_pcm_hw_params(h, params);
	s = active ? slot_of(h) : NULL;
	if (s) {
		struct pcm_hw hw = { -1, 0, 0, 0, 0 };
		struct ev *e;
		if (r == 0) {
			int fmt = -1, dir = 0;
			unsigned ch = 0, rate = 0;
			unsigned long period = 0, buffer = 0;
			if (get_format) get_format(params, &fmt);
			if (get_channels) get_channels(params, &ch);
			if (get_rate) get_rate(params, &rate, &dir);
			if (get_period_size) get_period_size(params, &period, &dir);
			if (get_buffer_size) get_buffer_size(params, &buffer);
			hw.format = fmt;
			hw.channels = ch;
			hw.rate = rate;
			hw.period = (uint32_t)period;
			hw.buffer = (uint32_t)buffer;
			s->hw = hw;
			boot_note("hw_params pcm%d fmt=%d ch=%u rate=%u period=%lu", (int)(s - pcm), fmt, ch, rate, period);
		}
		if ((e = ev_new(EV_PCM_HW, now_us()))) {
			e->a = (uint32_t)(s - pcm);
			e->c = r;
			ev_data(e, &hw, sizeof hw);
			ev_commit(e);
		}
	}
	return r;
}

EXPORT int snd_pcm_prepare(void *h)
{
	struct pcm_slot *s;
	int r;
	resolve_alsa();
	r = real_pcm_prepare(h);
	s = active ? slot_of(h) : NULL;
	if (s) {
		struct ev *e = ev_new(EV_PCM_PREPARE, now_us());
		if (e) {
			e->a = (uint32_t)(s - pcm);
			e->c = r;
			ev_commit(e);
		}
	}
	return r;
}

/* An xrun or a real error (rare): one line in the capture folder's log. */
static void pcm_err(struct pcm_slot *s, long r, unsigned long frames)
{
	struct ev *e;
	if (r == -EAGAIN || !(e = ev_new(EV_PCM_ERR, now_us()))) return;
	e->a = (uint32_t)(s - pcm);
	e->b = (uint32_t)s->stream;
	e->c = (int32_t)r;
	e->e = (uint32_t)frames;
	ev_commit(e);
}

/* The audio path: MASTER/BOOTH MONO where on (or fading off), the master's periods for the safe start;
 * nothing measured, no clock read. */
EXPORT long snd_pcm_writei(void *h, const void *buf, unsigned long frames)
{
	struct pcm_slot *s;
	const void *out = buf;
	long r;
	resolve_alsa();
	s = active ? slot_of(h) : NULL;
	if (!s)
		return real_pcm_writei(h, buf, frames);
	if (s->role == 1) master_periods++;
	if (s->role && s->hw.format == FMT_S24_LE && s->hw.channels == 2 && frames <= MONO_MAX_FRAMES && buf) {
		float want = (s->role == 1 ? master_mono : booth_mono) ? 1.0f : 0.0f;
		if (want != 0.0f || s->mono_g != 0.0f) {
			int32_t *m = mono_buf[s - pcm];
			mono_mix_s24(m, buf, frames, s->mono_g, want);
			s->mono_g = want;
			out = m;
		}
	}
	r = real_pcm_writei(h, out, frames);
	if (r < 0) pcm_err(s, r, frames);
	return r;
}

EXPORT long snd_pcm_readi(void *h, void *buf, unsigned long frames)
{
	struct pcm_slot *s;
	long r;
	resolve_alsa();
	r = real_pcm_readi(h, buf, frames);
	if (r < 0 && active && (s = slot_of(h))) pcm_err(s, r, frames);
	return r;
}

/* ---------- log thread ---------- */

#define OUT_BUF 65536
static char outbuf[OUT_BUF];
static size_t outlen;
static uint64_t written_total;
#define WRITE_CAP (32u << 20)	/* stop writing to the stick after 32 MB */
static int cap_noted;
static uint8_t log_tx_prev[16][128];
static uint8_t log_tx_seen[16];
static uint8_t log_rx_prev[128];
static int log_rx_seen;

static void out_append(const char *s)
{
	size_t n = strlen(s);
	if (outlen + n >= OUT_BUF) return;	/* flush() runs before this can happen in practice */
	memcpy(outbuf + outlen, s, n);
	outlen += n;
}

/* XDJMOD_OUT=scan:<name>: log into the first folder /media/usb<N>/<partition>/<name> that exists.
 * Used when the library is started by apl_start at power-on, before any stick is mounted. */
static char scan_name[64];

static int find_out_dir(void)
{
	char port[32], cand[256];
	int u;
	for (u = 1; u <= 4; u++) {
		DIR *d;
		struct dirent *de;
		snprintf(port, sizeof port, "/media/usb%d", u);
		if (!(d = opendir(port))) continue;
		while ((de = readdir(d))) {
			DIR *t;
			if (de->d_name[0] == '.') continue;
			snprintf(cand, sizeof cand, "%s/%s/%s", port, de->d_name, scan_name);
			if ((t = opendir(cand))) {
				closedir(t);
				closedir(d);
				if (strcmp(out_dir, cand) != 0) snprintf(out_dir, sizeof out_dir, "%s", cand);
				__atomic_store_n(&out_found, 1, __ATOMIC_RELEASE);
				return 1;
			}
		}
		closedir(d);
	}
	return 0;
}

static void out_flush(void)
{
	char path[300];
	int fd;
	if (!outlen) return;
	if (written_total >= WRITE_CAP) {
		outlen = 0;
		return;
	}
	if (!__atomic_load_n(&out_found, __ATOMIC_ACQUIRE))
		return;	/* scan mode, no folder yet: keep the buffer (start-up notes) until one appears */
	snprintf(path, sizeof path, "%s/hook.log", out_dir);
	fd = real_open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd < 0) {
		if (scan_name[0]) {	/* stick pulled: diagnostics off, look again, keep the buffer */
			__atomic_store_n(&out_found, 0, __ATOMIC_RELEASE);
			return;
		}
	} else {		/* fixed folder gone: drop this block, try again next time */
		ssize_t w = real_write(fd, outbuf, outlen);
		if (w > 0) written_total += (uint64_t)w;
		real_close(fd);
	}
	outlen = 0;
	if (written_total >= WRITE_CAP && !cap_noted) {
		cap_noted = 1;	/* the log is full: nothing more is written */
	}
}

static double tsec(uint64_t t) { return (double)(t - t0_us) / 1e6; }

static void format_event(const struct ev *e)
{
	char line[1024], hex[600], diff[600];
	double t = tsec(e->t_us);
	switch (e->type) {
	case EV_NOTE:
		snprintf(line, sizeof line, "%9.3f note %.*s\n", t, (int)e->len, (const char *)e->d);
		break;
	case EV_OPEN:
		snprintf(line, sizeof line, "%9.3f open fd=%u %s %.*s\n", t, e->a,
			 e->b < 5 ? class_name[e->b] : "?", (int)e->len, (const char *)e->d);
		break;
	case EV_PCM_OPEN:
		snprintf(line, sizeof line, "%9.3f pcm%u open name=%.*s stream=%s mode=0x%x -> %d tid=%d\n", t,
			 e->a, (int)e->len, (const char *)e->d, e->b ? "capture" : "playback", e->e, e->c, e->tid);
		break;
	case EV_PCM_HW: {
		struct pcm_hw hw;
		memcpy(&hw, e->d, sizeof hw);
		snprintf(line, sizeof line,
			 "%9.3f pcm%u hw_params format=%s(%d) channels=%u rate=%u period=%u buffer=%u -> %d\n", t,
			 e->a, fmt_name(hw.format), hw.format, hw.channels, hw.rate, hw.period, hw.buffer, e->c);
		break;
	}
	case EV_PCM_PREPARE:
		snprintf(line, sizeof line, "%9.3f pcm%u prepare -> %d tid=%d\n", t, e->a, e->c, e->tid);
		break;
	case EV_PCM_CLOSE:
		snprintf(line, sizeof line, "%9.3f pcm%u close -> %d\n", t, e->a, e->c);
		break;
	case EV_PCM_ERR:
		snprintf(line, sizeof line, "%9.3f pcm%u %s error %d (%s) frames=%u tid=%d\n", t, e->a,
			 e->b ? "read" : "write", e->c, e->c == -EPIPE ? "xrun" : "other", e->e, e->tid);
		break;
	case EV_I2C_WRITE:
		hex_bytes(hex, sizeof hex, e->d, e->len);
		snprintf(line, sizeof line, "%9.3f i2c fd=%u write %s -> %d tid=%d\n", t, e->a, hex, e->c, e->tid);
		break;
	case EV_I2C_READ:
		hex_bytes(hex, sizeof hex, e->d, e->len);
		snprintf(line, sizeof line, "%9.3f i2c fd=%u read %s tid=%d\n", t, e->a, hex, e->tid);
		break;
	case EV_I2C_ADDR:
		snprintf(line, sizeof line, "%9.3f i2c fd=%u slave 0x%02x -> %d tid=%d\n", t, e->a, e->e, e->c, e->tid);
		break;
	case EV_I2C_RDWR: {
		size_t o = 0, lo;
		lo = (size_t)snprintf(line, sizeof line, "%9.3f i2c fd=%u rdwr msgs=%u -> %d tid=%d:", t, e->a, e->b,
				      e->c, e->tid);
		while (o + 6 <= e->len && lo < sizeof line - 64) {
			uint16_t addr, flags, len;
			unsigned nb;
			memcpy(&addr, e->d + o, 2);
			memcpy(&flags, e->d + o + 2, 2);
			memcpy(&len, e->d + o + 4, 2);
			nb = len < 16 ? len : 16;
			if (o + 6 + nb > e->len) nb = (unsigned)(e->len - o - 6);
			hex_bytes(hex, sizeof hex, e->d + o + 6, nb);
			lo += (size_t)snprintf(line + lo, sizeof line - lo, " [0x%02x %s len=%u %s]", addr,
					       (flags & 1) ? "rd" : "wr", len, hex);
			o += 6 + nb;
		}
		snprintf(line + lo, sizeof line - lo, "\n");
		break;
	}
	case EV_I2C_SMBUS:
		hex_bytes(hex, sizeof hex, e->d, e->len < 4 ? e->len : 4);
		snprintf(line, sizeof line, "%9.3f i2c fd=%u smbus %s cmd=0x%02x size=%u data=%s -> %d tid=%d\n", t,
			 e->a, (e->b >> 8) ? "read" : "write", e->b & 0xff, e->e, hex, e->c, e->tid);
		break;
	case EV_I2C_IOCTL:
		snprintf(line, sizeof line, "%9.3f i2c fd=%u ioctl 0x%x arg=0x%x -> %d\n", t, e->a, e->b, e->e, e->c);
		break;
	case EV_SPI_TX: {
		unsigned key = (((e->a >> 8) & 3u) << 2) | (e->a & 3u);
		if (!log_tx_seen[key]) {
			hex_bytes(hex, sizeof hex, e->d, e->len);
			snprintf(line, sizeof line, "%9.3f panel tx m%u p%u/%u len=%u full %s\n", t, e->a >> 8,
				 e->a & 0xff, e->b, e->len, hex);
			log_tx_seen[key] = 1;
		} else {
			frame_diff(diff, sizeof diff, log_tx_prev[key], e->d, e->len);
			snprintf(line, sizeof line, "%9.3f panel tx m%u p%u %s\n", t, e->a >> 8, e->a & 0xff, diff);
		}
		memcpy(log_tx_prev[key], e->d, e->len);
		break;
	}
	case EV_SPI_RX:
		if (!log_rx_seen) {
			hex_bytes(hex, sizeof hex, e->d, e->len);
			snprintf(line, sizeof line, "%9.3f panel rx len=%u full %s\n", t, e->len, hex);
			log_rx_seen = 1;
		} else {
			frame_diff(diff, sizeof diff, log_rx_prev, e->d, e->len);
			snprintf(line, sizeof line, "%9.3f panel rx %s\n", t, diff);
		}
		memcpy(log_rx_prev, e->d, e->len);
		break;
	case EV_SPI_WRITE:
		hex_bytes(hex, sizeof hex, e->d, e->len);
		snprintf(line, sizeof line, "%9.3f panel write %s -> %d\n", t, hex, e->c);
		break;
	case EV_SPI_IOCTL:
		snprintf(line, sizeof line, "%9.3f panel ioctl 0x%08x val=%u -> %d tid=%d\n", t, e->b, e->e, e->c,
			 e->tid);
		break;
	case EV_HID_IN:
	case EV_HID_OUT:
		hex_bytes(hex, sizeof hex, e->d, e->len);
		snprintf(line, sizeof line, "%9.3f hid %s fd=%u len=%u %s\n", t, e->type == EV_HID_IN ? "in" : "out",
			 e->a, e->len, hex);
		break;
	default:
		snprintf(line, sizeof line, "%9.3f event %u\n", t, e->type);
		break;
	}
	out_append(line);
}

/* ---- Log and safe start ----
 * The log (start-up, settings changes, errors) is written only while the xdjmod-capture folder is on a stick; the
 * safe start always runs.
 * Safe start: apl_start writes the marker XDJMOD_STARTING before starting rbp with the library and starts rbp
 * without it if the marker is still there. The library removes it once rbp has run 60 s with the master
 * running. So a start that hangs, or one switched off within the first minute, makes the next start pure
 * Pioneer (and that one clears the marker again). */
static char starting_path[128];

/* The log thread (lowest priority, core 0): once a second the notes go to the capture folder (kept in memory until
 * there is one, up to OUT_BUF); every 5 s it looks for the folder; and it runs the safe start (see starting_path):
 * healthy if the master wrote over 100 periods in the 2 s after minute 1. */
static void *gig_log_thread(void *unused)
{
	cpu_set_t cpus;
	unsigned tick = 0;
	int safe = starting_path[0] ? 0 : 2;	/* 0 before minute 1, 1 counting, 2 done */
	uint32_t mp = 0;
	uint64_t t_mp = 0;
	struct ev e;
	(void)unused;
	prctl(PR_SET_NAME, "xdjmod_log", 0, 0, 0);
	setpriority(PRIO_PROCESS, (id_t)gettid_(), 19);
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	sched_setaffinity(0, sizeof cpus, &cpus);	/* never on the audio core */
	boot_note("log thread running");
	for (;;) {
		struct timespec ts = { 1, 0 };
		uint64_t t;
		nanosleep(&ts, NULL);
		t = now_us();
		while (ev_take(&ring, &e)) {
			format_event(&e);
			if (outlen > OUT_BUF - 4096) out_flush();
		}
		out_flush();
		if (++tick % 5 == 0) {
			if (scan_name[0] && !__atomic_load_n(&out_found, __ATOMIC_ACQUIRE) && find_out_dir())
				note("log: folder %s", out_dir);
		}
		if (safe == 0 && t - t0_us >= 60000000u) {
			mp = master_periods;
			t_mp = t;
			safe = 1;
		} else if (safe == 1 && t - t_mp >= 2000000u) {
			safe = 2;
			if (master_periods - mp > 100) {
				int r = unlink(starting_path);
				sync();
				note("safe start: start was healthy, marker %s (%d)", r == 0 ? "cleared" : "not cleared", errno);
			} else {
				note("safe start: master not running after 60 s, marker kept: next start without library");
			}
		}
	}
	return NULL;
}

/* ---------- start ---------- */

__attribute__((constructor)) static void xdjmod_init(void)
{
	char exe[64];
	ssize_t n;
	const char *s;
	pthread_attr_t attr;
	pthread_t th;

	resolve_libc();
	resolve_alsa();

	n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n <= 0) return;
	exe[n] = 0;
	if (strcmp(exe, RBP_PATH) != 0)
		return;	/* some other program: stay a pass-through */
	boot_note("constructor in %s pid=%d, v%s", exe, (int)getpid(), XDJMOD_VERSION);
	if (getenv("XDJMOD_PASSIVE")) {	/* probe: interpose only, do nothing else */
		unsetenv("LD_PRELOAD");
		unsetenv("XDJMOD_PASSIVE");
		boot_note("passive mode: pure pass-through");
		return;
	}

	if ((s = getenv("XDJMOD_OUT"))) {
		if (!strncmp(s, "scan:", 5))
			snprintf(scan_name, sizeof scan_name, "%s", s + 5);
		else {
			snprintf(out_dir, sizeof out_dir, "%s", s);
			out_found = 1;
		}
	}
	if ((s = getenv("XDJMOD_SWITCH"))) snprintf(mode_path, sizeof mode_path, "%s", s);
	if ((s = getenv("XDJMOD_STARTING"))) snprintf(starting_path, sizeof starting_path, "%s", s);
	log_frames = getenv("XDJMOD_FRAMES") != NULL;
	/* programs that rbp starts must not load us */
	unsetenv("LD_PRELOAD");
	unsetenv("XDJMOD_OUT");
	unsetenv("XDJMOD_STATE");
	unsetenv("XDJMOD_SWITCH");
	unsetenv("XDJMOD_STARTING");
	unsetenv("XDJMOD_FRAMES");

	if (!real_open || !real_close || !real_read || !real_write || !real_ioctl || !real_pcm_writei ||
	    !real_pcm_readi || !real_pcm_open || !real_pcm_close || !real_pcm_hw_params || !real_pcm_prepare)
	{
		boot_note("a symbol is missing (open=%p pcm_open=%p writei=%p): pass-through", (void *)real_open,
			  (void *)real_pcm_open, (void *)real_pcm_writei);
		return;	/* a symbol is missing: stay a pass-through */
	}

	t0_us = now_us();
	ev_ring_init(&ring);
	active = 1;
	note("libxdjmod %s active in rbp pid=%d", XDJMOD_VERSION, (int)getpid());

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th, &attr, gig_log_thread, NULL) != 0) {
		active = 0;	/* nobody would drain the ring; stay out of the way */
		boot_note("pthread_create failed: inactive");
	}
	if (active && mode_path[0]) {
		pthread_mutexattr_t ma;
		pthread_mutexattr_init(&ma);
		pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);	/* rbp's panel thread may be real-time */
		pthread_mutex_init(&led_lock, &ma);
		pthread_mutexattr_destroy(&ma);
		mode_load();
		if (measure_next) {	/* never: measure_next is always 0 */
			measure_only = 1;
			measure_next = 0;
			mode_save();
			boot_note("measure only: frame meter, nothing else");
		}
		if (!measure_only) meter_install();
		gui_install();
		if (!measure_only) pos_install();
		if (!measure_only) wave_install();
		if (!measure_only) bj_install();
		if (!measure_only) menu_install();
		mem_fd = real_open("/proc/self/mem", O_RDWR);
		mem_writable = mem_fd >= 0;
		if (mem_fd < 0) mem_fd = real_open("/proc/self/mem", O_RDONLY);
		if (!measure_only && pthread_create(&th, &attr, ui_thread, NULL) == 0)
			sw_on = 1;
		if (wave_ok && pthread_create(&th, &attr, wave_thread, NULL) != 0)
			note("wave: thread failed (Pioneer's colours only)");
		note("waveform: sequence %d, overview %d, %s", wave_ok, ov_ok, WAVE_NAME(wave_mode));
		if (gui_ok && !measure_only) {	/* SCREEN SYNC, always on (SYNC_GIG) */
			pthread_attr_t sa;
			pthread_attr_init(&sa);
			pthread_attr_setstacksize(&sa, 64 * 1024);
			pthread_attr_setdetachstate(&sa, PTHREAD_CREATE_DETACHED);
			if (pthread_create(&th, &sa, sync_thread, NULL) == 0)
				__atomic_store_n(&sync_started, 1, __ATOMIC_RELEASE);
			else
				note("sync: thread failed, Pioneer's refresh wait");
			pthread_attr_destroy(&sa);
		}
		note("switch: %s (mode file %s, mode %s, split %s, meter %s, mem %d)", sw_on ? "on" : "thread failed",
		     mode_path, xdj_cue_name(hp_mode), xdj_cue_name(split_pref), meter_pre ? "pre" : "post", mem_fd >= 0);
		note("patches: meter %d, display %d, scroll %d, menu %d; master mono %d, booth mono %d, %s fps", meter_ok,
		     gui_ok, pos_ok, menu_ok, master_mono, booth_mono, xdj_fps_name[fps_sel]);
	}
	pthread_attr_destroy(&attr);
	boot_note("constructor done, active=%d switch=%d", active, sw_on);
}
