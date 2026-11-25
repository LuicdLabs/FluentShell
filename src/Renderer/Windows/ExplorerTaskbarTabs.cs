using System.Runtime.InteropServices;
using FluentShell.Renderer.Runtime;

namespace FluentShell.Renderer.Windows;

/// <summary>Where a taskbar-button wish goes: the shell's taskbar, or a test's record.</summary>
internal interface ITaskbarTabSink
{
    /// <summary>Queues the latest wish for one native window; never blocks on the shell.</summary>
    void Request(nint nativeWindow, bool present);

    /// <summary>Waits, at most <paramref name="timeout"/>, until every queued wish was applied.</summary>
    bool Flush(TimeSpan timeout);
}

/// <summary>
/// The taskbar button of a native Explorer dialog while its proxy holds the screen.
/// </summary>
/// <remarks>
/// A cloaked window keeps its taskbar button, so a projected dialog showed twice on the
/// taskbar.  For every other application the Bridge removes the native button itself
/// (TaskbarTabs in RendererSession.cpp).  The Bridge that runs inside explorer.exe does
/// not: calling ITaskbarList from inside the shell mutated the shell's own taskbar from
/// its own process.  So for an Explorer parent the renderer does it, out of process,
/// through the public ITaskbarList: the button goes when the projection is committed
/// interactive and comes back when the surface closes, is rolled back (a commit that
/// hides it, a fault, a window.close) or the renderer shuts down.  Never for any other
/// parent, where the Bridge still owns the button.
/// </remarks>
internal sealed class ExplorerTaskbarTabs
{
    private readonly Lazy<bool> _explorerScope;
    private readonly ITaskbarTabSink _sink;
    private readonly Dictionary<Guid, nint> _hidden = [];

    internal ExplorerTaskbarTabs(Func<bool> explorerScope, ITaskbarTabSink sink)
    {
        _explorerScope = new Lazy<bool>(explorerScope, LazyThreadSafetyMode.ExecutionAndPublication);
        _sink = sink;
    }

    internal IReadOnlyCollection<Guid> HiddenSurfaces => _hidden.Keys;

    internal void OnCommit(Guid surfaceId, string surfaceKind, string? nativeHwnd, bool showInTaskbar,
        bool show, bool interactive)
    {
        if (!show)
        {
            Restore(surfaceId);
            return;
        }
        // Like the Bridge: the button goes only once the proxy is the interactive window.
        if (!interactive || _hidden.ContainsKey(surfaceId)) return;
        // A translated message box or task dialog stands for no native window of its own.
        if (surfaceKind != "window" || !showInTaskbar || nativeHwnd is null) return;
        if (TranslatedWindow.TryParseHwnd(nativeHwnd) is not { } native) return;
        if (!_explorerScope.Value) return;
        _hidden[surfaceId] = native;
        RendererDiagnostics.Log($"explorer taskbar tab hidden surface={surfaceId} native=0x{(ulong)(nuint)native:X}");
        _sink.Request(native, false);
    }

    internal void OnClosed(Guid surfaceId) => Restore(surfaceId);

    /// <summary>Every hidden button back, and waits (bounded) so an exit cannot drop them.</summary>
    internal void RestoreAll(TimeSpan flushTimeout)
    {
        foreach (var surfaceId in _hidden.Keys.ToArray()) Restore(surfaceId);
        if (!_sink.Flush(flushTimeout))
            RendererDiagnostics.Log("explorer taskbar tabs not flushed before the deadline");
    }

    private void Restore(Guid surfaceId)
    {
        if (!_hidden.Remove(surfaceId, out var native)) return;
        RendererDiagnostics.Log($"explorer taskbar tab restored surface={surfaceId} native=0x{(ulong)(nuint)native:X}");
        _sink.Request(native, true);
    }
}

/// <summary>The operations of ITaskbarList this code uses.</summary>
internal interface ITaskbarTabOperations
{
    void AddTab(nint window);
    void DeleteTab(nint window);
}

