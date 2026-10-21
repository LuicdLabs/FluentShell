using System.Runtime.InteropServices;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Windows;

/// <summary>
/// The window messages the proxy has to reason about, and the two classifications
/// the pre-interactive gate makes from them.
///
/// While a surface is committed but not yet interactive, the proxy is visible but
/// must behave as if it were not there: input is swallowed and geometry is pinned.
/// Naming the message ranges here keeps that policy readable at the call site.
/// </summary>
internal static class WindowMessages
{
    public const uint EnterSizeMove = 0x0231;
    public const uint ExitSizeMove = 0x0232;
    public const uint WindowPosChanging = 0x0046;
    public const uint MouseActivate = 0x0021;
    public const uint SysCommand = 0x0112;
    public const uint NcDestroy = 0x0082;

    public const uint SwpNoSize = 0x0001;
    public const uint SwpNoMove = 0x0002;

    // MA_NOACTIVATEANDEAT: do not activate, and discard the click.
    public const int NoActivateAndEat = 4;

    private const uint Gesture = 0x0119;
    private const uint GestureNotify = 0x011A;
    private const uint Touch = 0x0240;
    private const uint KeyFirst = 0x0100;
    private const uint KeyLast = 0x0109;
    private const uint NcMouseFirst = 0x00A0;
    private const uint NcMouseLast = 0x00AD;
    private const uint MouseFirst = 0x0200;
    private const uint MouseLast = 0x020E;
    private const uint PointerFirst = 0x0245;
    private const uint PointerLast = 0x0253;

    private const uint ScSize = 0xF000;
    private const uint ScMove = 0xF010;
    private const uint ScMinimize = 0xF020;
    private const uint ScMaximize = 0xF030;
    private const uint ScClose = 0xF060;
    private const uint ScRestore = 0xF120;

    /// <summary>Keyboard, mouse, touch, pen, and gesture input.</summary>
    public static bool IsInputMessage(uint message) =>
        message is >= KeyFirst and <= KeyLast or
        >= NcMouseFirst and <= NcMouseLast or
        >= MouseFirst and <= MouseLast or
        Gesture or GestureNotify or Touch or
        >= PointerFirst and <= PointerLast;

    /// <summary>A WM_SYSCOMMAND that would move, size, or close the window.</summary>
    public static bool IsPlacementSystemCommand(nint wParam) =>
        ((nuint)wParam & 0xFFF0u) is ScSize or ScMove or ScMinimize or ScMaximize or
            ScRestore or ScClose;
}

