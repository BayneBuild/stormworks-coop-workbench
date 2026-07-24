<#
  Stormworks co-op — connect & inject BOTH mods with one run.
    coop.dll    = block/paint/delete/connection sync (the co-op editor)
    wsdraw.dll  = world-space overlay (see your partner's camera)
  1) shows YOUR SteamID64 (give it to your partner)
  2) asks for your PARTNER's SteamID64 -> writes coop-peer.txt (BOTH DLLs read it)
  3) injects coop.dll then wsdraw.dll into a running stormworks64.exe
  Just run inject-both.bat.
#>
$ErrorActionPreference = 'Stop'
$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$CoopDll  = Join-Path $here 'coop.dll'
$DrawDll  = Join-Path $here 'wsdraw.dll'
$PeerFile = Join-Path $here 'coop-peer.txt'
foreach ($d in @($CoopDll,$DrawDll)) {
  if (-not (Test-Path $d)) { Write-Host "$([IO.Path]::GetFileName($d)) not found next to this script." -ForegroundColor Red; Read-Host 'Press Enter'; exit 1 }
}
$CoopDll = (Resolve-Path $CoopDll).Path
$DrawDll = (Resolve-Path $DrawDll).Path

$STEAMID_BASE = [uint64]76561197960265728
function Get-MySteamID64 {
  try {
    $acct = (Get-ItemProperty 'HKCU:\Software\Valve\Steam\ActiveProcess' -Name ActiveUser -ErrorAction Stop).ActiveUser
    if ($acct -and [uint64]$acct -ne 0) { return ($STEAMID_BASE + [uint64]$acct) }
  } catch {}
  return $null
}

Write-Host "==== Stormworks Co-op + Overlay ====" -ForegroundColor Cyan
$my = Get-MySteamID64
if ($my) {
  Write-Host "`nYOUR SteamID64:  " -NoNewline; Write-Host "$my" -ForegroundColor Green
  Write-Host "(send this to your partner so they can connect to you)`n" -ForegroundColor DarkGray
} else {
  Write-Host "`nCouldn't read your SteamID from the registry (is Steam running & logged in?)." -ForegroundColor Yellow
  Write-Host "After injecting, your ID is printed as 'our=...' in coop-log.txt.`n" -ForegroundColor DarkGray
}

# prompt for partner's SteamID64 (blank = watch-only: receive, don't send)
$peer = $null
while ($true) {
  $inp = (Read-Host "Paste your PARTNER's SteamID64 (or leave blank to just watch)").Trim()
  if ($inp -eq '') { Write-Host "No partner set - watch-only (you receive; you won't send)." -ForegroundColor Yellow; break }
  if ($inp -notmatch '^\d{17}$' -or [uint64]$inp -lt $STEAMID_BASE) { Write-Host "That doesn't look like a SteamID64 (17 digits starting 765...). Try again." -ForegroundColor Yellow; continue }
  if ($my -and [uint64]$inp -eq $my) { Write-Host "That's YOUR id. Enter your PARTNER's (or blank to watch)." -ForegroundColor Yellow; continue }
  $peer = $inp; break
}
if ($peer) { Set-Content -Path $PeerFile -Value $peer -Encoding ascii -NoNewline; Write-Host "Connecting to partner $peer ..." -ForegroundColor Cyan }
else { Set-Content -Path $PeerFile -Value '0' -Encoding ascii -NoNewline }

$p = Get-Process stormworks64 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "`nStormworks is not running. Launch it, open the workbench, then run this again." -ForegroundColor Red; Read-Host 'Press Enter'; exit 1 }

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Inj {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool inh, int pid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, IntPtr size, uint type, uint prot);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, IntPtr stack, IntPtr start, IntPtr param, uint flags, out uint tid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetExitCodeThread(IntPtr h, out uint code);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetModuleHandleA(string m);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr h, string p);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
  const uint PROCESS_NEEDED = 0x0002|0x0008|0x0010|0x0020|0x0400;
  const uint MEM_COMMIT_RESERVE = 0x1000|0x2000;
  const uint PAGE_RW = 0x04;
  const uint INFINITE = 0xFFFFFFFF;
  public static string Run(int pid, string dllPath) {
    IntPtr h = OpenProcess(PROCESS_NEEDED, false, pid);
    if (h==IntPtr.Zero) return "ERR OpenProcess "+Marshal.GetLastWin32Error();
    try {
      byte[] path = System.Text.Encoding.Unicode.GetBytes(dllPath + "\0");
      IntPtr mem = VirtualAllocEx(h, IntPtr.Zero, (IntPtr)path.Length, MEM_COMMIT_RESERVE, PAGE_RW);
      if (mem==IntPtr.Zero) return "ERR VirtualAllocEx "+Marshal.GetLastWin32Error();
      IntPtr written;
      if (!WriteProcessMemory(h, mem, path, (IntPtr)path.Length, out written)) return "ERR WriteProcessMemory "+Marshal.GetLastWin32Error();
      IntPtr loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
      if (loadLib==IntPtr.Zero) return "ERR GetProcAddress LoadLibraryW";
      uint tid;
      IntPtr th = CreateRemoteThread(h, IntPtr.Zero, IntPtr.Zero, loadLib, mem, 0, out tid);
      if (th==IntPtr.Zero) return "ERR CreateRemoteThread "+Marshal.GetLastWin32Error();
      WaitForSingleObject(th, INFINITE);
      uint code; GetExitCodeThread(th, out code);
      CloseHandle(th);
      return code!=0 ? "OK" : "WARN LoadLibrary returned 0";
    } finally { CloseHandle(h); }
  }
}
"@ -Language CSharp

function Inject-One([string]$label, [string]$dll) {
  Write-Host "Injecting $label ..." -NoNewline -ForegroundColor Cyan
  $r = [Inj]::Run($p.Id, $dll)
  if ($r -eq 'OK') { Write-Host " OK" -ForegroundColor Green } else { Write-Host " $r" -ForegroundColor Yellow }
  Start-Sleep -Milliseconds 400
}

Write-Host "`nStormworks PID $($p.Id)" -ForegroundColor DarkGray
Inject-One 'coop.dll (block/wire sync)' $CoopDll
Inject-One 'wsdraw.dll (overlay)'       $DrawDll

Start-Sleep -Milliseconds 400
$clog = Join-Path $here 'coop-log.txt'
$wlog = Join-Path $here 'wsdraw-log.txt'
if (Test-Path $clog) { Write-Host "`ncoop status:" -ForegroundColor Cyan; Get-Content $clog -Tail 3 }
if (Test-Path $wlog) { Write-Host "`noverlay status:" -ForegroundColor Cyan; Get-Content $wlog -Tail 3 }

Write-Host "`n--- In the workbench ---" -ForegroundColor Cyan
Write-Host "  * Build/erase/paint/wire blocks - they sync to your partner (and theirs to you)." -ForegroundColor Gray
Write-Host "  * Look for your partner's camera as a cyan 'PARTNER' frustum in the world." -ForegroundColor Gray
Write-Host "  * To unload later: run unload-both.bat" -ForegroundColor DarkGray
Read-Host "`nPress Enter to close"
