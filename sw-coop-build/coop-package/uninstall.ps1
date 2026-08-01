<#
  Coop Workbench uninstaller. Removes exactly what install.ps1 added, and nothing else.

  Deliberately conservative: it deletes the mod and the logs it writes, then removes the plugins folder ONLY
  if it is empty - another mod may well live there, and an uninstaller that takes someone else's plugin with
  it is worse than one that leaves an empty folder behind.

  It removes the mod, its logs and its config files, so nothing is left to change the behaviour of a future
  install. It does NOT remove dinput8.dll (the ASI loader), because another mod may be relying on it - to
  take that out too, delete <Stormworks>\dinput8.dll yourself once you are sure nothing else needs it.
#>

$ErrorActionPreference = 'Stop'

function Find-Stormworks {
    $steam = $null
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try { $p = (Get-ItemProperty -Path $k -ErrorAction Stop).SteamPath
              if ($p) { $steam = $p -replace '/', '\'; break } } catch {}
    }
    if (-not $steam) { return $null }
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += ($m.Groups[1].Value -replace '\\\\', '\')
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $cand = Join-Path $lib 'steamapps\common\Stormworks'
        if (Test-Path (Join-Path $cand 'stormworks64.exe')) { return $cand }
    }
    return $null
}

Write-Host ''
Write-Host '  Coop Workbench - uninstaller' -ForegroundColor Cyan
Write-Host '  ----------------------------' -ForegroundColor DarkGray

$game = Find-Stormworks
if (-not $game) { $game = (Read-Host '  Stormworks folder').Trim('"',' ') }
if (-not (Test-Path (Join-Path $game 'stormworks64.exe'))) {
    Write-Host "  No stormworks64.exe in: $game" -ForegroundColor Red
    Read-Host '  Press Enter to close'; exit 1
}

$plugins = Join-Path $game 'plugins'
# NEVER remove dinput8.dll. install.ps1 deliberately KEEPS a pre-existing one, because any
# Ultimate-ASI-Loader loads our plugin and another mod may own that file - possibly a newer build. Deleting
# it here would mean uninstalling Coop Workbench silently breaks somebody else's mod, and it directly
# contradicts what install.ps1 promises. This script already refuses to remove a non-empty plugins folder
# for exactly that reason; the loader deserves the same care.
#
# DO remove the config files. An earlier version of this script kept them, reasoning that they are the
# player's and a reinstall should keep their pairing. That was wrong once we understood what coop-peer.txt
# actually does: any id in it PINS the partner and switches auto-discovery off. A leftover one is not a
# saved preference, it is a trap - reinstall, and the mod silently pairs with someone who may have stopped
# playing months ago, reports "connecting" forever, and never looks for anybody else. That exact file cost a
# live test session. Uninstall should mean uninstalled, with nothing left to ambush the next install.
$targets = @(
    (Join-Path $plugins 'coopworkbench.asi'),
    (Join-Path $plugins 'coopworkbench-log.txt'),
    (Join-Path $plugins 'wsdraw-log.txt'),
    (Join-Path $plugins 'coopworkbench-cmd.txt'),
    (Join-Path $plugins 'coopworkbench-CRASH.txt'),
    (Join-Path $plugins 'coop-peer.txt'),
    (Join-Path $plugins 'coop-autoconnect-off.txt'),
    (Join-Path $plugins 'coop-noprops.txt'),
    (Join-Path $plugins 'coop-selftest.txt')
)
$present = $targets | Where-Object { Test-Path $_ }

if (-not $present) {
    Write-Host "  Nothing to remove in: $game" -ForegroundColor Yellow
    Read-Host '  Press Enter to close'; exit 0
}

Write-Host ''
Write-Host "  Found Stormworks:  $game" -ForegroundColor Green
Write-Host ''
Write-Host '  This will delete:' -ForegroundColor White
$present | ForEach-Object { Write-Host "     $_" }
Write-Host ''

if ((Read-Host '  Remove? [y/N]') -notmatch '^[Yy]') {
    Write-Host '  Cancelled - nothing was changed.' -ForegroundColor Yellow
    Read-Host '  Press Enter to close'; exit 0
}

$failed = @()
foreach ($f in $present) {
    try { Remove-Item $f -Force } catch { $failed += $f }
}

# Only if OUR files were the only thing in there - never take another mod's plugin with us.
if ((Test-Path $plugins) -and -not (Get-ChildItem $plugins -Force)) {
    try { Remove-Item $plugins -Force } catch {}
}

Write-Host ''
if ($failed) {
    Write-Host '  Some files could not be removed (is Stormworks running?):' -ForegroundColor Yellow
    $failed | ForEach-Object { Write-Host "     $_" }
    Write-Host '  Close the game and run this again.' -ForegroundColor Yellow
} else {
    Write-Host '  Removed. Stormworks is back to stock.' -ForegroundColor Green
}
Write-Host ''
Read-Host '  Press Enter to close'
