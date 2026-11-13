{name} for the Pioneer DJ XDJ-RX (first model)
================================================

Unofficial. Not affiliated with, endorsed by or supported by Pioneer DJ or AlphaTheta. You use it at
your own risk. Licence: GPL-3.0.

These files were built on your computer from Pioneer's own downloads. Don't pass them on: they contain
Pioneer's firmware. Point people to the builder page instead.


WHAT'S IN THIS FOLDER
---------------------
install/XDJRX.UPD        the mod
restore-R1/XDJRX.UPD     Pioneer's original app back (the normal way to undo the mod)
emergency-R2/XDJRX.UPD   Pioneer's complete official firmware 2.21, numbered {r2} (last resort, see below)
build-log.txt            what the builder checked

All three files are called XDJRX.UPD on purpose: the XDJ-RX only looks for that name. Put ONE of them on
the stick at a time.


BEFORE YOU START
----------------
- Your XDJ-RX must run Pioneer's firmware 2.21 (the last one). The installer checks this and refuses
  anything else without writing a single byte.
- Use an otherwise empty USB stick, formatted FAT32 (MS-DOS FAT) with the partition scheme MBR
  (Master Boot Record). FAT32 alone is not enough: a FAT32 stick with a GPT partition table is not
  recognised by the update mode.
    Mac:     Disk Utility > View > Show All Devices > select the stick itself (the top entry, not the
             volume below it) > Erase > Format "MS-DOS (FAT)", Scheme "Master Boot Record".
    Windows: most sticks already are MBR. To check: right-click Start > Disk Management > right-click
             the stick's disk (on the left) > Properties > Volumes > "Partition style". If it says GPT:
             right-click the stick's volume > Delete Volume (this erases the stick; make sure it's the
             stick), right-click its disk > Convert to MBR Disk, then right-click the empty space > New
             Simple Volume > FAT32. (Windows makes FAT32 volumes only up to 32 GB; on a bigger stick a
             32 GB volume is fine.)
- Don't switch the XDJ-RX off while it's writing. A power cut during any firmware update is the one
  real risk; even then the update mode itself stays intact, so R1 or R2 still work afterwards.


INSTALL
-------
1. Copy install/XDJRX.UPD to the top level of the stick (FAT32 + MBR, not into a folder).
   Name: XDJRX.UPD.
2. Switch the XDJ-RX off. Plug the stick into the DECK 1 USB port.
3. Hold DECK 1 USB STOP and DECK 1 RELOOP/EXIT, and switch the XDJ-RX on. Keep holding until the
   update screen appears.
4. The screen shows "{name}: install", then "Writing ...", then "{name} installed."
   This takes about a minute.
5. Switch off, remove the stick, switch on. The mod's settings are in UTILITY.

If it says "{name} was already installed.", nothing was written: the mod is already on.


THE FIRST MINUTE AFTER SWITCHING ON (SAFE START)
------------------------------------------------
If a start hangs, or the XDJ-RX is switched off within its first minute, the NEXT start runs without
the mod (pure Pioneer), and the one after that is modded again. So: after switching on, leave it on
for at least one minute before switching off. If the mod's UTILITY entries are missing, that start was
a safe start; switch off and on once more.


WHAT THE MOD ADDS (all in UTILITY)
----------------------------------
- CUE MODE: NORMAL / SPLIT / SPLIT STEREO.
  SPLIT: Pioneer's built-in split cue, always on: cue left, master right.
  SPLIT STEREO: like an Allen & Heath Xone:PX5. Nothing cued = master in stereo (4 dB quieter, so it
  matches the cue level); a channel cued = cue left, master right. Both decks can be cued at once.
  In both split modes the CUE/MASTER knob has no effect.
- MASTER LEVEL DISPLAY: PRE / POST MASTER LEVEL. PRE shows the mix before the master knob (A&H style).
- MASTER MONO, BOOTH MONO: NO / YES.
- DISPLAY REFRESH: 20 FPS (ORIGINAL) / 30 FPS.
- WAVEFORM COLOR: BLUE (ORIGINAL) / RGB / 3-BAND. Needs tracks analysed and exported by rekordbox with
  colour waveform data; other tracks stay blue.
- WAVEFORM SCROLL: ORIGINAL / SMOOTH.
- BEAT JUMP A, BEAT JUMP B: 1, 2, 4, 8, 16, 32 BEATS. Press SHIFT + AUTO BEAT LOOP to switch the pads to
  beat jump (back A, forward A, back B, forward B); AUTO BEAT LOOP flashes while it's on. Needs tracks
  with a beat grid (analysed in rekordbox). Inside an active loop, a jump stays inside the loop.
- JOG LIGHT: TOUCH / ON AIR. ON AIR lights the jog ring while the channel's fader and the crossfader
  are open (no touch feedback then).
All settings survive switching off.


GETTING BACK TO PIONEER'S SOFTWARE
----------------------------------
Normal way: R1. Same steps as INSTALL with restore-R1/XDJRX.UPD. The screen ends with "Original Pioneer
app restored." Only the app area is rewritten, exactly as Pioneer shipped it.

Last resort: R2 (emergency-R2/XDJRX.UPD). This is Pioneer's complete official firmware 2.21, unchanged
except that it calls itself version {r2}, because the XDJ-RX refuses to install the same version again.
It rewrites everything the official way. Afterwards your XDJ-RX reports {r2} for good, and a later R2 must
have a higher number (the builder page has a field for that). Use it only if R1 doesn't help.
Pioneer's own XDJRX.UPD is refused on a 2.21 unit ("already updated") and writes nothing.


ERROR SCREENS (nothing is written in E1 to E4)
----------------------------------------------
E1 unexpected device       not an XDJ-RX partition layout we know
E2 update file damaged     copy the file to the stick again
E3 cannot read app area    try again; if it repeats, report it
E4 unknown app version     your XDJ-RX doesn't run stock 2.21 (or another mod is on it): R1 first
E5 write failed            use restore-R1 next
A log of every run is saved on the stick as xdjmod-update.log.


REPORTING A BUG
---------------
1. Make a folder called xdjmod-capture at the top level of your music stick. While it's there, the mod
   writes a log (hook.log) into it; without it, nothing is written anywhere.
2. Reproduce the problem with that stick plugged in, then send hook.log, your firmware version, what
   you did and what happened.
Never send firmware files, keys or music.


CHECKSUMS OF YOUR FILES (MD5)
-----------------------------
install/XDJRX.UPD        {md5_install}
restore-R1/XDJRX.UPD     {md5_r1}
emergency-R2/XDJRX.UPD   {md5_r2}
Everyone who builds from the same Pioneer files gets the same checksums.
