// XDJRX-MOD web builder: the zip parts we need.
//   entry(bytes)   the first local file header of a zip (Pioneer's downloads each hold one file)
//   store(files)   a zip without compression, for "download everything"
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.zip = (function () {
	'use strict';

	function entry(buf) {
		if (buf.length < 30) throw new Error('not a zip file');
		const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
		if (dv.getUint32(0, true) !== 0x04034b50) throw new Error('not a zip file (no local header)');
		const flags = dv.getUint16(6, true), method = dv.getUint16(8, true);
		const nlen = dv.getUint16(26, true), xlen = dv.getUint16(28, true);
		return {
			flags, method,
			crc: dv.getUint32(14, true),
			csize: dv.getUint32(18, true),
			usize: dv.getUint32(22, true),
			name: XRM.hash.text(buf.subarray(30, 30 + nlen)),
			start: 30 + nlen + xlen,
		};
	}

	// files: [{ name, data: Uint8Array }]; fixed date 2026-11-13 00:00 so the zip is the same every time
	function store(files) {
		const DOS_TIME = 0, DOS_DATE = ((2026 - 1980) << 9) | (11 << 5) | 13;
		const enc = (s) => XRM.hash.ascii(s);
		const parts = [], central = [];
		let offset = 0;
		for (const f of files) {
			const name = enc(f.name), crc = XRM.hash.crc32(f.data);
			const h = new Uint8Array(30 + name.length), dv = new DataView(h.buffer);
			dv.setUint32(0, 0x04034b50, true); dv.setUint16(4, 20, true); dv.setUint16(6, 0, true);
			dv.setUint16(8, 0, true); dv.setUint16(10, DOS_TIME, true); dv.setUint16(12, DOS_DATE, true);
			dv.setUint32(14, crc, true); dv.setUint32(18, f.data.length, true); dv.setUint32(22, f.data.length, true);
			dv.setUint16(26, name.length, true); dv.setUint16(28, 0, true);
			h.set(name, 30);
			const c = new Uint8Array(46 + name.length), cv = new DataView(c.buffer);
			cv.setUint32(0, 0x02014b50, true); cv.setUint16(4, 20, true); cv.setUint16(6, 20, true);
			cv.setUint16(8, 0, true); cv.setUint16(10, 0, true); cv.setUint16(12, DOS_TIME, true);
			cv.setUint16(14, DOS_DATE, true); cv.setUint32(16, crc, true); cv.setUint32(20, f.data.length, true);
			cv.setUint32(24, f.data.length, true); cv.setUint16(28, name.length, true);
			cv.setUint32(38, 0, true); cv.setUint32(42, offset, true);
			c.set(name, 46);
			parts.push(h, f.data);
			central.push(c);
			offset += h.length + f.data.length;
		}
		const cdSize = central.reduce((n, c) => n + c.length, 0);
		const end = new Uint8Array(22), ev = new DataView(end.buffer);
		ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, files.length, true); ev.setUint16(10, files.length, true);
		ev.setUint32(12, cdSize, true); ev.setUint32(16, offset, true);
		const all = parts.concat(central, [end]);
		const out = new Uint8Array(all.reduce((n, p) => n + p.length, 0));
		let o = 0;
		for (const p of all) { out.set(p, o); o += p.length; }
		return out;
	}

	return { entry, store };
})();
