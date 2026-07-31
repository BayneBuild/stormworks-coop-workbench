<#
  Coop Workbench uninstaller. Removes exactly what install.ps1 added, and nothing else.

  Deliberately conservative: it deletes the two files it placed plus the logs the mod writes beside itself,
  then removes the plugins folder ONLY if it is empty - another mod may well live there, and an uninstaller
  that takes someone else's plugin with it is worse than one that leaves an empty folder behind.
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
$targets = @(
    (Join-Path $game 'dinput8.dll'),
    (Join-Path $plugins 'coopworkbench.asi'),
    (Join-Path $plugins 'coopworkbench-log.txt'),
    (Join-Path $plugins 'wsdraw-log.txt'),
    (Join-Path $plugins 'coopworkbench-cmd.txt'),
    (Join-Path $plugins 'coop-peer.txt'),
    (Join-Path $plugins 'coop-autoconnect-off.txt')
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
