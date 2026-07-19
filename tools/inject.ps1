<#
.SYNOPSIS
    Inject a DLL into a running process (dev validation of the standalone mod).

.DESCRIPTION
    Standard CreateRemoteThread(LoadLibraryW) injector. Used to validate that
    the multivoid payload DLL loads + runs inside VotV-Win64-Shipping.exe without UE4SS.
    The shipping loader will be a proxy DLL; this injector is a DEV TOOL.

    Run under Windows PowerShell 5.1:
      powershell.exe -ExecutionPolicy Bypass -File tools/inject.ps1 -Dll <abs path>
#>
param(
    [Parameter(Mandatory)] [string]$Dll,
    [string]$ProcessName = "VotV-Win64-Shipping"
)
$ErrorActionPreference = 'Stop'
$Dll = (Resolve-Path $Dll).Path

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Inj {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool inh, uint pid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, uint size, uint type, uint prot);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, uint size, out UIntPtr written);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, uint stack, IntPtr start, IntPtr param, uint flags, IntPtr tid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetModuleHandle(string name);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr h, string name);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
}
"@
$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "ERR: $ProcessName not running"; exit 1 }

$PROCESS_ALL = 0x1F0FFF
$MEM_COMMIT_RESERVE = 0x3000
$PAGE_RW = 0x04
$h = [Inj]::OpenProcess($PROCESS_ALL, $false, [uint32]$proc.Id)
if ($h -eq [IntPtr]::Zero) { Write-Output "ERR: OpenProcess failed ($([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message))"; exit 2 }

$bytes = [System.Text.Encoding]::Unicode.GetBytes($Dll + "`0")
$mem = [Inj]::VirtualAllocEx($h, [IntPtr]::Zero, [uint32]$bytes.Length, $MEM_COMMIT_RESERVE, $PAGE_RW)
if ($mem -eq [IntPtr]::Zero) { Write-Output "ERR: VirtualAllocEx failed"; exit 3 }
$written = [UIntPtr]::Zero
[void][Inj]::WriteProcessMemory($h, $mem, $bytes, [uint32]$bytes.Length, [ref]$written)

$loadLib = [Inj]::GetProcAddress([Inj]::GetModuleHandle("kernel32.dll"), "LoadLibraryW")
$thread = [Inj]::CreateRemoteThread($h, [IntPtr]::Zero, 0, $loadLib, $mem, 0, [IntPtr]::Zero)
if ($thread -eq [IntPtr]::Zero) { Write-Output "ERR: CreateRemoteThread failed"; exit 4 }
[void][Inj]::WaitForSingleObject($thread, 5000)
[void][Inj]::CloseHandle($thread); [void][Inj]::CloseHandle($h)
Write-Output ("OK: injected '{0}' into {1} (PID {2})" -f $Dll, $ProcessName, $proc.Id)
