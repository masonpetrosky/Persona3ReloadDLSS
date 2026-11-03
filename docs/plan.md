# Persona 3 Reload DLSS: plan

Goal: NVIDIA DLSS in place of Persona 3 Reload's TAA, first as DLAA at native resolution, then as upscaling. A
ReShade add-on, free and open source (MIT), built like [EldenRingDLSS](https://github.com/masonpetrosky/EldenRingDLSS).

## Why a new mod (survey of 2026-10-22)

- No DLSS, DLAA, FSR or XeSS mod for the game on Nexus (67 mods) or GameBanana.
- Luma's generic Unreal Engine mod replaces UE4's TAA with DLSS, but only in DX11 games; its wiki lists Persona 3
  Reload as "Doesn't boot in DX11".
- OptiScaler needs an upscaler the game already calls (DLSS, FSR 2+ or XeSS); the game ships none.
- HDR is covered by RenoDX's "Unreal Engine Extended" add-on, which lists the game as working and has settings and
  shader fixes made for it. This add-on must work next to it, in the same ReShade.

## The game

Steam app 2161700, build 22672075 (exe linked 2026-04-02), Unreal Engine 4.27.2, D3D12 only, ray traced reflections
and ambient occlusion, Denuvo. Details and sources in [recon.md](recon.md).

## Approach

A ReShade add-on (D3D12) that finds the main TAA dispatch of every frame and hands DLSS the same inputs:

| DLSS input | From the TAA dispatch (UE 4.27; confirmed in the captures, slots and offsets in [recon.md](recon.md)) |
| --- | --- |
| color | `InputSceneColor` (t1): linear HDR, pre-exposed by `View.PreExposure` (1.0 so far) |
| depth | `SceneDepthTexture` (t2): reverse Z, so DLSS's inverted-depth flag |
| motion vectors | our own pass: the velocity buffer (t3) where an object wrote it (0 = not written), otherwise camera motion from depth and `View.ClipToPrevClip`, the way the TAA does it; in pixels, not dilated; neither has jitter in it |
| jitter | `View.TemporalAAJitter` (the non-upsampling permutation has no `TemporalJitterPixels`) |
| reset | the TAA's `bCameraCut` (`$Globals` +8) |
| exposure | the eye adaptation texture (t0, 1.0 so far) and `View.PreExposure` |
| output | the TAA's output texture (u0: the new history, which the post-processing reads) |

Expected obstacles:
- **Jitter.** The game's per-area table `DT_FldTemporalAATable` sets `r.TemporalAASamples` (and the current frame
  weight) at each area load through console commands; Ultra+ notes it forces 2 samples. Two jitter positions are
  far too few for DLSS. The add-on keeps `r.TemporalAASamples` at a DLSS-friendly count (8 times the inverse square
  of the render scale) by writing the variable's storage, found from the code that registers it, as
  FF7RebirthNpcMotion does for FF7 Rebirth. `r.TemporalAA.Upsampling=1` may be useful even at 100%: in stock 4.27 it
  should switch the jitter from a Gaussian-weighted to a uniform Halton sequence, which is what DLSS expects (to
  check in the captures).
- **Other TAA dispatches.** A planar reflection view runs the same TAA permutation each frame (without jitter or
  velocity), and scene captures (menus, Persona previews: the game's OffscreenRendering plugin) may run their own
  TAA. Only the main view's pass is replaced: matched by shader hash, then by its velocity binding (a real buffer,
  not the 1x1 dummy).
- **Extra outputs.** None: the main pass writes only its history/output texture; the half-size bloom input is a
  separate draw that reads it.
- **Denuvo.** No code patches unless there is no other way. Console variables are data.

## Stages

1. **Recon** (RenderDoc captures plus the executable). Exit: the TAA dispatch's shader hashes, root signature and
   bindings; the constant buffer offsets of everything in the table above; velocity format and encoding; the
   jitter sequence over consecutive frames; which other passes read the TAA's outputs.
2. **DLAA.** DLSS at native resolution replaces the main TAA pass; the motion vector pass; the jitter override;
   camera cuts; pre-exposure; ReShade overlay settings and an ini; a toggle key. Exit: the user plays with it and
   prefers it to the game's TAA. Status 2026-10-22: built (`src/addon.cpp`, `src/motion_vectors.hlsl`; installs with
   `tools\deploy.ps1`) and running in game since ec95957 (DLAA at 3840x2160, F9 A/B). The user: better on thin hair,
   on a near pole and on a back wall (the TAA's horizontal lines are gone); moving ceiling fan blades look "a little
   wavy". 2026-11-03: an hour of play, about 420,000 DLSS frames without a failure. The fan check in the captures
   ([recon.md](recon.md)) found DLSS's inputs right: the fan writes velocity, the motion vectors and the jitter match
   the images, and no pixel has the responsive AA bit, so neither planned fix (that bit as the bias-current-color
   mask, `r.VertexDeformationOutputsVelocity`) applies, and NVIDIA advises against dilating the motion vectors
   ourselves. The one departure from NVIDIA's guidance is UE's 16-phase jitter pattern (a Gaussian disc that never
   samples the pixel's corners), which the engine's temporal upsampling path replaces with uniform Halton: stage 3.
3. **Upscaling.** `r.ScreenPercentage` with the engine's own temporal upsampling path, replaced by DLSS at that
   dispatch; check the texture mip bias; render scale in the overlay. Exit: in-game test at 50-67%. First step: a
   capture with `r.TemporalAA.Upsampling 1` at 100% (`tools\launch_rdc_upsampling.bat`: `[SystemSettings]` in the
   user's `%LOCALAPPDATA%\P3R\Saved\Config\WindowsNoEditor\Engine.ini` for that session only) for the upsampling
   permutation's bindings and its jitter; DLAA through that path then gets uniform Halton jitter too.
4. **Release.** README, GitHub, Nexus (each confirmed with the user).

Later, if wanted: DLSS Frame Generation and Reflex, as in EldenRingDLSS 0.4.0.

## Workflow

- Captures: `tools\launch_rdc.bat` (opened through explorer.exe) starts the game under RenderDoc with ReShade off for
  the session; `tools\rdc_trigger.py` (inside qrenderdoc) triggers captures; `tools\rdc.sh` runs analysis scripts
  headless. DLSS does not run under RenderDoc.
- Game folder changes (ReShade, the add-on) need the user's OK, the game closed, and a backup in
  `Documents\Backups\Persona3Reload\<date>-before-<change>`.
- The user plays and judges; measurements (motion vectors, flicker, timings) come from our own captures and logs.
