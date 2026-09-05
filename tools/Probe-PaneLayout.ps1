# Checks whether the panes of a private container keep a geometry the projection
# assigns them: it moves the split by SetWindowPos, waits, re-reads the rects, and
# then resizes the top-level window to see whether the application's own layout
# pass takes the panes back.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [Parameter(Mandatory = $true)][string] $ContainerClass,
    [int] $SplitClientX = 400,
    [int] $GapWidth = 3
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace PaneProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr window, int x, int y, int cx, int cy, bool repaint);
public delegate bool EnumProc(IntPtr window, IntPtr param);
public struct RECT { public int Left, Top, Right, Bottom; }
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][PaneProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$script:container = [IntPtr]::Zero
$find = [PaneProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq $ContainerClass -and [PaneProbe.Win32]::IsWindowVisible($window)) {
        $script:container = $window
        return $false
    }
    return $true
}
[void][PaneProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)
if ($script:container -eq [IntPtr]::Zero) { "container '$ContainerClass' not found"; exit 1 }
$container = $script:container

function Get-Panes {
    $panes = New-Object System.Collections.Generic.List[object]
    $collect = [PaneProbe.Win32+EnumProc] {
        param($window, $param)
        if ([PaneProbe.Win32]::GetParent($window) -eq $script:container -and
            [PaneProbe.Win32]::IsWindowVisible($window)) {
            $rect = New-Object PaneProbe.Win32+RECT
            [void][PaneProbe.Win32]::GetWindowRect($window, [ref] $rect)
            $panes.Add([pscustomobject]@{
                Handle = $window
                Class  = Get-Class $window
                Left   = $rect.Left
                Top    = $rect.Top
                Right  = $rect.Right
                Bottom = $rect.Bottom
            })
        }
        return $true
    }
    [void][PaneProbe.Win32]::EnumChildWindows($script:container, $collect, [IntPtr]::Zero)
    $panes | Sort-Object Left
}

function Show-Panes([string] $label) {
    Get-Panes | ForEach-Object { "  {0,-8} {1,-18} x={2}..{3}" -f $label, $_.Class, $_.Left, $_.Right }
}

$containerRect = New-Object PaneProbe.Win32+RECT
[void][PaneProbe.Win32]::GetWindowRect($container, [ref] $containerRect)
Show-Panes 'before'

# Move the first gap to the requested client position: the pane on the left ends
# there and the pane on the right starts one gap later.
$panes = Get-Panes
if ($panes.Count -lt 2) { 'container has fewer than two panes'; exit 1 }
$left = $panes[0]
$right = $panes[1]
$splitScreenX = $containerRect.Left + $SplitClientX
[void][PaneProbe.Win32]::MoveWindow($left.Handle, $left.Left - $containerRect.Left, $left.Top - $containerRect.Top,
    $splitScreenX - $left.Left, $left.Bottom - $left.Top, $true)
[void][PaneProbe.Win32]::MoveWindow($right.Handle, $splitScreenX + $GapWidth - $containerRect.Left,
    $right.Top - $containerRect.Top, $right.Right - ($splitScreenX + $GapWidth), $right.Bottom - $right.Top, $true)
Start-Sleep -Milliseconds 600
Show-Panes 'moved'

# The application relays out on its own WM_SIZE, so resizing the frame says whether
# the assigned split survives ordinary use.
$frameRect = New-Object PaneProbe.Win32+RECT
[void][PaneProbe.Win32]::GetWindowRect($process.MainWindowHandle, [ref] $frameRect)
[void][PaneProbe.Win32]::MoveWindow($process.MainWindowHandle, $frameRect.Left, $frameRect.Top,
    ($frameRect.Right - $frameRect.Left) - 40, ($frameRect.Bottom - $frameRect.Top), $true)
Start-Sleep -Milliseconds 600
Show-Panes 'resized'
[void][PaneProbe.Win32]::MoveWindow($process.MainWindowHandle, $frameRect.Left, $frameRect.Top,
    ($frameRect.Right - $frameRect.Left), ($frameRect.Bottom - $frameRect.Top), $true)
