# Dumps the HWND tree of a running process: class, styles, rect, and text.
# Diagnostic only; it never touches the target's state.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [int] $ProcessId = 0
)

Add-Type -Namespace FluentShellDiag -Name Win32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr param);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr param);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, System.Text.StringBuilder text, int max);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
[DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hwnd);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
[DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hwnd);
public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'@

function Get-WindowInfo([IntPtr] $hwnd) {
    $className = New-Object System.Text.StringBuilder 256
    [void][FluentShellDiag.Win32]::GetClassName($hwnd, $className, 256)
    $text = New-Object System.Text.StringBuilder 256
    [void][FluentShellDiag.Win32]::GetWindowText($hwnd, $text, 256)
    $rect = New-Object FluentShellDiag.Win32+RECT
    [void][FluentShellDiag.Win32]::GetWindowRect($hwnd, [ref] $rect)
    [pscustomobject]@{
        Hwnd    = $hwnd
        Class   = $className.ToString()
        Text    = $text.ToString()
        Style   = [uint32]([FluentShellDiag.Win32]::GetWindowLongPtr($hwnd, -16).ToInt64() -band 0xffffffff)
        ExStyle = [uint32]([FluentShellDiag.Win32]::GetWindowLongPtr($hwnd, -20).ToInt64() -band 0xffffffff)
        Visible = [FluentShellDiag.Win32]::IsWindowVisible($hwnd)
        CtrlId  = [FluentShellDiag.Win32]::GetDlgCtrlID($hwnd)
        Rect    = "{0},{1} {2}x{3}" -f $rect.Left, $rect.Top, ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
        Parent  = [FluentShellDiag.Win32]::GetParent($hwnd)
    }
}

$targets = if ($ProcessId -ne 0) { @(Get-Process -Id $ProcessId) } else { @(Get-Process -Name $ProcessName -ErrorAction Stop) }
$targetIds = $targets | ForEach-Object { [uint32] $_.Id }

$roots = New-Object System.Collections.Generic.List[IntPtr]
$collectRoot = [FluentShellDiag.Win32+EnumProc] {
    param($hwnd, $param)
    $owner = 0
    [void][FluentShellDiag.Win32]::GetWindowThreadProcessId($hwnd, [ref] $owner)
    if ($targetIds -contains $owner) { $roots.Add($hwnd) }
    return $true
}
[void][FluentShellDiag.Win32]::EnumWindows($collectRoot, [IntPtr]::Zero)

foreach ($root in $roots) {
    $info = Get-WindowInfo $root
    "ROOT hwnd=0x{0:X} class='{1}' text='{2}' style=0x{3:X8} exStyle=0x{4:X8} visible={5} rect={6}" -f `
        $info.Hwnd.ToInt64(), $info.Class, $info.Text, $info.Style, $info.ExStyle, $info.Visible, $info.Rect

    $children = New-Object System.Collections.Generic.List[IntPtr]
    $collectChild = [FluentShellDiag.Win32+EnumProc] {
        param($hwnd, $param)
        $children.Add($hwnd)
        return $true
    }
    [void][FluentShellDiag.Win32]::EnumChildWindows($root, $collectChild, [IntPtr]::Zero)

    $depthOf = @{ $root.ToInt64() = 0 }
    foreach ($child in $children) {
        $childInfo = Get-WindowInfo $child
        $parentKey = $childInfo.Parent.ToInt64()
        $depth = if ($depthOf.ContainsKey($parentKey)) { $depthOf[$parentKey] + 1 } else { 1 }
        $depthOf[$child.ToInt64()] = $depth
        "{0}hwnd=0x{1:X} class='{2}' text='{3}' id={4} style=0x{5:X8} exStyle=0x{6:X8} visible={7} rect={8}" -f `
            ('  ' * $depth), $childInfo.Hwnd.ToInt64(), $childInfo.Class, $childInfo.Text, $childInfo.CtrlId,
            $childInfo.Style, $childInfo.ExStyle, $childInfo.Visible, $childInfo.Rect
    }
    ''
}
