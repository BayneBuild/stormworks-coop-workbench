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
Write-Host "`nDone. Now re-inject (game still running):" -ForegroundColor Cyan
Write-Host "  powershell -ExecutionPolicy Bypass -File $dir\inject.ps1 -Name stormworks64 -Dll $dll" -ForegroundColor Gray