/// <summary>
/// One worker thread that asks the shell, so a busy Explorer never holds the UI thread;
/// it applies the latest wish per window.  A button comes back only for a window that
/// still exists and is visible, and goes only from one that still exists.
/// </summary>
internal sealed class TaskbarTabWorker : ITaskbarTabSink
{
    private readonly Func<ITaskbarTabOperations?> _connect;
    private readonly Func<nint, bool> _exists;
    private readonly Func<nint, bool> _visible;
    private readonly object _gate = new();
    private readonly Dictionary<nint, bool> _wishes = [];
    private Thread? _thread;
    private bool _busy;

    internal static TaskbarTabWorker Shell { get; } = new(ShellTaskbarList.Connect,
        window => IsWindow(window), window => IsWindowVisible(window));

    internal TaskbarTabWorker(Func<ITaskbarTabOperations?> connect, Func<nint, bool> exists, Func<nint, bool> visible)
    {
        _connect = connect;
        _exists = exists;
        _visible = visible;
    }

    public void Request(nint nativeWindow, bool present)
    {
        if (nativeWindow == 0) return;
        lock (_gate)
        {
            _wishes[nativeWindow] = present;
            if (_thread is null)
            {
                // Background: an Explorer that never answers must not keep the renderer alive.
                _thread = new Thread(Run) { IsBackground = true, Name = "FluentShell taskbar tabs" };
                _thread.SetApartmentState(ApartmentState.MTA);
                _thread.Start();
            }
            Monitor.PulseAll(_gate);
        }
    }

    public bool Flush(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        lock (_gate)
        {
            while (_wishes.Count != 0 || _busy)
            {
                var remaining = deadline - DateTime.UtcNow;
                if (remaining <= TimeSpan.Zero) return false;
                Monitor.Wait(_gate, remaining);
            }
            return true;
        }
    }

    internal static void Apply(ITaskbarTabOperations taskbar, nint window, bool present,
        Func<nint, bool> exists, Func<nint, bool> visible)
    {
        if (!exists(window)) return;
        if (present)
        {
            if (visible(window)) taskbar.AddTab(window);
        }
        else
        {
            taskbar.DeleteTab(window);
        }
    }

    private void Run()
    {
        ITaskbarTabOperations? taskbar = null;
        try { taskbar = _connect(); }
        catch (Exception exception) { RendererDiagnostics.Log("taskbar list unavailable: " + exception.Message); }
        while (true)
        {
            KeyValuePair<nint, bool>[] wishes;
            lock (_gate)
            {
                _busy = false;
                Monitor.PulseAll(_gate);
                while (_wishes.Count == 0) Monitor.Wait(_gate);
                wishes = _wishes.ToArray();
                _wishes.Clear();
                _busy = true;
            }
            if (taskbar is null) continue;
            foreach (var (window, present) in wishes)
            {
                try { Apply(taskbar, window, present, _exists, _visible); }
                catch (Exception exception)
                {
                    RendererDiagnostics.Log($"taskbar tab {(present ? "add" : "delete")} failed native=0x{(ulong)(nuint)window:X}: {exception.Message}");
                }
            }
        }
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint hwnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(nint hwnd);
}

/// <summary>The shell's public ITaskbarList, created in the worker's apartment.</summary>
internal sealed class ShellTaskbarList : ITaskbarTabOperations
{
    private readonly ITaskbarList _list;

    private ShellTaskbarList(ITaskbarList list) => _list = list;

    internal static ITaskbarTabOperations? Connect()
    {
        var type = Type.GetTypeFromCLSID(new Guid("56FDF344-FD6D-11D0-958A-006097C9A090"), throwOnError: false);
        if (type is null || Activator.CreateInstance(type) is not ITaskbarList list) return null;
        if (list.HrInit() < 0)
        {
            Marshal.ReleaseComObject(list);
            return null;
        }
        return new ShellTaskbarList(list);
    }

    public void AddTab(nint window) => Marshal.ThrowExceptionForHR(_list.AddTab(window));

    public void DeleteTab(nint window) => Marshal.ThrowExceptionForHR(_list.DeleteTab(window));

    [ComImport, Guid("56FDF342-FD6D-11D0-958A-006097C9A090"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITaskbarList
    {
        [PreserveSig] int HrInit();
        [PreserveSig] int AddTab(nint hwnd);
        [PreserveSig] int DeleteTab(nint hwnd);
        [PreserveSig] int ActivateTab(nint hwnd);
        [PreserveSig] int SetActiveAlt(nint hwnd);
    }
}
