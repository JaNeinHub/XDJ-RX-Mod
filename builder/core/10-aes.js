// XDJRX-MOD web builder: AES-256 (FIPS-197) and the XDJ-RX's sector-wise CBC (Linux cryptoloop):
// each 512-byte sector on its own, IV = sector index as LE32 + 12 zero bytes.
// Own code instead of WebCrypto: WebCrypto's AES-CBC always adds/removes padding, which raw sectors don't have.
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.aes = (function () {
	'use strict';

	const S = new Uint8Array(256), Si = new Uint8Array(256);
	const Te = [new Uint32Array(256), new Uint32Array(256), new Uint32Array(256), new Uint32Array(256)];
	const Td = [new Uint32Array(256), new Uint32Array(256), new Uint32Array(256), new Uint32Array(256)];

	(function tables() {
		const exp = new Uint8Array(512), log = new Uint8Array(256);
		const xtime = (a) => ((a << 1) ^ (a & 0x80 ? 0x11b : 0)) & 0xff;
		let p = 1;
		for (let i = 0; i < 255; i++) { exp[i] = p; log[p] = i; p ^= xtime(p); }	// generator 3
		for (let i = 255; i < 512; i++) exp[i] = exp[i - 255];
		const mul = (a, b) => (a && b ? exp[log[a] + log[b]] : 0);
		const rotl8 = (x, n) => ((x << n) | (x >>> (8 - n))) & 0xff;
		for (let i = 0; i < 256; i++) {
			const inv = i ? exp[255 - log[i]] : 0;
			const s = inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63;
			S[i] = s;
			Si[s] = i;
		}
		for (let i = 0; i < 256; i++) {
			const s = S[i], si = Si[i];
			const te = ((mul(s, 2) << 24) | (s << 16) | (s << 8) | mul(s, 3)) >>> 0;
			const td = ((mul(si, 14) << 24) | (mul(si, 9) << 16) | (mul(si, 13) << 8) | mul(si, 11)) >>> 0;
			for (let r = 0; r < 4; r++) {
				Te[r][i] = ((te >>> (8 * r)) | (te << (32 - 8 * r))) >>> 0;
				Td[r][i] = ((td >>> (8 * r)) | (td << (32 - 8 * r))) >>> 0;
			}
		}
	})();

	const [Te0, Te1, Te2, Te3] = Te, [Td0, Td1, Td2, Td3] = Td;
	const NR = 14;

	// key: 32 bytes -> { ek, dk } round keys (60 words each)
	function expandKey(key) {
		if (key.length !== 32) throw new Error('AES-256 needs a 32-byte key');
		const w = new Uint32Array(60);
		for (let i = 0; i < 8; i++) w[i] = (key[4 * i] << 24 | key[4 * i + 1] << 16 | key[4 * i + 2] << 8 | key[4 * i + 3]) >>> 0;
		let rcon = 1;
		for (let i = 8; i < 60; i++) {
			let t = w[i - 1];
			if (i % 8 === 0) {
				t = ((t << 8) | (t >>> 24)) >>> 0;
				t = (S[t >>> 24] << 24 | S[(t >>> 16) & 255] << 16 | S[(t >>> 8) & 255] << 8 | S[t & 255]) ^ (rcon << 24);
				rcon = ((rcon << 1) ^ (rcon & 0x80 ? 0x11b : 0)) & 0xff;
			} else if (i % 8 === 4) {
				t = S[t >>> 24] << 24 | S[(t >>> 16) & 255] << 16 | S[(t >>> 8) & 255] << 8 | S[t & 255];
			}
			w[i] = (w[i - 8] ^ t) >>> 0;
		}
		const dk = new Uint32Array(60);
		for (let r = 0; r <= NR; r++) {
			for (let j = 0; j < 4; j++) {
				const v = w[4 * (NR - r) + j];
				dk[4 * r + j] = r === 0 || r === NR ? v
					: (Td0[S[v >>> 24]] ^ Td1[S[(v >>> 16) & 255]] ^ Td2[S[(v >>> 8) & 255]] ^ Td3[S[v & 255]]) >>> 0;
			}
		}
		return { ek: w, dk };
	}

	// One block in place: words s[0..3] (big-endian columns).
	function encryptWords(k, s) {
		let s0 = s[0] ^ k[0], s1 = s[1] ^ k[1], s2 = s[2] ^ k[2], s3 = s[3] ^ k[3], t0, t1, t2, t3, o = 4;
		for (let r = 1; r < NR; r++, o += 4) {
			t0 = Te0[s0 >>> 24] ^ Te1[(s1 >>> 16) & 255] ^ Te2[(s2 >>> 8) & 255] ^ Te3[s3 & 255] ^ k[o];
			t1 = Te0[s1 >>> 24] ^ Te1[(s2 >>> 16) & 255] ^ Te2[(s3 >>> 8) & 255] ^ Te3[s0 & 255] ^ k[o + 1];
			t2 = Te0[s2 >>> 24] ^ Te1[(s3 >>> 16) & 255] ^ Te2[(s0 >>> 8) & 255] ^ Te3[s1 & 255] ^ k[o + 2];
			t3 = Te0[s3 >>> 24] ^ Te1[(s0 >>> 16) & 255] ^ Te2[(s1 >>> 8) & 255] ^ Te3[s2 & 255] ^ k[o + 3];
			s0 = t0; s1 = t1; s2 = t2; s3 = t3;
		}
		s[0] = (S[s0 >>> 24] << 24 | S[(s1 >>> 16) & 255] << 16 | S[(s2 >>> 8) & 255] << 8 | S[s3 & 255]) ^ k[o];
		s[1] = (S[s1 >>> 24] << 24 | S[(s2 >>> 16) & 255] << 16 | S[(s3 >>> 8) & 255] << 8 | S[s0 & 255]) ^ k[o + 1];
		s[2] = (S[s2 >>> 24] << 24 | S[(s3 >>> 16) & 255] << 16 | S[(s0 >>> 8) & 255] << 8 | S[s1 & 255]) ^ k[o + 2];
		s[3] = (S[s3 >>> 24] << 24 | S[(s0 >>> 16) & 255] << 16 | S[(s1 >>> 8) & 255] << 8 | S[s2 & 255]) ^ k[o + 3];
	}

	function decryptWords(k, s) {
		let s0 = s[0] ^ k[0], s1 = s[1] ^ k[1], s2 = s[2] ^ k[2], s3 = s[3] ^ k[3], t0, t1, t2, t3, o = 4;
		for (let r = 1; r < NR; r++, o += 4) {
			t0 = Td0[s0 >>> 24] ^ Td1[(s3 >>> 16) & 255] ^ Td2[(s2 >>> 8) & 255] ^ Td3[s1 & 255] ^ k[o];
			t1 = Td0[s1 >>> 24] ^ Td1[(s0 >>> 16) & 255] ^ Td2[(s3 >>> 8) & 255] ^ Td3[s2 & 255] ^ k[o + 1];
			t2 = Td0[s2 >>> 24] ^ Td1[(s1 >>> 16) & 255] ^ Td2[(s0 >>> 8) & 255] ^ Td3[s3 & 255] ^ k[o + 2];
			t3 = Td0[s3 >>> 24] ^ Td1[(s2 >>> 16) & 255] ^ Td2[(s1 >>> 8) & 255] ^ Td3[s0 & 255] ^ k[o + 3];
			s0 = t0; s1 = t1; s2 = t2; s3 = t3;
		}
		s[0] = (Si[s0 >>> 24] << 24 | Si[(s3 >>> 16) & 255] << 16 | Si[(s2 >>> 8) & 255] << 8 | Si[s1 & 255]) ^ k[o];
		s[1] = (Si[s1 >>> 24] << 24 | Si[(s0 >>> 16) & 255] << 16 | Si[(s3 >>> 8) & 255] << 8 | Si[s2 & 255]) ^ k[o + 1];
		s[2] = (Si[s2 >>> 24] << 24 | Si[(s1 >>> 16) & 255] << 16 | Si[(s0 >>> 8) & 255] << 8 | Si[s3 & 255]) ^ k[o + 2];
		s[3] = (Si[s3 >>> 24] << 24 | Si[(s2 >>> 16) & 255] << 16 | Si[(s1 >>> 8) & 255] << 8 | Si[s0 & 255]) ^ k[o + 3];
	}

	function load(b, i, s) {
		for (let j = 0; j < 4; j++, i += 4) s[j] = (b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3]) >>> 0;
	}

	function store(s, b, i) {
		for (let j = 0; j < 4; j++, i += 4) {
			const v = s[j];
			b[i] = v >>> 24; b[i + 1] = (v >>> 16) & 255; b[i + 2] = (v >>> 8) & 255; b[i + 3] = v & 255;
		}
	}

	// One block (16 bytes) with ECB, for test vectors.
	function encryptBlock(key, block) {
		const k = expandKey(key), s = new Uint32Array(4), out = new Uint8Array(16);
		load(block, 0, s); encryptWords(k.ek, s); store(s, out, 0);
		return out;
	}

	const SECTOR = 512;

	function ivWords(sector) {	// LE32(sector) + 12 zero bytes, as big-endian words
		const b0 = sector & 255, b1 = (sector >>> 8) & 255, b2 = (sector >>> 16) & 255, b3 = (sector >>> 24) & 255;
		return [(b0 << 24 | b1 << 16 | b2 << 8 | b3) >>> 0, 0, 0, 0];
	}

	// Encrypts/decrypts whole sectors of `src` into `dst` (may be the same array); `first` = sector index of src[0].
	function cryptSectors(keys, src, dst, first, decrypt) {
		if (src.length % SECTOR) throw new Error('data is not a whole number of 512-byte sectors');
		const s = new Uint32Array(4), c = new Uint32Array(4);
		for (let off = 0, n = first; off < src.length; off += SECTOR, n++) {
			let prev = ivWords(n);
			for (let i = off; i < off + SECTOR; i += 16) {
				load(src, i, s);
				if (decrypt) {
					c[0] = s[0]; c[1] = s[1]; c[2] = s[2]; c[3] = s[3];
					decryptWords(keys.dk, s);
					s[0] ^= prev[0]; s[1] ^= prev[1]; s[2] ^= prev[2]; s[3] ^= prev[3];
					prev = [c[0], c[1], c[2], c[3]];
				} else {
					s[0] ^= prev[0]; s[1] ^= prev[1]; s[2] ^= prev[2]; s[3] ^= prev[3];
					encryptWords(keys.ek, s);
					prev = [s[0], s[1], s[2], s[3]];
				}
				store(s, dst, i);
			}
		}
		return dst;
	}

	return {
		SECTOR,
		expandKey,
		encryptBlock,
		encryptSectors: (keys, data, first) => cryptSectors(keys, data, new Uint8Array(data.length), first || 0, false),
		decryptSectors: (keys, data, first) => cryptSectors(keys, data, new Uint8Array(data.length), first || 0, true),
	};
})();
