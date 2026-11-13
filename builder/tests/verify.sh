#!/bin/bash
# Check the web builder on a Mac with your own Pioneer downloads (see builder/README.md, "Verify").
#   builder/tests/verify.sh <XDJ-RX_v221.zip or its XDJRX.UPD> <GPL part .00: the zip or the unzipped file>
# 1. build the page (build.py)
# 2. run the page's core in macOS's JavaScriptCore shell on your files; compare with expected.json; refusals
# 3. inflate: all zlib block types; the whole Deflate64 part against unzip (if you give the zip)
# 4. the page in headless Chrome, if installed (loads without errors, self-test, CSP blocks a network request)
# Outputs go to builder/build/verify/. They contain Pioneer's firmware: keep them to yourself.
set -eu
[ $# -eq 2 ] || { sed -n 2,3p "$0"; exit 2; }
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
FW=$(abs "$1")
GPL=$(abs "$2")
B=$(cd "$(dirname "$0")/.." && pwd)
JSC=/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc
[ -x "$JSC" ] || { echo "needs macOS (JavaScriptCore's jsc shell)"; exit 1; }
OUT="$B/build"
V="$OUT/verify"
rm -rf "$V" && mkdir -p "$V"

echo "== 1. build the page"
python3 -I "$B/build.py" "$OUT"

echo "== 2. the core on your Pioneer files"
"$JSC" "$B/tests/jsc_build.js" -- "$B" "$OUT/assets.js" "$V" "$FW" "$GPL" | tee "$V/jsc.txt"
if grep -q "REFUSAL MISSING" "$V/jsc.txt"; then echo "FAIL: a wrong input was not refused"; exit 1; fi
grep -q 'match {"install":true,"r1":true,"r2":true}' "$V/jsc.txt" ||
	{ echo "FAIL: the outputs differ from expected.json"; exit 1; }
echo "ok: install, R1 and R2 match the published MD5s (expected.json)"

echo "== 3. inflate"
I="$V/inflate"
mkdir -p "$I"
python3 -I - "$I" "$B/README.md" "$B/release/libxdjmod.so" <<'EOF'
import os, random, sys, zlib
d = sys.argv[1]; random.seed(7); names = []
text = open(sys.argv[2], 'rb').read() * 20
binary = open(sys.argv[3], 'rb').read() * 3
rnd = bytes(random.getrandbits(8) for _ in range(300000))
mixed = b''.join([text[:50000], rnd[:40000], bytes(70000), binary[:90000], b'ab' * 40000])
runs = b''.join(bytes([random.randrange(4)]) * random.randrange(1, 300) for _ in range(5000))
for dn, data in [('text', text), ('binary', binary), ('random', rnd), ('mixed', mixed), ('runs', runs), ('empty', b''), ('one', b'x')]:
    for lvl in (0, 1, 6, 9):
        for st, sn in ((zlib.Z_DEFAULT_STRATEGY, 'def'), (zlib.Z_FILTERED, 'filt'), (zlib.Z_HUFFMAN_ONLY, 'huff'),
                       (zlib.Z_RLE, 'rle'), (zlib.Z_FIXED, 'fixed')):
            c = zlib.compressobj(lvl, zlib.DEFLATED, -15, 9, st)
            n = '%s-%d-%s' % (dn, lvl, sn); names.append(n)
            open(os.path.join(d, n + '.raw'), 'wb').write(c.compress(data) + c.flush())
            open(os.path.join(d, n + '.plain'), 'wb').write(data)
open(os.path.join(d, 'list.txt'), 'w').write('\n'.join(names) + '\n')
EOF
if unzip -Z1 "$GPL" >/dev/null 2>&1; then
	"$JSC" "$B/tests/jsc_inflate.js" -- "$B" "$I" "$GPL" "$(unzip -p "$GPL" | md5 -q)" | tee "$V/inflate.txt"
	grep -q "md5 matches unzip" "$V/inflate.txt" || { echo "FAIL: Deflate64 output differs from unzip"; exit 1; }
else
	"$JSC" "$B/tests/jsc_inflate.js" -- "$B" "$I" | tee "$V/inflate.txt"
	echo "(the GPL part was given unzipped: Deflate64 check skipped)"
fi
grep -q " 0 failed" "$V/inflate.txt" || { echo "FAIL: inflate"; exit 1; }
rm -rf "$I"

echo "== 4. the page in Chrome"
python3 -I "$B/tests/chrome_check.py" "$OUT/xdjrx-mod-builder.html"

echo
echo "verify: all passed. Page: $OUT/xdjrx-mod-builder.html (md5 $(md5 -q "$OUT/xdjrx-mod-builder.html"))"
