// XDJRX-MOD web builder: the XDJRX.UPD container.
// Layout (verified on 2.21): ISO 9660 image encrypted sector by sector (XRM.aes), then a 12-byte tag
// "XDJAERO2.21\0" and the CRC32 (little-endian) of the encrypted part. The initramfs checks tag + CRC,
// decrypts with losetup and runs ./update from the image.
// SPDX-License-Identifier: GPL-3.0-only
var XRM = typeof XRM !== 'undefined' ? XRM : {};

XRM.upd = (function () {
	'use strict';

	const TRAILER = 16, SECTOR = 512;

	// Key = first line of the key file, first 31 bytes + one NUL (losetup's bounded copy), padded to 32.
	function loadKey(keyFile) {
		let end = keyFile.indexOf(10);
		if (end < 0) end = keyFile.length;
		const k = new Uint8Array(32);
		k.set(keyFile.subarray(0, Math.min(end, 31)));
		return XRM.aes.expandKey(k);
	}

	function split(upd) {
		if (upd.length < TRAILER + SECTOR || (upd.length - TRAILER) % SECTOR) throw new Error('not an XDJRX.UPD file (size)');
		const payload = upd.subarray(0, upd.length - TRAILER);
		const tag = upd.subarray(upd.length - TRAILER, upd.length - 4);
		const crc = (upd[upd.length - 4] | upd[upd.length - 3] << 8 | upd[upd.length - 2] << 16 | upd[upd.length - 1] << 24) >>> 0;
		if (XRM.hash.text(tag.subarray(0, 7)) !== 'XDJAERO') throw new Error('not an XDJRX.UPD file (tag)');
		if (XRM.hash.crc32(payload) !== crc) throw new Error('XDJRX.UPD is damaged (CRC)');
		return { payload, tag };
	}

	// A read(offset, length) function over the decrypted image, decrypting only the sectors asked for.
	function reader(keys, payload) {
		return function (offset, length) {
			const first = Math.floor(offset / SECTOR), last = Math.ceil((offset + length) / SECTOR);
			if (last * SECTOR > payload.length) throw new Error('read past the end of the image');
			const plain = XRM.aes.decryptSectors(keys, payload.subarray(first * SECTOR, last * SECTOR), first);
			return plain.subarray(offset - first * SECTOR, offset - first * SECTOR + length);
		};
	}

	function make(keys, iso, tag) {
		const padded = new Uint8Array(Math.ceil(iso.length / SECTOR) * SECTOR);
		padded.set(iso);
		const enc = XRM.aes.encryptSectors(keys, padded, 0);
		const out = new Uint8Array(enc.length + TRAILER);
		out.set(enc);
		out.set(tag, enc.length);
		const crc = XRM.hash.crc32(enc);
		out.set([crc & 255, crc >>> 8 & 255, crc >>> 16 & 255, crc >>> 24 & 255], enc.length + 12);
		return out;
	}

	// R2: replace `bytes` at image offset `at` (same length) and re-encrypt only the touched sectors.
	function patch(keys, upd, at, bytes) {
		const { payload, tag } = split(upd);
		const first = Math.floor(at / SECTOR), last = Math.ceil((at + bytes.length) / SECTOR);
		const plain = XRM.aes.decryptSectors(keys, payload.subarray(first * SECTOR, last * SECTOR), first);
		plain.set(bytes, at - first * SECTOR);
		const enc = XRM.aes.encryptSectors(keys, plain, first);
		const out = new Uint8Array(upd);
		out.set(enc, first * SECTOR);
		const crc = XRM.hash.crc32(out.subarray(0, payload.length));
		out.set(tag, payload.length);
		out.set([crc & 255, crc >>> 8 & 255, crc >>> 16 & 255, crc >>> 24 & 255], payload.length + 12);
		return out;
	}

	return { TRAILER, loadKey, split, reader, make, patch };
})();
