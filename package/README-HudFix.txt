Persona 3 Reload HUD fix @VERSION@
https://github.com/masonpetrosky/Persona3ReloadDLSS

A ReShade add-on for HDR mods that draw the game's HUD onto a high-precision copy of the screen, as RenoDX does.
With them, a bright, hard-edged shape flashes over the minimap while the HUD slides back in after unpausing. The game
builds masks in the screen's alpha channel and blends the HUD through them; the game's own 8-bit screen keeps every
step between 0 and 1, the HDR mod's copy does not. After each such HUD draw, the add-on draws the same shape again,
alpha only, holding it between 0 and 1 as the 8-bit screen did. Without an HDR mod it changes nothing. It works with
or without the Persona 3 Reload DLSS add-on and does not change RenoDX.

Requirements
------------
- Persona 3 Reload on Steam (tested with Steam build 22672075 in November 2026).
- ReShade 6.8 or newer *with full add-on support* (https://reshade.me), installed for P3R.exe in P3R\Binaries\Win64
  with DirectX 10/11/12, and the HDR mod it is for (RenoDX).

Installing
----------
1. Open the game's folder: in Steam, right-click Persona 3 Reload > Manage > Browse local files.
2. Extract this zip there, so its P3R folder merges with the game's. P3R\Binaries\Win64 then holds
   Persona3ReloadHudFix.addon64 and the Persona3ReloadHudFix folder (this file and the licenses).

Using it
--------
The ReShade overlay's Add-ons tab (Home key) has a checkbox to switch it off for a comparison, and shows how many
draws it clamps. The switch is saved to Persona3ReloadHudFix.ini; ReShade.log has its messages, prefixed [P3RHUD].

Uninstalling
------------
Delete Persona3ReloadHudFix.addon64, Persona3ReloadHudFix.ini (if there) and the Persona3ReloadHudFix folder from
P3R\Binaries\Win64.

Licenses
--------
MIT licensed (licenses\LICENSE.txt). Contains code from ReShade (licenses\ReShade-LICENSE.md) and uses Dear ImGui's
headers (licenses\ImGui-LICENSE.txt). Not affiliated with or endorsed by ATLUS, SEGA or the RenoDX authors. Persona
is a trademark of ATLUS.
