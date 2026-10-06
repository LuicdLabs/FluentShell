<#
Diagnoses how a native TreeView/ListView host answers WM_CONTEXTMENU, without
FluentShell. Starts a disposable MMC console at the caller's integrity level,
finds the Nth visible item of its first visible tree (or list), posts
WM_CONTEXTMENU with either that item's screen point or the keyboard form
(-1,-1), and reports whether a native popup menu (#32768) appeared, the items it
holds, and whether the selection moved. The menu is dismissed with Escape and the
disposable console is closed. No menu command is executed.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Console = 'compmgmt',
    [ValidateSet('SysTreeView32', 'SysListView32')][string]$ClassName = 'SysTreeView32',
    [ValidateRange(0, 64)][int]$ItemIndex = 1,
    [ValidateSet('Point', 'Keyboard', 'SelectThenKeyboard', 'NotifyRClick')][string]$Mode = 'Point',
    [ValidateRange(1, 30)][int]$StartupSeconds = 5
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class ContextMenuProbe {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetMenuStringW(IntPtr menu, uint item, StringBuilder text, int count, uint flags);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, IntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr process, IntPtr address, IntPtr size, uint type);
    [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr written);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    static string ClassOf(IntPtr hwnd) { var b = new StringBuilder(256); GetClassNameW(hwnd, b, b.Capacity); return b.ToString(); }
    public static List<IntPtr> Children(IntPtr root, string cls) {
        var found = new List<IntPtr>();
        EnumChildWindows(root, (h, l) => { if (ClassOf(h) == cls && IsWindowVisible(h)) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static List<IntPtr> TopLevel(uint pid, string cls) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && ClassOf(h) == cls && IsWindowVisible(h)) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static List<string> MenuItems(IntPtr menuWindow) {
        var items = new List<string>();
        IntPtr menu = SendMessageW(menuWindow, 0x01E1 /* MN_GETHMENU */, IntPtr.Zero, IntPtr.Zero);
        int count = GetMenuItemCount(menu);
        for (uint i = 0; i < count && i < 64; i++) {
            var b = new StringBuilder(256);
            GetMenuStringW(menu, i, b, b.Capacity, 0x400 /* MF_BYPOSITION */);
            items.Add(b.ToString());
        }
        return items;
    }
    // Sends a message whose lParam points at a RECT-sized buffer in the target.
    static int[] RemoteRect(IntPtr hwnd, uint message, IntPtr wParam, long seed) {
        uint pid; GetWindowThreadProcessId(hwnd, out pid);
        IntPtr process = OpenProcess(0x0008 | 0x0010 | 0x0020, false, pid);
        if (process == IntPtr.Zero) return null;
        IntPtr remote = VirtualAllocEx(process, IntPtr.Zero, (IntPtr)16, 0x3000, 0x04);
        try {
            IntPtr io;
            var bytes = new byte[16];
            BitConverter.GetBytes(seed).CopyTo(bytes, 0);
            WriteProcessMemory(process, remote, bytes, (IntPtr)16, out io);
            if (SendMessageW(hwnd, message, wParam, remote) == IntPtr.Zero) return null;
            ReadProcessMemory(process, remote, bytes, (IntPtr)16, out io);
            return new[] { BitConverter.ToInt32(bytes, 0), BitConverter.ToInt32(bytes, 4), BitConverter.ToInt32(bytes, 8), BitConverter.ToInt32(bytes, 12) };
        } finally { VirtualFreeEx(process, remote, IntPtr.Zero, 0x8000); CloseHandle(process); }
    }
    public static IntPtr TreeItem(IntPtr tree, int index) {
        IntPtr item = SendMessageW(tree, 0x110A /* TVM_GETNEXTITEM */, (IntPtr)0x5 /* FIRSTVISIBLE */, IntPtr.Zero);
        for (int i = 0; i < index && item != IntPtr.Zero; i++)
            item = SendMessageW(tree, 0x110A, (IntPtr)0x6 /* NEXTVISIBLE */, item);
        return item;
    }
    public static IntPtr TreeCaret(IntPtr tree) { return SendMessageW(tree, 0x110A, (IntPtr)0x9, IntPtr.Zero); }
    public static POINT ItemPoint(IntPtr hwnd, bool tree, IntPtr treeItem, int index) {
        int[] r = tree
            ? RemoteRect(hwnd, 0x1104 /* TVM_GETITEMRECT */, (IntPtr)1, treeItem.ToInt64())
            : RemoteRect(hwnd, 0x100E /* LVM_GETITEMRECT */, (IntPtr)index, 2 /* LVIR_LABEL */);
        if (r == null) throw new InvalidOperationException("item rectangle unavailable");
        var p = new POINT { X = (r[0] + r[2]) / 2, Y = (r[1] + r[3]) / 2 };
        ClientToScreen(hwnd, ref p);
        return p;
    }
    public static int ListFocus(IntPtr list) { return (int)SendMessageW(list, 0x100C /* LVM_GETNEXTITEM */, (IntPtr)(-1), (IntPtr)0x2 /* SELECTED */); }
}
'@

$system = [Environment]::SystemDirectory
$previous = $env:__COMPAT_LAYER
$env:__COMPAT_LAYER = 'RunAsInvoker'
try {
    $process = Start-Process -FilePath (Join-Path $system 'mmc.exe') -ArgumentList (Join-Path $system "$Console.msc") -PassThru
} finally { $env:__COMPAT_LAYER = $previous }
try {
    Start-Sleep -Seconds $StartupSeconds
    $process.Refresh()
    [void][ContextMenuProbe]::SetThreadDpiAwarenessContext([IntPtr](-4))
    $frame = $process.MainWindowHandle
    if ($frame -eq [IntPtr]::Zero) { throw "MMC $($process.Id) has no main window" }
    $control = [ContextMenuProbe]::Children($frame, $ClassName) | Select-Object -First 1
    if (-not $control) { throw "No visible $ClassName under MMC $($process.Id)" }
    $isTree = $ClassName -eq 'SysTreeView32'
    $treeItem = if ($isTree) { [ContextMenuProbe]::TreeItem($control, $ItemIndex) } else { [IntPtr]::Zero }
    if ($isTree -and $treeItem -eq [IntPtr]::Zero) { throw "Tree has no visible item $ItemIndex" }
    $before = if ($isTree) { [ContextMenuProbe]::TreeCaret($control) } else { [ContextMenuProbe]::ListFocus($control) }
    $point = [ContextMenuProbe]::ItemPoint($control, $isTree, $treeItem, $ItemIndex)
    if ($Mode -eq 'SelectThenKeyboard') {
        if ($isTree) { [void][ContextMenuProbe]::SendMessageW($control, 0x110B, [IntPtr]0x9, $treeItem) }
        Start-Sleep -Milliseconds 500
    }
    if ($Mode -eq 'NotifyRClick') {
        throw 'NotifyRClick needs an in-process NMHDR; use the Bridge diagnostics instead.'
    }
    $lParam = if ($Mode -eq 'Point') { [IntPtr](($point.Y -shl 16) -bor ($point.X -band 0xFFFF)) } else { [IntPtr](-1) }
    [void][ContextMenuProbe]::PostMessageW($control, 0x007B, $control, $lParam)
    Start-Sleep -Milliseconds 1500
    $menus = [ContextMenuProbe]::TopLevel([uint32]$process.Id, '#32768')
    $after = if ($isTree) { [ContextMenuProbe]::TreeCaret($control) } else { [ContextMenuProbe]::ListFocus($control) }
    [pscustomobject]@{
        Console = $Console
        Class = $ClassName
        Mode = $Mode
        TargetItem = if ($isTree) { '0x{0:X}' -f $treeItem.ToInt64() } else { $ItemIndex }
        Point = "$($point.X),$($point.Y)"
        PopupShown = $menus.Count -gt 0
        PopupItems = if ($menus.Count) { ([ContextMenuProbe]::MenuItems($menus[0]) -join ' | ') } else { '' }
        SelectionBefore = if ($isTree) { '0x{0:X}' -f $before.ToInt64() } else { $before }
        SelectionAfter = if ($isTree) { '0x{0:X}' -f $after.ToInt64() } else { $after }
    }
    foreach ($menu in $menus) { [void][ContextMenuProbe]::PostMessageW($menu, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero) }
} finally {
    Start-Sleep -Milliseconds 300
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
