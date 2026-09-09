# KCD1VR

Experimental true-stereo OpenXR mod for **Kingdom Come: Deliverance (2018)**.

KCD1VR makes CryEngine render both eyes every game frame and submits them to
OpenXR. It does **not** use alternating-eye rendering (AER). Head tracking,
asymmetric per-eye projection, a head-locked HUD, and a flat dialogue screen
are implemented. Motion-controller interaction is not implemented; use the
normal keyboard/mouse or gamepad controls.

This is an unofficial community project and is not affiliated with Warhorse
Studios, Deep Silver, NVIDIA, Khronos, or Microsoft.

## Status

Version 0.1.67 has been tested in-headset with the Steam build of KCD 1.9.7.
The mod uses version-specific hooks, so other game builds are not currently
supported unless their addresses and signatures are independently validated.

Working in the tested build:

- native dual-render stereo with no AER;
- OpenXR head orientation, position, real IPD, and asymmetric eye FOV;
- HUD and menus on a configurable head-locked quad;
- dialogue world and UI on one comfortable monoscopic virtual screen;
- graphics-preset/resize recovery without permanently blacking out VR;
- an optional vertical-camera-input comfort lock;
- F11 headset recentering;
- VR-safe shader isolation hotkeys for diagnosing remaining effects;
- experimental NVIDIA DLSS Super Resolution.

Known limitations:

- no tracked motion-controller input;
- cutscenes and unusual camera transitions have not all been tested;
- some distant-object culling may still require a performance-heavy profile;
- DLSS is experimental and did not deliver a large net performance gain on the
  development system. Native rendering may look better for a similar cost;
- only the exact KCD 1.9.7 executable used during development is confirmed.

## Download

Download the ZIP from the [GitHub releases page](../../releases). Do not
download the repository source archive unless you intend to compile the mod.

## Requirements

- Windows 10 or 11;
- Kingdom Come: Deliverance 1.9.7 using the Direct3D 11 renderer;
- a PC VR headset and an active OpenXR runtime;
- an NVIDIA RTX GPU if the experimental DLSS path is enabled.

## Installation

1. Close KCD completely.
2. Extract the release ZIP to a temporary folder.
3. Make sure KCD's `Bin\Win64` folder does not already contain a different
   `dinput8.dll` proxy. KCD1VR cannot be installed alongside another mod that
   claims that filename.
4. Turn on the headset and ensure the desired OpenXR runtime is active.
5. Right-click `install.ps1`, choose **Run with PowerShell**, and answer the
   prompts:
   - `GameRoot`: the folder containing `KingdomCome.exe`'s `Bin` folder;
   - `DisplayScale`: multiplier applied to OpenXR's recommended eye size;
   - `DLSSPreset`: `Quality`, `Balanced`, `Performance`,
     `UltraPerformance`, or `DLAA`;
   - `DLSSRenderPreset`: `Default`, `J`, `K`, `L`, or `M`.
6. Add the following to KCD's Steam launch options:

   ```text
   +exec KCD1VR.cfg +exec KCD1VR-dlss.cfg
   ```

7. Start KCD with the headset awake. Press **F11** once in game to recenter.

The installer asks OpenXR for the headset's current recommended per-eye size,
applies `DisplayScale`, and derives the internal render size from the selected
DLSS quality mode. It writes an explicitly marked KCD1VR block to `user.cfg`
because CryEngine must receive its stereo and resolution settings before the
renderer starts. Existing settings outside that block are preserved. The
installer deliberately does not create `.bak` files.

You can also run it with named parameters:

```powershell
.\install.ps1 `
  -GameRoot 'D:\Games\KingdomComeDeliverance' `
  -DisplayScale 1.0 `
  -DLSSPreset Quality `
  -DLSSRenderPreset Default
```

If OpenXR cannot be queried during installation, provide an exact output size:

```powershell
.\install.ps1 `
  -GameRoot 'D:\Games\KingdomComeDeliverance' `
  -DisplayScale 1.0 `
  -DLSSPreset Quality `
  -DLSSRenderPreset K `
  -OutputWidth 2064 `
  -OutputHeight 2208
```

`OutputWidth` and `OutputHeight` are **per-eye output** dimensions. They do not
name the side-by-side backbuffer size.

### Native rendering without DLSS

The current installer is DLSS-aware. To retain the selected output size for
native rendering, install using the `DLAA` quality option, then edit
`Bin\Win64\KCD1VR.ini` and set:

```ini
DLSS=0
DLSSSubmit=0
```

Use only `+exec KCD1VR.cfg` in the launch options in that configuration. Other
DLSS quality choices intentionally reduce CryEngine's internal render size and
therefore should not be used as a native-resolution preset.

## Updating

Close the game and run the new release's `install.ps1` again. It replaces only
the files owned by KCD1VR and refreshes the marked block in `user.cfg`.

## Uninstalling

With the game closed, remove these KCD1VR-owned files:

- `Bin\Win64\dinput8.dll`
- `Bin\Win64\KCD1VR.ini`
- `Bin\Win64\KCD1VRResolution.exe`
- `Bin\Win64\nvngx_dlss.dll`
- `Bin\Win64\NVIDIA-DLSS-LICENSE.txt`
- `KCD1VR.cfg` and the other `KCD1VR-*.cfg` files in the game root

