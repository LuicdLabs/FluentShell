# Decides whether a private container's splitter can be driven by posted mouse
# messages: it reports the pane rects, posts one drag sequence into the container
# in client coordinates, and reports the rects again.  If the panes move, a
# projected splitter can drive the native one without touching the real cursor.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [Parameter(Mandatory = $true)][string] $ContainerClass,
    [int] $Delta = -60,
    [int] $Steps = 6,
    [int] $TargetClientX = 0,
    [switch] $Activate
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace DragProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr window, ref POINT point);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
public delegate bool EnumProc(IntPtr window, IntPtr param);
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][DragProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$container = [IntPtr]::Zero
$roots = New-Object System.Collections.Generic.List[IntPtr]
foreach ($handle in ($process.MainWindowHandle)) { $roots.Add($handle) }
$callback = [DragProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq $ContainerClass -and [DragProbe.Win32]::IsWindowVisible($window)) {
        $script:container = $window
        return $false
    }
    return $true
}
[void][DragProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $callback, [IntPtr]::Zero)
if ($script:container -eq [IntPtr]::Zero) { "container '$ContainerClass' not found"; exit 1 }
$container = $script:container

function Get-Panes([IntPtr] $parent) {
    $panes = New-Object System.Collections.Generic.List[object]
    $collect = [DragProbe.Win32+EnumProc] {
        param($window, $param)
        if ([DragProbe.Win32]::GetParent($window) -eq $parent -and [DragProbe.Win32]::IsWindowVisible($window)) {
            $rect = New-Object DragProbe.Win32+RECT
            [void][DragProbe.Win32]::GetWindowRect($window, [ref] $rect)
            $panes.Add([pscustomobject]@{
                Handle = $window
                Class  = Get-Class $window
                Left   = $rect.Left
                Right  = $rect.Right
                Top    = $rect.Top
                Bottom = $rect.Bottom
            })
        }
        return $true
    }
    [void][DragProbe.Win32]::EnumChildWindows($parent, $collect, [IntPtr]::Zero)
    $panes
}

if ($Activate) {
    [void][DragProbe.Win32]::SetForegroundWindow($process.MainWindowHandle)
    Start-Sleep -Milliseconds 400
}
$before = Get-Panes $container
"container 0x{0:X} '{1}'" -f [int64] $container, $ContainerClass
$before | ForEach-Object { "  before {0,-18} x={1}..{2}" -f $_.Class, $_.Left, $_.Right }

# The splitter sits in the horizontal gap between two panes.
$sorted = $before | Sort-Object Left
$gap = $null
for ($i = 0; $i -lt $sorted.Count - 1; ++$i) {
    $space = $sorted[$i + 1].Left - $sorted[$i].Right
    if ($space -gt 0 -and $space -le 12) {
        $gap = [pscustomobject]@{ X = [int](($sorted[$i].Right + $sorted[$i + 1].Left) / 2); Width = $space }
        break
    }
}
if ($null -eq $gap) { 'no thin gap between panes'; exit 1 }

$containerRect = New-Object DragProbe.Win32+RECT
[void][DragProbe.Win32]::GetWindowRect($container, [ref] $containerRect)
$point = New-Object DragProbe.Win32+POINT
$point.X = $gap.X
$point.Y = [int](($containerRect.Top + $containerRect.Bottom) / 2)
[void][DragProbe.Win32]::ScreenToClient($container, [ref] $point)
"gap width={0} at client x={1} y={2}" -f $gap.Width, $point.X, $point.Y

function New-LParam([int] $x, [int] $y) {
    [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
}

$WM_MOUSEMOVE = 0x0200
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP = 0x0202
$WM_SETCURSOR = 0x0020
$MK_LBUTTON = [IntPtr] 1

$finalX = if ($TargetClientX -ne 0) { $TargetClientX } else { $point.X + $Delta }
$travel = $finalX - $point.X
"dragging client x {0} -> {1}" -f $point.X, $finalX
[void][DragProbe.Win32]::PostMessage($container, $WM_MOUSEMOVE, [IntPtr]::Zero, (New-LParam $point.X $point.Y))
[void][DragProbe.Win32]::PostMessage($container, $WM_LBUTTONDOWN, $MK_LBUTTON, (New-LParam $point.X $point.Y))
Start-Sleep -Milliseconds 120
for ($step = 1; $step -le $Steps; ++$step) {
    $x = $point.X + [int]($travel * $step / $Steps)
    [void][DragProbe.Win32]::PostMessage($container, $WM_MOUSEMOVE, $MK_LBUTTON, (New-LParam $x $point.Y))
    Start-Sleep -Milliseconds 60
}
[void][DragProbe.Win32]::PostMessage($container, $WM_LBUTTONUP, [IntPtr]::Zero, (New-LParam $finalX $point.Y))
Start-Sleep -Milliseconds 400

$after = Get-Panes $container
$after | ForEach-Object { "  after  {0,-18} x={1}..{2}" -f $_.Class, $_.Left, $_.Right }
$moved = $false
foreach ($pane in $after) {
    $match = $before | Where-Object { $_.Handle -eq $pane.Handle } | Select-Object -First 1
    if ($null -ne $match -and ($match.Left -ne $pane.Left -or $match.Right -ne $pane.Right)) { $moved = $true }
}
if ($moved) { 'RESULT: posted mouse messages moved the splitter' }
else { 'RESULT: posted mouse messages did not move the splitter' }
