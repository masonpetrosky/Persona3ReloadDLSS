@echo off
rem launch_rdc.bat for stage 3 (DLSS upscaling): the engine's temporal upsampling path at a 67% render scale, set for
rem the session only (r.TemporalAA.Upsampling 1, r.ScreenPercentage 67). Explorer cannot pass arguments, hence this file.
rem Start it with: Start-Process explorer.exe '"<repo>\tools\launch_rdc_upscaling.bat"'
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch_rdc.ps1" -Cvar r.TemporalAA.Upsampling=1,r.ScreenPercentage=67
