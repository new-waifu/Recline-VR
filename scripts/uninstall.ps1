# Uninstalls Recline VR: unregisters the overlay and the driver.
#
#   uninstall.cmd [-Dest <dir>] [-NoPause]
param(
  [string]$Dest = (Join-Path $env:LOCALAPPDATA 'Programs\recline-vr'),
  [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
$failed = $false

# vrpathreg.exe of the SteamVR runtime the OpenVR loader would use.
function Find-VRPathReg {
  $paths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
  if (Test-Path -LiteralPath $paths) {
    foreach ($runtime in (Get-Content -Raw -LiteralPath $paths | ConvertFrom-Json).runtime) {
      $exe = Join-Path $runtime 'bin\win64\vrpathreg.exe'
      if (Test-Path -LiteralPath $exe) { return $exe }
    }
  }
  throw "SteamVR not found (no runtime in $paths)"
}

try {
  $vrpathreg = Find-VRPathReg
  # Works with SteamVR stopped too (see install.ps1). Checked before
  # unregistering, which may start a short-lived vrserver.
  $steamvrRunning = [bool](Get-Process -Name vrserver -ErrorAction SilentlyContinue)
  & (Join-Path $Dest 'bin\win64\recline_overlay.exe') --unregister
  if ($LASTEXITCODE -ne 0) { throw "recline_overlay --unregister failed ($LASTEXITCODE)" }
  & $vrpathreg removedriver $Dest
  if ($LASTEXITCODE -ne 0) { throw "vrpathreg removedriver failed ($LASTEXITCODE)" }
  Write-Host 'driver unregistered.'
  if ($steamvrRunning) {
    Write-Host "Now quit SteamVR so the driver is unloaded, then delete $Dest"
  } else {
    Write-Host "Now delete $Dest"
  }
} catch {
  Write-Host "error: $_" -ForegroundColor Red
  $failed = $true
} finally {
  if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
}
if ($failed) { exit 1 }
