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

## Captures

None yet. Plan: a field scene with walking characters (3 consecutive frames, camera moving), later a battle.
