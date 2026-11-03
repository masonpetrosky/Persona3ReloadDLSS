# Starts Persona 3 Reload under RenderDoc 1.46 for frame captures and waits for the game to exit.
# For the session only, the D3D layers in the game folder are switched off (ReShade as dxgi.dll, a d3d12.dll proxy
# such as SCSKiller's recorder), so the captures show the game's own rendering, and a steam_appid.txt lets the game
# start outside Steam. All of it is undone when the game exits, or with -Restore after a crash of this script; the
# session file in captures\ records what was changed, so -Restore never touches a file this script did not create
# or rename.
# Captures land in captures\p3r_frame<N>.rdc. tools\rdc_trigger.py triggers them remotely; RenderDoc's F12 works too,
# but Steam also takes a screenshot on F12.
# -Cvar 'name=value[,name=value...]' sets console variables for the session only, as a [SystemSettings] section in the
# user's Engine.ini (%LOCALAPPDATA%\P3R\Saved\Config\WindowsNoEditor). The original file is copied first to
# Documents\Backups\Persona3Reload\<date>-before-rdc-cvars and put back when the session is undone.
param([switch]$Restore, [string]$Cvar)
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
$engineIni = Join-Path $env:LOCALAPPDATA 'P3R\Saved\Config\WindowsNoEditor\Engine.ini'
$backups = Join-Path $PSScriptRoot '..\..\..\Backups\Persona3Reload'

# The console variables of -Cvar, checked, as "name=value" lines.
function Get-CvarLines([string]$list) {
  $lines = @($list -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
  foreach ($line in $lines) {
    if ($line -notmatch '^[A-Za-z][\w.]*=[^\s=]+$') { throw "Not a console variable setting: '$line' (name=value)" }
  }
  return $lines
}

# Copies Engine.ini to $backup. Refuses a file that already has a [SystemSettings] section (by hand, or from a session
# that was not undone), so the backup is always the user's own file.
function Save-EngineIni([string]$ini, [string]$backup) {
  if ([IO.File]::ReadAllText($ini) -match '(?m)^\[SystemSettings\]') {
    throw "$ini already has a [SystemSettings] section; not changing it."
  }
  New-Item -ItemType Directory -Force (Split-Path $backup) | Out-Null
  Copy-Item $ini $backup -Force
}

# Appends a [SystemSettings] section with the variables (Engine.ini is ASCII with CRLF line ends).
function Add-SessionCvars([string[]]$lines, [string]$ini) {
  $text = [IO.File]::ReadAllText($ini)
  $add = $(if ($text.Length -eq 0 -or $text.EndsWith("`n")) { '' } else { "`r`n" }) + "[SystemSettings]`r`n" +
    (($lines | ForEach-Object { "$_`r`n" }) -join '')
  [IO.File]::AppendAllText($ini, $add, [Text.Encoding]::ASCII)
  Write-Host "Engine.ini: $($lines -join ', ') for the session."
}

function Restore-EngineIni([string]$ini, [string]$backup) {
  if (-not (Test-Path $backup)) { Write-Warning "The saved Engine.ini is missing ($backup); $ini is left as it is."; return }
  Copy-Item $backup $ini -Force
  Write-Host "Engine.ini restored (a copy stays in $(Split-Path $backup))."
}

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
  if ($state.EngineIniBackup) { Restore-EngineIni $engineIni $state.EngineIniBackup }
  Remove-Item $session
}

if ($Restore) { Undo-Session; return }

if (Get-Process -Name 'P3R' -ErrorAction SilentlyContinue) { throw 'The game is already running.' }
if (Test-Path $session) { throw "A previous session was not undone; run $PSCommandPath -Restore first." }
if (-not (Test-Path $rd)) { throw "RenderDoc not found: $rd" }
foreach ($name in $layers) {
  if (Test-Path (Join-Path $win64 "$name.rdc-off")) { throw "$name.rdc-off already exists in $win64" }
}
$cvarLines = @(if ($Cvar) { Get-CvarLines $Cvar })
if ($cvarLines.Count -and -not (Test-Path $engineIni)) { throw "No Engine.ini at $engineIni (start the game once first)." }
$state = [ordered]@{ LayersOff = @(); AppIdCreated = $false; EngineIniBackup = $null }
if ($cvarLines.Count) {
  # The backup first, then the session file that names it, then the change: -Restore can undo any step.
  $backup = [IO.Path]::GetFullPath((Join-Path $backups "$(Get-Date -Format yyyy-MM-dd)-before-rdc-cvars\Engine.ini"))
  Save-EngineIni $engineIni $backup
  $state.EngineIniBackup = $backup
  $state | ConvertTo-Json | Set-Content $session -Encoding utf8
  Add-SessionCvars $cvarLines $engineIni
}
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
