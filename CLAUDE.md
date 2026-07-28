# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Nintendo 3DS homebrew app (devkitPro **devkitARM** toolchain + **libctru**
+ **citro2d**). It's a from-scratch 3DS port of a sibling Wii U project
(`../mii-uploader`, WUT + SDL2) - see this repo's README.md for the full
old-vs-new mapping table. It reads three Mii sources on the console (the Mii
Library via `CFL_DB.dat`, the local account's Mii(s) via ACT, friends' Miis
via FRD), and shows them across both screens: a touch/D-pad-navigable
scrolling list on the bottom screen, and a persistent, always-populated
detail panel for whatever's currently focused on the top screen. Face images
are fetched over plain HTTP from `mii-unsecure.ariankordi.net` by a
background downloader (`source/mii_image_fetch.{h,cpp}`, on libctru's own
`httpc` — an earlier revision used `libcurl`+`mbedtls` instead, since
`httpc`'s TLS support is frozen at the console's original-era protocol/
cipher set and can't complete a handshake with any modern *HTTPS* server;
confirmed both on-device and by libctru's own maintainer. That blocker
doesn't apply to this endpoint specifically, since it's plain HTTP with no
TLS handshake involved at all — see that header's comment). Friends' Miis
(FRD, `source/ctr_friends.{h,cpp}`) are read straight from the console's own
locally-synced friend cache with no login step at all — libctru's
`FRD_Login()` is specifically for making *this* console visible as online
and unlocking live presence data, not a prerequisite for reading the friend
list/Mii data itself, which has no such requirement. `frdInit()` (opening
the `frd:` service handle at all)
runs on a background thread regardless, because it has its own known risk:
`.3dsx` apps launched via Homebrew Launcher inherit HBL's own exheader
service-access list rather than getting one of their own, and that list may
lack an `frd:` entry entirely, which blocks `frdInit()` indefinitely (via
`SRV:GetServiceHandle`, which waits rather than failing fast) rather than
failing fast - backgrounding it means the Friends tab just stays
"connecting" forever in that case instead of taking the whole app down.

## Build

CMake with devkitPro's 3DS toolchain is the only supported build. One-time
portlib install:

```sh
dkp-pacman -S 3ds-libpng 3ds-zlib
```

Configure then build. `DEVKITPRO` must point at the devkitPro install:

```sh
DEVKITPRO=/opt/devkitpro \
  cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build build
```

Use `-DCMAKE_BUILD_TYPE=Release` for an optimized build; delete `build/` for
a clean rebuild. Output lands in `build/`: `.elf` → `.3dsx` (via
`ctr_create_3dsx()`, with `romfs/` embedded and an smdh built from
`meta/icon-48x48.png`). Copy the `.3dsx` to `sd:/3ds/mii-transfer/`.

There are no tests or linters in this project.

## Architecture

Every non-UI subsystem is its own small module, each documented in its own
header - read those first, this file only gives the map:

- **`source/main.cpp`** — the app: dual-screen citro2d render loop, `hid`
  input (D-pad focus movement + repeat via `hidKeysDownRepeat()`, L/R
  cycling between the three tabs - Library, Friends, RecentGames), the
  per-tab `TabState` (Mii list + scroll position + focus, preserved across
  tab switches), the dedicated Mii-details and local-HTTP-server screens
  (mutually exclusive with the list/portrait view - see main.cpp's own
  comment on why), and SFX/BGM triggers.
- **`source/ctr_mii_db.{h,cpp}`** — reads `CFL_DB.dat` out of the Mii Maker
  applet's shared extdata (`0xf000000b`), reusing
  `source/lib/DbAndCreateId.{hpp,cpp}`'s `CharDatabaseCtr` desc (that
  generated library is shared verbatim with the Wii U build and was already
  CTR-aware) for the file's main 100-Mii array (Library tab), plus a
  manually-built `CharDatabaseDesc` for the same file's embedded "CFRA"
  section (RecentGames tab - Miis recently picked in *other* titles, e.g.
  friend-request/NPC pickers; offsets confirmed against 3dbrew's own
  CFL_DB.dat layout docs, not guessed) - see that header's own comment on
  `LoadResult::recentMiis`.
