@echo off
rem launch_rdc.bat with r.TemporalAA.Upsampling 1 for the session: the TAA's upsampling permutation and the engine's
rem uniform Halton jitter (stage 3). Explorer cannot pass arguments, hence this file.
rem Start it with: Start-Process explorer.exe '"<repo>\tools\launch_rdc_upsampling.bat"'
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch_rdc.ps1" -Cvar r.TemporalAA.Upsampling=1
