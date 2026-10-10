<#
Where is a process's busiest or hung thread? No debugger is needed: each sample suspends a thread, reads
its instruction pointer, and scrapes the words on its stack that point into loaded modules (return
addresses, so an approximate call stack), naming a function wherever a PDB is found (the Bridge's
build directory is searched). Use it on a hung mmc.exe to see which module and function the GUI thread
is in. Diagnostic only: it suspends the thread for a few milliseconds per sample.

  .\tools\Sample-ThreadStacks.ps1 -ProcessId 1234                    the thread with the most CPU time
  .\tools\Sample-ThreadStacks.ps1 -ProcessId 1234 -ThreadId 5678 -Samples 3
  .\tools\Sample-ThreadStacks.ps1 -ProcessId 1234 -Registers          also the general registers, each
                                                                      pointer-looking one with the state of
                                                                      the memory it points into

Frames marked '*' are in a module with a PDB; the rest are module+offset.
#>
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    # 0 picks the thread that has used the most CPU.
    [int]$ThreadId = 0,
    [ValidateRange(1, 20)][int]$Samples = 3,
    [ValidateRange(8, 200)][int]$Frames = 40,
    [string]$SymbolPath = '',
    [switch]$Registers
)

$ErrorActionPreference = 'Stop'
if (-not $SymbolPath) {
    $repository = Split-Path -Parent $PSScriptRoot
    $SymbolPath = Join-Path $repository 'build\bin\x64\Release'
}
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class StackSampler {
    const uint THREAD_ALL = 0x1FFFFF;
    const uint PROCESS_ALL = 0x1FFFFF;

    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetThreadContext(IntPtr thread, IntPtr context);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr VirtualQueryEx(IntPtr process, IntPtr address, IntPtr buffer, IntPtr length);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool SymInitializeW(IntPtr process, string path, bool invade);
    [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint options);
    [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool SymFromAddrW(IntPtr process, ulong address, out ulong displacement, IntPtr symbol);
    [DllImport("dbghelp.dll")] static extern bool SymCleanup(IntPtr process);

    public class Module { public string Name; public ulong Base; public ulong Size; }

    static string Describe(IntPtr process, List<Module> modules, ulong address, out bool named) {
        named = false;
        foreach (Module module in modules) {
            if (address < module.Base || address >= module.Base + module.Size) continue;
            IntPtr symbol = Marshal.AllocHGlobal(88 + 512 * 2);
            try {
                // SYMBOL_INFOW: SizeOfStruct (4) at 0, MaxNameLen (4) at 80, Name at 84.
                for (int index = 0; index < 88 + 1024; index++) Marshal.WriteByte(symbol, index, 0);
                Marshal.WriteInt32(symbol, 0, 88);
                Marshal.WriteInt32(symbol, 80, 512);
                ulong displacement;
                if (SymFromAddrW(process, address, out displacement, symbol)) {
                    named = true;
                    return module.Name + "!" + Marshal.PtrToStringUni(new IntPtr(symbol.ToInt64() + 84)) + "+0x" + displacement.ToString("X") +
                        " [rva 0x" + (address - module.Base).ToString("X") + "]";
                }
            } finally { Marshal.FreeHGlobal(symbol); }
            return module.Name + "+0x" + (address - module.Base).ToString("X");
        }
        return null;
    }

    // The state of the memory a register points into (MEMORY_BASIC_INFORMATION is 48 bytes on x64).
    static string Region(IntPtr process, ulong address) {
        if (address < 0x10000 || address > 0x7FFFFFFEFFFF) return null;
        IntPtr info = Marshal.AllocHGlobal(48);
        try {
            if (VirtualQueryEx(process, new IntPtr((long)address), info, new IntPtr(48)) == IntPtr.Zero) return null;
            uint state = (uint)Marshal.ReadInt32(info, 32), protect = (uint)Marshal.ReadInt32(info, 36), type = (uint)Marshal.ReadInt32(info, 40);
            ulong size = (ulong)Marshal.ReadInt64(info, 24);
            string kind = state == 0x1000 ? "commit" : state == 0x2000 ? "reserve" : state == 0x10000 ? "free" : "state " + state.ToString("X");
            string backing = type == 0x20000 ? "private" : type == 0x40000 ? "mapped" : type == 0x1000000 ? "image" : "";
            return kind + " " + backing + " protect=0x" + protect.ToString("X") + " regionBase=0x" +
                ((ulong)Marshal.ReadInt64(info, 0)).ToString("X") + " size=0x" + size.ToString("X");
        } finally { Marshal.FreeHGlobal(info); }
    }

    static readonly string[] RegisterNames = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };

    public static string[] Sample(uint processId, uint threadId, int frames, string symbolPath, List<Module> modules, bool registers) {
        List<string> lines = new List<string>();
        IntPtr process = OpenProcess(PROCESS_ALL, false, processId);
        if (process == IntPtr.Zero) { lines.Add("OpenProcess failed " + Marshal.GetLastWin32Error()); return lines.ToArray(); }
        SymSetOptions(0x2 | 0x4 | 0x10); // UNDNAME | DEFERRED_LOADS | LOAD_LINES
        bool symbols = SymInitializeW(process, symbolPath, true);
        IntPtr thread = OpenThread(THREAD_ALL, false, threadId);
        if (thread == IntPtr.Zero) { lines.Add("OpenThread failed " + Marshal.GetLastWin32Error()); CloseHandle(process); return lines.ToArray(); }
        // x64 CONTEXT: 1232 bytes, 16-byte aligned. ContextFlags at 0x30, Rsp at 0x98, Rip at 0xF8.
        IntPtr raw = Marshal.AllocHGlobal(1232 + 16);
        IntPtr context = new IntPtr((raw.ToInt64() + 15) & ~15L);
        try {
            for (int index = 0; index < 1232; index++) Marshal.WriteByte(context, index, 0);
            Marshal.WriteInt32(context, 0x30, 0x10001F); // CONTEXT_ALL (AMD64 | control | integer | segments | float | debug)
            if (SuspendThread(thread) == 0xFFFFFFFF) { lines.Add("SuspendThread failed"); return lines.ToArray(); }
            ulong rip = 0, rsp = 0;
            byte[] stack = new byte[0];
            try {
                if (!GetThreadContext(thread, context)) { lines.Add("GetThreadContext failed " + Marshal.GetLastWin32Error()); return lines.ToArray(); }
                rip = (ulong)Marshal.ReadInt64(context, 0xF8);
                rsp = (ulong)Marshal.ReadInt64(context, 0x98);
                stack = new byte[32768];
                IntPtr read;
                if (!ReadProcessMemory(process, new IntPtr((long)rsp), stack, new IntPtr(stack.Length), out read)) {
                    // The stack may end sooner than the window: read what is there in smaller steps.
                    int size = 4096;
                    stack = new byte[size];
                    if (!ReadProcessMemory(process, new IntPtr((long)rsp), stack, new IntPtr(size), out read)) stack = new byte[0];
                } else if (read.ToInt64() < stack.Length) {
                    Array.Resize(ref stack, (int)read.ToInt64());
                }
            } finally { ResumeThread(thread); }
            bool named;
            string top = Describe(process, modules, rip, out named);
            lines.Add("  rip " + (top ?? ("0x" + rip.ToString("X"))) + (named ? " *" : ""));
            if (registers) {
                // CONTEXT: Rax at 0x78 .. R15 at 0xF0, in the order RegisterNames lists them.
                for (int index = 0; index < RegisterNames.Length; index++) {
                    ulong value = (ulong)Marshal.ReadInt64(context, 0x78 + index * 8);
                    string where = Describe(process, modules, value, out named);
                    string region = where ?? Region(process, value);
                    lines.Add("    " + RegisterNames[index].PadRight(3) + " = 0x" + value.ToString("X16") + (region == null ? "" : "  -> " + region));
                }
                // The word the string-copy loop reads is [r10 + rcx]: show where it points, too.
                ulong source = (ulong)Marshal.ReadInt64(context, 0xC8) + (ulong)Marshal.ReadInt64(context, 0x80);
                string sourceRegion = Region(process, source);
                lines.Add("    [r10+rcx] = 0x" + source.ToString("X16") + (sourceRegion == null ? "" : "  -> " + sourceRegion));
            }
            int count = 0;
            for (int offset = 0; offset + 8 <= stack.Length && count < frames; offset += 8) {
                ulong value = BitConverter.ToUInt64(stack, offset);
                string text = Describe(process, modules, value, out named);
                if (text == null) continue;
                lines.Add("    [rsp+0x" + offset.ToString("X") + "] " + text + (named ? " *" : ""));
                count++;
            }
        } finally {
            Marshal.FreeHGlobal(raw);
            CloseHandle(thread);
            if (symbols) SymCleanup(process);
            CloseHandle(process);
        }
        return lines.ToArray();
    }
}
'@

$process = Get-Process -Id $ProcessId
$modules = New-Object 'System.Collections.Generic.List[StackSampler+Module]'
foreach ($module in $process.Modules) {
    $entry = New-Object 'StackSampler+Module'
    $entry.Name = $module.ModuleName
    $entry.Base = [uint64]$module.BaseAddress.ToInt64()
    $entry.Size = [uint64]$module.ModuleMemorySize
    $modules.Add($entry)
}
if ($ThreadId -eq 0) {
    $ThreadId = ($process.Threads | Sort-Object { $_.TotalProcessorTime } -Descending | Select-Object -First 1).Id
}
"process $ProcessId ($($process.ProcessName)) thread $ThreadId, $Samples sample(s), symbols from $SymbolPath"
for ($sample = 1; $sample -le $Samples; ++$sample) {
    "sample $sample"
    [StackSampler]::Sample([uint32]$ProcessId, [uint32]$ThreadId, $Frames, $SymbolPath, $modules, [bool]$Registers)
    Start-Sleep -Milliseconds 400
}