Then remove the lines between `BEGIN KCD1VR MANAGED SETTINGS` and
`END KCD1VR MANAGED SETTINGS` from the game-root `user.cfg`, including the two
marker lines. Do not delete unrelated content from `user.cfg`.

## Main configuration

The most useful settings are in `Bin\Win64\KCD1VR.ini`:

| Setting | Meaning |
| --- | --- |
| `WorldScale` | KCD world units per tracked OpenXR metre. |
| `HudQuad` | `1` uses the head-locked HUD/menu quad; `0` embeds UI in the eye images. |
| `HudDistance`, `HudWidth` | HUD-quad placement and size. |
| `DialogueScreen` | `1` uses the validated flat dialogue screen. |
| `DialogueScreenDistance`, `DialogueScreenWidth` | Dialogue-screen placement and size. |
| `DialogueScreenAspect` | `0` follows the live eye texture; positive values force an aspect ratio. |
| `PositionTracking` | `1` enables headset translation; `0` keeps orientation only. |
| `LockVerticalCameraInput` | Prevents mouse/gamepad pitch while retaining headset pitch. |
| `CullingFovDegrees` | Widens the center camera used by visibility and LOD decisions. |
| `DLSS` / `DLSSSubmit` | Enable DLSS and submit its output to OpenXR. |
| `DLSSQuality` | `0` Performance, `1` Balanced, `2` Quality, `3` Ultra Performance, `5` DLAA. |
| `DLSSRenderPreset` | NVIDIA model hint: `Default`, `J`, `K`, `L`, or `M`. |

The INI contains comments for every diagnostic and experimental option. Leave
unknown hook addresses unchanged.

`KCD1VR.cfg` contains the required stereo startup CVars. It also uses
`e_CoverageBufferReproj=4`, which fixed the central view-dependent blinking in
the tested build with a small performance cost. The other `KCD1VR-*.cfg` files
are optional diagnostic profiles and are not recommended as permanent defaults.

## Controls

- **F11** — recenter headset tracking.
- **F6** — capture the currently visible pixel-shader list.
- **F7 / F8** — select the next/previous captured shader and skip its draws.
- **F9** — mark and save the selected shader under
  `Bin\Win64\KCD1VR-shaders`.
- **F10** — stop shader skipping and restore all draws.

The shader browser is a diagnostic tool. Skipping a shader can remove most of
the scene, and selections are intentionally not persistent after restart.

## Troubleshooting

### The headset stays black

- Confirm the headset is awake and the intended OpenXR runtime is active.
- Remove conflicting `dinput8.dll` injectors or overlays.
- Confirm the game is using Direct3D 11.
- Fully exit KCD before relaunching; the stereo device is startup-only.
- Inspect `Bin\Win64\KCD1VR.log`. The mod refuses to split or submit a frame
  until CryEngine's backbuffer matches its live left/right eye resources.

### The resolution is wrong or supersampling changes do nothing

Rerun `install.ps1` with the headset/runtime active. The resolution is selected
at installation time, not continuously while the game is running. Use
`-OutputWidth` and `-OutputHeight` when you need an exact per-eye size.

### Dialogue UI is clipped or the character is doubled

Confirm `DialogueScreen=1`. Version 0.1.67 intentionally replaces the stereo
projection and HUD layers only while KCD's dialogue camera is active, showing
one complete eye as an opaque, monoscopic OpenXR screen. Normal gameplay and
menus retain the 0.1.64 stereo/HUD path.

### Mouse or gamepad vertical look causes discomfort

Set `LockVerticalCameraInput=1`. This removes vertical camera input from the
mouse/gamepad while preserving full headset pitch tracking.

### Performance is poor

- Test native rendering before assuming DLSS is faster on your system.
- Reduce `DisplayScale` and reinstall.
- Avoid the broad culling-disable profiles unless required; their performance
  impact can be substantial.
- Keep `DLSSGpuTiming=0` outside a targeted diagnostic run.

## How it works

1. CryEngine uses `STEREO_MODE_DUAL_RENDERING` to render left and right eyes in
   the same game frame.
2. KCD1VR intercepts DXGI `Present`, copies both eye rectangles into a two-slice
   OpenXR swapchain, and submits one stereo projection layer.
3. The next predicted headset pose is applied after KCD builds the player
   camera. CryEngine receives the runtime's real eye offsets and asymmetric
   projections.
4. Outside dialogue, the validated 0.1.64 path preserves the private eye
   textures before Scaleform draws and submits UI as a transparent quad.
5. During KCD's explicit dialogue camera, one complete rendered eye replaces
   the stereo projection/HUD combination as an opaque flat screen shown
   identically to both headset eyes.

## Building from source

Requirements:

- Windows 10 or 11;
- Visual Studio 2022 with **Desktop development with C++**;
- CMake 3.24 or newer;
- Git.

From a PowerShell prompt in the repository root:

```powershell
.\build.ps1
```

The script locates Visual Studio, downloads the pinned NVIDIA DLSS SDK when it
is absent, and configures the project. CMake downloads pinned revisions of the
Khronos OpenXR SDK/Loader and MinHook. Ready-to-install files are written to
`dist\`.

Dependency revisions are intentionally pinned in `build.ps1` and
`CMakeLists.txt` for reproducibility. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
for licenses and upstream sources.

## License

KCD1VR source code is released under the MIT License. See [LICENSE](LICENSE).
Bundled and linked third-party components retain their own licenses.
