# Forza Horizon 3 - Extra Options (Azimuth)

An unofficial port of [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
by Arthur (RokkuDayo) to the **Xbox One version of Forza Horizon 3**, running on the Azimuth
translation layer.

The same `winmm.dll` serves both games. When Azimuth loads Forza Horizon 2, the original
FH2 Extra Options 0.4.4 runs unchanged. When it loads Forza Horizon 3, a separate FH3
profile takes over with its own settings file and log. FH2 code is never entered for FH3.

> **WARNING (from the original project):** there is **NO** "All-In-One" installer, and
> anything that asks you to paste commands into PowerShell is a **scam**. Only download
> releases from this repository or from the original FH2 repository.

## Features

| Setting (`FH3_ExtraOptions.ini`) | What it does | Patches | Tested in-game |
|---|---|---|---|
| `[Frontend] SkipIntro` | Skips the developer and publisher opening videos | 4 | Yes |
| `[Content] OfflineVIP` | VIP status: VIP wheelspins, VIP rewards and the profile VIP flag | 5 | Yes |
| `[Content] UnlockTreasureMap` | Treasure Map for the mainland, Blizzard Mountain and Hot Wheels | 5 | Not yet |
| `[Content] UnlockDLC` | Every car pack, Car Pass car and both expansions | 1 | Not yet |
| `[Graphics] SmoothSunShadows` | Smooth sun shadows: all four cascades refresh every frame | 6 | Not yet |
| `[Graphics] BlurRimThreshold` | Wheel speed at which the 3D rim is swapped for the flat blurred rim | 2 | Yes |
| `[Compatibility] DisableAzimuthSaveChecksums` | Same as FH2: bypass Azimuth's save size/hash rejection (off by default) | - | - |

Notes:

- **UnlockDLC.** Build 1.0.125.0 already ships every car pack and both expansions in its base
  media and database. Only `ContentOffers` ownership gates them: the game resets every offer
  with `UPDATE ContentOffers SET IsPurchased=0` at startup. This statement becomes `=1`, the
  same approach as FH2's `catalog_purchased`. It also marks the VIP, Treasure Map, horn and
  token offers as purchased.
- **SmoothSunShadows** costs GPU time. It renders four shadow cascades per frame instead of
  roughly one. FH3 adds a staggered mode that FH2 does not have, with the schedule
  `1,0,2,0,1,0,3,0,...`, so the far cascade updates only every 16th frame. This feature
  disables that mode as well.
- **BlurRimThreshold** is a percentage of the game's blur `MaxRPM` (350). `50` is the game
  default, about 15 mph. Any value above `100` (default `200`) never swaps, so the real 3D
  rim always spins. The stock swap is instant because `GeomBlendStart` equals
  `GeomBlendEnd` (0.5) in `GlobalCarAttributes.xml`, and no car shader reads
  `g_BlurRimGeomBlend`, so the game cannot cross-fade the two meshes. At very high speed a
  real rim can show the wagon-wheel effect; `BlurRimThreshold=90` swaps late instead.

Everything else from FH2 Extra Options (garage from pause, autodrive, drift camera, FOV,
vehicle lights, video menu, rims, mod loader and so on) is **not** ported to FH3. The original
feature list is kept in [docs/FH2_README.md](docs/FH2_README.md).

## Requirements

- Azimuth 1.0.1, 1.0.2, 1.0.3 or 1.0.4 (the same builds the FH2 mod supports).
- Forza Horizon 3 for Xbox One, executable version **1.0.125.0** (title ID `4CE1DFAB`):
  `forza_durango_release_final.exe`, 85,425,664 bytes, SHA-256
  `e6a362a2fde118577ab3018c8e042e62a3bb42f83a63ace41145c0864527a661`.

Any other executable is refused and nothing is patched.

## Installation

1. Download `FH3_ExtraOptions_Azimuth.zip` from [Releases](../../releases).
2. Close Azimuth.
3. **Back up your saves:** `<Azimuth folder>\data\titles\4CE1DFAB\saves`.
4. Extract `winmm.dll` and `FH3_ExtraOptions.ini` into the Azimuth folder, next to
   `azimuth.exe`, replacing existing files.
5. Start Forza Horizon 3 through Azimuth.

`FH3_ExtraOptions.log` appears next to the DLL. A successful start lists 23
`FH3: patched ...` lines and `OK: requested FH3 tweaks applied in memory.`

If you turn `UnlockDLC` off after using DLC cars, those cars may stay in or vanish from your
garage. Keep the save backup.

Uninstall: delete `FH3_ExtraOptions.ini` and `FH3_ExtraOptions.log`, then restore your
previous `winmm.dll`, or delete it if you use no other mods.

## How it works

- Nothing on disk is modified. Azimuth hands the mapped game image to the DLL before the game
  starts, and the FH3 profile patches code and data **in memory**.
- The executable is checked by size, SHA-256, PE timestamp and image size before any write.
- Every requested patch site is compared with its expected original bytes first. If any site
  differs, **no byte is changed**.
- Patch sites were found statically. Each FH2 Extra Options patch was matched to the equivalent
  FH3 code through shared strings, config-flag offsets, MSVC RTTI vtables (for example
  `ShadowMapRendererAnthem11`) and identical instruction shapes. The game database and shaders
  were inspected for the DLC and wheel features.

The full patch table, with the reasoning for every site, lives in
[features/fh3.c](features/fh3.c).

| Group | Sites (RVA) |
|---|---|
| Intro | `0x1724ab6`, `0x17265f0`, `0x1727e32`, `0x1729558` (FH2 `+0x319b` / FH3 `+0x32d8` skip-intro flag) |
| VIP | `0x3499e5`, `0x33c91e`, `0x3c5b74`, `0x992770`, `0x9826c8` (`Main/IsVip` readers) |
| Treasure Map | getters `0x1071870`, `0x10713f0`, `0x1071450`; direct readers `0x4a53c4`, `0x151f3f5` |
| DLC | `0x2d30565` (`IsPurchased=0` becomes `=1`) |
| Sun shadows | `0x1b7d459`, `0x1b82711`, `0x1b82dcd` (FH2 equivalents); `0x1b80a50`, `0x1b80a58`, `0x1b7d4a6` (FH3 staggered mode) |
| Blur rims | `0x1e37b6d` (threshold load), `0x1e37c8c` (threshold value in padding) |

## Building

Requires Python 3.10+ and Zig 0.15.2, exactly as for FH2 (see [BUILD.md](BUILD.md)):

```text
python build.py /path/to/zig.exe --azimuth
```

The FH3 profile (`features/fh3.c`) is part of every Azimuth build. The Mezcal (`xg.dll`)
build is FH2 only.

To re-check every FH3 patch site against your own copy of the executable (needs `pefile` and
`capstone`):

```text
set FH3_EXE=X:\path\to\Forza Horizon 3\Mount\forza_durango_release_final.exe
python tools/fh3_port/check_fh3_patches.py
```

`tools/fh3_port/` also contains the small helpers used during the port:

- `xbox_image.py` - PE and xref helpers;
- `rtti.py` - MSVC RTTI vtable lookup;
- `dxbc_dis.py` - DXBC shader disassembly through the system `d3dcompiler_47.dll`.

## Changes to the FH2 code

These are the only changes to existing FH2 files. FH2 behaviour is unchanged: the code path
for title `1A96545B` is the same as in FH2 Extra Options 0.4.4.

- `adapters/azimuth.c`: accepts title `4CE1DFAB` and routes it to `fh3_initialize`.
- `adapters/azimuth_save.c`, `adapters/azimuth_markers.c`, `adapters/azimuth_host.h`: save
  compatibility and marker repair take a title ID; FH2 keeps calling them with `1A96545B`.
- `logging.c/.h`: `log_set_file` lets the FH3 profile write its own log.
- `kernel32.def`: imports `GetPrivateProfileIntW`.
- `build.py`: compiles `features/fh3.c` into Azimuth builds, and skips includes that are not
  shipped (`features/fm5.h`) when scanning dependencies.

## Credits and license

- **Forza Horizon 2 Extra Options**: Arthur (RokkuDayo),
  <https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions>. All FH2 features, the DLL
  architecture, the Azimuth and Mezcal adapters and the patch engine come from that project.
- **Forza Horizon 3 port**: alezwork, November 2026. This is a modified version of the original
  work: the FH3 profile was added and the files listed above were changed.
- The FH3 reverse engineering and code were done with the help of Claude (Anthropic), in the
  same spirit as the original project's AI usage disclosure.

Licensed under the GNU General Public License v3.0, the same as the original. See
[LICENSE](LICENSE).

This project is not affiliated with Microsoft, Playground Games, Turn 10 Studios or the
Azimuth authors. Forza Horizon is a trademark of Microsoft.
