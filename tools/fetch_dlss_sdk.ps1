# Downloads the parts of NVIDIA's DLSS SDK that the add-on builds against into third_party\dlss.
# The SDK stays under NVIDIA's license (third_party\dlss\LICENSE.txt) and is not committed to this repo.
param(
  [string]$Version = 'v310.9.1',
  [string]$Destination = (Join-Path $PSScriptRoot '..\third_party\dlss')
)
$ErrorActionPreference = 'Stop'

$dst = [System.IO.Path]::GetFullPath($Destination)
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$Version"
# Complete include closure for the D3D12 super resolution helpers at the pinned SDK version. No GitHub CLI or login
# is needed.
$headers = @(
  'nvsdk_ngx.h', 'nvsdk_ngx_defs.h', 'nvsdk_ngx_params.h', 'nvsdk_ngx_loader.h',
  'nvsdk_ngx_helpers.h', 'nvsdk_ngx_helpers_d3d.h', 'nvsdk_ngx_helpers_cuda.h'
)
$files = @{
  'LICENSE.txt' = 'LICENSE.txt'
  'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib' = 'lib/nvsdk_ngx_s.lib'
  'lib/Windows_x86_64/rel/nvngx_dlss.dll' = 'bin/rel/nvngx_dlss.dll'
  'lib/Windows_x86_64/dev/nvngx_dlss.dll' = 'bin/dev/nvngx_dlss.dll'
}
foreach ($h in $headers) { $files["include/$h"] = "include/$h" }

foreach ($src in $files.Keys) {
  $out = Join-Path $dst $files[$src]
  New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
  Invoke-WebRequest -Uri "$base/$src" -OutFile $out -UseBasicParsing
}
Set-Content -Path (Join-Path $dst 'VERSION') -Value $Version
Get-ChildItem $dst -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring((Resolve-Path $dst).Path.Length + 1) } }, Length
