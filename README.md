# XDJRX-MOD

An unofficial software mod for the **Pioneer DJ XDJ-RX** (the first model, firmware 2.21). It adds new settings to
the UTILITY menu: split cue modes, colour waveforms, beat jump, mono outputs and more. Pioneer's own software keeps
running unchanged; the mod works alongside it.

> **Unofficial.** Not affiliated with, endorsed by or supported by Pioneer DJ or AlphaTheta. You install it at your
> own risk. Licence: [GPL-3.0](LICENSE).

## What it adds

All settings are in **UTILITY** and are kept when you switch off.

| Setting | Options |
| --- | --- |
| CUE MODE | NORMAL / SPLIT / SPLIT STEREO |
| MASTER LEVEL DISPLAY | PRE / POST MASTER LEVEL |
| MASTER MONO, BOOTH MONO | NO / YES |
| DISPLAY REFRESH | 20 FPS (original) / 30 FPS |
| WAVEFORM COLOR | BLUE (original) / RGB / 3-BAND |
| WAVEFORM SCROLL | ORIGINAL / SMOOTH |
| BEAT JUMP A, BEAT JUMP B | 1 to 32 beats, on the pads (SHIFT + AUTO BEAT LOOP) |
| JOG LIGHT | TOUCH / ON AIR |

- **SPLIT** is Pioneer's built-in split cue, always on: cue left, master right.
- **SPLIT STEREO** works like an Allen & Heath Xone:PX5. With nothing cued you hear the master in stereo; when a
  channel is cued, cue is left and master is right.
- **Colour waveforms** and **beat jump** need tracks analysed and exported by rekordbox. Other tracks stay blue.

The full description of each setting is in `README-FIRST.txt`, which the builder puts next to your files.

## How you get it

This repository contains **nothing from Pioneer**: no firmware, no keys. You build the update files yourself, on
your own computer, from two files you download from Pioneer/AlphaTheta. The builder is a single web page that runs
offline in your browser. It sends nothing anywhere; the page's security policy blocks every network connection.

1. Download `xdjrx-mod-builder.html` from this repository's
   [Releases](https://github.com/JaNeinHub/XDJ-RX-Mod/releases) page.
2. Open it in a current browser (Safari, Chrome, Edge or Firefox) by double-clicking it. Nothing to install.
3. Follow the page. It links to Pioneer's two downloads:
   - the **XDJ-RX firmware 2.21** (`XDJ-RX_v221.zip`), from AlphaTheta's support page;
   - one part of Pioneer's **GPL source code** (`pioneer_xdj_rx.tar.gz.00`, version 1.50), from Pioneer's GPL
     page. The builder needs it for the update key.
4. Drop both files on the page and click **Build**. After a few seconds you get three files and instructions.

Every build from the same Pioneer files gives the same files, byte for byte. The page shows "same as the published
build ✓" when yours match the published checksums.

## Installing

You need a USB stick formatted **FAT32 with the partition scheme MBR** (Master Boot Record). FAT32 alone is not
enough: the XDJ-RX's update mode doesn't recognise a FAT32 stick with a GPT partition table. The page and
`README-FIRST.txt` explain how to check and fix this on Mac and Windows.

1. Copy `install/XDJRX.UPD` to the top level of the stick.
2. Switch the XDJ-RX off and plug the stick into the DECK 1 USB port.
3. Hold **DECK 1 USB STOP** and **DECK 1 RELOOP/EXIT**, and switch on. Keep holding until the update screen appears.
4. Wait for "XDJRX-MOD 1.0 installed." (about a minute), switch off, remove the stick, switch on.

Don't switch off while it's writing.

## Safety and undoing it

- **The installer only writes the app area.** The XDJ-RX's update mode, kernels and system stay untouched, so you
  can always run another update afterwards.
- **It checks before writing.** It refuses anything other than Pioneer's 2.21 app or a known mod build, and then
  writes nothing.
- **Safe start.** If a start hangs, or you switch off within the first minute, the next start runs without the mod.
  The one after that is modded again.
- **Undo (R1).** `restore-R1/XDJRX.UPD` writes Pioneer's original app back, exactly as shipped.
- **Last resort (R2).** `emergency-R2/XDJRX.UPD` is Pioneer's complete official firmware 2.21. Its only change is a
  higher version number, because the XDJ-RX refuses to reinstall the same version.

Don't share the built files. They contain Pioneer's firmware. Point people to this page instead.

## Reporting a bug

Make a folder called `xdjmod-capture` at the top level of your music stick. While that folder exists, the mod
writes a log (`hook.log`) into it; without it, the mod writes nothing anywhere. Reproduce the problem, then open an
issue with:

- `hook.log`;
- your firmware version;
- what you did and what happened.

Never attach firmware files, keys or music.

## Repository layout

| Path | What |
| --- | --- |
| [`builder/`](builder/) | the web builder: its source, the build script and the tests |
| [`device/`](device/) | what runs on the XDJ-RX: the library `libxdjmod.so` (C) and the updater script |
| `LICENSE`, `NOTICE` | GPL-3.0, and what is not included |
| `CHANGELOG.md` | releases |

## Building from source

- **The page:** `python3 builder/build.py` writes `builder/build/xdjrx-mod-builder.html`. It needs only Python 3.
  The page built from a release tag is byte for byte the page attached to that release. See
  [`builder/README.md`](builder/README.md).
- **The library:** `device/libxdjmod/build.sh` cross-compiles it with zig 0.16.0 and compares the result with the
  library inside the builder. See [`device/README.md`](device/README.md).
- **Checking the builder (macOS):** `builder/tests/verify.sh <XDJ-RX_v221.zip> <GPL part .00 zip>` runs the
  builder's code on Pioneer's files outside the browser. It compares the outputs with the published checksums.

## Credits

- [pako](https://github.com/nodeca/pako) 2.1.0 (MIT and Zlib licence) for gzip in the browser.
- [xdj-rx-tools](https://github.com/thomas-alrek/xdj-rx-tools) for inspiration.