/// <summary>
/// The Win32 surface the WinUI proxy still needs: comctl32 subclassing for the
/// modal move/size loop, DWM cloaking, owner/enable state, per-window DPI, and
/// IME composition. Every entry point is wrapped so the raw signatures appear
/// once and callers read as intent.
/// </summary>
internal static class NativeWindowInterop
{
    // Only the authenticated Explorer parent, with a live global grant, may
    // supply a native modal owner. Never enable/disable this owner from WinUI.
    internal static nint ResolveExplorerDialogOwner(string? rootText, string ownerText,
        uint parentPid, ulong parentCreated)
    {
        try
        {
            if (parentPid == 0 || parentCreated == 0 || rootText is null) return 0;
            var root = (nint)Convert.ToUInt64(rootText[2..], 16);
            var owner = (nint)Convert.ToUInt64(ownerText[2..], 16);
            GetWindowThreadProcessId(root, out var rootPid);
            GetWindowThreadProcessId(owner, out var ownerPid);
            if (rootPid != parentPid || ownerPid != parentPid ||
                !IsWindowVisible(root) || !IsWindowVisible(owner) || IsWindowEnabled(owner)) return 0;
            var name = new System.Text.StringBuilder(128);
            GetClassNameW(root, name, name.Capacity);
            if (name.ToString() != "#32770") return 0;
            name.Clear();
            GetClassNameW(owner, name, name.Capacity);
            if (name.ToString() is not ("CabinetWClass" or "ExploreWClass")) return 0;
            var actualOwner = GetWindow(root, 4); // GW_OWNER
            for (var depth = 0; actualOwner != 0 && depth < 256; depth++)
            {
                if (IsWindowVisible(actualOwner)) break;
                actualOwner = GetWindow(actualOwner, 4);
            }
            if (actualOwner != owner) return 0;
            using var parent = System.Diagnostics.Process.GetProcessById(checked((int)parentPid));
            if ((ulong)parent.StartTime.ToFileTimeUtc() != parentCreated ||
                !string.Equals(parent.MainModule?.FileName,
                    System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe"),
                    StringComparison.OrdinalIgnoreCase)) return 0;
            using var watcher = Mutex.OpenExisting(@"Local\FluentShell.Watch");
            using var grant = EventWaitHandle.OpenExisting(
                $@"Local\FluentShell.ExplorerDialogs.{parentPid}.{parentCreated}");
            return grant.WaitOne(0) ? owner : 0;
        }
        catch { return 0; }
    }

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint hwnd, out uint pid);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowEnabled(nint hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(nint hwnd, System.Text.StringBuilder name, int count);
    [DllImport("user32.dll")]
    private static extern nint GetWindow(nint hwnd, uint command);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AllowSetForegroundWindow(uint processId);
    internal delegate nint SubclassProc(
        nint hwnd, uint message, nint wParam, nint lParam, nuint subclassId, nuint refData);

    private const int GwlHwndParent = -8;
    private const int DwmwaCloak = 13;
    private const uint GcsCompositionString = 0x0008;

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowPos
    {
        public nint HwndInsertAfter;
        public nint Hwnd;
        public int X;
        public int Y;
        public int Cx;
        public int Cy;
        public uint Flags;
    }

    [DllImport("comctl32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowSubclass(
        nint hwnd, SubclassProc callback, nuint subclassId, nuint refData);

    [DllImport("comctl32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool RemoveWindowSubclass(
        nint hwnd, SubclassProc callback, nuint subclassId);

    [DllImport("comctl32.dll")]
    public static extern nint DefSubclassProc(nint hwnd, uint message, nint wParam, nint lParam);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr(nint hwnd, int index, nint newValue);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnableWindow(nint hwnd, bool enabled);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(nint hwnd);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(nint hwnd, int attribute, ref int value, int size);

    [DllImport("user32.dll")]
    private static extern nint GetSystemMenu(nint hwnd, [MarshalAs(UnmanagedType.Bool)] bool revert);

    [DllImport("user32.dll")]
    private static extern int EnableMenuItem(nint menu, uint item, uint flags);

    [DllImport("imm32.dll")]
    private static extern nint ImmGetContext(nint hwnd);

    [DllImport("imm32.dll")]
    private static extern int ImmGetCompositionStringW(
        nint inputContext, uint index, nint buffer, uint bufferLength);

    [DllImport("imm32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ImmReleaseContext(nint hwnd, nint inputContext);

    /// <summary>Establishes the real Win32 owner relationship behind a modal proxy.</summary>
    public static void SetOwner(nint hwnd, nint ownerHwnd) =>
        SetWindowLongPtr(hwnd, GwlHwndParent, ownerHwnd);

    /// <summary>
    /// Lets the injected process take the foreground for the length of one native
    /// press. The proxy owns the foreground and received the user's input, so it is
    /// the process that may hand that right over. Windows gates some native
    /// controls on the window being the foreground one: Explorer's "Open File -
    /// Security Warning" runs a file only while its dialog is in the foreground,
    /// and a projected surface keeps the WinUI proxy in the foreground instead.
    /// </summary>
    internal static void AllowForegroundForNativePress(uint parentProcessId)
    {
        if (parentProcessId == 0) return;
        try { AllowSetForegroundWindow(parentProcessId); }
        catch { }
    }

    public static void SetEnabled(nint hwnd, bool enabled) => EnableWindow(hwnd, enabled);

    /// <summary>DWM application cloak: composited or not, without changing z-order.</summary>
    public static bool SetCloaked(nint hwnd, bool cloaked)
    {
        var value = cloaked ? 1 : 0;
        return DwmSetWindowAttribute(hwnd, DwmwaCloak, ref value, sizeof(int)) >= 0;
    }

    /// <summary>
    /// Enables or greys the title-bar close button, the way a native window whose close
    /// does nothing (a message box with no Cancel) shows it: through its system menu.
    /// </summary>
    public static void SetCloseEnabled(nint hwnd, bool enabled)
    {
        const uint ScClose = 0xF060;
        const uint ByCommand = 0x0000;
        const uint Grayed = 0x0001;
        const uint Disabled = 0x0002;
        var menu = GetSystemMenu(hwnd, false);
        if (menu != 0) EnableMenuItem(menu, ScClose, ByCommand | (enabled ? 0 : Grayed | Disabled));
    }

    /// <summary>
    /// The dark caption Windows draws for a dark-mode window.  Only the fallback for a
    /// system without AppWindowTitleBar.PreferredTheme.
    /// </summary>
    public static void SetDarkCaption(nint hwnd, bool dark)
    {
        const int DwmwaUseImmersiveDarkMode = 20;
        var value = dark ? 1 : 0;
        DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, ref value, sizeof(int));
    }

    /// <summary>Per-window DPI, or 0 when the window cannot report one.</summary>
    public static uint GetWindowDpi(nint hwnd) => GetDpiForWindow(hwnd);

    /// <summary>
    /// True while an IME composition is open.  A composition in progress owns the
    /// text, so canonical native text must not overwrite it mid-word.
    /// </summary>
    public static bool IsImeComposing(nint hwnd)
    {
        var context = ImmGetContext(hwnd);
        if (context == 0) return false;
        try
        {
            return ImmGetCompositionStringW(context, GcsCompositionString, 0, 0) > 0;
        }
        finally
        {
            ImmReleaseContext(hwnd, context);
        }
    }

    /// <summary>
    /// Rewrites an in-flight WM_WINDOWPOSCHANGING back to `bounds`, which is how
    /// the pre-interactive gate keeps the visible proxy from being dragged before
    /// the Bridge has handed input over.
    /// </summary>
    public static void ClampWindowPosition(nint lParam, PixelRect bounds)
    {
        if (lParam == 0) return;
        var position = Marshal.PtrToStructure<WindowPos>(lParam);
        if ((position.Flags & WindowMessages.SwpNoMove) == 0)
        {
            position.X = bounds.X;
            position.Y = bounds.Y;
        }
        if ((position.Flags & WindowMessages.SwpNoSize) == 0)
        {
            position.Cx = bounds.Width;
            position.Cy = bounds.Height;
        }
        Marshal.StructureToPtr(position, lParam, false);
    }
}

/// <summary>The system's double-click time: how close two presses must be to be one gesture.</summary>
internal static class ProjectedDoubleClickTime
{
    [DllImport("user32.dll")]
    private static extern uint GetDoubleClickTime();

    public static long Milliseconds => Math.Clamp(GetDoubleClickTime(), 200u, 5000u);
}
