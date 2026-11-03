# Installs the built add-on into Persona 3 Reload's Binaries\Win64 folder, next to ReShade (dxgi.dll, with add-on
# support), which must already be there:
#   Persona3ReloadDLSS.addon64            the add-on (build\)
#   Persona3ReloadDLSS.ini                its settings, written only when missing
#   Persona3ReloadDLSS\nvngx_dlss.dll     NVIDIA's release DLSS DLL; with -Dev the developer DLL (watermark, more logs)
# Files it replaces are copied first to Backups\Persona3Reload\<date>-before-dlss-<time> beside the Projects folder,
# with a listing of the folder. -Uninstall removes the add-on's files and folder again (NGX's logs included). The
# game has to be closed.
param(
  [switch]$Dev,
  [switch]$Uninstall,
  [string]$Win64 = 'C:\Program Files (x86)\Steam\steamapps\common\P3R\P3R\Binaries\Win64'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
if (-not (Test-Path (Join-Path $Win64 'P3R.exe'))) { throw "P3R.exe is not in $Win64." }
if (Get-Process P3R -ErrorAction SilentlyContinue) { throw 'Persona 3 Reload is running; close it first.' }
$addon = Join-Path $Win64 'Persona3ReloadDLSS.addon64'
$ini = Join-Path $Win64 'Persona3ReloadDLSS.ini'
$data = Join-Path $Win64 'Persona3ReloadDLSS'
$dll = Join-Path $data 'nvngx_dlss.dll'

# Copies what is about to change, and a listing of the folder, into a fresh backup folder.
$backup = Join-Path $root "..\..\Backups\Persona3Reload\$(Get-Date -Format 'yyyy-MM-dd')-before-dlss-$(Get-Date -Format 'HHmmss')"
New-Item -ItemType Directory -Force $backup | Out-Null
Get-ChildItem $Win64 -Recurse | Select-Object FullName, Length, LastWriteTime |
  Format-Table -AutoSize | Out-String -Width 400 | Set-Content (Join-Path $backup 'Win64-listing.txt')
foreach ($file in $addon, $ini, $dll) {
  if (Test-Path $file) { Copy-Item $file $backup }
}
Write-Host "Backup: $((Resolve-Path $backup).Path)"

if ($Uninstall) {
  foreach ($file in $addon, $ini) {
    if (Test-Path $file) { Remove-Item $file; Write-Host "Removed $file" }
  }
  if (Test-Path $data) { Remove-Item $data -Recurse; Write-Host "Removed $data" }
  return
}

if ((Get-Item (Join-Path $Win64 'dxgi.dll') -ErrorAction SilentlyContinue).VersionInfo.ProductName -ne 'ReShade') {
  throw "ReShade (dxgi.dll) is not installed in $Win64."
}
$build = Join-Path $root 'build\Persona3ReloadDLSS.addon64'
$dlssBuild = if ($Dev) { 'dev' } else { 'rel' }
$source = Join-Path $root "third_party\dlss\bin\$dlssBuild\nvngx_dlss.dll"
foreach ($file in $build, $source) {
  if (-not (Test-Path $file)) { throw "$file is missing; run build.ps1 first." }
}
New-Item -ItemType Directory -Force $data | Out-Null
Copy-Item $build $addon -Force
Copy-Item $source $dll -Force
if (-not (Test-Path $ini)) {
  @'
[Persona3ReloadDLSS]
; 1: DLSS replaces the game's TAA; 0: the game's TAA. ToggleKey switches in game (120 = F9).
Enabled=1
; 0 DLAA (100%), 1 Quality (67%), 2 Balanced (58%), 3 Performance (50%), 4 Ultra Performance (33%).
Mode=0
ToggleKey=120
; 0: NVIDIA's default per mode (K; M for Performance, L for Ultra Performance); 5 = E, 10 = J, 11 = K, 12 = L, 13 = M.
Preset=0
; The camera jitter's phases while DLSS runs (r.TemporalAASamples); 0 keeps the game's. Ultra Performance: at most 14.
JitterPhases=16
; 1: while DLSS runs, the engine's temporal upsampling path (Halton jitter over the whole pixel); 0: the game's path.
UpsamplingPath=1
; The texture mip bias on that path; NVIDIA recommends -1.0 for DLAA, toward 0 if fine textures shimmer.
MipBias=-1.00
; NVIDIA Reflex low-latency mode (1 on, 0 off).
Reflex=1
; 0 keeps the game's frame rate limit; otherwise this cap (a few under the refresh rate with VRR, e.g. 237 at 240 Hz).
FrameRateCap=0
; 1: DLSS gets the game's eye adaptation exposure; 0: DLSS estimates one.
GameExposure=1
'@ | Set-Content -Path $ini -Encoding ascii
}
Write-Host "Installed into $Win64 (DLSS DLL: $dlssBuild)."
