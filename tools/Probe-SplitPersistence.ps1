# Measures whether the application keeps a split the projection moved, across a window
# resize.  Drags the projected splitter through its RangeValue pattern, records the
# native pane rectangles, resizes the projected window, and records them again.
#
# Diagnostic only.  Run it while a surface is committed.
param(
    [string] $ProcessName = 'FluentShell.Renderer',
    [string] $NativeProcessName = 'mmc',
    [int] $Delta = 160
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -Namespace SplitProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
public delegate bool EnumProc(IntPtr window, IntPtr param);
public struct RECT { public int Left, Top, Right, Bottom; }
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][SplitProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

function Get-PaneRects {
    $process = Get-Process -Name $NativeProcessName -ErrorAction Stop | Select-Object -First 1
    $rows = New-Object System.Collections.Generic.List[string]
    $collect = [SplitProbe.Win32+EnumProc] {
        param($window, $param)
        if (-not [SplitProbe.Win32]::IsWindowVisible($window)) { return $true }
        $class = Get-Class $window
        if ($class -notin @('SysTreeView32', 'SysListView32', 'AfxFrameOrView42u')) { return $true }
        $rect = New-Object SplitProbe.Win32+RECT
        [void][SplitProbe.Win32]::GetWindowRect($window, [ref] $rect)
        $rows.Add(('{0} left={1} width={2}' -f $class, $rect.Left, ($rect.Right - $rect.Left)))
        return $true
    }
    [void][SplitProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $collect, [IntPtr]::Zero)
    $rows
}

$root = [System.Windows.Automation.AutomationElement]::RootElement
$thumbCondition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'ProjectedPaneSplitter')
$splitters = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $thumbCondition)
if ($splitters.Count -eq 0) { 'no projected splitter found'; exit 1 }

'native panes before:'
Get-PaneRects | ForEach-Object { "  $_" }

$moved = $false
foreach ($splitter in $splitters) {
    $pattern = $null
    if (-not $splitter.TryGetCurrentPattern(
            [System.Windows.Automation.RangeValuePattern]::Pattern, [ref] $pattern)) { continue }
    $current = $pattern.Current.Value
    $target = [Math]::Min($pattern.Current.Maximum, $current + $Delta)
    if ($target -le $current) { $target = [Math]::Max($pattern.Current.Minimum, $current - $Delta) }
    "splitter value {0} -> {1} (range {2}..{3})" -f $current, $target,
        $pattern.Current.Minimum, $pattern.Current.Maximum
    $pattern.SetValue($target)
    $moved = $true
    break
}
if (-not $moved) { 'no splitter exposed RangeValue'; exit 1 }

Start-Sleep -Seconds 3
'native panes after the drag:'
Get-PaneRects | ForEach-Object { "  $_" }

# Resize the projected window: the application re-lays out on WM_SIZE, which is where a
# stored proportion the projection never wrote would reassert itself.
$proxy = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$window = $root.FindFirst([System.Windows.Automation.TreeScope]::Children,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proxy.Id)))
if ($null -eq $window) { 'projected window not found'; exit 1 }
$transform = $null
if (-not $window.TryGetCurrentPattern(
        [System.Windows.Automation.TransformPattern]::Pattern, [ref] $transform)) {
    'projected window exposes no Transform pattern'; exit 1
}
$bounds = $window.Current.BoundingRectangle
"resizing projected window {0}x{1} -> {2}x{3}" -f $bounds.Width, $bounds.Height,
    ($bounds.Width - 200), ($bounds.Height - 120)
$transform.Resize($bounds.Width - 200, $bounds.Height - 120)
Start-Sleep -Seconds 4
'native panes after the resize:'
Get-PaneRects | ForEach-Object { "  $_" }
