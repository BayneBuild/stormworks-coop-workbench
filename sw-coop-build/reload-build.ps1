<#
  Dev hot-reload helper (workspace). Unloads the running coopworkbench.dll, waits for the file lock to
  release, then rebuilds. After this, re-inject with inject.ps1 - the GAME STAYS RUNNING.
  (Injection is user-run; this script only does file ops + build.)
#>
param([switch]$Force)

$dir = $PSScriptRoot
$dll = "$dir\coopworkbench.dll"

# SAFETY GATE. This script's first act is to UNLOAD the running mod - which is correct for a dev cycle, and
# destructive in the middle of someone's test run. It has already yanked the DLL out from under an
# in-progress verification once. So: if the DLL is currently locked, it is loaded in the game and somebody is
# using it. Refuse, and make the caller say -Force explicitly.
$locked = $false
try { $fs=[IO.File]::Open($dll,'Open','ReadWrite','None'); $fs.Close() } catch { $locked = $true }
if ($locked -and -not $Force) {
  Write-Host "REFUSING: coopworkbench.dll is locked, so the mod is loaded and a session is live." -ForegroundColor Red
  Write-Host "Unloading now would interrupt whatever is being tested." -ForegroundColor Red
  Write-Host "  - to verify a code change compiles without touching the session:  .\build-check.ps1" -ForegroundColor Gray
  Write-Host "  - when the test is genuinely finished:                            .\reload-build.ps1 -Force" -ForegroundColor Gray
  exit 2
}

Set-Content "$dir\coopworkbench-cmd.txt" "unload" -Encoding ascii -NoNewline
Write-Host "sent 'unload'; waiting for coopworkbench.dll to release..." -ForegroundColor Cyan
$freed = $false
for ($i=0; $i -lt 50; $i++) {
  try { $fs=[IO.File]::Open($dll,'Open','ReadWrite','None'); $fs.Close(); $freed=$true; break } catch { Start-Sleep -Milliseconds 200 }
}
if (-not $freed) { Write-Host "coopworkbench.dll still locked - mod may not be loaded, or unload failed. Building anyway." -ForegroundColor Yellow }
else { Write-Host "released. rebuilding..." -ForegroundColor Green }
& "$dir\build-coop.cmd"
# Check the EXIT CODE, not just that a DLL exists: a failed compile leaves the PREVIOUS binary sitting there,
# and propagating that is precisely how a stale build gets tested and misread as a pass or fail of a fix that
# is not in it. Stop here instead.
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) {
  Write-Host "BUILD FAILED - keeping the existing DLL and NOT propagating it." -ForegroundColor Red
  exit 1
}

# Propagate the fresh DLL everywhere a copy is injected from. Testing a STALE build has now cost three test
# rounds (each time the result looked like a pass/fail of a fix that was not even in the loaded DLL), so the
# rebuild pushes itself out rather than relying on anyone remembering to copy it.
$targets = @(
  "$dir\coop-package\coopworkbench.dll",
  "$env:USERPROFILE\OneDrive\Desktop\New folder\coopworkbench.dll",
  "$env:USERPROFILE\Desktop\New folder\coopworkbench.dll"
)
foreach ($t in $targets) {
  $tdir = Split-Path $t -Parent
  if (Test-Path $tdir) {
    try { Copy-Item $dll $t -Force; Write-Host "  -> updated $t" -ForegroundColor DarkGray }
    catch { Write-Host "  -> COULD NOT update $t (in use? unload that copy first)" -ForegroundColor Yellow }
  }
}

$stamp = (Get-Item $dll).LastWriteTime.ToString("HH:mm:ss")
Write-Host "`nBuilt at $stamp - the overlay header and log must show this time, or you are running a stale DLL." -ForegroundColor Cyan
Write-Host "Now re-inject (game still running):" -ForegroundColor Cyan
Write-Host "  powershell -ExecutionPolicy Bypass -File $dir\inject.ps1 -Name stormworks64 -Dll $dll" -ForegroundColor Gray
