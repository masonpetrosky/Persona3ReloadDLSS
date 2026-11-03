# Packs the release zips from the built add-ons, laid out like the game's folder so that extracting one into it
# installs it where tools\deploy.ps1 and tools\deploy_hud_fix.ps1 do:
#   dist\Persona3ReloadDLSS-<version>.zip    P3R\Binaries\Win64\Persona3ReloadDLSS.addon64, its default .ini, and the
#                                            Persona3ReloadDLSS folder (NVIDIA's release nvngx_dlss.dll, README.txt,
#                                            licenses)
#   dist\Persona3ReloadHudFix-<version>.zip  P3R\Binaries\Win64\Persona3ReloadHudFix.addon64 and the
#                                            Persona3ReloadHudFix folder (README.txt, licenses)
# ReShade is not included. Build first (build.ps1); the version comes from CMakeLists.txt.
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw),
  'project\(Persona3ReloadDLSS VERSION ([\d.]+)').Groups[1].Value
if (-not $version) { throw 'CMakeLists.txt has no project version.' }
$package = Join-Path $root 'package'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null

function Get-Built([string]$file) {
  $path = Join-Path $root "build\$file"
  if (-not (Test-Path $path)) { throw "build\$file is missing; run build.ps1." }
  if ((Get-Item $path).VersionInfo.FileVersion -ne $version) { throw "build\$file is not version $version; run build.ps1." }
  return $path
}

# A text file with the version filled in and Windows line endings.
function Copy-Text([string]$source, [string]$destination) {
  $text = (Get-Content $source -Raw).Replace('@VERSION@', $version).Replace("`r`n", "`n").Replace("`n", "`r`n")
  Set-Content -Path $destination -Value $text -Encoding utf8NoBOM -NoNewline
}

function Copy-Licenses([string]$folder, [hashtable]$files) {
  New-Item -ItemType Directory -Force $folder | Out-Null
  foreach ($name in $files.Keys) { Copy-Item (Join-Path $root $files[$name]) (Join-Path $folder $name) }
}

# Stages dist\<name>\P3R\Binaries\Win64 with $fill, zips the P3R folder and prints the zip's entries and hash.
function New-ReleaseZip([string]$name, [scriptblock]$fill) {
  $stage = Join-Path $dist $name
  if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
  $win64 = Join-Path $stage 'P3R\Binaries\Win64'
  New-Item -ItemType Directory -Force $win64 | Out-Null
  & $fill $win64
  $zip = Join-Path $dist "$name.zip"
  if (Test-Path $zip) { Remove-Item $zip -Force }
  Compress-Archive -Path (Join-Path $stage 'P3R') -DestinationPath $zip -CompressionLevel Optimal
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $archive = [IO.Compression.ZipFile]::OpenRead($zip)
  try { $archive.Entries | ForEach-Object { '  {0,10}  {1}' -f $_.Length, $_.FullName } } finally { $archive.Dispose() }
  '{0}: {1} bytes, SHA256 {2}' -f (Split-Path $zip -Leaf), (Get-Item $zip).Length, (Get-FileHash $zip -Algorithm SHA256).Hash
}

$common = @{
  'LICENSE.txt'        = 'LICENSE'
  'ReShade-LICENSE.md' = 'third_party\reshade\LICENSE.md'
  'ImGui-LICENSE.txt'  = 'third_party\imgui\LICENSE.txt'
}

New-ReleaseZip "Persona3ReloadDLSS-$version" {
  param($win64)
  Copy-Item (Get-Built 'Persona3ReloadDLSS.addon64') $win64
  Copy-Text (Join-Path $package 'Persona3ReloadDLSS.ini') (Join-Path $win64 'Persona3ReloadDLSS.ini')
  $folder = Join-Path $win64 'Persona3ReloadDLSS'
  New-Item -ItemType Directory -Force $folder | Out-Null
  Copy-Item (Join-Path $root 'third_party\dlss\bin\rel\nvngx_dlss.dll') $folder
  Copy-Text (Join-Path $package 'README-DLSS.txt') (Join-Path $folder 'README.txt')
  Copy-Licenses (Join-Path $folder 'licenses') ($common + @{
    'NVIDIA-RTX-SDKs-LICENSE.txt' = 'third_party\dlss\LICENSE.txt'
    'NVAPI-LICENSE.txt'           = 'third_party\nvapi\License.txt'
  })
}

New-ReleaseZip "Persona3ReloadHudFix-$version" {
  param($win64)
  Copy-Item (Get-Built 'Persona3ReloadHudFix.addon64') $win64
  $folder = Join-Path $win64 'Persona3ReloadHudFix'
  New-Item -ItemType Directory -Force $folder | Out-Null
  Copy-Text (Join-Path $package 'README-HudFix.txt') (Join-Path $folder 'README.txt')
  Copy-Licenses (Join-Path $folder 'licenses') $common
}
