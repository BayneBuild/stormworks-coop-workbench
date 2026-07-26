<#
  Dev hot-reload helper (workspace). Unloads the running coopworkbench.dll, waits for the file lock to
  release, then rebuilds. After this, re-inject with inject.ps1 - the GAME STAYS RUNNING.
  (Injection is user-run; this script only does file ops + build.)
#>
$dir = $PSScriptRoot
$dll = "$dir\coopworkbench.dll"
Set-Content "$dir\coopworkbench-cmd.txt" "unload" -Encoding ascii -NoNewline
Write-Host "sent 'unload'; waiting for coopworkbench.dll to release..." -ForegroundColor Cyan
$freed = $false
for ($i=0; $i -lt 50; $i++) {
  try { $fs=[IO.File]::Open($dll,'Open','ReadWrite','None'); $fs.Close(); $freed=$true; break } catch { Start-Sleep -Milliseconds 200 }
}
if (-not $freed) { Write-Host "coopworkbench.dll still locked - mod may not be loaded, or unload failed. Building anyway." -ForegroundColor Yellow }
else { Write-Host "released. rebuilding..." -ForegroundColor Green }
& "$dir\build-coop.cmd"
if (-not (Test-Path $dll)) { Write-Host "BUILD FAILED - no DLL produced." -ForegroundColor Red; exit 1 }

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
