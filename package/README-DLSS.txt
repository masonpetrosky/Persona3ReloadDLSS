Persona 3 Reload DLSS @VERSION@
https://github.com/masonpetrosky/Persona3ReloadDLSS

A ReShade add-on that replaces Persona 3 Reload's temporal anti-aliasing with NVIDIA DLSS: DLAA at native
resolution, or DLSS upscaling from a lower render resolution (Quality, Balanced, Performance, Ultra Performance).
It also adds NVIDIA Reflex and can lift the game's 120 FPS limit.

Requirements
------------
- An NVIDIA RTX graphics card (RTX 20 series or newer) and a current driver.
- Persona 3 Reload on Steam (tested with Steam build 22672075 in November 2026). Other stores are untested.
- ReShade 6.8 or newer *with full add-on support* (https://reshade.me, the "with full add-on support" download),
  installed for P3R.exe in P3R\Binaries\Win64 with DirectX 10/11/12. It goes there as dxgi.dll.

Installing
----------
1. Open the game's folder: in Steam, right-click Persona 3 Reload > Manage > Browse local files.
2. Extract this zip there, so its P3R folder merges with the game's. P3R\Binaries\Win64 then holds
   Persona3ReloadDLSS.addon64, Persona3ReloadDLSS.ini and the Persona3ReloadDLSS folder (NVIDIA's nvngx_dlss.dll,
   this file and the licenses), next to ReShade's dxgi.dll.

Updating: extract the new zip the same way; skip Persona3ReloadDLSS.ini if you want to keep your settings.

Using it
--------
- F9 switches between DLSS and the game's TAA and remembers the choice (not while Alt, Ctrl or Shift is held).
- The Home key opens the ReShade overlay; its Add-ons tab has the settings, which apply at once and are saved to
  Persona3ReloadDLSS.ini (each one is described there):
  - Mode: DLAA (100%, the default) for image quality; Quality (67%), Balanced (58%), Performance (50%) or Ultra
    Performance (33%) render at a lower resolution and let DLSS upscale, for more frames.
  - Preset: NVIDIA's default picks K for DLAA, Quality and Balanced, M for Performance and L for Ultra Performance.
  - Reflex: NVIDIA Reflex low-latency mode, on by default.
  - Frame rate cap: the game's own options stop at 120 FPS. A cap here replaces the game's limit, for example 237 on
    a 240 Hz display with G-SYNC or FreeSync; 0 keeps the game's.
  - Texture mip bias and jitter phases: set as NVIDIA's DLSS guide asks, by default.
- ReShade.log in P3R\Binaries\Win64 has the add-on's messages, prefixed [P3RDLSS].

HDR
---
Works with RenoDX's HDR add-on for Unreal Engine games. With it, a bright shape can flash over the minimap while
the HUD slides back in after unpausing; that is RenoDX's, not this add-on's, and the separate Persona 3 Reload HUD
fix add-on (Persona3ReloadHudFix) removes it.

Not getting more frames?
------------------------
- DLAA is for image quality and costs a little more than the game's TAA. For frames, choose an upscaling mode.
- At 120 FPS the game's limit is reached; set a frame rate cap to go higher.
- Where the CPU is the limit, a lower render resolution cannot help much.
- Presets M and L cost more than K, much more on RTX 20 and 30 cards.

Troubleshooting
---------------
- The game does not start or the overlay does not list the add-on: make sure ReShade is the "full add-on support"
  version, 6.8 or newer, and that no other DLSS or upscaler mod is installed in P3R\Binaries\Win64.
- Report problems on GitHub or Nexus with ReShade.log, your GPU, driver, resolution and settings.

Uninstalling
------------
Delete Persona3ReloadDLSS.addon64, Persona3ReloadDLSS.ini and the Persona3ReloadDLSS folder from
P3R\Binaries\Win64.

Known limitations
-----------------
- Tested on one machine: RTX 5070 Ti, 4K, HDR with RenoDX. A game update can break it; it then leaves the game's TAA
  in place (the overlay shows what it could not find).

Licenses
--------
The add-on's own code is MIT licensed (licenses\LICENSE.txt). Persona3ReloadDLSS.addon64 contains NVIDIA's DLSS SDK
library, and nvngx_dlss.dll is NVIDIA's DLSS runtime; both are under NVIDIA's RTX SDKs license
(licenses\NVIDIA-RTX-SDKs-LICENSE.txt). The add-on also contains NVIDIA's NVAPI library (licenses\NVAPI-LICENSE.txt)
and code from ReShade (licenses\ReShade-LICENSE.md), and uses Dear ImGui's headers (licenses\ImGui-LICENSE.txt). Not
affiliated with or endorsed by ATLUS, SEGA or NVIDIA. Persona is a trademark of ATLUS; NVIDIA, DLSS and Reflex are
trademarks of NVIDIA Corporation.
