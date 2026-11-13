// Runs the web builder's core in macOS's JavaScriptCore shell, on the same inputs a user drops in.
//   jsc jsc_build.js -- <webbuilder src dir> <assets.js> <out dir> <fw file> <gpl file> [<fw file 2> <gpl file 2>]
// Writes <out>/install.UPD, r1.UPD, r2.UPD, files.zip, README-FIRST.txt, log.txt and prints the MD5s.
const [src, assetsPath, out, fw, gpl, fw2, gpl2] = arguments;
load(src + '/vendor/pako-2.1.0/pako.js');
for (const f of ['00-hash.js', '10-aes.js', '20-inflate64.js', '30-zip.js', '40-tar.js', '50-iso.js', '60-upd.js', '70-build.js'])
	load(src + '/core/' + f);
load(assetsPath);

const B = XRM.build, H = XRM.hash;
const read = (p) => new Uint8Array(readFile(p, 'binary'));
let t0 = Date.now();
const lap = (what) => { print('  ' + what + ': ' + (Date.now() - t0) + ' ms'); t0 = Date.now(); };

function prepare(fwPath, gplPath) {
	const f = read(fwPath), fk = B.identify(fwPath, f.length, f.subarray(0, B.HEAD_BYTES));
	const upd = B.firmware(fk, f);
	lap('firmware (' + fk + ')');
	const g = read(gplPath), gk = B.identify(gplPath, g.length, g.subarray(0, B.HEAD_BYTES));
	const keyFile = B.key(gk, g.subarray(0, B.GPL_READ));
	lap('key (' + gk + ')');
	return { upd, keyFile, kinds: fk + '+' + gk };
}

const a = prepare(fw, gpl);
const state = B.run({ upd: a.upd, keyFile: a.keyFile }, { r2Release: '2.22' }, (s) => print('step: ' + s));
lap('build');
writeFile(out + '/install.UPD', state.out.install);
writeFile(out + '/r1.UPD', state.out.r1);
writeFile(out + '/r2.UPD', state.out.r2);
writeFile(out + '/files.zip', B.zipAll(state));
writeFile(out + '/README-FIRST.txt', B.readme(state));
writeFile(out + '/log.txt', H.ascii(state.log.join('\n') + '\n'));
print('md5 install ' + H.md5(state.out.install));
print('md5 r1 ' + H.md5(state.out.r1));
print('md5 r2 ' + H.md5(state.out.r2));
B.rebuildR2(state, '2.23');
print('md5 r2_2.23 ' + H.md5(state.out.r2));
print('match ' + JSON.stringify(state.match));

if (fw2) {	// the other input forms (unzipped downloads) must give the same files
	const b = prepare(fw2, gpl2);
	const s2 = B.run({ upd: b.upd, keyFile: b.keyFile }, { r2Release: '2.22' });
	print('second input form ' + b.kinds + ' same: ' +
		(H.md5(s2.out.install) === H.md5(state.out.install) && H.md5(s2.out.r1) === H.md5(state.out.r1)));
}

// refusals: wrong files must stop the build with a message
function expectFail(what, fn) {
	try { fn(); print('REFUSAL MISSING: ' + what); } catch (e) { print('refused ok: ' + what + ' (' + e.message + ')'); }
}
const bad = new Uint8Array(a.upd); bad[1000000] ^= 1;
expectFail('damaged firmware', () => B.firmware('fw-upd', bad));
expectFail('R2 release not higher', () => B.rebuildR2(state, '2.21'));
expectFail('R2 release malformed', () => B.rebuildR2(state, '3'));
print('identify junk: ' + B.identify('x.zip', 1234, new Uint8Array(256)));
// the Freescale BSP part .00 (same list on Pioneer's page; same 200 MiB when unzipped), if its head is at hand
const bspHead = arguments[7];
if (bspHead) {
	const h = read(bspHead), e = XRM.zip.entry(h);
	print('identify bsp zip: ' + B.identify('28A88412.zip', 209715394, h.subarray(0, B.HEAD_BYTES)));
	print('identify bsp unzipped: ' + B.identify('L3.0.35_4.1.0_130816_source.tar.gz.00', 209715200, h.subarray(e.start, e.start + B.HEAD_BYTES)));
}
