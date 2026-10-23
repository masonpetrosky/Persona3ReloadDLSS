@echo off
rem Runs launch_rdc.ps1 in a console window of its own, so the game does not depend on the shell that started it.
rem Start it with: Start-Process explorer.exe '"<repo>\tools\launch_rdc.bat"'
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch_rdc.ps1" %*
