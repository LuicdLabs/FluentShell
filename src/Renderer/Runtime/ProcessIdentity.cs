using System.ComponentModel;
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace FluentShell.Renderer.Runtime;

internal static class ProcessIdentity
{
    public static uint GetPipeServerProcessId(SafePipeHandle pipeHandle)
    {
        if (!GetNamedPipeServerProcessId(pipeHandle, out var processId))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not identify the named-pipe server.");
        }
        return processId;
    }

    public static ulong GetCreationFileTime(Process process)
    {
        if (!GetProcessTimes(process.SafeHandle, out var creation, out _, out _, out _))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not read process creation time.");
        }
        return ((ulong)(uint)creation.dwHighDateTime << 32) | (uint)creation.dwLowDateTime;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint serverProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetProcessTimes(
        SafeProcessHandle process,
        out System.Runtime.InteropServices.ComTypes.FILETIME creation,
        out System.Runtime.InteropServices.ComTypes.FILETIME exit,
        out System.Runtime.InteropServices.ComTypes.FILETIME kernel,
        out System.Runtime.InteropServices.ComTypes.FILETIME user);
}
