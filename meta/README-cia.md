# Building the .cia

Not part of the CMake build (devkitPro's `Nintendo3DS.cmake` only wires up
`ctr_create_3dsx()` - see `CMakeLists.txt`) - built by hand with `makerom`
and `bannertool`, neither of which devkitPro's own `pacman` repos ship
(installed directly from their GitHub releases: 3DSGuy/Project_CTR and
carstene1ns/3ds-bannertool, into `$DEVKITPRO/tools/bin/`).

Why bother with a CIA at all when the `.3dsx` (Homebrew Launcher) build
already works: a `.3dsx` has no exheader of its own and inherits whatever
System Mode Homebrew Launcher itself happens to be running under (the 64MB
Prod default), with no way to ask for more - see `main.cpp`'s own comment.
A real CIA gets its own exheader, so `meta/app.rsf`'s
`AccessControlInfo/SystemMode` can ask for `96MB` (Dev1, the largest mode
Old3DS supports) outright.

## One-time setup

`makerom.exe` and `bannertool.exe` need to already be on `PATH` inside the
MSYS2 shell (same one used for the normal `.3dsx` build) - copy them into
`$DEVKITPRO/tools/bin/` alongside `3dsxtool.exe` etc.

## Build steps

All commands run from the **project root** (not `meta/`) inside MSYS2 bash
(`"/c/devkitPro/msys2/usr/bin/bash.exe"`), after the normal CMake build has
already produced `build/mii-transfer.elf`:

```sh
# 1. Banner (256x128 PNG required - see meta/banner.png; WAV tune optional,
#    <=3s, 16-bit/44.1kHz/stereo - see meta/banner.wav).
bannertool makebanner -i meta/banner.png -a meta/banner.wav -o build/mii-transfer-banner.bnr
# (drop -a meta/banner.wav entirely for a silent banner)

# 2. SMDH - same short title/publisher as the .3dsx build's own metadata
#    (CMakeLists.txt's APP_NAME/APP_AUTHOR); long description is the
#    standard two-line convention (title, then tagline, joined by a real
#    newline - $'...' bash quoting, not a literal "\n") - built via
#    bannertool instead of devkitPro's own smdhtool since bannertool's
#    makesmdh can (if ever wanted) set different text per region - every
#    region defaults to the same single string here, which is normal for
#    homebrew.
bannertool makesmdh -s "Mii Transfer (3DS)" -l $'Mii Transfer (3DS)\nTransfer Miis to other devices' -p "jamesrhg/Arian Kordi" -i meta/icon-48x48.png -o build/mii-transfer-cia.smdh

# 3. The CIA itself. No -romfs flag (that's for *rebuilding* an
#    already-extracted NCCH's romfs binary, not a fresh build) - romfs
#    embedding instead comes from meta/app.rsf's own RomFs/RootPath field,
#    which is resolved relative to *this* (the project root) working
#    directory, not relative to app.rsf's own location (confirmed by
#    testing - a "../romfs" RootPath actually resolved one level above the
#    project root and failed).
makerom -f cia -o build/mii-transfer.cia -rsf meta/app.rsf -elf build/mii-transfer.elf -icon build/mii-transfer-cia.smdh -banner build/mii-transfer-banner.bnr -target t -exefslogo
```

`-target t` (test keys) is what makes the CIA installable at all without a
real Nintendo signature - Luma3DS's own signature patches (already needed
for Homebrew Launcher content to run at all) accept these "fake-signed"
CIAs the same way. Install with FBI or any other CIA installer.

## meta/app.rsf notes

Based on FlagBrew/Checkpoint's own `3ds/assets/app.rsf` (a real,
actively-maintained homebrew CIA) rather than assembled from scratch - see
that file's own header comment for exactly what was changed
(`ProductCode`/`UniqueId`/`SystemMode`/adding `act:u` access).

- `UniqueId: 0xF4D33` is self-assigned, not officially registered - normal
  for homebrew (only matters if it collides with another title already
  installed on the same SD card).
- `ProductCode: CTR-P-MIITR` is arbitrary (`FreeProductCode: true` lifts
  Nintendo's own format restrictions on this field).
