# Starts Persona 3 Reload under RenderDoc 1.46 for frame captures and waits for the game to exit.
# For the session only, the D3D layers in the game folder are switched off (ReShade as dxgi.dll, a d3d12.dll proxy
# such as SCSKiller's recorder), so the captures show the game's own rendering, and a steam_appid.txt lets the game
# start outside Steam. All of it is undone when the game exits, or with -Restore after a crash of this script; the
# session file in captures\ records what was changed, so -Restore never touches a file this script did not create
# or rename.
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
$layers = 'dxgi.dll', 'd3d12.dll'
$appid = Join-Path $win64 'steam_appid.txt'

# Puts a layer back. If something put the file back during the session (SCSKiller re-installs its recorder), the
# switched-off copy is dropped when it is the same file and kept for review when it is not.
function Restore-Layer([string]$name) {
  $on = Join-Path $win64 $name
  $off = "$on.rdc-off"
  if (-not (Test-Path $off)) { return }
  if (-not (Test-Path $on)) {
    Rename-Item $off $name
    Write-Host "$name restored."
  } elseif ((Get-FileHash $on).Hash -eq (Get-FileHash $off).Hash) {
    Remove-Item $off
    Write-Host "$name was already back (the same file); the switched-off copy is removed."
  } else {
    Write-Warning "$name came back as a different file during the session; $name.rdc-off is left for review."
  }
}

function Undo-Session {
  if (-not (Test-Path $session)) { Write-Host 'No session to undo.'; return }
  $state = Get-Content $session -Raw | ConvertFrom-Json
  foreach ($name in $state.LayersOff) { Restore-Layer $name }
  if ($state.AppIdCreated -and (Test-Path $appid)) { Remove-Item $appid; Write-Host 'steam_appid.txt removed.' }
  Remove-Item $session
}

if ($Restore) { Undo-Session; return }

if (Get-Process -Name 'P3R' -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
if (Test-Path $session) { throw "A previous session was not undone; run $PSCommandPath -Restore first." }
if (-not (Test-Path $rd)) { throw "RenderDoc not found: $rd" }
foreach ($name in $layers) {
  if (Test-Path (Join-Path $win64 "$name.rdc-off")) { throw "$name.rdc-off already exists in $win64" }
}
$state = [ordered]@{ LayersOff = @(); AppIdCreated = $false }
foreach ($name in $layers) {
  $path = Join-Path $win64 $name
  if (Test-Path $path) {
    Write-Host "Switching off $name ($((Get-Item $path).VersionInfo.FileDescription)) for the session."
    $state.LayersOff += $name
    $state | ConvertTo-Json | Set-Content $session -Encoding utf8
    Rename-Item $path "$name.rdc-off"
  }
}
if (-not (Test-Path $appid)) {
  $state.AppIdCreated = $true
  $state | ConvertTo-Json | Set-Content $session -Encoding utf8
  Set-Content -Path $appid -Value '2161700' -NoNewline -Encoding ascii
}
$state | ConvertTo-Json | Set-Content $session -Encoding utf8
try {
  foreach ($name in $state.LayersOff) {
    if (Test-Path (Join-Path $win64 $name)) { throw "$name came back before the game started; not launching." }
  }
  Write-Host "Launching under RenderDoc; captures go to $captures"
  & $rd capture -w -d $win64 -c (Join-Path $captures 'p3r') (Join-Path $win64 'P3R.exe')
  Write-Host "renderdoccmd exited with $LASTEXITCODE"
} finally {
  Undo-Session
}
