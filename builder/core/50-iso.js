// XDJRX-MOD web builder: ISO 9660 images.
//   list(read)          reads an image (Pioneer's update) through read(offset, length) -> Uint8Array;
//                       returns { '/images/pioneer.tar.gz': { extent, size, dir }, ... } using Rock Ridge
//                       names where present.
//   build(files, date)  writes an image with Rock Ridge 1.09 (RRIP_1991A), the structure pycdlib writes for
//                       our update files today (which the RX runs): SP/RR/PX/TF/CE in the root's "." record,
//                       ER in the continuation area, RR/NM/PX/TF on every entry. No Joliet: Linux uses Rock
//                       Ridge whenever it finds the ER entry (fs/isofs/inode.c, kernel 3.0.35).
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.iso = (function () {
	'use strict';

	const SECTOR = 2048;
	const le32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;

	// ---------- reading ----------

	function list(read) {
		const pvd = read(16 * SECTOR, SECTOR);
		if (pvd[0] !== 1 || XRM.hash.text(pvd.subarray(1, 6)) !== 'CD001') throw new Error('not an ISO 9660 image');
		const out = {};
		const walk = (extent, size, path, depth) => {
			if (depth > 16) throw new Error('ISO: directories nested too deep');
			const d = read(extent * SECTOR, size);
			let o = 0;
			while (o < d.length) {
				const len = d[o];
				if (len === 0) { o = (Math.floor(o / SECTOR) + 1) * SECTOR; continue; }
				const r = d.subarray(o, o + len);
				const nl = r[32], flags = r[25];
				const id = r.subarray(33, 33 + nl);
				if (!(nl === 1 && (id[0] === 0 || id[0] === 1))) {
					let name = rrName(r.subarray(33 + nl + ((nl + 1) % 2)));
					if (name === null) name = XRM.hash.text(id).replace(/;\d+$/, '').replace(/\.$/, '');
					const p = path + '/' + name, ext = le32(r, 2), sz = le32(r, 10);
					out[p] = { extent: ext, size: sz, dir: !!(flags & 2) };
					if (flags & 2) walk(ext, sz, p, depth + 1);
				}
				o += len;
			}
		};
		const root = pvd.subarray(156, 190);
		walk(le32(root, 2), le32(root, 10), '', 0);
		return out;
	}

	function rrName(su) {	// concatenated NM entries of a System Use area, or null
		let p = 0, name = null;
		while (p + 4 <= su.length) {
			const sig = String.fromCharCode(su[p], su[p + 1]), len = su[p + 2];
			if (len < 4 || p + len > su.length) break;
			if (sig === 'NM' && !(su[p + 4] & 6)) name = (name || '') + XRM.hash.text(su.subarray(p + 5, p + len));
			if (sig === 'ST') break;
			p += len;
		}
		return name;
	}

	// ---------- writing ----------

	function both16(v) { return [v & 255, v >> 8 & 255, v >> 8 & 255, v & 255]; }
	function both32(v) {
		const l = [v & 255, v >>> 8 & 255, v >>> 16 & 255, v >>> 24 & 255];
		return l.concat(l.slice().reverse());
	}
	const ascii = (s) => Array.from(XRM.hash.ascii(s));

	function recDate(d) {	// 7-byte directory record date, UTC
		return [d.y - 1900, d.mo, d.d, d.h, d.mi, d.s, 0];
	}
	function vdDate(d) {	// 17-byte volume descriptor date
		const p = (n, w) => String(n).padStart(w, '0');
		return ascii(p(d.y, 4) + p(d.mo, 2) + p(d.d, 2) + p(d.h, 2) + p(d.mi, 2) + p(d.s, 2) + '00').concat([0]);
	}

	const RR_PX = 1, RR_NM = 8, RR_TF = 0x80;
	const ER_ID = 'RRIP_1991A';
	const ER_DES = 'THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS';
	const ER_SRC = 'PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  SEE PUBLISHER IDENTIFIER IN PRIMARY ' +
		'VOLUME DESCRIPTOR FOR CONTACT INFORMATION.';

	function suRR(flags) { return ascii('RR').concat([5, 1, flags]); }
	function suPX(mode, links) { return ascii('PX').concat([36, 1], both32(mode), both32(links), both32(0), both32(0)); }
	function suTF(date) { const t = recDate(date); return ascii('TF').concat([26, 1, 0x0e], t, t, t); }
	function suNM(name) { const n = ascii(name); return ascii('NM').concat([5 + n.length, 1, 0], n); }
	function suSP() { return ascii('SP').concat([7, 1, 0xbe, 0xef, 0]); }
	function suCE(extent, size) { return ascii('CE').concat([28, 1], both32(extent), both32(0), both32(size)); }
	function suER() {
		return ascii('ER').concat([8 + ER_ID.length + ER_DES.length + ER_SRC.length, 1, ER_ID.length, ER_DES.length,
			ER_SRC.length, 1], ascii(ER_ID), ascii(ER_DES), ascii(ER_SRC));
	}

	function record(id, extent, size, isDir, date, su) {
		const pad = id.length % 2 === 0 ? 1 : 0;
		const len = 33 + id.length + pad + su.length;
		if (len > 255) throw new Error('ISO: directory record too long');
		return [len, 0].concat(both32(extent), both32(size), recDate(date), [isDir ? 2 : 0, 0, 0], both16(1),
			[id.length], id, pad ? [0] : [], su);
	}

	// pycdlib-like unique ISO names (the real names are in Rock Ridge NM)
	function isoName(name, isDir, n) {
		const dot = name.lastIndexOf('.');
		const stem = (dot > 0 ? name.slice(0, dot) : name).toUpperCase().replace(/[^A-Z0-9_]/g, '').slice(0, 6) || 'F';
		const base = stem + String(n % 100).padStart(2, '0');
		if (isDir) return base;
		const ext = dot > 0 ? name.slice(dot + 1).toUpperCase().replace(/[^A-Z0-9_]/g, '').slice(0, 3) : '';
		return base + '.' + ext + ';1';
	}

	// files: [{ path: 'images/pioneer.tar.gz', data, mode: 0o100444 }]; date: { y, mo, d, h, mi, s } (UTC)
	function build(files, date, volumeId) {
		// tree
		const root = { name: '', children: [], dir: true, parent: null };
		let counter = 0;
		const sorted = files.slice().sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
		const dirOf = (parts) => {
			let d = root;
			for (const part of parts) {
				let c = d.children.find((x) => x.dir && x.name === part);
				if (!c) {
					c = { name: part, children: [], dir: true, parent: d, iso: null };
					d.children.push(c);
				}
				d = c;
			}
			return d;
		};
		for (const f of sorted) {
			const parts = f.path.split('/');
			const d = dirOf(parts.slice(0, -1));
			d.children.push({ name: parts[parts.length - 1], dir: false, data: f.data, mode: f.mode, parent: d });
		}
		// ISO names in a depth-first, name-sorted walk (like make_autoexec.py's counter)
		const assign = (d) => {
			d.children.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
			for (const c of d.children) {
				counter++;
				c.iso = isoName(c.name, c.dir, counter);
				if (c.dir) assign(c);
			}
		};
		assign(root);
		// directories in path-table order (breadth first, sorted by ISO name per parent)
		const dirs = [root];
		for (let i = 0; i < dirs.length; i++) {
			const subs = dirs[i].children.filter((c) => c.dir).sort((a, b) => (a.iso < b.iso ? -1 : 1));
			dirs.push(...subs);
		}
		dirs.forEach((d, i) => { d.number = i + 1; });

		// layout: 16 system, 17 PVD at 16, terminator 17, L table 18, M table 19, ER area 20, dirs, files
		const ERA = suER();
		let next = 21;
		// directory sizes: records sorted by ISO identifier; a record never crosses a sector
		const recordsFor = (d, extentOf) => {
			const recs = [];
			const self = d === root
				? suSP().concat(suRR(RR_PX | RR_TF), suPX(0o40555, 2 + d.children.filter((c) => c.dir).length), suTF(date),
					suCE(20, ERA.length))
				: suRR(RR_PX | RR_TF).concat(suPX(0o40555, 2 + d.children.filter((c) => c.dir).length), suTF(date));
			const par = d.parent || d;
			recs.push(record([0], extentOf(d), d.size, true, date, self));
			recs.push(record([1], extentOf(par), par.size, true, date,
				suRR(RR_PX | RR_TF).concat(suPX(0o40555, 2 + par.children.filter((c) => c.dir).length), suTF(date))));
			const kids = d.children.slice().sort((a, b) => (a.iso < b.iso ? -1 : a.iso > b.iso ? 1 : 0));
			for (const c of kids) {
				const su = suRR(RR_PX | RR_NM | RR_TF).concat(suNM(c.name),
					suPX(c.dir ? 0o40555 : c.mode, c.dir ? 2 + c.children.filter((x) => x.dir).length : 1), suTF(date));
				recs.push(record(ascii(c.iso), extentOf(c), c.dir ? c.size : c.data.length, c.dir, date, su));
			}
			return recs;
		};
		const packed = (recs) => {	// byte length with records kept inside sectors
			let o = 0;
			for (const r of recs) {
				if (Math.floor(o / SECTOR) !== Math.floor((o + r.length - 1) / SECTOR)) o = Math.ceil(o / SECTOR) * SECTOR;
				o += r.length;
			}
			return Math.ceil(o / SECTOR) * SECTOR;
		};
		// first pass: sizes with dummy extents (record lengths don't depend on extents)
		for (const d of dirs) d.size = SECTOR;
		for (const d of dirs) d.size = packed(recordsFor(d, () => 0));
		for (const d of dirs) { d.extent = next; next += d.size / SECTOR; }
		const fileList = [];
		const collect = (d) => {
			for (const c of d.children.slice().sort((a, b) => (a.iso < b.iso ? -1 : 1))) {
				if (c.dir) collect(c); else fileList.push(c);
			}
		};
		collect(root);
		for (const f of fileList) { f.extent = f.data.length ? next : 0; next += Math.ceil(f.data.length / SECTOR); }
		const total = next;

		const img = new Uint8Array(total * SECTOR);
		const put = (at, arr) => img.set(arr instanceof Uint8Array ? arr : Uint8Array.from(arr), at);
		const space = (s, n) => ascii(s.padEnd(n, ' ').slice(0, n));

		// path tables
		const pathTable = (bigEndian) => {
			const out = [];
			for (const d of dirs) {
				const id = d === root ? [0] : ascii(d.iso);
				const ext = bigEndian ? both32(d.extent).slice(4) : both32(d.extent).slice(0, 4);
				const par = (d.parent || d).number;
				const parb = bigEndian ? [par >> 8 & 255, par & 255] : [par & 255, par >> 8 & 255];
				out.push(id.length, 0, ...ext, ...parb, ...id);
				if (id.length % 2) out.push(0);
			}
			return out;
		};
		const ptl = pathTable(false), ptm = pathTable(true);
		if (ptl.length > SECTOR) throw new Error('ISO: path table too long');
		put(18 * SECTOR, ptl);
		put(19 * SECTOR, ptm);

		// PVD
		const pvd = [1].concat(ascii('CD001'), [1, 0], space('LINUX', 32), space(volumeId || 'XDJRXMOD', 32),
			new Array(8).fill(0), both32(total), new Array(32).fill(0), both16(1), both16(1), both16(SECTOR),
			both32(ptl.length), [18, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 19], [0, 0, 0, 0],
			record([0], root.extent, root.size, true, date, []),
			space('', 128), space('', 128), space('', 128), space('XDJRX-MOD WEB BUILDER', 128),
			space('', 37), space('', 37), space('', 37),
			vdDate(date), vdDate(date), ascii('0000000000000000').concat([0]), vdDate(date), [1, 0]);
		put(16 * SECTOR, pvd);
		put(17 * SECTOR, [255].concat(ascii('CD001'), [1]));
		put(20 * SECTOR, ERA);

		// directories
		for (const d of dirs) {
			let o = d.extent * SECTOR;
			const start = o;
			for (const r of recordsFor(d, (x) => x.extent)) {
				const rel = o - start;
				if (Math.floor(rel / SECTOR) !== Math.floor((rel + r.length - 1) / SECTOR)) o = start + Math.ceil(rel / SECTOR) * SECTOR;
				put(o, r);
				o += r.length;
			}
		}
		for (const f of fileList) if (f.data.length) img.set(f.data, f.extent * SECTOR);
		return img;
	}

	return { SECTOR, list, build };
})();
