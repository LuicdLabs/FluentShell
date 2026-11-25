using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

/// <summary>
/// What the renderer does for the Bridge that runs inside Explorer: it takes the cloaked native
/// dialog's taskbar button away itself (the guest Bridge never edits the shell's taskbar) and puts
/// it back on every teardown path; it never offers to relaunch or pin the shell; and it draws a
/// Properties page's frameless read-only values as text.
/// </summary>
public sealed class ExplorerHostPresentationTests
{
    private sealed class RecordingSink : ITaskbarTabSink
    {
        public List<(nint Window, bool Present)> Requests { get; } = [];
        public int Flushes { get; private set; }
        public void Request(nint nativeWindow, bool present) => Requests.Add((nativeWindow, present));
        public bool Flush(TimeSpan timeout) { Flushes++; return true; }
    }

    private const string Native = "0x0000000000ABCDEF";
    private const nint NativeHandle = 0xABCDEF;

    [Fact]
    public void AnExplorerSurfaceLosesItsNativeButtonOnlyOnceItIsInteractive()
    {
        var sink = new RecordingSink();
        var tabs = new ExplorerTaskbarTabs(() => true, sink);
        var surface = Guid.NewGuid();
        tabs.OnCommit(surface, "window", Native, showInTaskbar: true, show: true, interactive: false);
        Assert.Empty(sink.Requests);
        tabs.OnCommit(surface, "window", Native, showInTaskbar: true, show: true, interactive: true);
        Assert.Equal([(NativeHandle, false)], sink.Requests);
        // A second interactive commit of the same surface asks nothing more.
        tabs.OnCommit(surface, "window", Native, showInTaskbar: true, show: true, interactive: true);
        Assert.Single(sink.Requests);
    }

    [Fact]
    public void EveryTeardownPathPutsTheButtonBack()
    {
        var sink = new RecordingSink();
        var tabs = new ExplorerTaskbarTabs(() => true, sink);
        var closed = Guid.NewGuid();
        var hidden = Guid.NewGuid();
        var remaining = Guid.NewGuid();
        foreach (var surface in new[] { closed, hidden, remaining })
            tabs.OnCommit(surface, "window", Native, showInTaskbar: true, show: true, interactive: true);
        tabs.OnClosed(closed);
        tabs.OnCommit(hidden, "window", Native, showInTaskbar: true, show: false, interactive: false);
        Assert.Equal([remaining], tabs.HiddenSurfaces);
        tabs.RestoreAll(TimeSpan.FromSeconds(1));
        Assert.Empty(tabs.HiddenSurfaces);
        Assert.Equal(3, sink.Requests.Count(request => request.Present));
        Assert.Equal(1, sink.Flushes);
        // Closing an already restored surface asks for nothing.
        tabs.OnClosed(closed);
        Assert.Equal(3, sink.Requests.Count(request => request.Present));
    }

    [Theory]
    [InlineData(false, "window", true, Native)]   // another application's Bridge owns its buttons
    [InlineData(true, "messageBox", true, Native)] // a translated dialog stands for no native window
    [InlineData(true, "window", false, Native)]    // the native window had no button
    [InlineData(true, "window", true, null)]
    public void OnlyAnExplorerNativeWindowWithAButtonIsTouched(bool explorer, string kind, bool showInTaskbar,
        string? nativeHwnd)
    {
        var sink = new RecordingSink();
        var tabs = new ExplorerTaskbarTabs(() => explorer, sink);
        tabs.OnCommit(Guid.NewGuid(), kind, nativeHwnd, showInTaskbar, show: true, interactive: true);
        tabs.RestoreAll(TimeSpan.FromSeconds(1));
        Assert.Empty(sink.Requests);
    }

    [Fact]
    public void ARestoredButtonComesBackOnlyForAVisibleWindow()
    {
        var calls = new List<string>();
        var taskbar = new RecordingTaskbar(calls);
        TaskbarTabWorker.Apply(taskbar, 1, present: true, exists: _ => true, visible: _ => false);
        TaskbarTabWorker.Apply(taskbar, 2, present: true, exists: _ => false, visible: _ => true);
        TaskbarTabWorker.Apply(taskbar, 3, present: false, exists: _ => false, visible: _ => true);
        Assert.Empty(calls);
        TaskbarTabWorker.Apply(taskbar, 4, present: true, exists: _ => true, visible: _ => true);
        TaskbarTabWorker.Apply(taskbar, 5, present: false, exists: _ => true, visible: _ => false);
        Assert.Equal(["add 4", "delete 5"], calls);
    }

    private sealed class RecordingTaskbar(List<string> calls) : ITaskbarTabOperations
    {
        public void AddTab(nint window) => calls.Add($"add {window}");
        public void DeleteTab(nint window) => calls.Add($"delete {window}");
    }

    [Fact]
    public void AnExplorerProxyCanNeitherRelaunchNorPinTheShell()
    {
        var explorer = ExplorerHostScope.SystemExplorerPath;
        var plan = ProxyIdentity.PlanTaskbarGroup(explorer, null, explorerHost: false, _ => "Windows Explorer");
        Assert.NotNull(plan);
        Assert.Null(plan.RelaunchCommand);
        Assert.True(plan.PreventPinning);
        Assert.Null(plan.Id);
        var granted = ProxyIdentity.PlanTaskbarGroup(@"C:\Tools\host.exe", "Vendor.App", explorerHost: true, _ => "Host");
        Assert.NotNull(granted);
        Assert.Null(granted.RelaunchCommand);
        Assert.True(granted.PreventPinning);
        Assert.Equal("Vendor.App", granted.Id);
    }

    [Fact]
    public void AnApplicationProxyKeepsItsRelaunchIdentity()
    {
        var plan = ProxyIdentity.PlanTaskbarGroup(@"C:\Tools\Legacy Host.exe", null, explorerHost: false, _ => "Legacy");
        Assert.NotNull(plan);
        Assert.Equal("FluentShell.Projection.LegacyHost", plan.Id);
        Assert.Equal("\"C:\\Tools\\Legacy Host.exe\"", plan.RelaunchCommand);
        Assert.False(plan.PreventPinning);
    }

    [Fact]
    public void ANameLikeExplorerIsNotTheSystemExplorer()
    {
        Assert.True(ExplorerHostScope.IsSystemExplorerImage(@"C:\Windows\explorer.exe", @"C:\Windows\explorer.exe"));
        Assert.False(ExplorerHostScope.IsSystemExplorerImage(@"C:\Temp\explorer.exe", @"C:\Windows\explorer.exe"));
        Assert.False(ExplorerHostScope.IsSystemExplorerImage(null, @"C:\Windows\explorer.exe"));
        Assert.False(ExplorerHostScope.IsExplorerParent(0, 0));
    }

    [Theory]
    [InlineData(true, 0x50000880UL, 0x4UL, true)]   // Properties' Type/Location/Size: ES_READONLY, no frame
    [InlineData(true, 0x50800880UL, 0x4UL, false)]  // WS_BORDER keeps the box
    [InlineData(true, 0x50000880UL, 0x204UL, false)] // WS_EX_CLIENTEDGE keeps the box
    [InlineData(false, 0x50010080UL, 0x4UL, false)] // the editable name box stays an input
    public void FramelessReadOnlyEditsPresentAsText(bool readOnly, ulong style, ulong exStyle, bool plain) =>
        Assert.Equal(plain, ControlFactory.PresentsAsPlainText(readOnly, style, exStyle));
}
