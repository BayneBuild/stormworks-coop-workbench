<#
  Coop Workbench installer.

  Copies exactly two files into the Stormworks folder and touches nothing else:
      <Stormworks>\dinput8.dll                 Ultimate-ASI-Loader (MIT), loads .asi plugins
      <Stormworks>\plugins\coopworkbench.asi   the mod

  It finds Stormworks itself by reading Steam's own library index, so nobody has to go hunting for the
  folder. It SHOWS what it is about to do and waits for a yes before writing anything - this writes into a
  Program Files directory, which is exactly the shape of thing people are right to be suspicious of, and
  replacing the old injector's Defender warning with a silent installer would be a poor trade.
#>

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Stormworks {
    # Steam records its own install path, then each library folder, then the apps in each library.
    $steam = $null
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try { $p = (Get-ItemProperty -Path $k -ErrorAction Stop).SteamPath
              if ($p) { $steam = $p -replace '/', '\'; break } } catch {}
    }
    if (-not $steam) { return $null }

    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        # Every "path" line in the vdf is a library root. Deliberately a regex rather than a vdf parser -
        # the format has changed between Steam versions and this line has not.
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
Write-Host '  Coop Workbench - installer' -ForegroundColor Cyan
Write-Host '  --------------------------' -ForegroundColor DarkGray

$loader = Join-Path $here 'dinput8.dll'
$asi    = Join-Path $here 'coopworkbench.asi'
foreach ($f in @($loader, $asi)) {
    if (-not (Test-Path $f)) {
        Write-Host "  MISSING: $(Split-Path $f -Leaf)" -ForegroundColor Red
        Write-Host '  Extract the whole zip to a folder first, then run this from there.' -ForegroundColor Yellow
        Read-Host '  Press Enter to close'; exit 1
    }
}

$game = Find-Stormworks
if (-not $game) {
    Write-Host '  Could not find Stormworks automatically.' -ForegroundColor Yellow
    Write-Host '  In Steam: right-click Stormworks > Manage > Browse local files, and paste that path here.'
    $game = (Read-Host '  Stormworks folder').Trim('"',' ')
}
if (-not (Test-Path (Join-Path $game 'stormworks64.exe'))) {
    Write-Host "  No stormworks64.exe in: $game" -ForegroundColor Red
    Read-Host '  Press Enter to close'; exit 1
}

$dstLoader = Join-Path $game 'dinput8.dll'
$dstPlugin = Join-Path $game 'plugins\coopworkbench.asi'

# The game holds both files open while it runs, so a copy would fail halfway and leave a half-installed
# state. Check up front and say so plainly rather than failing mid-write.
if (Get-Process stormworks64 -ErrorAction SilentlyContinue) {
    Write-Host ''
    Write-Host '  Stormworks is running - close it first, then run this again.' -ForegroundColor Yellow
    Write-Host '  (The game holds the mod files open while it is open.)' -ForegroundColor DarkGray
    Read-Host '  Press Enter to close'; exit 1
}

# Version strings are embedded in the binary, so an upgrade can say what it is replacing instead of
# silently overwriting. Falls back to the file date when the string cannot be found.
function Get-ModVersion([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    try {
        $bytes = [IO.File]::ReadAllBytes($path)
        $text  = [Text.Encoding]::ASCII.GetString($bytes)
        $m = [regex]::Match($text, 'v\d+\.\d+\.\d+-alpha')
        if ($m.Success) { return $m.Value }
    } catch {}
    return "(built $((Get-Item $path).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))"
}

$isUpgrade = Test-Path $dstPlugin
$oldVer = Get-ModVersion $dstPlugin
$newVer = Get-ModVersion $asi

Write-Host ''
Write-Host "  Found Stormworks:  $game" -ForegroundColor Green
Write-Host ''

if ($isUpgrade) {
    Write-Host "  Coop Workbench is already installed:  $oldVer" -ForegroundColor Cyan
    Write-Host "  This package is:                      $newVer" -ForegroundColor Cyan
    if ($oldVer -eq $newVer) {
        Write-Host '  Same version - reinstalling will just replace the files.' -ForegroundColor DarkGray
    }
    Write-Host ''
    Write-Host '  Your settings are kept: coop-peer.txt and any config beside the mod are not touched.' -ForegroundColor DarkGray
    Write-Host ''
}

Write-Host '  This will copy:' -ForegroundColor White
Write-Host "     $dstPlugin"

# Any Ultimate-ASI-Loader works, and another mod may have installed its own - possibly a NEWER build. If one
# is already there and differs from ours, keep theirs rather than risking a downgrade that breaks their mod.
$skipLoader = $false
if (Test-Path $dstLoader) {
    $same = (Get-Item $dstLoader).Length -eq (Get-Item $loader).Length
    if ($same) {
        $skipLoader = $true
        Write-Host "     $dstLoader" -ForegroundColor DarkGray
        Write-Host '        already present and identical - leaving it alone' -ForegroundColor DarkGray
    } else {
        $skipLoader = $true
        Write-Host "     $dstLoader" -ForegroundColor Yellow
        Write-Host '        a DIFFERENT ASI loader is already installed (another mod?).' -ForegroundColor Yellow
        Write-Host '        Keeping theirs - any Ultimate-ASI-Loader loads our plugin,' -ForegroundColor Yellow
        Write-Host '        and replacing it could break the other mod.' -ForegroundColor Yellow
    }
} else {
    Write-Host "     $dstLoader"
}
Write-Host ''

# ---- Other mods. Worth surfacing BEFORE writing anything: the first two can genuinely conflict with us,
# and the third is a co-op specific gotcha that nobody would think to check.
$notes = @()

# 1. Other .asi plugins share our loader. They are not a problem in themselves, but if the game misbehaves
#    afterwards it matters enormously whether something else is also hooking it.
$otherAsi = @(Get-ChildItem (Join-Path $game 'plugins') -Filter *.asi -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -ne 'coopworkbench.asi' })
if ($otherAsi) { $notes += "Other ASI plugins installed: $(($otherAsi | ForEach-Object Name) -join ', ')" }

# 2. Other proxy DLLs hook the game the same way we do. Stock Stormworks ships the ones excluded here, so
#    anything else with a known proxy name arrived with another mod.
$stock = @('OpenAL64.dll','openal32.dll','steam_api.dll','steam_api64.dll','steamclient.dll',
           'steamclient64.dll','tier0_s.dll','tier0_s64.dll','vstdlib_s.dll','vstdlib_s64.dll','dinput8.dll')
$proxyNames = @('version.dll','winmm.dll','dsound.dll','d3d9.dll','d3d11.dll','d3d12.dll','dxgi.dll',
                'xinput1_3.dll','xinput1_4.dll','dbghelp.dll','wininet.dll','bink2w64.dll')
$otherProxy = @(Get-ChildItem $game -Filter *.dll -ErrorAction SilentlyContinue |
                Where-Object { $stock -notcontains $_.Name -and $proxyNames -contains $_.Name })
if ($otherProxy) { $notes += "Other mod loaders present: $(($otherProxy | ForEach-Object Name) -join ', ')" }

# 3. Workshop PART mods only. This is about SYNC, not conflict: a part the partner does not have is logged
#    and skipped rather than placed, which looks exactly like a broken sync. But counting workshop items is
#    useless - a real library is overwhelmingly vehicles, microcontrollers and mission playlists, none of
#    which affect part availability. Measured on the author's install: 148 items, ZERO part mods. Only an
#    item carrying mod.xml or a definitions folder adds components, so only those are worth mentioning.
$ws = Join-Path (Split-Path (Split-Path $game -Parent) -Parent) 'workshop\content\573090'
$partMods = 0
if (Test-Path $ws) {
    foreach ($item in Get-ChildItem $ws -Directory -ErrorAction SilentlyContinue) {
        if ((Test-Path (Join-Path $item.FullName 'mod.xml')) -or
            (Test-Path (Join-Path $item.FullName 'definitions'))) { $partMods++ }
    }
}

if ($notes -or $partMods -gt 0) {
    Write-Host '  Heads up:' -ForegroundColor Yellow
    foreach ($n in $notes) {
        Write-Host "     - $n" -ForegroundColor Yellow
    }
    if ($notes) {
        Write-Host '       These hook the game too. Coop Workbench should coexist with them, but if' -ForegroundColor DarkGray
        Write-Host '       anything misbehaves, try with only one mod installed to narrow it down.' -ForegroundColor DarkGray
    }
    if ($partMods -gt 0) {
        Write-Host "     - $partMods workshop mod(s) add PARTS" -ForegroundColor Yellow
        Write-Host '       Your co-op partner needs the same ones. A part they do not have is skipped' -ForegroundColor DarkGray
        Write-Host '       rather than placed, which looks like a sync failure but is not one.' -ForegroundColor DarkGray
    }
    Write-Host ''
}

Write-Host '  To uninstall later, run uninstall.bat, or just delete those two.' -ForegroundColor DarkGray
Write-Host ''

$verb = if ($isUpgrade) { 'Update' } else { 'Install' }
if ((Read-Host "  $verb`? [y/N]") -notmatch '^[Yy]') {
    Write-Host '  Cancelled - nothing was changed.' -ForegroundColor Yellow
    Read-Host '  Press Enter to close'; exit 0
}

try {
    New-Item -ItemType Directory -Force (Join-Path $game 'plugins') | Out-Null
    if (-not $skipLoader) { Copy-Item $loader $dstLoader -Force }
    Copy-Item $asi $dstPlugin -Force
} catch {
    Write-Host ''
    Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host '  If Stormworks is running, close it and try again.' -ForegroundColor Yellow
    Write-Host '  If the folder is protected, right-click install.bat > Run as administrator.' -ForegroundColor Yellow
    Read-Host '  Press Enter to close'; exit 1
}

Write-Host ''
Write-Host $(if ($isUpgrade) { "  Updated to $newVer." } else { '  Installed.' }) -ForegroundColor Green
Write-Host ''
Write-Host '  Just launch Stormworks - the mod loads with it. No injector, nothing to run each session.'
Write-Host '  You will see it start up on the loading screen. Press F6 in-game for the overlay and controls.'
Write-Host ''
Write-Host '  Playing with a friend? If you are Steam friends and you both have this installed, you pair'
Write-Host '  automatically - there is nothing to configure.' -ForegroundColor DarkGray
Write-Host ''
Read-Host '  Press Enter to close'
