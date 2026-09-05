# Answers two questions about an application's menu bar toolbar at once: whether a
# posted click makes the control open its menu, and whether that menu is a real HMENU
# popup the projection could read.  Both are prerequisites for projecting the menu as a
# WinUI flyout instead of letting a native popup appear.
#
# Diagnostic only.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [string] $ButtonName = 'File'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace MenuProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMenuItemInfo(IntPtr menu, uint item, bool byPosition, ref MENUITEMINFO info);
[DllImport("user32.dll")] public static extern bool IsMenu(IntPtr menu);
[DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr window, ref POINT point);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
public delegate bool EnumProc(IntPtr window, IntPtr param);
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct MENUITEMINFO {
    public uint cbSize; public uint fMask; public uint fType; public uint fState;
    public uint wID; public IntPtr hSubMenu; public IntPtr hbmpChecked;
    public IntPtr hbmpUnchecked; public IntPtr dwItemData; public IntPtr dwTypeData;
    public uint cch; public IntPtr hbmpItem;
}
'

[void][MenuProbe.Win32]::SetThreadDpiAwarenessContext([IntPtr](-4))

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][MenuProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
$script:bars = New-Object System.Collections.Generic.List[IntPtr]
$find = [MenuProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq 'ToolbarWindow32' -and [MenuProbe.Win32]::IsWindowVisible($window)) {
        $script:bars.Add($window)
    }
    return $true
}
[void][MenuProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)

# The button rectangle comes from the accessibility tree, because TB_GETITEMRECT needs a
# pointer in the target's own address space.
$root = [System.Windows.Automation.AutomationElement]::RootElement
$window = $root.FindFirst([System.Windows.Automation.TreeScope]::Children,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)))
if ($null -eq $window) { 'native window not found in the accessibility tree'; exit 1 }
$button = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $ButtonName)))
if ($null -eq $button) { "'$ButtonName' not found"; exit 1 }
$bounds = $button.Current.BoundingRectangle
"button '{0}' screen {1},{2} {3}x{4}" -f $ButtonName, $bounds.X, $bounds.Y, $bounds.Width, $bounds.Height

# The click has to reach the toolbar that owns that rectangle.
$toolbar = [IntPtr]::Zero
foreach ($candidate in $script:bars) {
    $rect = New-Object MenuProbe.Win32+RECT
    [void][MenuProbe.Win32]::GetWindowRect($candidate, [ref] $rect)
    if ($bounds.X -ge $rect.Left -and $bounds.X -lt $rect.Right -and
        $bounds.Y -ge $rect.Top -and $bounds.Y -lt $rect.Bottom) {
        $toolbar = $candidate
        break
    }
}
if ($toolbar -eq [IntPtr]::Zero) { 'no toolbar contains that button'; exit 1 }

$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP = 0x0202
$MN_GETHMENU = 0x01E1

$point = New-Object MenuProbe.Win32+POINT
$point.X = [int]($bounds.X + $bounds.Width / 2)
$point.Y = [int]($bounds.Y + $bounds.Height / 2)
[void][MenuProbe.Win32]::ScreenToClient($toolbar, [ref] $point)
$x = $point.X
$y = $point.Y
"toolbar 0x{0:X} client click at {1},{2}" -f $toolbar.ToInt64(), $x, $y

$point = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
[void][MenuProbe.Win32]::PostMessage($toolbar, $WM_LBUTTONDOWN, [IntPtr] 1, $point)
[void][MenuProbe.Win32]::PostMessage($toolbar, $WM_LBUTTONUP, [IntPtr] 0, $point)
Start-Sleep -Milliseconds 900

$script:popups = New-Object System.Collections.Generic.List[IntPtr]
$scan = [MenuProbe.Win32+EnumProc] {
    param($window, $param)
    if (-not [MenuProbe.Win32]::IsWindowVisible($window)) { return $true }
    if ((Get-Class $window) -ne '#32768') { return $true }
    $script:popups.Add($window)
    return $true
}
[void][MenuProbe.Win32]::EnumWindows($scan, [IntPtr]::Zero)
"visible #32768 popups: $($script:popups.Count)"

foreach ($popup in $script:popups) {
    $owner = 0
    [void][MenuProbe.Win32]::GetWindowThreadProcessId($popup, [ref] $owner)
    $bounds = New-Object MenuProbe.Win32+RECT
    [void][MenuProbe.Win32]::GetWindowRect($popup, [ref] $bounds)
    $menu = [MenuProbe.Win32]::SendMessage($popup, $MN_GETHMENU, [IntPtr]::Zero, [IntPtr]::Zero)
    "  popup 0x{0:X} pid={1} at {2},{3} {4}x{5} hmenu=0x{6:X} isMenu={7}" -f `
        $popup.ToInt64(), $owner, $bounds.Left, $bounds.Top,
        ($bounds.Right - $bounds.Left), ($bounds.Bottom - $bounds.Top),
        $menu.ToInt64(), [MenuProbe.Win32]::IsMenu($menu)
    if (-not [MenuProbe.Win32]::IsMenu($menu)) { continue }
    $count = [MenuProbe.Win32]::GetMenuItemCount($menu)
    "    items=$count"
    $text = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
    try {
        for ($index = 0; $index -lt $count; $index++) {
            $info = New-Object MenuProbe.Win32+MENUITEMINFO
            $info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
            # MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_FTYPE
            $info.fMask = 0x01 -bor 0x02 -bor 0x04 -bor 0x40 -bor 0x100
            $info.dwTypeData = $text
            $info.cch = 250
            if (-not [MenuProbe.Win32]::GetMenuItemInfo($menu, $index, $true, [ref] $info)) { continue }
            $label = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($text)
            "      [{0}] id={1,-6} type=0x{2:X} state=0x{3:X} sub=0x{4:X} '{5}'" -f `
                $index, $info.wID, $info.fType, $info.fState, $info.hSubMenu.ToInt64(), $label
        }
    }
    finally {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($text)
    }
}
