<#
  sw-mods.ps1 - see and unload our injected DLLs, WITH CONFIRMATION.

  Why this exists: `echo unload > x-cmd.txt` gives no feedback and silently fails two ways.
    1. PowerShell's > writes UTF-8 WITH A BOM, so the DLL sees EF BB BF before "unload",
       the strncmp fails, and the watcher deletes the file anyway -> silent no-op.
    2. In bash, a Windows path like C:\path\to\repo\... has its backslashes eaten as escapes,
       so the file lands somewhere else entirely.
  This script writes plain ASCII with no BOM and no trailing newline, then VERIFIES by
  enumerating the game's loaded modules until the DLL is actually gone.

  ORDER MATTERS. Every probe/overlay DLL IAT-hooks SwapBuffers and CHAINS to whatever it
  displaced. They must unload newest-first (LIFO) or the slot ends up pointing into a freed
  DLL and the game crashes. -UnloadAll derives load order from the module list and reverses it.

  Usage:
    .\sw-mods.ps1                      # list what is loaded
    .\sw-mods.ps1 -Unload glprobe3     # unload one, with confirmation
    .\sw-mods.ps1 -UnloadAll           # unload every probe/overlay, newest first
    .\sw-mods.ps1 -UnloadAll -IncludeCoop   # also unload coopworkbench.dll (another session may own it)
#>
[CmdletBinding()]
param(
    [string] $Unload,
    [switch] $UnloadAll,
    [switch] $IncludeCoop,
    [int]    $TimeoutSec = 15
)

$dir = $PSScriptRoot

function Get-Injected {
    $p = Get-Process stormworks64 -ErrorAction SilentlyContinue
    if (-not $p) { return $null }
    # NOTE: Modules is in LOAD ORDER, which is what the LIFO unload depends on.
    # The leading comma stops PowerShell unwrapping a single-element array into a bare
    # string (which would then index by character - "coop" printing as "c").
    $names = @($p.Modules | Where-Object { $_.FileName -like "*sw-coop-build*" } |
               ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.ModuleName) })
    return ,$names
}

function Show-List {
    $mods = Get-Injected
    if ($null -eq $mods) { Write-Host "stormworks64 is not running." -ForegroundColor Yellow; return }
    if ($mods.Count -eq 0) { Write-Host "No mod DLLs injected." -ForegroundColor Gray; return }
    Write-Host "Injected DLLs (load order -> unload in REVERSE):" -ForegroundColor Cyan
    for ($i = 0; $i -lt $mods.Count; $i++) { Write-Host ("  {0}. {1}" -f ($i+1), $mods[$i]) -ForegroundColor Gray }
}

function Invoke-Unload([string]$name) {
    $mods = Get-Injected
    if ($null -eq $mods) { Write-Host "stormworks64 is not running." -ForegroundColor Yellow; return $false }
    if ($mods -notcontains $name) {
        Write-Host ("  {0}: not loaded - nothing to do." -f $name) -ForegroundColor Gray
        return $true
    }
    # plain ASCII, no BOM, no trailing newline - this is the part that was silently failing
    [IO.File]::WriteAllBytes("$dir\$name-cmd.txt", [Text.Encoding]::ASCII.GetBytes("unload"))
    Write-Host ("  {0}: sent 'unload', waiting..." -f $name) -NoNewline -ForegroundColor Cyan

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        $now = Get-Injected
        if ($null -eq $now) { Write-Host ""; Write-Host "  game exited!" -ForegroundColor Red; return $false }
        if ($now -notcontains $name) {
            Write-Host " OK - unloaded." -ForegroundColor Green
            $log = "$dir\$name-log.txt"
            if (Test-Path $log) {
                $tail = Get-Content $log -Tail 1
                if ($tail) { Write-Host ("     log: {0}" -f $tail) -ForegroundColor DarkGray }
            }
            return $true
        }
        Write-Host "." -NoNewline -ForegroundColor DarkGray
    }
    Write-Host ""
    Write-Host ("  {0}: TIMED OUT after {1}s - still loaded." -f $name, $TimeoutSec) -ForegroundColor Red
    Write-Host "     The watcher only acts while the game renders frames, and the GL cleanup" -ForegroundColor DarkGray
    Write-Host "     handshake needs a present. Alt-tab INTO the game and re-run." -ForegroundColor DarkGray
    return $false
}

if ($Unload) { Invoke-Unload $Unload | Out-Null; Write-Host ""; Show-List; return }

if ($UnloadAll) {
    $mods = Get-Injected
    if ($null -eq $mods) { Write-Host "stormworks64 is not running." -ForegroundColor Yellow; return }
    if (-not $IncludeCoop) { $mods = @($mods | Where-Object { $_ -ne "coop" }) }
    if ($mods.Count -eq 0) { Write-Host "Nothing to unload." -ForegroundColor Gray; return }
    # reverse load order = LIFO, required so the SwapBuffers hook chain unwinds correctly
    [array]::Reverse($mods)
    Write-Host ("Unloading {0} DLL(s), newest first: {1}" -f $mods.Count, ($mods -join ' -> ')) -ForegroundColor Cyan
    foreach ($m in $mods) {
        if (-not (Invoke-Unload $m)) {
            Write-Host "STOPPING - unloading further DLLs out of order would break the hook chain." -ForegroundColor Red
            break
        }
    }
    Write-Host ""; Show-List; return
}

Show-List
