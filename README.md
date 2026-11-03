# Persona 3 Reload DLSS

A ReShade add-on that puts NVIDIA DLSS in place of Persona 3 Reload's TAA: DLAA at native resolution first, then
upscaling. Free and open source (MIT).

Status: DLAA runs in game since 2026-10-22; upscaling (stage 3, the quality modes) built 2026-11-03, being tested. See [docs/plan.md](docs/plan.md)
and [docs/recon.md](docs/recon.md).

## How it works

At the main view's TAA dispatch (Unreal Engine 4.27's `TAAStandalone.usf`, found by its shader hash) the add-on
reads the pass's bindings and constants, turns the game's velocity buffer into DLSS motion vectors (camera motion
from depth where no object wrote velocity, as the TAA does), hands DLSS the scene color, depth, camera jitter and
the eye adaptation exposure, writes DLSS's image into the pass's output and skips the original dispatch. Bloom, the
tonemapper and the next frame then read DLSS's image. The game moves its camera jitter between only two positions;
while DLSS runs, the add-on raises `r.TemporalAASamples` to 16 and switches the engine to its temporal upsampling path
(`r.TemporalAA.Upsampling` 1, at 100%), whose jitter is the uniform Halton sequence DLSS is trained on (both found in
memory from the code that registers them). The planar reflection view's own TAA pass is left alone.

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
| `Mode` | 0 | 0 DLAA (100%), 1 Quality (67%), 2 Balanced (58%), 3 Performance (50%), 4 Ultra Performance (33%): the render scale while DLSS runs (`r.ScreenPercentage`, on the upsampling path); DLSS upscales to the output |
| `ToggleKey` | 120 (F9) | switches between DLSS and the game's TAA in game (not while Alt, Ctrl or Shift is held) |
| `Preset` | 0 | 0 = NVIDIA's default per mode (DLSS 310: K for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance); 5 = E, 10 = J, 11 = K, 12 = L, 13 = M for every mode |
| `JitterPhases` | 16 | the camera jitter's phases while DLSS runs; 0 keeps the game's |
| `UpsamplingPath` | 1 | while DLSS runs, the engine's temporal upsampling path: Halton jitter over the whole pixel and the texture mip bias below; 0 keeps the game's TAA path (jitter in a disc, mip bias 0) |
| `MipBias` | -1.0 | the texture mip bias on that path (`r.ViewTextureMipBias.Offset`, the engine's default -0.3): NVIDIA's recommendation for DLAA is -1.0; toward 0 if fine textures shimmer |
| `Reflex` | 1 | NVIDIA Reflex low-latency mode, injected at the present (the game has none), with or without DLSS |
| `FrameRateCap` | 0 | 0 keeps the game's frame rate limit; otherwise the game presents on every refresh (`rhi.SyncInterval` 1 instead of the game's 2 at 120 fps) and Reflex's sleep holds this cap (`t.MaxFPS` without Reflex); with a variable refresh rate display a few frames under its refresh rate, such as 237 at 240 Hz |
| `GameExposure` | 1 | DLSS gets the game's eye adaptation exposure; 0 lets DLSS estimate one |
| `MotionVectors` | 0 | development: 1 camera motion only, 2 none |

## HUD fix (a second add-on)

`Persona3ReloadHudFix.addon64` is for HDR mods that render the game's 8-bit back buffer onto a float copy, as RenoDX
does: with them, a bright hard-edged shape appears over the minimap while the HUD slides in after unpausing. Unreal's
HUD builds masks in the back buffer's alpha channel and blends content through them; an 8-bit target keeps every
step in 0..1, a float copy does not. After each draw whose alpha blend can leave 0..1, the add-on draws the same
geometry again, alpha only, with a Maximum(0) and a Minimum(1) blend, which is what the 8-bit target did; on a real
8-bit target that changes nothing. It is independent of the DLSS add-on and does not change RenoDX. The overlay (Add-ons
tab) switches it off for a comparison and shows how many draws it clamps; `Persona3ReloadHudFix.ini` keeps that
switch. Install with `tools\deploy_hud_fix.ps1` (backup first, `-Uninstall` to remove). Details in
[docs/recon.md](docs/recon.md) and [docs/plan.md](docs/plan.md).

## Credits and licenses

This add-on: MIT. NVIDIA's DLSS SDK under NVIDIA's license (not in the repository; `tools\fetch_dlss_sdk.ps1`
downloads it). ReShade's add-on headers and state tracking helper (BSD-3-Clause OR MIT, `third_party/reshade`),
Dear ImGui's headers (MIT, `third_party/imgui`).
