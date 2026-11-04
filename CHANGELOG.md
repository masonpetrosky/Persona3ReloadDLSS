# Changelog

## 0.1.0 (2026-11-03)

First release.

- DLSS in place of Persona 3 Reload's TAA: DLAA, or upscaling with the Quality (67%), Balanced (58%), Performance
  (50%) and Ultra Performance (33%) modes (`Mode`, also in the overlay; switches at once). In Paulownia Mall at 4K on
  an RTX 5070 Ti: 88.2 fps with DLAA, 142.5 fps with Quality.
- DLSS gets what NVIDIA's guide asks for: motion vectors from the game's velocity buffer, with camera motion from
  depth where nothing wrote velocity; the engine's own temporal upsampling path with its uniform Halton jitter (16
  phases instead of the game's 2, at most 14 at Ultra Performance, where the engine multiplies them by 9 and the
  game's jitter breaks above 128); the texture mip bias for the render scale; the game's eye adaptation exposure.
- NVIDIA's default preset per mode (K; M for Performance, L for Ultra Performance), or one preset for every mode.
- NVIDIA Reflex (on by default) and a frame rate cap (`FrameRateCap`) that replaces the game's 120 FPS limit.
- F9 switches between DLSS and the game's TAA; the ReShade overlay's Add-ons tab has every setting and shows what the
  add-on is doing.
- A separate add-on, Persona 3 Reload HUD fix, removes the bright shape over the minimap that RenoDX's HDR shows
  while the HUD slides in after unpausing.
