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

| DLSS input | From the TAA dispatch (UE 4.27, to be confirmed in the captures) |
| --- | --- |
| color | `InputSceneColor`: linear HDR, pre-exposed by `View.PreExposure` |
| depth | `SceneDepthTexture`: reverse Z, so DLSS's inverted-depth flag |
| motion vectors | our own pass: `GBufferVelocity` where an object wrote it (0,0 = not written), otherwise camera motion from depth and `View.ClipToPrevClip`, the way the TAA does it; in pixels, not dilated |
| jitter | `TemporalJitterPixels` in the TAA's parameters, or `View.TemporalAAJitter` |
| reset | the TAA's `bCameraCut` |
| exposure | the pre-exposure value, plus DLSS auto exposure or the engine's eye adaptation texture |
| output | the TAA's output texture (the new history, which the post-processing reads) |

Expected obstacles:
- **Jitter.** The game's per-area table `DT_FldTemporalAATable` sets `r.TemporalAASamples` (and the current frame
  weight) at each area load through console commands; Ultra+ notes it forces 2 samples. Two jitter positions are
  far too few for DLSS. The add-on keeps `r.TemporalAASamples` at a DLSS-friendly count (8 times the inverse square
  of the render scale) by writing the variable's storage, found from the code that registers it, as
  FF7RebirthNpcMotion does for FF7 Rebirth. `r.TemporalAA.Upsampling=1` may be useful even at 100%: in stock 4.27 it
  should switch the jitter from a Gaussian-weighted to a uniform Halton sequence, which is what DLSS expects (to
  check in the captures).
- **Other TAA dispatches.** The same shader file also filters screen space reflections, light shafts and depth of
  field, and scene captures (menus, Persona previews: the game's OffscreenRendering plugin) may run their own TAA.
  Only the main view's pass is replaced: matched by shader hash, then by its bindings and output size.
- **Extra outputs.** If the main pass also writes a half-size copy for later passes, the add-on must write it too,
  or first keep the game's TAA running and overwrite its output (simple, costs the TAA's time).
- **Denuvo.** No code patches unless there is no other way. Console variables are data.

## Stages

1. **Recon** (RenderDoc captures plus the executable). Exit: the TAA dispatch's shader hashes, root signature and
   bindings; the constant buffer offsets of everything in the table above; velocity format and encoding; the
   jitter sequence over consecutive frames; which other passes read the TAA's outputs.
2. **DLAA.** DLSS at native resolution replaces the main TAA pass; the motion vector pass; the jitter override;
   camera cuts; pre-exposure; ReShade overlay settings and an ini; a toggle key. Exit: the user plays with it and
   prefers it to the game's TAA.
3. **Upscaling.** `r.ScreenPercentage` with the engine's own temporal upsampling path, replaced by DLSS at that
   dispatch; check the texture mip bias; render scale in the overlay. Exit: in-game test at 50-67%.
4. **Release.** README, GitHub, Nexus (each confirmed with the user).

Later, if wanted: DLSS Frame Generation and Reflex, as in EldenRingDLSS 0.4.0.

## Workflow

- Captures: `tools\launch_rdc.bat` (opened through explorer.exe) starts the game under RenderDoc with ReShade off for
  the session; `tools\rdc_trigger.py` (inside qrenderdoc) triggers captures; `tools\rdc.sh` runs analysis scripts
  headless. DLSS does not run under RenderDoc.
- Game folder changes (ReShade, the add-on) need the user's OK, the game closed, and a backup in
  `Documents\Backups\Persona3Reload\<date>-before-<change>`.
- The user plays and judges; measurements (motion vectors, flicker, timings) come from our own captures and logs.
