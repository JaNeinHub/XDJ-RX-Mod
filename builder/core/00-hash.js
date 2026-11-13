// XDJRX-MOD web builder: checksums and Base64 (CRC32, MD5, SHA-256).
// Plain JavaScript without browser APIs, so the same code runs in the page and in macOS's `jsc` shell
// (tests). All functions take and return Uint8Array / strings.
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.hash = (function () {
	'use strict';

	const CRC_TABLE = new Int32Array(256);
	for (let n = 0; n < 256; n++) {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
		CRC_TABLE[n] = c;
	}

	// crc32(bytes[, previous crc]) -> unsigned 32-bit number (same as zlib.crc32)
	function crc32(buf, prev) {
		let c = (prev === undefined ? 0 : prev) ^ -1;
		for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 255] ^ (c >>> 8);
		return (c ^ -1) >>> 0;
	}

	function hex(words, littleEndian) {
		let s = '';
		for (const w of words) {
			for (let i = 0; i < 4; i++) {
				const b = littleEndian ? (w >>> (8 * i)) & 255 : (w >>> (24 - 8 * i)) & 255;
				s += (b < 16 ? '0' : '') + b.toString(16);
			}
		}
		return s;
	}

	// Pads a message the MD5/SHA-256 way; returns a Uint8Array whose length is a multiple of 64.
	function pad(buf, littleEndianLength) {
		const n = buf.length, total = (((n + 8) >> 6) + 1) << 6;
		const out = new Uint8Array(total);
		out.set(buf);
		out[n] = 0x80;
		const bitsLo = (n * 8) >>> 0, bitsHi = Math.floor(n / 0x20000000) >>> 0;
		const dv = new DataView(out.buffer);
		if (littleEndianLength) {
			dv.setUint32(total - 8, bitsLo, true);
			dv.setUint32(total - 4, bitsHi, true);
		} else {
			dv.setUint32(total - 8, bitsHi, false);
			dv.setUint32(total - 4, bitsLo, false);
		}
		return out;
	}

	const MD5_S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
	const MD5_K = new Int32Array(64);
	for (let i = 0; i < 64; i++) MD5_K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000) | 0;

	function md5(buf) {
		const m = pad(buf, true), dv = new DataView(m.buffer), x = new Int32Array(16);
		let a0 = 0x67452301, b0 = 0xefcdab89 | 0, c0 = 0x98badcfe | 0, d0 = 0x10325476;
		for (let off = 0; off < m.length; off += 64) {
			for (let i = 0; i < 16; i++) x[i] = dv.getInt32(off + 4 * i, true);
			let a = a0, b = b0, c = c0, d = d0;
			for (let i = 0; i < 64; i++) {
				let f, g;
				if (i < 16) { f = (b & c) | (~b & d); g = i; }
				else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
				else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
				else { f = c ^ (b | ~d); g = (7 * i) & 15; }
				const t = (a + f + MD5_K[i] + x[g]) | 0;
				a = d; d = c; c = b;
				b = (b + ((t << MD5_S[i]) | (t >>> (32 - MD5_S[i])))) | 0;
			}
			a0 = (a0 + a) | 0; b0 = (b0 + b) | 0; c0 = (c0 + c) | 0; d0 = (d0 + d) | 0;
		}
		return hex([a0, b0, c0, d0], true);
	}

	const SHA_K = new Int32Array([
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);

	function sha256(buf) {
		const m = pad(buf, false), dv = new DataView(m.buffer), w = new Int32Array(64);
		const h = new Int32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
		for (let off = 0; off < m.length; off += 64) {
			for (let i = 0; i < 16; i++) w[i] = dv.getInt32(off + 4 * i, false);
			for (let i = 16; i < 64; i++) {
				const x = w[i - 15], y = w[i - 2];
				const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
				const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
				w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
			}
			let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
			for (let i = 0; i < 64; i++) {
				const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
				const t1 = (k + S1 + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i]) | 0;
				const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
				const t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
				k = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
			}
			h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
			h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + k) | 0;
		}
		return hex(h, false);
	}

	const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
	const B64_REV = new Int16Array(128).fill(-1);
	for (let i = 0; i < 64; i++) B64_REV[B64.charCodeAt(i)] = i;

	function fromBase64(s) {
		s = s.replace(/[^A-Za-z0-9+/]/g, '');
		const out = new Uint8Array(Math.floor(s.length * 3 / 4));
		let bits = 0, n = 0, j = 0;
		for (let i = 0; i < s.length; i++) {
			bits = (bits << 6) | B64_REV[s.charCodeAt(i)];
			n += 6;
			if (n >= 8) { n -= 8; out[j++] = (bits >>> n) & 255; }
		}
		return out.subarray(0, j);
	}

	function ascii(s) {
		const out = new Uint8Array(s.length);
		for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 255;
		return out;
	}

	function text(bytes) {
		let s = '';
		for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
		return s;
	}

	return { crc32, md5, sha256, fromBase64, ascii, text };
})();
