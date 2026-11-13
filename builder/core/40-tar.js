// XDJRX-MOD web builder: tar reading and the mod's archive edit.
// buildMod() reproduces byte for byte what the mod's original Python tool (Python's tarfile) made from
// Pioneer's 2.21 app archive: every header re-written as POSIX ustar (magic "ustar\0" "00", checksum
// "%06o\0 "), apl_start with one line replaced, pioneer/libxdjmod.so inserted after pioneer/rbp,
// two zero blocks and padding to a 10240-byte record at the end. The page checks the result against
// the md5 of the hardware-tested archive, so it can only ever produce that one archive.
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.tar = (function () {
	'use strict';

	const BLOCK = 512, RECORD = 10240;
	const text = (b) => XRM.hash.text(b);

	function octal(field) {
		const s = text(field).replace(/[\0 ]+$/, '').replace(/^[\0 ]+/, '');
		return s ? parseInt(s, 8) : 0;
	}

	function name(h) {
		const n = text(h.subarray(0, 100)).replace(/\0.*$/, '');
		const magic = text(h.subarray(257, 263));
		const prefix = magic.startsWith('ustar') ? text(h.subarray(345, 500)).replace(/\0.*$/, '') : '';
		return prefix ? prefix + '/' + n : n;
	}

	// Members in order: { name, type, size, hdr (512-byte view), data (view) }. Stops at the first zero block.
	function parse(tar) {
		const out = [];
		let off = 0;
		while (off + BLOCK <= tar.length) {
			const h = tar.subarray(off, off + BLOCK);
			if (h.every((b) => b === 0)) break;
			const size = octal(h.subarray(124, 136));
			const m = { name: name(h), type: String.fromCharCode(h[156] || 48), size, hdr: h, offset: off };
			if (m.type === 'L' || m.type === 'K' || m.type === 'x' || m.type === 'g')
				throw new Error('tar: unexpected long-name/pax member ' + m.type);
			m.data = tar.subarray(off + BLOCK, off + BLOCK + size);
			if (m.data.length !== size) throw new Error('tar: archive ends inside ' + m.name);
			out.push(m);
			off += BLOCK + Math.ceil(size / BLOCK) * BLOCK;
		}
		return out;
	}

	// Streaming search: is a complete member called `want` inside `tar` (which may be cut off)?
	// Returns { data } when found, { more: true } when the archive ends before it, throws on garbage.
	function find(tar, want) {
		let off = 0;
		while (off + BLOCK <= tar.length) {
			const h = tar.subarray(off, off + BLOCK);
			if (h.every((b) => b === 0)) return { end: true };
			let sum = 0;
			for (let i = 0; i < BLOCK; i++) sum += i >= 148 && i < 156 ? 32 : h[i];
			if (sum !== octal(h.subarray(148, 156))) throw new Error('tar: bad header checksum');
			const size = octal(h.subarray(124, 136));
			if (name(h) === want) {
				if (off + BLOCK + size > tar.length) return { more: true };
				return { data: tar.slice(off + BLOCK, off + BLOCK + size) };
			}
			off += BLOCK + Math.ceil(size / BLOCK) * BLOCK;
		}
		return { more: true };
	}

	function putOctal(h, at, width, value) {	// Python tarfile's itn(): width-1 octal digits + NUL
		const s = value.toString(8).padStart(width - 1, '0');
		if (s.length > width - 1) throw new Error('tar: value too large');
		for (let i = 0; i < width - 1; i++) h[at + i] = s.charCodeAt(i);
		h[at + width - 1] = 0;
	}

	function checksum(h) {	// "%06o\0 " over the header with the checksum field as spaces
		for (let i = 148; i < 156; i++) h[i] = 32;
		let sum = 0;
		for (let i = 0; i < BLOCK; i++) sum += h[i];
		const s = sum.toString(8).padStart(6, '0');
		for (let i = 0; i < 6; i++) h[148 + i] = s.charCodeAt(i);
		h[154] = 0;
		h[155] = 32;
	}

	function ustar(h) {	// a copy of the header as Python's tarfile re-writes it (USTAR format)
		const n = new Uint8Array(h);
		const magic = XRM.hash.ascii('ustar\u000000');
		n.set(magic, 257);
		return n;
	}

	// stockTar: Pioneer's uncompressed 2.21 tar; lib: our library; aplOld/aplNew: Uint8Array lines.
	function buildMod(stockTar, lib, aplOld, aplNew) {
		const members = parse(stockTar);
		const parts = [];
		let total = 0;
		const push = (b) => { parts.push(b); total += b.length; };
		const pushData = (d) => {
			push(d);
			const pad = (BLOCK - (d.length % BLOCK)) % BLOCK;
			if (pad) push(new Uint8Array(pad));
		};
		let sawApl = false, sawRbp = false;
		for (const m of members) {
			let data = m.data, h = ustar(m.hdr);
			if (m.name === 'pioneer/apl_start') {
				const s = text(data), o = text(aplOld);
				const at = s.indexOf(o);
				if (at < 0 || s.indexOf(o, at + 1) >= 0) throw new Error('apl_start: start line not found exactly once');
				const nd = new Uint8Array(data.length - aplOld.length + aplNew.length);
				nd.set(data.subarray(0, at));
				nd.set(aplNew, at);
				nd.set(data.subarray(at + aplOld.length), at + aplNew.length);
				data = nd;
				putOctal(h, 124, 12, data.length);
				sawApl = true;
			}
			checksum(h);
			push(h);
			pushData(data);
			if (m.name === 'pioneer/rbp') {
				// TarInfo('pioneer/libxdjmod.so'): mode 0755, uid/gid/uname/gname/mtime from rbp, type '0'
				const l = new Uint8Array(BLOCK);
				l.set(XRM.hash.ascii('pioneer/libxdjmod.so'), 0);
				putOctal(l, 100, 8, 0o755);
				l.set(h.subarray(108, 124), 108);	// uid, gid
				putOctal(l, 124, 12, lib.length);
				l.set(h.subarray(136, 148), 136);	// mtime
				l[156] = 48;	// '0'
				l.set(XRM.hash.ascii('ustar\u000000'), 257);
				l.set(h.subarray(265, 329), 265);	// uname, gname
				// devmajor/devminor stay NUL: tarfile writes them only for device files
				checksum(l);
				push(l);
				pushData(lib);
				sawRbp = true;
			}
		}
		if (!sawApl || !sawRbp) throw new Error('tar: apl_start or rbp missing');
		push(new Uint8Array(2 * BLOCK));
		const rem = total % RECORD;
		if (rem) push(new Uint8Array(RECORD - rem));
		const out = new Uint8Array(total);
		let o = 0;
		for (const p of parts) { out.set(p, o); o += p.length; }
		return out;
	}

	function file(tar, wanted) {
		for (const m of parse(tar)) if (m.name === wanted) return m.data;
		throw new Error('tar: ' + wanted + ' not found');
	}

	return { parse, find, file, buildMod };
})();
