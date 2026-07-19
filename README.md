# DayZ OpenXR VR Mod

An experimental, unofficial VR prototype for the Windows x64 version of DayZ. It presents DayZ through an OpenXR runtime and modifies selected rendering, camera, GUI, and mouse-input behavior inside the running game. The proxy currently recognizes the matching retail `DayZ_x64.exe` and diagnostic `DayZDiag_x64.exe` builds.

## Showcase video

[![DayZ OpenXR VR Mod](https://img.youtube.com/vi/35tf5tsaD5c/0.jpg)](https://youtu.be/35tf5tsaD5c)

> [!WARNING]
> This mod is **not compatible with anti-cheat software**. It loads a local `dxgi.dll` proxy and installs hooks in the game process, behavior that anti-cheat systems can detect or block. Do not use it on BattlEye-protected or other anti-cheat-enabled servers. Use it only in a private, offline, or otherwise explicitly permitted environment. You assume all risk, including crashes, account restrictions, or bans. For the mod to work, the server must have anti-cheat disabled using the program https://github.com/JonathanEke/DayZ-Server-Battleye-Remover

## How it works

The mod is loaded as a local DXGI proxy placed next to `DayZ_x64.exe`. The proxy forwards the normal DXGI factory exports to the Windows system library, intercepts swap-chain creation and presentation, and passes rendered frames to an OpenXR session through Direct3D 11.

The runtime component contains build-specific DayZ hooks used to observe and adjust the camera/render pipeline, alternate eye state, apply HMD rotation, remap GUI coordinates, and composite the interface for VR. Because these hooks rely on executable offsets, a DayZ update may make the mod stop working until the offsets are updated.

The supported executable identities are checked before any game hook is installed:

```text
DayZ_x64.exe      PE timestamp 0x6A47B9AA, SizeOfImage 0x04407000
DayZDiag_x64.exe  PE timestamp 0x6A47BAF9, SizeOfImage 0x049E7000
```

Support for `DayZDiag_x64.exe` is based on static signature relocation and should be treated as runtime-experimental until its startup log confirms that the diagnostic profile and all render signatures were accepted.

The system cursor is deliberately locked to the center of the game window while GUI cursor mode is active. Mouse movement updates a separate virtual cursor in DayZ's GUI coordinate space. This custom cursor remains aligned when the GUI is resized and is drawn into the VR-visible interface layer; the normal desktop cursor is therefore not used for interaction.

Main-menu and inventory rendering is captured separately from the normal in-game HUD and submitted as a world-locked OpenXR quad. The quad is anchored in front of the HMD whenever cursor-driven GUI mode opens; closing it leaves the existing gameplay HUD path unchanged. Its texture resolution, physical width, distance, and vertical offset are configurable in the `[gui]` section of `dayz_openxr.ini`.

The same section contains `inventory_hmd_look`, which keeps visual HMD look active while DayZ owns mouse input for the in-game inventory; `inventory_preview_rotation_scale`, which tunes only the character-preview compensation (item previews remain on the GUI path); and `inventory_blur_enabled`, which can suppress DayZ's Gauss blur only while the inventory character-preview camera is active.

`inventory_player_preview_visible=false` hides the inventory character preview without changing the item panels. The `[controls]` section enables tracked-controller input and diagnostic XYZ axes. The right aim pose drives the virtual cursor by ray-intersecting the world-locked GUI quad. This pointer ray is displayed only while the main menu or inventory is visible. `hmd_position_scale` in `[stereo]` controls physical HMD translation applied to the DayZ camera. OpenXR always submits the headset's native FOV. `game_fov` is an absolute gameplay FOV in radians that replaces DayZ's loaded `fov` value only in memory; the user's `.DayZProfile` is never written. Set it to `0` to disable the override.

## Controller mapping

Controller buttons mirror their emulated keyboard or mouse state: the emulated input goes down when the controller input goes down and is released when the controller input is released. `LGRAB` acts as a modifier for the left face buttons.

| Controller input | DayZ input |
| --- | --- |
| Left stick | `W`, `A`, `S`, `D` |
| Right stick, horizontal | Mouse horizontal turn |
| `X` | `C` |
| `Y` | `R` |
| `LGRAB + X` | `Esc` (menu) |
| `LGRAB + Y` | `Tab` (inventory) |
| `LGRAB + A` | Previous quickbar slot (`1` through `0`, cyclic) |
| `LGRAB + B` | Next quickbar slot (`1` through `0`, cyclic) |
| `RGRAB` | `F` |
| `A` without `LGRAB` | `Shift` |
| `B` without `LGRAB` | `Space` |
| Left trigger | Right mouse button |
| Right trigger | Left mouse button |

The quickbar cycle is maintained by the mod and emits the corresponding number key. Selecting a slot directly on the physical keyboard does not currently resynchronize the mod's cycle position.

The solution also includes `xr_probe.exe`, a standalone OpenXR/D3D11 diagnostic application. It can verify the active OpenXR runtime and headset before the proxy is loaded into DayZ.

## Current limitations

- This is an experimental prototype tied to a specific DayZ executable build.
- It must not be used with anti-cheat enabled.
- UI element sizing is currently derived from the frame height. With a square render resolution, interface elements may therefore appear much larger than expected.
- DayZ must run in windowed mode so the render resolution can be changed in `\Documents\DayZ\DayZ.cfg` while testing. Close the game before editing the file, then set the desired window dimensions there and restart DayZ.
- The physical system cursor is intentionally locked while the custom GUI cursor is active.
- Do not combine this proxy with ReShade, Special K, or another local `dxgi.dll`; proxy chaining is not implemented.

## Requirements

- Windows 10 or 11 x64
- Visual Studio with the C++ desktop workload, MSVC v145 toolset, and Windows 10 SDK
- A working OpenXR runtime and connected VR headset (SteamVR is the default expected runtime)
- OpenXR headers and `openxr_loader.lib`; the project defaults to the copy included with Unreal Engine 4.26
- MinHook sources in `third_party/minhook` (included in this source tree)

The project files currently default to:

```text
OpenXrSdkDir=C:\Program Files\Epic Games\UE_4.26\Engine\Source\ThirdParty\OpenXR
OpenXrRuntimeDir=C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64
```

If your files are elsewhere, override the `OpenXrSdkDir` and `OpenXrRuntimeDir` MSBuild properties or update them in both `.vcxproj` files.

## Building

### Visual Studio

1. Open `vr_mod.slnx` in Visual Studio.
2. Select the `x64` platform. The projects do not define an x86 build even if the solution UI lists x86.
3. Select `Release` (recommended) or `Debug`.
4. Build the solution.

### Developer command prompt

From a Visual Studio Developer PowerShell or Developer Command Prompt, run:

```powershell
msbuild vr_mod.slnx /m /p:Configuration=Release /p:Platform=x64
```

To override the dependency paths from the command line:

```powershell
msbuild vr_mod.slnx /m /p:Configuration=Release /p:Platform=x64 `
  /p:OpenXrSdkDir="C:\path\to\OpenXR" `
  /p:OpenXrRuntimeDir="C:\path\to\SteamVR\bin\win64"
```

Build outputs are kept separate:

- `bin\Release\proxy\` contains the DayZ proxy build.
- `bin\Release\probe\` contains the standalone OpenXR probe.

The post-build step copies `openxr_loader.dll` and `dayz_openxr.ini` into each output directory.

## Testing the OpenXR runtime

1. Make SteamVR, or another compatible runtime, the active OpenXR runtime and connect the headset.
2. Run `bin\Release\probe\xr_probe.exe`.
3. Confirm that the headset displays the diagnostic grid. Press Esc to exit.
4. If initialization fails, inspect `xr_probe.log` beside the executable.

## Installation

1. Build the `Release|x64` configuration.
2. Make sure DayZ is closed.
3. Copy these files from `bin\Release\proxy\` into the DayZ installation directory beside `DayZ_x64.exe`:
   - `dxgi.dll`
   - `openxr_loader.dll`
   - `dayz_openxr.ini`
4. Configure the mod in `dayz_openxr.ini`. Keep `[openxr] enabled=true` to start the OpenXR path.
5. Configure DayZ to run in windowed mode.
6. Close DayZ before changing resolution, edit `\Documents\DayZ\DayZ.cfg`, set the desired window width and height, save the file, and start the game again.
7. Start the active OpenXR runtime and headset before launching DayZ.
8. Launch DayZ only in an environment where anti-cheat is disabled and mod usage is permitted.

For a cautious forwarding-only test, first set `[openxr] enabled=false`, launch DayZ, and verify that the DXGI proxy loads without changing normal rendering. Close the game, re-enable OpenXR, and test again.

## Configuration notes

`dayz_openxr.ini` controls OpenXR startup, logging, swap-chain hooks, stereo presentation, image fitting and scaling, HUD experiments, GUI mouse remapping, the custom cursor, and the optional render-resolution override. Experimental options may be incomplete or specific to the currently supported DayZ build.

When using a square game image, expect oversized UI elements because the current UI scale follows frame height. Adjust the window resolution in `\Documents\DayZ\DayZ.cfg` and the relevant HUD settings in `dayz_openxr.ini` as needed.

## Uninstallation

Close DayZ and remove `dxgi.dll`, `openxr_loader.dll`, and `dayz_openxr.ini` from the directory containing `DayZ_x64.exe`. Runtime log files such as `dayz_openxr.log` can also be removed.

## Disclaimer

This project is unofficial, experimental, and provided as-is without warranty. It is not affiliated with or endorsed by Bohemia Interactive, DayZ, Valve, SteamVR, Khronos, or any headset manufacturer. DayZ and all related trademarks belong to their respective owners. You are responsible for complying with the game's terms, server rules, software licenses, and all applicable platform policies.
