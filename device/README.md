# What runs on the XDJ-RX

Two parts: the updater script, which runs once from the USB stick, and the library, which runs alongside Pioneer's
program on every start.

## `update`: the app-only updater

Pioneer's update mode (the initramfs) decrypts `XDJRX.UPD`, mounts the ISO image inside it and runs `./update USB
<language>` as root. In our update files this is our own script instead of Pioneer's.

- **It writes only `/dev/mtd7`**, the application archive (`pioneer.tar.gz`, under 3 MB). It never touches U-Boot,
  its environment, the kernels, the root file system, data, GUI or the panel MCU. So the update mode itself always
  stays intact, and another update file can always be run afterwards.
- **`images/target.conf` says what to write and over what.** It holds:
  - `TITLE`, shown in the log;
  - `SIZE` and `MD5` of the archive to write;
  - `ACCEPT`: the archives that may be replaced (`size:md5` pairs, Pioneer's 2.21 app and known mod builds).
  The same script serves the installer and the restore file (R1).
- **It checks everything before writing.** It checks the partition table, the new archive's MD5, and the MD5 of
  what is on the device now. If anything is unexpected, it shows an error screen (E1 to E4) and writes nothing. If
  the target archive is already there, it says so and writes nothing.
- **It verifies the write.** It erases, writes with `nandwrite -p` and checks the MD5 of what it reads back, as
  Pioneer's own updater does. It makes up to three attempts.
- **Screens.** The update mode's `image` tool can only show its own pictures, so the script copies its screens
  (640×120, RGB565, `screens/*.bin`) over one of them in RAM.
- **Log.** Each run appends a log to `xdjmod-update.log` on the stick.
- **It never returns.** It shows the result and waits for power-off.

`XDJMOD_SIM_ROOT` is set only by a simulator on a computer. On the XDJ-RX it is empty.

The script's header comment points to `flash-plan.md`, a design note that isn't part of this repository; this
section covers it. The script is kept byte for byte as tested, because it goes into the install and R1 files and
so is part of their published MD5s.

## `libxdjmod/`: the library

`libxdjmod.so` is loaded into Pioneer's main program (`/root/pioneer/rbp`) with `LD_PRELOAD`. The installer puts it
next to `rbp` and changes the one line in `apl_start` that starts `rbp`. The new start block (in
`builder/release/release.json`, `aplNew`) adds the safe start:

- Before each start with the library it writes a marker file. After the first minute, the library deletes the
  marker if the audio output is running normally.
- If the marker is still there at the next start (a hang, or a switch-off within the first minute), that start runs
  `rbp` without the library, and the one after that is modded again.

What the library does inside `rbp`:

- **Wraps a few library calls.** It wraps `open`, `close`, `ioctl`, `fopen` and the ALSA PCM calls, and always
  passes them on to the real function.
- **Changes nothing in other programs.** It is a pure pass-through in any process other than `rbp`, and it removes
  `LD_PRELOAD` from `rbp`'s environment so programs that `rbp` starts don't load it.
- **Checks before patching.** Before it changes anything in `rbp`'s memory, it compares short byte sequences with
  the expected ones for firmware 2.21. If they don't match, it changes nothing.
- **Keeps the audio thread real-time safe.** On the audio thread it only does arithmetic and lock-free stores: no
  allocation, no locks, no file access.
- **Writes a log only on request.** It writes `hook.log` (start-up, settings changes, errors) only while a folder
  `xdjmod-capture` exists on a stick. Until then a low-priority thread keeps the notes in memory and looks for the
  folder every 5 seconds. A few start-up lines also go to RAM (`/dev/shm/xdjmod-boot.txt`); nothing else is written.
- **Screen sync.** A thread of its own keeps the phase of the screen's refresh, and the copy of each finished frame
  starts 4.5 ms before the refresh, so the picture doesn't tear. This is always on and has no UTILITY entry.

The settings live in UTILITY and are saved in `/root/rbp/xdjmod-hpmode`. R1 doesn't remove that file (it only
writes the app area); Pioneer's software ignores it.

### Source

- `xdjmod.c`: the part that runs inside `rbp`. Wrappers, patches, settings, waveforms, beat jump.
- `xdjmod_core.h`: the logic that doesn't depend on Linux or on `rbp`.

This is the source of exactly the released library, nothing else: the development builds' measurement and test
code isn't in it, and the comments describe this version.

### Build

```sh
device/libxdjmod/build.sh            # needs zig 0.16.0 on PATH, or ZIG=/path/to/zig
```

It builds `device/libxdjmod/build/libxdjmod.so` and compares it with `builder/release/libxdjmod.so`. With zig
0.16.0 they are identical, byte for byte. That's how you can check that the library in the builder comes from this
source.
