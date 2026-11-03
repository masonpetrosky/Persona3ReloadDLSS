# Persona 3 Reload: recon log

## Offline findings (2026-10-22)

### Install

- Steam app 2161700, build 22672075, installed 2026-10-22 in `C:\Program Files (x86)\Steam\steamapps\common\P3R`.
  Steam starts `P3R\Binaries\Win64\P3R.exe` with no arguments.
- `P3R.exe` (366 MB) is linked 2026-04-02 and has no version resource. Its sections (`.arch`, `.xcode`, `.xtext`,
  `.srdata`, `.udata`, `.xpdata`, `.data2` beside a 254 MB `.text`) are Denuvo's.
- Unreal Engine 4.27.2 (Luma's wiki), D3D12 only: the game does not start in DX11 (same source). RenoDX's P3R
  shader fixes are `ps_6_0`, so the game's shaders are DXIL.
- No upscaler libraries in the install (no `nvngx_dlss.dll`, FSR or XeSS); the exe names only
  `FDefaultTemporalUpscaler`. Third-party DLLs: GeForce NOW runtime, Aftermath, PhysX/APEX, OpenImageDenoise, TBB.
- User config `%LOCALAPPDATA%\P3R\Saved\Config\WindowsNoEditor` (first written 2026-10-22 20:15):
  3840x2160, `FullscreenMode=0`, VSync on, `FrameRateLimit=60`, `sg.ResolutionQuality=100`,
  `sg.AntiAliasingQuality=3`, shadows 2, the rest 3. `WinGDK` holds the user's 2024 Game Pass config (same settings).
  The game writes no `D3DDriverByteCodeBlob_*` there, so it does not seem to use the driver PSO disk cache that
  made FF7 Rebirth assert under RenderDoc.

### Strings in P3R.exe (`tools/exe_strings.py`)

- Gen4 TAA shaders: `/Engine/Private/TemporalAA/TAAStandalone.usf`; the Gen5 set (`TAADilateVelocity.usf`,
  `TAAUpdateHistory.usf`, ...) and `r.TemporalAA.Algorithm` are present too (stock 4.27).
- `r.TemporalAA.Upsampling`, `r.TemporalAA.Upscaler` ("GTemporalUpscaler which may be overridden by a third party
  plugin"), `r.TemporalAA.HistoryScreenPercentage`, `r.TemporalAA.AllowDownsampling`,
  `r.TemporalAA.R11G11B10History`, `r.TemporalAAFilterSize`, `r.TemporalAACatmullRom`.
- Game code that drives the TAA per area: the data table `/Game/Xrd777/Field/Data/DataTable/DT_FldTemporalAATable`
  (`FldTemporalAATableRow`, `EFldDataTable::TemporalAAList`) and the console command formats
  `r.TemporalAASamples %d` and `r.TemporalAACurrentFrameWeight %.1f`. Ultra+ says the game forces 2 samples at each
  level load and ships an add-on that raises it to 8.
- `r.ScreenPercentage %f` is formatted somewhere too (the game sets the screen percentage itself).
- View uniform buffer members named in the exe: `TemporalAAJitter`, `TemporalAAParams`.

### Other mods (for reference, not used)

- [RenoDX](https://github.com/clshortfuse/renodx/wiki/Mods) "Unreal Engine Extended" (marat569's snapshot): the
  game is listed as working with `Upgrade Path: On`; its source has `P3R.exe` settings
  (`Upgrade_B8G8R8A8_TYPELESS` = output size) and P3R shader fixes (`godray_0xCB9976C8`, `mainmenu_text_0xBFE48347`,
  July 2026). The legacy generic UE mod listed it as in progress ("Map background is bright when animating in").
- [Luma](https://github.com/Filoppi/Luma-Framework/wiki): generic UE4 TAA -> DLSS replacement for DX11 only; P3R "Doesn't
  boot in DX11". Its approach (decode velocity and camera motion into DLSS motion vectors, DLSS before tonemapping in
  linear HDR) is the standard one; we write our own.
- [P3RFix](https://codeberg.org/Lyall/P3RFix) (Lyall): custom resolutions, ultrawide, developer console, render
  scale override, pause on focus loss control.
- [Ultra+](https://www.nexusmods.com/persona3reload/mods/71): engine.ini tuning; add-ons `EnableRT` and `TAASamples`.
- Stuttering Fix DX12 Async Compile (Nexus 82, a `d3d12.dll` proxy): confirms the game is D3D12 and that it
  "bypasses the usual private command-list vtable routes during active rendering".

## Captures (2026-10-22)

`captures/p3r_frame10211.rdc`, `10212` and `10213` (git-ignored, 1.2 GB each): three consecutive frames in the dorm
lounge ("4/17 Fri Evening"), the protagonist walking, the camera still. Taken with `tools/launch_rdc.bat` (ReShade,
RenoDX and SCSKiller's recorder switched off for the session) and `tools/rdc_trigger.py`. Analysis scripts:
`rdc_quick.py` (overview), `rdc_survey.py` (every action with its bindings: `captures/survey_10211`),
`rdc_inspect.py` (full state, constant buffers and disassembly of chosen events: `captures/inspect_10211`) and
`rdc_taa.py` (the TAA dispatches of several captures side by side: `captures/taa_frames.txt`).

### Frame layout (10211)

3198 actions (2809 draws, 145 dispatches, one present), no debug markers. The scene is rendered twice:

1. Events 1107-13843: a **planar reflection** view, 2880x1620 (75%) in the 3840x2160 scene textures: its own
   shadows, G-buffer, velocity, lights and TAA (13829), copied (13843) into a 2880x1620 target that the main view
   composites into its reflections at 21135 (after the screen space reflections at 21125).
2. Events 13847-21585: the **main view** at 3840x2160: shadows, G-buffer (258 draws), velocity (91 draws into
   `tex29598`), lights, translucency, separate translucency composite (21563), a post-process material (21578, a
   UE "before tonemapping" material, which 4.27 runs before TAA) and the **main TAA at 21585**.
3. After the TAA: a half-size copy for bloom (21597, 1920x1080 R11G11B10), the eye adaptation readback (21599
   copies the 1x1 eye adaptation texture into a 16-byte buffer), eye adaptation (21674), bloom, the tonemapper
   (21908, which reads the TAA output, the bloom and a 32x32x32 LUT), the back buffer (21923), the UI at 1920x1080,
   present.

### The TAA dispatch

Compute shader `TAAStandalone.usf` `MainCS`, DXIL hash `203152694ac6f5d3b98e5464598f5669` (SM 6.0, 8x8 threads,
wave intrinsics). Both views use this permutation; the dispatch is 480x270 groups for the main view. Root
signature (UE's compute root signature): `[0]` SRV table t0-t63, `[1]` sampler table s0-s15, `[2]` UAV table u0-u15,
`[3]` root CBV b0 (`$Globals`, 412 bytes), `[4]` root CBV b1 (`View`, 5220 bytes). Resource names are stripped from
the DXIL, but the constant buffer layouts are kept, and they match UE 4.27.

| Slot | Main view (21585) | Planar reflection (13829) | UE parameter |
| --- | --- | --- | --- |
| t0 | 1x1 R32G32B32A32_FLOAT, value 1.0 | 1x1 B8G8R8A8 dummy | eye adaptation (rotates over three textures) |
| t1 | 3840x2160 R16G16B16A16_FLOAT | scene color itself | `InputSceneColor` (linear HDR, max ~4.6 here) |
| t2 | 3840x2160 D32S8, depth plane | same | `SceneDepthTexture` (reverse Z, near plane 10) |
| t3 | 3840x2160 R16G16B16A16_UNORM | 1x1 B8G8R8A8 dummy | `GBufferVelocityTexture` |
| t4 | same D32S8, stencil plane (`uint2`) | same | stencil; the shader tests bit 3 (value 8), responsive AA |
| t5 | 3840x2160 R16G16B16A16_FLOAT | same | `HistoryBuffer[0]` (the previous frame's output) |
| u0 | 3840x2160 R16G16B16A16_FLOAT | same | `OutComputeTex[0]`, the new history and the TAA output |

There is no second output (the half-size bloom input is a separate draw), so DLSS can write straight into u0. The
history textures are pooled: three 3840x2160 RGBA16F textures rotate between the two views' histories and outputs
from frame to frame, so the add-on must take the bindings at each dispatch. The planar reflection pass is told apart
by its dummy velocity texture (and by its zero jitter).

`$Globals` (b0, 412 bytes, offsets in bytes): `HistoryPreExposureCorrection` +0 (1.0), `CurrentFrameWeight` +4
(0.2, the game's table), `bCameraCut` +8 (int), `SampleWeights[9]` +16 (unused here), `PlusWeights[5]` +160 (one per
16-byte row), `ViewportUVToInputBufferUV` +240, `MaxViewportUVAndSvPositionToViewportUV` +256, `ScreenPosAbsMax`
+272, `ScreenPosToHistoryBufferUV` +288, `InputSceneColorSize` +304, `InputMinPixelCoord`/`InputMaxPixelCoord`
+320/+328, `HistoryBufferSize` +336, `HistoryBufferUVMinMax` +352, `OutputViewportSize` +368, `OutputViewportRect`
+384, `OutputQuantizationError` +400.

`View` (b1, 5220 bytes, UE 4.27's `FViewUniformShaderParameters`, laid out from the DXIL type and confirmed by the
values): `ViewToClip` +448 (jitter in its third row), `ViewToClipNoAA` +512, `ScreenPositionScaleBias` +1056,
`ClipToPrevClip` +1952 (without jitter), `TemporalAAJitter` +2016 (xy this frame, zw the previous one, in clip
units: pixels = x * width / 2 and -y * height / 2), `ViewRectMin` +2064, `ViewSizeAndInvSize` +2080,
`BufferSizeAndInvSize` +2112, `NumSceneColorMSAASamples` +2160, `PreExposure` +2164 (1.0 here),
`OneOverPreExposure` +2168 (the TAA multiplies the eye adaptation value by it).

### Values over three frames (`captures/taa_frames.txt`)

- Jitter alternates between (-0.25, -0.25) and (+0.25, +0.25) pixels: `r.TemporalAASamples` is 2 (UE's 2x MSAA
  pattern). The planar reflection has no jitter.
- `bCameraCut` 0, `PreExposure` 1, eye adaptation 1.0, `CurrentFrameWeight` 0.2 in all three.
- `ClipToPrevClip` is the identity within 1e-7 (the camera did not move), so it carries no jitter: it is built from
  the unjittered matrices.

### Velocity (`tex29598`, R16G16B16A16_UNORM)

The velocity pixel shaders (for example 19366) write UE 4.27's encoding with `VELOCITY_ENCODE_DEPTH`:
`xy = V * 0.2495 + 32767/65535` with `V = (this frame's screen position - the previous one) - (jitter - previous
jitter)`, in clip units (y up), so the velocity has no jitter in it; `z` and `w` are the high and low 16 bits of the
change in device depth. Pixels that no object wrote are 0 (the TAA then uses camera motion from depth and
`ClipToPrevClip`). So DLSS's motion vectors, in pixels and from this frame to the previous one, are
`-V.x * width / 2`, `+V.y * height / 2`, with `V` from the velocity buffer where `x > 0` and from depth otherwise.

### Who reads the TAA output

The bloom downsample (21597) and the tonemapper (21908) in the same frame; in the next frame the screen space
reflections (21125, as the previous frame's color) and the TAA itself (as history). The UI is drawn later at
1920x1080 and is not part of the TAA input.

### The ceiling fan and DLSS's inputs (2026-11-03)

With DLAA the user saw the lounge's moving ceiling fan blades "a little wavy". Checked in the three captures with
`tools/rdc_taa_inputs.py` (raw dumps of the main TAA's inputs in `captures/taa_inputs`), `tools/taa_inputs_region.py`,
`tools/taa_motion_check.py` and `tools/rdc_stencil_probe.py`. The fan is at about x 1130-1850, y 180-320.

- The fan writes velocity, on 26% of that rectangle (its blades). From 10211 to 10212 they move 0.6 px at the median
  (2.6 px at the tips); from 10212 to 10213, 37 px (115 px), probably a long game frame while RenderDoc saved the
  previous capture.
- The add-on's motion vectors are right. Following them from the later frame lands each blade pixel on the same
  surface in the earlier frame's scene color (with the jitter shift below). Median color error relative to the
  median luma: 0.026 against 0.48-1.50 for none, mirrored, half or double at 37 px; 0.024 against 0.032-0.068 at
  0.6 px.
- The jitter the add-on hands DLSS matches the images. Static pixels with detail move by +1 times the difference of
  the two frames' jitter as the add-on computes it (x right, y down): error 0.073 against 0.104 unshifted and 0.142
  the other way (10211 to 10212; 0.076, 0.172 and 0.265 for 10212 to 10213). That is the projection's jitter in
  pixels, as NVIDIA asks (DLSS Programming Guide of 31 March 2026, 3.7.3).
- No responsive AA: the stencil is 0 on every pixel of the frame, in the raw dump and through RenderDoc's pixel
  picker (2,916 pixels, the fan included). There is no mask for DLSS's bias-current-color input to take from it.
- No dilation. The guide (3.6.2): render-resolution motion vectors "do not need to be dilated, DLSS dilates them
  internally"; dilated display-resolution ones are "not the preferred option". The game's TAA does dilate (depth and
  velocity from the nearest of the four diagonal neighbors two pixels away, UE's `AA_CROSS` 2, as its DXIL shows), but
  DLSS keeps getting the plain ones. The D32S8 depth is fine (3.3: "any depth-stencil format").
- The one input that departs from the guide is the jitter pattern. All 16 positions the add-on logged on 2026-11-03
  are UE 4.27's 16-phase sequence for TAA without upsampling: Halton (2, 3) bent into a Gaussian
  (`r.TemporalAAFilterSize` 1) inside a disc of radius 0.5, so the pixel's corners (21.5% of its area) are never
  sampled. DLSS is trained on Halton over the whole pixel (3.7.1). Stock 4.27 jitters that way (`Halton(i, 2) - 0.5`,
  `Halton(i, 3) - 0.5`) only with temporal upsampling, `r.TemporalAA.Upsampling 1` (in the exe), which also switches
  the TAA to its upsampling permutation: to confirm in a capture.

### The temporal upsampling permutation (2026-11-03)

`captures/p3r_frame9458`-`9460`: three consecutive frames at the School Entranceway (4/21 After School), the
protagonist running and the camera following, taken with `tools\launch_rdc_upsampling.bat`, which sets
`r.TemporalAA.Upsampling 1` for the session in the user's Engine.ini (the game honors `[SystemSettings]` there).

- The main TAA dispatch (event 8122 in 9458, 480x270 groups) is another TAAStandalone permutation: DXIL hash
  `979c50e34b7323ab38a40f496667bffd` (container digest `9abf99a7...`). Same bindings (t0-t5, u0, root CBVs b0 and
  b1, same formats) and the same View. `$Globals` is 464 bytes instead of 412: the same fields at +0 to +8
  (`bCameraCut` +8) and +240 to +400; other values at +32 to +223 (no `SampleWeights`, `PlusWeights`); +448
  `TemporalJitterPixels` (-0.25, +0.1667 in 9458, equal to the add-on's jitter from `View.TemporalAAJitter`), then
  1.0 and 1.0.
- Its jitter is the uniform Halton sequence, `Halton(i, 2) - 0.5`, `Halton(i, 3) - 0.5`: 9458 to 9460 alternate
  (-0.250, +0.167) and (0.000, -0.167), its first two points (this area's `r.TemporalAASamples` is 2).
- `View.MaterialTextureMipBias` (+2292) is -0.3 on this path (0 on the other), UE's default
  `r.ViewTextureMipBias.Offset`; `MaterialTextureDerivativeMultiply` (+2296) follows (2^-0.3). Then `Random` +2300,
  `FrameNumber` +2304, `StateFrameIndexMod8` +2308. NVIDIA's guide (3.5) asks for -1.0 with DLAA.
- Camera motion vectors (`tools/taa_motion_check.py` on the dumps in `captures/taa_inputs_up`, which now keep the
  View values): from 9458 to 9459 the camera moves pixels without velocity by 8.2 px at the median (18.5 px at 90%);
  the add-on's camera motion from depth and `ClipToPrevClip` has a median color error of 0.243 against 0.909 for
  none, 0.444-0.975 mirrored and 0.81-0.84 half or double. The running students' velocity (29.5 px): 0.025 against
  0.043 and more. From 9459 to 9460, the game's long step after RenderDoc saved two frames (569 px): 0.599 against
  1.108 and more (camera), 0.116 against 0.934 and more (velocity). The jitter sign is +1 again.

### The minimap flash under RenoDX (2026-11-03)

RenoDX's known P3R issue: a bright hard-edged shape over the minimap while the HUD slides in after unpausing.
Captures `p3r_frame38307` to `39218` (ReShade and RenoDX off) at 0.15, 0.30, 0.45, 0.60 and 0.80 s after the button
that closes the pause menu (`tools/rdc_trigger.py` with `RDC_INPUT_DELAYS`); 0.45 to 0.80 show the slide-in.

- No HUD draw writes colors outside 0..1 near the minimap (`tools/rdc_hud_outputs.py`). The only out-of-range
  writer there is a full-screen pass onto the back buffer (PS `e1c5ad0f...`, red up to 1.13 at every timing, likely
  sharpening overshoot on the bright cherry blossoms).
- The HUD (Slate: VS `1b4c1084...`, PS `083ce4bf...` and `1a12378d...`) draws straight onto the 3840x2160
  B8G8R8A8_UNORM back buffer and uses its alpha channel as a mask (`tools/rdc_ui_blends.py`). Draws that leave the
  color alone set alpha (One, Zero), subtract a shape (alpha = dst - src.a^2, ReversedSubtract), add one
  (dst + src.a^2), intersect (Minimum) or clear it; the content is then blended through the mask (color =
  src * dstAlpha + dst * (1 - dstAlpha)). The 0.60 s frame has five subtracting draws (the wedges that animate in),
  the finished 0.80 s frame one.
- On the 8-bit back buffer every blend result is clamped to 0..1. RenoDX renders the HUD onto a float copy, where
  the subtraction leaves alpha at -1 and the additions at 1.25 to 2 (`tools/rdc_hud_blends.py` recomputes the
  blends without the clamp: 35 to 355 of the sampled writes per draw). Content drawn through alpha -1 comes out as
  src * -1 + dst * 2, a bright shape with the mask's hard edges; additions followed by a subtraction also leave content
  where the game masks it out.
- Fix, to build as our own add-on: make the float copy behave like the 8-bit buffer by clamping alpha to 0..1 right
  after each of these mask draws, drawing the same geometry again with alpha-only Maximum(0) and Minimum(1) blends. It
  has to match how RenoDX binds its float copy (render target format, pipelines): read in its source first.

Still to capture: a battle.
