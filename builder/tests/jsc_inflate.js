// Inflate tests: every *.raw in <dir> is a raw deflate stream of the matching *.plain (made by zlib in run.sh);
// optionally a whole Deflate64 zip member compared with a reference md5.
//   jsc jsc_inflate.js -- <webbuilder src> <dir> [<deflate64 zip> <md5 of its member>]
const [src, dir, d64zip, d64md5] = arguments;
load(src + '/core/00-hash.js'); load(src + '/core/20-inflate64.js'); load(src + '/core/30-zip.js');
const read = (p) => new Uint8Array(readFile(p, 'binary'));
const list = read(dir + '/list.txt'); const names = XRM.hash.text(list).trim().split('\n');
let ok = 0, bad = 0;
for (const n of names) {
	const r = XRM.inflate.inflateRaw(read(dir + '/' + n + '.raw'), {});
	const want = read(dir + '/' + n + '.plain');
	const same = !r.truncated && r.data.length === want.length && XRM.hash.md5(r.data) === XRM.hash.md5(want);
	if (same) ok++; else { bad++; print('FAIL ' + n); }
}
print('zlib streams: ' + ok + ' ok, ' + bad + ' failed');
if (d64zip) {
	const z = read(d64zip), e = XRM.zip.entry(z);
	const t = Date.now();
	const r = XRM.inflate.inflateRaw(z.subarray(e.start, e.start + e.csize), { deflate64: e.method === 9 });
	const m = XRM.hash.md5(r.data);
	print('deflate64 whole member: ' + r.data.length + ' bytes in ' + (Date.now() - t) + ' ms, md5 ' + (m === d64md5 ? 'matches unzip' : 'DIFFERS ' + m));
}
