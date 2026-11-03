# Installs the HUD fix (Persona3ReloadHudFix.addon64, built by build.ps1) into Persona 3 Reload's Binaries\Win64
# folder, next to ReShade (dxgi.dll, with add-on support), which must already be there. Files it replaces are copied
# first to Backups\Persona3Reload\<date>-before-hudfix-<time> beside the Projects folder, with a listing of the
# folder. -Uninstall removes the add-on and its ini again. The game has to be closed.
param(
  [switch]$Uninstall,
  [string]$Win64 = 'C:\Program Files (x86)\Steam\steamapps\common\P3R\P3R\Binaries\Win64'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
if (-not (Test-Path (Join-Path $Win64 'P3R.exe'))) { throw "P3R.exe is not in $Win64." }
if (Get-Process P3R -ErrorAction SilentlyContinue) { throw 'Persona 3 Reload is running; close it first.' }
$addon = Join-Path $Win64 'Persona3ReloadHudFix.addon64'
$ini = Join-Path $Win64 'Persona3ReloadHudFix.ini'

$backup = Join-Path $root "..\..\Backups\Persona3Reload\$(Get-Date -Format 'yyyy-MM-dd')-before-hudfix-$(Get-Date -Format 'HHmmss')"
New-Item -ItemType Directory -Force $backup | Out-Null
Get-ChildItem $Win64 -Recurse | Select-Object FullName, Length, LastWriteTime |
  Format-Table -AutoSize | Out-String -Width 400 | Set-Content (Join-Path $backup 'Win64-listing.txt')
foreach ($file in $addon, $ini) {
  if (Test-Path $file) { Copy-Item $file $backup }
}
Write-Host "Backup: $((Resolve-Path $backup).Path)"

if ($Uninstall) {
  foreach ($file in $addon, $ini) {
    if (Test-Path $file) { Remove-Item $file; Write-Host "Removed $file" }
  }
  return
}

if ((Get-Item (Join-Path $Win64 'dxgi.dll') -ErrorAction SilentlyContinue).VersionInfo.ProductName -ne 'ReShade') {
  throw "ReShade (dxgi.dll) is not installed in $Win64."
}
$build = Join-Path $root 'build\Persona3ReloadHudFix.addon64'
if (-not (Test-Path $build)) { throw "$build is missing; run build.ps1 first." }
Copy-Item $build $addon -Force
Write-Host "Installed $addon."
