// Only in the test build of the page (build.py --test): a self-test of the core in the
// browser, plus a network request that the page's Content-Security-Policy must block. Results go into
// <body data-selftest="..."> for tests/chrome_check.py.
(function () {
	'use strict';
	const results = [];
	const ok = (what, cond) => results.push((cond ? 'ok ' : 'FAIL ') + what);
	try {
		const hex = (b) => Array.from(b, (x) => (x < 16 ? '0' : '') + x.toString(16)).join('');
		const k = new Uint8Array(32).map((_, i) => i), pt = new Uint8Array(16).map((_, i) => i * 0x11);
		ok('aes', hex(XRM.aes.encryptBlock(k, pt)) === '8ea2b7ca516745bfeafc49904b496089');
		ok('md5', XRM.hash.md5(XRM.hash.ascii('abc')) === '900150983cd24fb0d6963f7d28e17f72');
		ok('sha256', XRM.hash.sha256(XRM.hash.ascii('abc')) === 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
		const data = XRM.hash.fromBase64(XRM.assets.lib);
		ok('library md5', XRM.hash.md5(data) === XRM.assets.release.lib.md5);
		const z = pako.deflateRaw(data, { level: 9 });
		ok('inflate', XRM.hash.md5(XRM.inflate.inflateRaw(z, {}).data) === XRM.assets.release.lib.md5);
		const keys = XRM.aes.expandKey(k), sec = new Uint8Array(4096).map((_, i) => i * 7);
		ok('sectors', hex(XRM.aes.decryptSectors(keys, XRM.aes.encryptSectors(keys, sec, 9), 9)) === hex(sec));
		const iso = XRM.iso.build([{ path: 'update', data: XRM.hash.ascii('#!/bin/sh\n'), mode: 0o100555 }],
			XRM.assets.release.date, 'T');
		ok('iso', XRM.iso.list((o, n) => iso.subarray(o, o + n))['/update'].size === 10);
		ok('zip', XRM.zip.entry(XRM.zip.store([{ name: 'a.txt', data: XRM.hash.ascii('x') }])).name === 'a.txt');
		ok('blob', typeof Blob === 'function' && typeof URL.createObjectURL === 'function');
	} catch (e) {
		results.push('FAIL exception ' + e.message);
	}
	const done = (net) => {
		results.push(net);
		document.body.dataset.selftest = results.join('; ');
		document.title = results.some((r) => r.startsWith('FAIL')) ? 'SELFTEST FAIL' : 'SELFTEST OK';
	};
	try {
		fetch('https://example.com/xdjrx-csp-probe').then(() => done('FAIL network request went through'),
			() => done('ok network blocked'));
	} catch (e) {
		done('ok network blocked (' + e.name + ')');
	}
})();
