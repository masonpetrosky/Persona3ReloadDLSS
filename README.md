# Persona 3 Reload DLSS

A ReShade add-on that puts NVIDIA DLSS in place of Persona 3 Reload's TAA: DLAA at native resolution first, then
upscaling. Free and open source (MIT).

Status: stage 2, DLAA: runs in game since 2026-10-22; upscaling (stage 3) is next. See [docs/plan.md](docs/plan.md)
and [docs/recon.md](docs/recon.md).

## How it works

At the main view's TAA dispatch (Unreal Engine 4.27's `TAAStandalone.usf`, found by its shader hash) the add-on
reads the pass's bindings and constants, turns the game's velocity buffer into DLSS motion vectors (camera motion
from depth where no object wrote velocity, as the TAA does), hands DLSS the scene color, depth, camera jitter and
the eye adaptation exposure, writes DLSS's image into the pass's output and skips the original dispatch. Bloom, the
tonemapper and the next frame then read DLSS's image. The game moves its camera jitter between only two positions;
while DLSS runs, the add-on raises `r.TemporalAASamples` to 16 (found in memory from the code that registers it).
The planar reflection view's own TAA pass is left alone.

## Requirements

An NVIDIA RTX GPU and ReShade 6.8 or newer with full add-on support, as `dxgi.dll` in `P3R\Binaries\Win64`. Works
next to RenoDX's HDR add-on.

## Building

`build.ps1`: Visual Studio with the C++ x64 tools (its CMake and Ninja). The first run fetches NVIDIA's DLSS SDK
into `third_party\dlss` (`tools\fetch_dlss_sdk.ps1`).

## Installing

With the game closed, `tools\deploy.ps1` copies `Persona3ReloadDLSS.addon64`, a default `Persona3ReloadDLSS.ini`
and `Persona3ReloadDLSS\nvngx_dlss.dll` into the game's `P3R\Binaries\Win64` folder, after copying anything it
replaces to a backup folder; `-Dev` installs NVIDIA's developer DLL instead (watermark, more logging) and
`-Uninstall` removes the add-on's files again.

## Settings

`Persona3ReloadDLSS.ini` next to the add-on; the ReShade overlay (Add-ons tab) has the same settings and shows
what the add-on is doing. The add-on logs to `ReShade.log`, prefixed `[P3RDLSS]`.

| Key | Default | |
| --- | --- | --- |
| `Enabled` | 1 | DLSS in place of the game's TAA |
| `ToggleKey` | 120 (F9) | switches between DLSS and the game's TAA in game (not while Alt, Ctrl or Shift is held) |
| `Preset` | 0 | 0 = NVIDIA's default (K); 5 = E, 10 = J, 11 = K, 12 = L, 13 = M |
| `JitterPhases` | 16 | the camera jitter's phases while DLSS runs; 0 keeps the game's |
| `GameExposure` | 1 | DLSS gets the game's eye adaptation exposure; 0 lets DLSS estimate one |
| `MotionVectors` | 0 | development: 1 camera motion only, 2 none |

## Credits and licenses

This add-on: MIT. NVIDIA's DLSS SDK under NVIDIA's license (not in the repository; `tools\fetch_dlss_sdk.ps1`
downloads it). ReShade's add-on headers and state tracking helper (BSD-3-Clause OR MIT, `third_party/reshade`),
Dear ImGui's headers (MIT, `third_party/imgui`).
