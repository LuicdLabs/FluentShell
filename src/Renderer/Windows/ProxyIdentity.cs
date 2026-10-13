using System.Diagnostics;
using System.Runtime.InteropServices;
using FluentShell.Renderer.Runtime;

namespace FluentShell.Renderer.Windows;

/// <summary>
/// The identity a proxy shows the shell: the icon and the taskbar group of the
/// application whose window it stands for.  Without it every proxy carried the
/// renderer's default icon and gathered under one "FluentShell.Renderer" taskbar
/// button, whatever application it projected.
/// </summary>
internal static class ProxyIdentity
{
    public static void Apply(nint proxy, nint native)
    {
        try
        {
            ApplyIcons(proxy, native);
            ApplyTaskbarGroup(proxy, native);
        }
        catch (Exception exception)
        {
            RendererDiagnostics.Log($"proxy identity not applied: {exception.Message}");
        }
    }

    // The native window's own icons, copied so they outlive whatever the application does
    // with its handles.  An icon handle is a session-wide USER object, so another process's
    // icon can be read; it cannot be owned.
    private static void ApplyIcons(nint proxy, nint native)
    {
        var big = ReadIcon(native, IconBig, GclpHIcon);
        var small = ReadIcon(native, IconSmall2, GclpHIconSm);
        if (small == 0) small = ReadIcon(native, IconSmall, GclpHIconSm);
        if (big == 0) big = small;
        if (small == 0) small = big;
        if (big != 0) SendMessageW(proxy, WmSetIcon, IconBig, CopyIcon(big));
        if (small != 0) SendMessageW(proxy, WmSetIcon, IconSmall, CopyIcon(small));
    }

    private static nint ReadIcon(nint window, nint which, int classIndex)
    {
        if (SendMessageTimeoutW(window, WmGetIcon, which, 0, SmtoAbortIfHung, 200, out var icon) != 0 && icon != 0)
            return icon;
        return GetClassLongPtrW(window, classIndex);
    }

    // One taskbar group per projected application: the application's own explicit
    // AppUserModelID when its window carries one, else one derived from its executable,
    // named and drawn from that executable so the group reads as the application.
    private static void ApplyTaskbarGroup(nint proxy, nint native)
    {
        var executable = ExecutablePath(native);
        if (executable is null) return;
        var id = ReadString(native, AppUserModelId);
        if (string.IsNullOrWhiteSpace(id))
        {
            var name = new string(Path.GetFileNameWithoutExtension(executable)
                .Where(char.IsLetterOrDigit).Take(64).ToArray());
            if (name.Length == 0) return;
            id = "FluentShell.Projection." + name;
        }
        var description = FileVersionInfo.GetVersionInfo(executable).FileDescription;
        if (string.IsNullOrWhiteSpace(description)) description = Path.GetFileNameWithoutExtension(executable);
        if (SHGetPropertyStoreForWindow(proxy, typeof(IPropertyStore).GUID, out var raw) != 0 ||
            raw is not IPropertyStore store) return;
        try
        {
            // The relaunch properties have to come together or the shell ignores them.
            SetString(store, RelaunchCommand, $"\"{executable}\"");
            SetString(store, RelaunchIconResource, $"{executable},0");
            SetString(store, RelaunchDisplayNameResource, description);
            SetString(store, AppUserModelId, id);
            store.Commit();
        }
        finally
        {
            Marshal.ReleaseComObject(store);
        }
    }

    private static string? ExecutablePath(nint window)
    {
        GetWindowThreadProcessId(window, out var processId);
        if (processId == 0) return null;
        var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == 0) return null;
        try
        {
            var buffer = new char[32768];
            var length = buffer.Length;
            return QueryFullProcessImageNameW(process, 0, buffer, ref length) ? new string(buffer, 0, length) : null;
        }
        finally
        {
            CloseHandle(process);
        }
    }

    private static string? ReadString(nint window, PropertyKey key)
    {
        if (SHGetPropertyStoreForWindow(window, typeof(IPropertyStore).GUID, out var raw) != 0 ||
            raw is not IPropertyStore store) return null;
        try
        {
            var value = new PropVariant();
            if (store.GetValue(ref key, ref value) != 0) return null;
            try
            {
                return value.Type == VtLpwstr ? Marshal.PtrToStringUni(value.Pointer) : null;
            }
            finally
            {
                PropVariantClear(ref value);
            }
        }
        finally
        {
            Marshal.ReleaseComObject(store);
        }
    }

    private static void SetString(IPropertyStore store, PropertyKey key, string text)
    {
        var value = new PropVariant { Type = VtLpwstr, Pointer = Marshal.StringToCoTaskMemUni(text) };
        try
        {
            Marshal.ThrowExceptionForHR(store.SetValue(ref key, ref value));
        }
        finally
        {
            PropVariantClear(ref value);
        }
    }

    private static readonly Guid AppUserModelFormat = new("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
    private static readonly PropertyKey RelaunchCommand = new(AppUserModelFormat, 2);
    private static readonly PropertyKey RelaunchIconResource = new(AppUserModelFormat, 3);
    private static readonly PropertyKey RelaunchDisplayNameResource = new(AppUserModelFormat, 4);
    private static readonly PropertyKey AppUserModelId = new(AppUserModelFormat, 5);

    private const ushort VtLpwstr = 31;
    private const uint WmGetIcon = 0x007F;
    private const uint WmSetIcon = 0x0080;
    private const nint IconSmall = 0;
    private const nint IconBig = 1;
    private const nint IconSmall2 = 2;
    private const int GclpHIcon = -14;
    private const int GclpHIconSm = -34;
    private const uint SmtoAbortIfHung = 0x0002;
    private const uint ProcessQueryLimitedInformation = 0x1000;

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private readonly struct PropertyKey(Guid format, uint id)
    {
        public readonly Guid Format = format;
        public readonly uint Id = id;
    }

    // PROPVARIANT, wide enough for the string form this code reads and writes.
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    private struct PropVariant
    {
        [FieldOffset(0)] public ushort Type;
        [FieldOffset(8)] public nint Pointer;
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }

    [DllImport("shell32.dll")]
    private static extern int SHGetPropertyStoreForWindow(
        nint hwnd, [MarshalAs(UnmanagedType.LPStruct)] Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out object store);

    [DllImport("ole32.dll")]
    private static extern int PropVariantClear(ref PropVariant value);

    [DllImport("user32.dll")]
    private static extern nint SendMessageW(nint hwnd, uint message, nint wParam, nint lParam);

    [DllImport("user32.dll")]
    private static extern nint SendMessageTimeoutW(
        nint hwnd, uint message, nint wParam, nint lParam, uint flags, uint timeout, out nint result);

    [DllImport("user32.dll")]
    private static extern nint GetClassLongPtrW(nint hwnd, int index);

    [DllImport("user32.dll")]
    private static extern nint CopyIcon(nint icon);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

    [DllImport("kernel32.dll")]
    private static extern nint OpenProcess(uint access, bool inherit, uint processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageNameW(nint process, uint flags, char[] name, ref int size);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(nint handle);
}
