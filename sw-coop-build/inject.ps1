<#
  LoadLibrary DLL injector (pure PowerShell / .NET P-Invoke).
  Injects a DLL into a target process by PID or process name.
    inject.ps1 -ProcId 1234 -Dll F:\...\probe.dll
    inject.ps1 -Name stormworks64 -Dll F:\...\probe.dll
#>
param(
  [int]$ProcId,
  [string]$Name,
  [Parameter(Mandatory)][string]$Dll
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Dll)) { throw "DLL not found: $Dll" }
$Dll = (Resolve-Path $Dll).Path

if (-not $ProcId) {
  if (-not $Name) { throw "Provide -ProcId or -Name" }
  $p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { throw "Process '$Name' not running" }
  $ProcId = $p.Id
}

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

  const uint PROCESS_NEEDED = 0x0002|0x0008|0x0010|0x0020|0x0400; // CreateThread|VMop|VMwrite|VMread|QueryInfo
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
      IntPtr k32 = GetModuleHandleA("kernel32.dll");
      IntPtr loadLib = GetProcAddress(k32, "LoadLibraryW");
      if (loadLib==IntPtr.Zero) return "ERR GetProcAddress LoadLibraryW";
      uint tid;
      IntPtr th = CreateRemoteThread(h, IntPtr.Zero, IntPtr.Zero, loadLib, mem, 0, out tid);
      if (th==IntPtr.Zero) return "ERR CreateRemoteThread "+Marshal.GetLastWin32Error();
      WaitForSingleObject(th, INFINITE);
      uint code; GetExitCodeThread(th, out code);
      CloseHandle(th);
      return code!=0 ? ("OK loaded, remote HMODULE(low32)=0x"+code.ToString("X")) : "WARN thread ran but LoadLibrary returned 0";
    } finally { CloseHandle(h); }
  }
}
"@ -Language CSharp

Write-Host "Injecting $Dll into PID $ProcId ..." -ForegroundColor Cyan
$r = [Inj]::Run($ProcId, $Dll)
if ($r -like 'OK*') { Write-Host $r -ForegroundColor Green } else { Write-Host $r -ForegroundColor Yellow }