- **`source/ctr_friends.{h,cpp}`** — FRD wrapper (`frd.h`), replacing the
  Wii U build's `nn_friends.h`/`nn::fp` wrapper. No login step (see this file's own "What
  this is" section above for why) - just a backgrounded `frdInit()` (for
  the known Homebrew-Launcher hang risk) followed by local
  `FRD_GetFriendKeyList()`/`FRD_GetFriendMii()` reads. `source/ctr_account.{h,cpp}`
  (the equivalent ACT/`nn::act` wrapper for the console's own local
  account Mii) is wired into `main.cpp` too, same backgrounded-`actInit()`
  shape - used only for the current console user's own Mii nickname (shown
  in the Friends tab header).
- **`source/mii_image_fetch.{h,cpp}`** — background image downloader on
  libctru's own `httpc` (an IPC-based system service, needing no `soc:u`
  and no portlibs) against a plain-HTTP endpoint - see that header's
  comment for why `httpc` is fine here despite being unusable for HTTPS.
  Same `Start/Stop/SetWanted/PollCompleted/FetchImageBlocking` contract as
  the Wii U build (which uses `libcurl`+`mbedtls` instead, since its own
  face-render endpoint is HTTPS).
- **`source/png_texture.{h,cpp}`** — the piece the Wii U build didn't need
  (SDL2_image did this there): decodes PNG bytes (libpng's simplified API)
  into a `C2D_Image` at runtime, including the R/B channel swap and
  GX-display-transfer tiling swizzle citro3d textures require - see that
  header's comment before touching texture upload code.
- **`source/mii_detail_panel.{h,cpp}`** — the top-screen live panel (the 3DS
  redesign of the Wii U build's modal `mii_detail_view.{h,cpp}`): call
  `Update(tab, miis, focusedIndex)` every frame, `Draw()` once per frame on
  the top screen. Synchronous (not backgrounded - see that header's own
  comment for why) windowed portrait prefetch, one cache shared between
  both tabs via a composite (tab, index) key ported from the Wii U build's
  own multi-tab image cache, non-blocking/always-on instead of opened/closed.
- **`source/ctr_ui.{h,cpp}`** — shared system-font/text-drawing helper used
  by every screen. Re-parses text fresh every frame (cheap on citro2d,
  unlike the Wii U build's SDL_ttf-backed per-string textures) - see that
  header's comment for why this app doesn't port over the old
  rebuild-only-on-change caching.
- **`source/sfx.{h,cpp}`** — SFX/BGM on `ndsp`, with a hand-rolled minimal
  WAV (RIFF) reader (no SDL2 to lean on for `SDL_LoadWAV`).
- **`source/mii_http_server.{h,cpp}`** — the local browser-facing file
  server, on `soc:u` BSD sockets. Serves `CFL_DB.dat`/act/friend data from
  in-memory blobs (unlike the Wii U build, `CFL_DB.dat` has no plain
  filesystem path to re-`fopen()` per request - it lives inside a
  shared-extdata archive).

Data flow mirrors the Wii U build: each loader (`CtrMiiDb::Load()`,
`CtrFriends::LoadFriendMiis()`) hands back a `std::vector<Ver3MiiDecoded>`
(see `source/ver3_mii.h` - the Ver3StoreData decode/detail logic,
unmodified from the Wii U build's `cafe_mii.*` besides the rename), stored
per-tab in `main.cpp`'s own `TabState` → the active tab's list and focused
index feed `MiiDetailPanel::Update()` each frame, which drives both the
top-screen portrait and its own windowed prefetch.

## Code style

Same conventions as the Wii U sibling project (Doxygen `///<` inline
comments, `c`-prefix or `ALL_CAPS` constants — never `k` outside a
`kFooBar`-style local constant, fully-indented preprocessor directives,
comments ending with a period). **Do not apply these to `source/lib/`** —
that code is generated (shared verbatim with the Wii U build).
