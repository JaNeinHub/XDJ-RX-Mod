// XDJRX-MOD web builder: raw inflate for zip methods 8 (Deflate) and 9 (Deflate64).
// Pioneer's GPL zips use Deflate64, which neither browsers nor pako can read. Deflate64 is Deflate with
// a 64 KB window, length code 285 = base 3 + 16 extra bits, and distance codes 30/31 (14 extra bits).
// Written from RFC 1951 and the Deflate64 differences. `maxOut` stops early (we only need the start of
// Pioneer's package); input that ends mid-stream is fine then, it just returns what was decoded.
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.inflate = (function () {
	'use strict';

	const LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
		131, 163, 195, 227, 258];
	const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
	const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
		2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153];
	const DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
		14, 14];
	const CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

	class EndOfInput extends Error {}

	// Canonical Huffman table with a 9-bit first-level lookup; longer codes fall back to a bit-by-bit walk.
	const FAST = 9;
	function huffman(lengths) {
		const count = new Uint16Array(16), offs = new Uint16Array(16), n = lengths.length;
		for (let i = 0; i < n; i++) count[lengths[i]]++;
		count[0] = 0;
		let left = 1;
		for (let len = 1; len < 16; len++) {
			left = (left << 1) - count[len];
			if (left < 0) throw new Error('inflate: over-subscribed code');
		}
		for (let len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];
		const symbol = new Uint16Array(n);
		for (let i = 0; i < n; i++) if (lengths[i]) symbol[offs[lengths[i]]++] = i;
		// fast table: entry = symbol << 4 | length (0 = not in fast table)
		const fast = new Int32Array(1 << FAST);
		const next = new Uint16Array(16);	// canonical first code per length (RFC 1951 3.2.2)
		let code = 0;
		for (let len = 1; len < 16; len++) { code = (code + count[len - 1]) << 1; next[len] = code; }
		for (let i = 0; i < n; i++) {
			const len = lengths[i];
			if (!len) continue;
			let c = next[len]++;
			if (len > FAST) continue;
			let rev = 0;	// deflate codes are read LSB first: reverse the code bits
			for (let b = 0; b < len; b++) { rev = (rev << 1) | (c & 1); c >>= 1; }
			for (let j = rev; j < (1 << FAST); j += 1 << len) fast[j] = (i << 4) | len;
		}
		return { count, symbol, fast };
	}

	function inflateRaw(input, opts) {
		const deflate64 = !!(opts && opts.deflate64);
		const maxOut = opts && opts.maxOut ? opts.maxOut : Infinity;
		let out = new Uint8Array(Math.min(maxOut === Infinity ? Math.max(input.length * 3, 1 << 16) : maxOut + 66000, 1 << 30));
		let op = 0, ip = 0, bitbuf = 0, bitcnt = 0;

		function need(n) {
			while (bitcnt < n) {
				if (ip >= input.length) throw new EndOfInput();
				bitbuf |= input[ip++] << bitcnt;
				bitcnt += 8;
			}
		}
		function bits(n) {
			if (n === 0) return 0;
			need(n);
			const v = bitbuf & ((1 << n) - 1);
			bitbuf >>>= n;
			bitcnt -= n;
			return v;
		}
		function grow(extra) {
			if (op + extra <= out.length) return;
			let size = out.length * 2;
			while (size < op + extra) size *= 2;
			const n = new Uint8Array(size);
			n.set(out.subarray(0, op));
			out = n;
		}
		function decode(h) {
			// try the fast table with whatever bits are available
			while (bitcnt < FAST && ip < input.length) { bitbuf |= input[ip++] << bitcnt; bitcnt += 8; }
			const e = h.fast[bitbuf & ((1 << FAST) - 1)];
			if (e && (e & 15) <= bitcnt) {
				const len = e & 15;
				bitbuf >>>= len; bitcnt -= len;
				return e >>> 4;
			}
			// slow path (puff style)
			let code = 0, first = 0, index = 0;
			for (let len = 1; len < 16; len++) {
				code |= bits(1);
				const count = h.count[len];
				if (code - count < first) return h.symbol[index + (code - first)];
				index += count;
				first = (first + count) << 1;
				code <<= 1;
			}
			throw new Error('inflate: bad code');
		}

		let fixedLit = null, fixedDist = null;
		let truncated = false;
		try {
			let last = 0;
			while (!last && op < maxOut) {
				last = bits(1);
				const type = bits(2);
				if (type === 0) {
					bitbuf = 0; bitcnt = 0;	// to the byte boundary
					if (ip + 4 > input.length) throw new EndOfInput();
					const len = input[ip] | input[ip + 1] << 8, nlen = input[ip + 2] | input[ip + 3] << 8;
					ip += 4;
					if ((len ^ 0xffff) !== nlen) throw new Error('inflate: stored block length check');
					const avail = Math.min(len, input.length - ip);
					grow(len);
					out.set(input.subarray(ip, ip + avail), op);
					op += avail; ip += avail;
					if (avail < len) throw new EndOfInput();
					continue;
				}
				let lit, dist;
				if (type === 1) {
					if (!fixedLit) {
						const l = new Uint8Array(288);
						for (let i = 0; i < 144; i++) l[i] = 8;
						for (let i = 144; i < 256; i++) l[i] = 9;
						for (let i = 256; i < 280; i++) l[i] = 7;
						for (let i = 280; i < 288; i++) l[i] = 8;
						fixedLit = huffman(l);
						fixedDist = huffman(new Uint8Array(32).fill(5));
					}
					lit = fixedLit; dist = fixedDist;
				} else if (type === 2) {
					const nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
					const cl = new Uint8Array(19);
					for (let i = 0; i < ncode; i++) cl[CL_ORDER[i]] = bits(3);
					const clh = huffman(cl);
					const lengths = new Uint8Array(nlen + ndist);
					for (let i = 0; i < nlen + ndist;) {
						const sym = decode(clh);
						if (sym < 16) { lengths[i++] = sym; continue; }
						let rep, val = 0;
						if (sym === 16) {
							if (i === 0) throw new Error('inflate: repeat without length');
							val = lengths[i - 1]; rep = 3 + bits(2);
						} else if (sym === 17) rep = 3 + bits(3);
						else rep = 11 + bits(7);
						if (i + rep > nlen + ndist) throw new Error('inflate: too many lengths');
						while (rep--) lengths[i++] = val;
					}
					lit = huffman(lengths.subarray(0, nlen));
					dist = huffman(lengths.subarray(nlen));
				} else throw new Error('inflate: bad block type');

				for (;;) {
					let sym = decode(lit);
					if (sym < 256) {
						if (op >= out.length) grow(1);
						out[op++] = sym;
						continue;
					}
					if (sym === 256) break;
					sym -= 257;
					if (sym >= 29) throw new Error('inflate: bad length code');
					let len = deflate64 && sym === 28 ? 3 + bits(16) : LEN_BASE[sym] + bits(LEN_EXTRA[sym]);
					const ds = decode(dist);
					if (ds >= (deflate64 ? 32 : 30)) throw new Error('inflate: bad distance code');
					const d = DIST_BASE[ds] + bits(DIST_EXTRA[ds]);
					if (d > op) throw new Error('inflate: distance too far back');
					grow(len);
					let from = op - d;
					while (len--) out[op++] = out[from++];
					if (op >= maxOut) break;
				}
			}
		} catch (e) {
			if (!(e instanceof EndOfInput)) throw e;
			truncated = true;
		}
		return { data: out.subarray(0, op), truncated, consumed: ip };
	}

	return { inflateRaw };
})();
