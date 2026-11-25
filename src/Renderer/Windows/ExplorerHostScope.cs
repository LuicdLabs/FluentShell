using System.Runtime.InteropServices;

namespace FluentShell.Renderer.Windows;

/// <summary>
/// Whether this renderer serves the Bridge that global apply runs inside the system
/// Explorer (the dialog-only session of src/Common/ExplorerDialogPolicy.h).  That Bridge
/// leaves to the renderer what it must not do from inside the shell (the taskbar button
/// of the cloaked dialog), and a proxy standing for an Explorer dialog must never offer
/// to relaunch or pin the shell.
/// </summary>
/// <remarks>
/// The parent is Explorer when the process the renderer was started for (its pid and
/// creation time, so a reused pid is not mistaken for it) runs %WINDIR%\explorer.exe and,
/// once, the watcher (<c>Local\FluentShell.Watch</c>) and its grant for that process
/// (<c>Local\FluentShell.ExplorerDialogs.&lt;pid&gt;.&lt;created&gt;</c>) were live.  The
/// answer is cached for the renderer session: the image never changes, and a session that
/// was granted stays an Explorer session after the grant goes (its taskbar buttons still
/// have to be put back).  Whether the grant is live right now is <see cref="GrantIsLive"/>.
/// </remarks>
internal static class ExplorerHostScope
{
    private const string WatcherMutexName = @"Local\FluentShell.Watch";

    private static readonly object Gate = new();
    private static (uint Pid, ulong Created) _key;
    private static bool _identityIsExplorer;
    private static bool _confirmed;

    internal static bool IsExplorerParent(uint parentPid, ulong parentCreated)
    {
        if (parentPid == 0 || parentCreated == 0) return false;
        lock (Gate)
        {
            if (_key != (parentPid, parentCreated))
            {
                _key = (parentPid, parentCreated);
                _identityIsExplorer = ParentRunsSystemExplorer(parentPid, parentCreated);
                _confirmed = false;
            }
            if (!_identityIsExplorer) return false;
            if (!_confirmed) _confirmed = GrantIsLive(parentPid, parentCreated);
            return _confirmed;
        }
    }

    /// <summary>The watcher runs and its grant for this Explorer process is signaled.</summary>
    internal static bool GrantIsLive(uint parentPid, ulong parentCreated)
    {
        try
        {
            using var watcher = Mutex.OpenExisting(WatcherMutexName);
            using var grant = EventWaitHandle.OpenExisting(GrantEventName(parentPid, parentCreated));
            return grant.WaitOne(0);
        }
        catch
        {
            return false;
        }
    }

    internal static string GrantEventName(uint parentPid, ulong parentCreated) =>
        $@"Local\FluentShell.ExplorerDialogs.{parentPid}.{parentCreated}";

    internal static string SystemExplorerPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe");

    /// <summary>The image is the system Explorer, not a file that only shares its name.</summary>
    internal static bool IsSystemExplorerImage(string? imagePath, string systemExplorerPath)
    {
        if (string.IsNullOrWhiteSpace(imagePath) || string.IsNullOrWhiteSpace(systemExplorerPath)) return false;
        try
        {
            return string.Equals(Path.GetFullPath(imagePath), Path.GetFullPath(systemExplorerPath),
                StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    internal static bool IsSystemExplorerImage(string? imagePath) =>
        IsSystemExplorerImage(imagePath, SystemExplorerPath);

    private static bool ParentRunsSystemExplorer(uint parentPid, ulong parentCreated)
    {
        var process = OpenProcess(ProcessQueryLimitedInformation, false, parentPid);
        if (process == 0) return false;
        try
        {
            if (!GetProcessTimes(process, out var created, out _, out _, out _) ||
                (ulong)created != parentCreated) return false;
            var buffer = new char[32768];
            var length = buffer.Length;
            return QueryFullProcessImageNameW(process, 0, buffer, ref length) &&
                IsSystemExplorerImage(new string(buffer, 0, length));
        }
        catch
        {
            return false;
        }
        finally
        {
            CloseHandle(process);
        }
    }

    private const uint ProcessQueryLimitedInformation = 0x1000;

    [DllImport("kernel32.dll")]
    private static extern nint OpenProcess(uint access, bool inherit, uint processId);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetProcessTimes(nint process, out long creation, out long exit, out long kernel, out long user);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageNameW(nint process, uint flags, char[] name, ref int size);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(nint handle);
}
