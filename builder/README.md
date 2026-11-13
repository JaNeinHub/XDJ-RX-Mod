# The XDJRX-MOD web builder

A single HTML page that builds the mod's update files in the user's browser, from two files the user downloads
from Pioneer/AlphaTheta. There is no server and no upload: the page's Content-Security-Policy forbids every network
connection (`connect-src 'none'`), and all scripts and styles are inline and allowed only by their SHA-256 hash. The
page contains nothing of Pioneer's.

## What the page does

1. **Reads the firmware.** It takes `XDJRX.UPD` from `XDJ-RX_v221.zip`, or the unzipped file, and checks its SHA-256.
2. **Reads the key.** It unpacks the first part of Pioneer's GPL source download (Deflate64 zip, then gzip, then
   tar) up to `initramfs.tar.gz`, and takes the update key from it. It checks the key's SHA-256; the key itself is
   never shown or saved.
3. **Opens the firmware.** It decrypts it (AES-256-CBC per 512-byte sector), reads the ISO 9660 image and takes
   Pioneer's app archive (`images/pioneer.tar.gz`). It checks the archive's MD5.
4. **Builds the mod archive.** It inserts `libxdjmod.so` and replaces one start line in `apl_start`. It checks the
   result against the MD5 of the hardware-tested archive (`release/release.json`), so it can only ever produce that
   one archive.
5. **Writes three update files.** Each is a new ISO image with the updater script (`device/update`) and its
   screens, encrypted and tagged as the XDJ-RX expects:
   - `install/XDJRX.UPD`, the mod;
   - `restore-R1/XDJRX.UPD`, Pioneer's app archive back;
   - `emergency-R2/XDJRX.UPD`, Pioneer's official firmware with only `release.txt` raised.
6. **Checks its own output.** It reads each file back and compares the MD5s with `expected.json`.

## Layout

| Path | What |
| --- | --- |
| `core/00-hash.js` | CRC32, MD5, SHA-256, Base64 |
| `core/10-aes.js` | AES-256 (FIPS-197) and the XDJ-RX's sector-wise CBC |
| `core/20-inflate64.js` | raw inflate for zip Deflate and **Deflate64** (Pioneer's GPL zips use Deflate64) |
| `core/30-zip.js` | read a zip's first entry; write a stored zip ("download everything") |
| `core/40-tar.js` | tar reading; the mod's archive edit |
| `core/50-iso.js` | ISO 9660 reader, and a writer with Rock Ridge 1.09 |
| `core/60-upd.js` | the `XDJRX.UPD` container: key, tag, CRC, sector-wise read and write, in-place patch (R2) |
| `core/70-build.js` | the build: identify inputs, key, firmware, steps with fixed checks, R2, readme, zip |
| `ui/` | the page (`page.html`, `style.css`, `app.js`) and `README-FIRST.txt`, the instructions in the user's zip |
| `release/release.json` | the release: name, texts, checksums of the tested archive, the start line |
| `release/libxdjmod.so` | the library, built from `device/libxdjmod` (its MD5 is in `release.json`) |
| `release/screens.json` | the RX screens, already rendered (their pixels are part of the output MD5s) |
| `vendor/pako-2.1.0/` | pako (MIT and Zlib licence): gzip level 9, byte-identical to zlib; see `VENDOR.md` |
| `expected.json` | the published MD5s of the three output files |
| `build.py` | puts it all into one page |
| `tests/` | `verify.sh` and its helpers |

The core is plain JavaScript without browser APIs, so the same files run in the page and in macOS's `jsc` shell.

## Build the page

```sh
python3 builder/build.py                 # -> builder/build/xdjrx-mod-builder.html
python3 builder/build.py --test <dir>    # the same page plus a self-test (tests/browser_selftest.js)
```

Needs Python 3 only. The output depends only on the files in this repository, so building from a release tag gives
the page attached to that release, byte for byte. `build.py --render-screens` draws the screens again from
`release.json`'s texts and needs Pillow. Only use it when the texts change: another Pillow version can draw
slightly different pixels, and then the output MD5s change.

## Verify (macOS)

```sh
builder/tests/verify.sh ~/Downloads/XDJ-RX_v221.zip ~/Downloads/<GPL part .00>.zip
```

It runs four checks:

1. It builds the page.
2. It runs the core in macOS's JavaScriptCore shell on your two Pioneer files and compares the outputs with
   `expected.json`. It also checks that wrong inputs are refused.
3. It runs the inflate tests: every zlib block type, and the whole Deflate64 part against `unzip`.
4. If Google Chrome is installed, it opens the page headless with a throw-away profile. The page must load without
   errors, the browser self-test must pass, and a network request must be blocked by the CSP.

The outputs land in `builder/build/verify/`. They contain Pioneer's firmware: keep them to yourself.
