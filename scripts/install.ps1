# Installs Recline VR for the current user from the directory this script is in:
# a build's stage or a release package. Copies the Windows files to $Dest, which
# becomes the driver's root, and registers the driver and the overlay with
# SteamVR. Only touches $Dest and SteamVR's per-user registries.
#
#   install.cmd [-Dest <dir>] [-NoPause]
param(
  [string]$Dest = (Join-Path $env:LOCALAPPDATA 'Programs\recline-vr'),
  [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
$failed = $false
$Src = $PSScriptRoot

# vrpathreg.exe of the SteamVR runtime the OpenVR loader would use.
function Find-VRPathReg {
  $paths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
  if (Test-Path -LiteralPath $paths) {
    foreach ($runtime in (Get-Content -Raw -LiteralPath $paths | ConvertFrom-Json).runtime) {
      $exe = Join-Path $runtime 'bin\win64\vrpathreg.exe'
      if (Test-Path -LiteralPath $exe) { return $exe }
    }
  }
  throw "SteamVR not found (no runtime in $paths): install SteamVR and start it once"
}

# A running SteamVR keeps the driver DLL and the overlay EXE open. Windows lets
# them be renamed but not overwritten, so a busy file is moved aside and the
# leftover removed by a later install.
function Copy-ToDest([string]$Rel) {
  $to = Join-Path $Dest $Rel
  New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
  try {
    Copy-Item -LiteralPath (Join-Path $Src $Rel) -Destination $to -Force
  } catch {
    Move-Item -LiteralPath $to -Destination "$to.old-$PID" -Force
    Copy-Item -LiteralPath (Join-Path $Src $Rel) -Destination $to -Force
  }
}

try {
  if (-not (Test-Path -LiteralPath (Join-Path $Src 'bin\win64\driver_recline.dll'))) {
    throw "$Src has no Windows build (bin\win64\driver_recline.dll)"
  }
  $vrpathreg = Find-VRPathReg
  New-Item -ItemType Directory -Force -Path $Dest | Out-Null
  $Dest = (Resolve-Path -LiteralPath $Dest).Path

  if ($Dest -ne (Resolve-Path -LiteralPath $Src).Path) {
    Get-ChildItem -LiteralPath $Dest -Recurse -File -Filter '*.old-*' |
      Remove-Item -Force -ErrorAction SilentlyContinue
    # Other platforms' bin\ directories and debug symbols stay behind.
    $files = @(Get-ChildItem -LiteralPath (Join-Path $Src 'bin\win64'), (Join-Path $Src 'resources') -Recurse -File |
      Where-Object { $_.Extension -ne '.pdb' } |
      ForEach-Object { $_.FullName.Substring($Src.Length + 1) })
    $files += 'driver.vrdrivermanifest', 'recline_overlay.vrmanifest', 'VERSION', 'LICENSE', 'THIRD_PARTY.md',
              'install.cmd', 'install.ps1', 'uninstall.cmd', 'uninstall.ps1'
    foreach ($f in $files) { Copy-ToDest $f }
  }

  & $vrpathreg adddriver $Dest
  if ($LASTEXITCODE -ne 0) { throw "vrpathreg adddriver failed ($LASTEXITCODE)" }
  Write-Host "driver registered: $Dest"

  $overlay = Join-Path $Dest 'bin\win64\recline_overlay.exe'
  if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) {
    & $overlay --register
    if ($LASTEXITCODE -ne 0) { throw "recline_overlay --register failed ($LASTEXITCODE)" }
  } else {
    Write-Host "SteamVR is not running: start it and run '$overlay --register'"
  }
  Write-Host 'Restart SteamVR to load the driver.'
} catch {
  Write-Host "error: $_" -ForegroundColor Red
  $failed = $true
} finally {
  if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
}
if ($failed) { exit 1 }
