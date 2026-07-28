# mii-transfer-3ds — Nintendo 3DS homebrew (libctru + citro2d)

A Nintendo 3DS port of the Wii U `mii-transfer` homebrew app. Reads three Mii
sources on the console - the Mii Library (`CFL_DB.dat`), Miis recently picked
in *other* titles (e.g. friend-request/NPC pickers - embedded in that same
`CFL_DB.dat` file, no extra source needed), and the logged-in user's Friends
List (via FRD) - and shows them across both screens: the **bottom screen**
has a scrolling, touch-and-D-pad-navigable list (nickname/creator per row);
the **top screen** permanently shows the currently-focused Mii's face render
with real stereoscopic 3D depth - it's a live, persistent panel, not a
separate screen you open, and the first Mii is auto-focused at startup so
the top screen is never empty. ACT is used only to show the current console
user's own Mii name in the Friends tab's header.

Face images are fetched over plain HTTP from `mii-unsecure.ariankordi.net` by
a background downloader, on libctru's own `httpc`. A local HTTP server can
also be started (**X**) so a browser on the same network can browse/download
the same three Mii sources in-page.

## Relationship to the Wii U build

This is a from-scratch platform port, not a shared codebase - see each
source file's own header comment for what changed and why relative to the
Wii U original (WUT + SDL2). The short version:

| Wii U | 3DS |
|---|---|
| SDL2 (window/renderer/text/image) | citro2d/citro3d (dual-screen render targets, `C2D_Text`, hand-rolled runtime PNG→texture loader - see `source/png_texture.h`) |
| `FFL_ODB.dat` via the Wii U's own SD save path | `CFL_DB.dat` via the Mii Maker applet's shared extdata (`0xf000000b`) - see `source/ctr_mii_db.h` |
| `nn::act` (12 system account slots) | ACT (`act.h`) - see `source/ctr_account.h` |
| `nn::fp` | FRD (`frd.h`) - see `source/ctr_friends.h` |
| libcurl + mbedtls | libctru's own `httpc` (no portlibs needed) against a plain-HTTP endpoint - `httpc`'s TLS support is frozen at the console's original-era protocol/cipher set and can't complete a handshake with any modern *HTTPS* server, but that blocker doesn't apply here since this endpoint is plain HTTP with no TLS handshake involved at all; see `source/mii_image_fetch.h` |
| Cafe AX audio | `ndsp` - see `source/sfx.h` |
| Cafe OS sockets (AC-managed) | `soc:u` BSD sockets, plus an explicit `AC` connect step (`source/ctr_network.h`) - 3DS Wi-Fi still needs `ACU_ConnectAsync()` brought up before anything network-related, same shape of requirement as the Wii U's own AC connect step - see `source/mii_http_server.h` |
| Full-screen modal Mii detail view | Persistent top-screen detail panel, always bound to whatever's focused in the bottom-screen list - see `source/mii_detail_panel.h` |
| Touch cursor visual (GamePad-touch-while-watching-TV mismatch) | Removed - 3DS touch is direct or bottom-screen input, no such mismatch exists |

`source/lib/DbAndCreateId.{hpp,cpp}` (the generated Mii-decode library),
`source/ver3_mii.{h,cpp}` (Ver3StoreData decode/detail helpers - renamed
from the Wii U build's `cafe_mii.*`, since the format itself is shared
between platforms, not Cafe-specific), and `source/base64.{h,cpp}` are
carried over essentially unmodified.

## Prerequisites

devkitPro with the 3DS toolchain (`devkitARM`, `libctru`, `citro2d`,
`citro3d`, `3ds-tools`). Install the one extra portlib this build needs
(PNG decoding, for turning fetched/bundled PNGs into citro2d textures at
runtime - see `source/png_texture.h`):

```sh
dkp-pacman -S 3ds-libpng 3ds-zlib
```

## Building

```sh
DEVKITPRO=/opt/devkitpro \
  cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build build
```

Use `-DCMAKE_BUILD_TYPE=Release` for an optimized build; delete `build/` for
a clean rebuild. Output: `build/mii-transfer.3dsx` (plus a `.smdh`) - copy
`mii-transfer.3dsx` to `sd:/3ds/mii-transfer/` and launch it from the
Homebrew Launcher, or push it over the network with `3dslink`.

There are no tests or linters in this project.

## Running

The app needs real internet access (the portrait fetch and local server both
depend on it), so it checks for a working connection before showing
anything else - a "Checking network connection..." screen, then either the
normal UI or a full-screen error (with **START** to exit) telling you
specifically whether the wireless radio itself is off or it's on but not
connected to any access point.

- **D-Pad / Circle Pad**: move focus in the bottom-screen list (scrolls to
  keep it in view).
- **Touch**: tap a row to focus it (same as moving the D-pad cursor there -
  the top screen updates immediately); tap the already-focused row again to
  open its details screen, same as **A**. Tap either scroll arrow to jump a
  full page (6 Miis) at once instead of one row at a time.
- **L / R**: switch tabs (Mii Library / Friends / Recent Miis from games).
- **A**: open the focused Mii's details screen (gender, birthday, create ID,
  platform of origin, etc.) - **B** or a touch anywhere closes it.
- **X**: host a local HTTP server on the same network (address shown
  top-left) - **B** or a touch anywhere closes it.
- **START**: only does anything on the no-network error screen above
  ("Press START to exit") - everywhere else, close the app via **HOME**
  instead, same as any other homebrew.

On New3DS, the CPU/L2 cache clock speedup is enabled automatically for the
duration of the app (`osSetSpeedupEnable()`), and disabled again on exit.

## AI assistance disclosure

Large parts of this port (architecture/porting decisions, C++ implementation,
on-device debugging, and this documentation) were developed with the
assistance of an LLM (Claude), directed and reviewed throughout by james_rhg.
Disclosed per the spirit of
[Universal-Team/db's CONTRIBUTING.md](https://github.com/Universal-Team/db/blob/master/CONTRIBUTING.md)
guidance on declaring LLM-assisted content.

## License

Mii Transfer (3DS port)
Copyright (C) 2026 james_rhg, Arian Kordi

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details.

You should have received a copy of the GNU General Public License along
with this program, in the [LICENSE](LICENSE) file. If not, see
<https://www.gnu.org/licenses/>.
