# Starts Persona 3 Reload under RenderDoc 1.46 for frame captures and waits for the game to exit.
# For the session only: ReShade (dxgi.dll), if installed, is switched off, and a steam_appid.txt lets the game start
# outside Steam. Both are undone when the game exits, or with -Restore after a crash of this script; the session file
# in captures\ records what was changed, so -Restore never touches a file this script did not create or rename.
# Captures land in captures\p3r_frame<N>.rdc. tools\rdc_trigger.py triggers them remotely; RenderDoc's F12 works too,
# but Steam also takes a screenshot on F12.
param([switch]$Restore)
$ErrorActionPreference = 'Stop'

$game = 'C:\Program Files (x86)\Steam\steamapps\common\P3R'
$win64 = Join-Path $game 'P3R\Binaries\Win64'
# RenderDoc 1.46: by default the shared copy in the Tools folder beside the Projects folder that holds this
# repository (override with RENDERDOC_DIR).
$tools = Join-Path $PSScriptRoot '..\..\..\Tools'
$rd = Join-Path ($env:RENDERDOC_DIR ?? (Join-Path $tools 'RenderDoc\RenderDoc_1.46_64')) 'renderdoccmd.exe'
$captures = Join-Path $PSScriptRoot '..\captures'
New-Item -ItemType Directory -Force $captures | Out-Null
$captures = (Resolve-Path $captures).Path
$session = Join-Path $captures 'rdc-session.json'
$reshade = Join-Path $win64 'dxgi.dll'
$reshadeOff = Join-Path $win64 'dxgi.dll.rdc-off'
$appid = Join-Path $win64 'steam_appid.txt'

function Undo-Session {
  if (-not (Test-Path $session)) { Write-Host 'No session to undo.'; return }
  $state = Get-Content $session -Raw | ConvertFrom-Json
  if ($state.ReShadeRenamed -and (Test-Path $reshadeOff)) {
    if (Test-Path $reshade) { throw "both dxgi.dll and dxgi.dll.rdc-off exist in $win64" }
    Rename-Item $reshadeOff 'dxgi.dll'
    Write-Host 'ReShade restored (dxgi.dll).'
  }
  if ($state.AppIdCreated -and (Test-Path $appid)) { Remove-Item $appid; Write-Host 'steam_appid.txt removed.' }
  Remove-Item $session
}

if ($Restore) { Undo-Session; return }

if (Get-Process -Name 'P3R' -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
if (Test-Path $session) { throw "A previous session was not undone; run $PSCommandPath -Restore first." }
if (-not (Test-Path $rd)) { throw "RenderDoc not found: $rd" }
$state = [ordered]@{ ReShadeRenamed = $false; AppIdCreated = $false }
if (Test-Path $reshade) {
  Rename-Item $reshade 'dxgi.dll.rdc-off'
  $state.ReShadeRenamed = $true
}
if (-not (Test-Path $appid)) {
  Set-Content -Path $appid -Value '2161700' -NoNewline -Encoding ascii
  $state.AppIdCreated = $true
}
$state | ConvertTo-Json | Set-Content $session -Encoding utf8
try {
  Write-Host "Launching under RenderDoc; captures go to $captures"
  & $rd capture -w -d $win64 -c (Join-Path $captures 'p3r') (Join-Path $win64 'P3R.exe')
  Write-Host "renderdoccmd exited with $LASTEXITCODE"
} finally {
  Undo-Session
}
