# Downloads the parts of NVIDIA's NVAPI SDK that the add-on builds against (Reflex low-latency sleep) into
# third_party\nvapi. The headers and nvapi64.lib are under the MIT license (third_party\nvapi\License.txt); like the DLSS
# SDK they are not committed to this repo.
param([string]$Commit = '70d337db9186e968eab622f7e786de7e437faf3d')
$ErrorActionPreference = 'Stop'

$dst = Join-Path $PSScriptRoot '..\third_party\nvapi'
$base = "https://raw.githubusercontent.com/NVIDIA/nvapi/$Commit"
$files = @('License.txt', 'nvapi.h', 'nvapi_lite_common.h', 'nvapi_lite_d3dext.h', 'nvapi_lite_salend.h',
           'nvapi_lite_salstart.h', 'nvapi_lite_sli.h', 'nvapi_lite_stereo.h', 'nvapi_lite_surround.h',
           'amd64/nvapi64.lib')
foreach ($f in $files) {
  $out = Join-Path $dst $f
  New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
  Invoke-WebRequest -Uri "$base/$f" -OutFile $out -UseBasicParsing
}
Set-Content -Path (Join-Path $dst 'VERSION') -Value $Commit
Get-ChildItem $dst -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring((Resolve-Path $dst).Path.Length + 1) } }, Length
