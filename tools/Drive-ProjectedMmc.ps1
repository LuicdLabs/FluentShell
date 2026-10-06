<#
Drives a projected MMC console with real pointer input and reports what the WinUI
projection showed. Starts a disposable console at the caller's integrity level,
injects the Release Bridge, waits for the committed projection, then runs one or
more steps:

  TreeContext:<item name>   right-clicks a projected tree item, lists the menu, Escape
  ListContext:<item name>   right-clicks a projected list item, lists the menu, Escape
  ...Context:<item>/<a>/<b> chooses menu entry a, then submenu entry b (read-only verbs only)
  Any step:<arg>><file.png> also captures the window while its menu is open
  MenuBar:<title>           opens a projected menu-bar root, lists its items, Escape
  MenuInvoke:<title>/<item> opens a menu-bar root and clicks one item (read-only verbs only)
  TreeSelect:<item name>    left-clicks a projected tree item
  Splitter:<dx>             drags the first vertical splitter by dx pixels in 24 moves
  Screenshot:<file>         captures the projected window

Pointer input really moves the cursor: do not use the machine while it runs. No
configuration is changed by the built-in steps; MenuInvoke must name a read-only
command (View/Refresh/Help). The console and its Renderer are closed at the end.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Console = 'compmgmt',
    # Steps separated by ';' (powershell.exe -File cannot pass an array).
    [string]$Steps = 'TreeContext:System Tools',
    [ValidateRange(1, 60)][int]$StartupSeconds = 4,
    [ValidateRange(5, 120)][int]$ProjectionSeconds = 40,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$injector = Join-Path $repository 'build\bin\x64\Release\FluentShell.Injector.exe'
$system = [Environment]::SystemDirectory
$mmc = Join-Path $system 'mmc.exe'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DriveInput {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    static void Mouse(uint flags) {
        var input = new INPUT { type = 0 };
        input.u.mi.dwFlags = flags;
        SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Move(int x, int y) { SetCursorPos(x, y); Mouse(0x0001); }
    public static void Click(int x, int y, bool right) {
        Move(x, y);
        Mouse(right ? 0x0008u : 0x0002u);
        Mouse(right ? 0x0010u : 0x0004u);
    }
    public static void Down(int x, int y) { Move(x, y); Mouse(0x0002); }
    public static void Up() { Mouse(0x0004); }
    public static void Key(ushort vk) {
        var down = new INPUT { type = 1 }; down.u.ki.vk = vk;
        var up = new INPUT { type = 1 }; up.u.ki.vk = vk; up.u.ki.flags = 2;
        SendInput(2, new[] { down, up }, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
# UIA bounding rectangles are physical pixels; keep SetCursorPos in the same space.
[void][DriveInput]::SetThreadDpiAwarenessContext([IntPtr](-4))

$ae = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]
function Find-Projection([int]$RendererId) {
    $condition = [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, $RendererId)
    foreach ($window in $ae::RootElement.FindAll($scope::Children, $condition)) {
        $viewport = $window.FindFirst($scope::Descendants,
            [System.Windows.Automation.PropertyCondition]::new($ae::AutomationIdProperty, 'FluentShell.ContentViewport'))
        if ($viewport) { return $window }
    }
    return $null
}
function Find-Named($root, [string]$name, $type) {
    $conditions = @([System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $name))
    if ($type) { $conditions += [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, $type) }
    $condition = if ($conditions.Count -eq 1) { $conditions[0] } else { [System.Windows.Automation.AndCondition]::new($conditions) }
    return $root.FindFirst($scope::Descendants, $condition)
}
function Center($element) {
    $r = $element.Current.BoundingRectangle
    # The middle of the row: a tree row's left edge holds its expand chevron.
    return @([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
}
# Popup menus (MenuFlyout) are hosted in their own top-level popup window of the Renderer.
# Only items of an open popup (ControlType.Menu) count; the menu bar's own roots
# are MenuItems too and are always present.
function Get-OpenMenuItems([int]$RendererId) {
    $menus = $ae::RootElement.FindAll($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, $RendererId),
        [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Menu)))
    $itemType = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)
    return @(foreach ($menu in $menus) {
        foreach ($item in $menu.FindAll($scope::Children, $itemType)) {
            '{0}{1}' -f $item.Current.Name, $(if ($item.Current.IsEnabled) { '' } else { ' (disabled)' })
        }
    })
}
function Get-ListItemNames($root) {
    $condition = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    return @($root.FindAll($scope::Descendants, $condition) | ForEach-Object { $_.Current.Name })
}
function Save-Screenshot($window, [string]$path) {
    $r = $window.Current.BoundingRectangle
    $bitmap = [System.Drawing.Bitmap]::new([int]$r.Width, [int]$r.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen([int]$r.X, [int]$r.Y, 0, 0, $bitmap.Size)
    $bitmap.Save($path)
    $graphics.Dispose(); $bitmap.Dispose()
}
function Wait-Until([scriptblock]$Probe, [int]$Milliseconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $Milliseconds) {
        $value = & $Probe
        if ($value) { return @{ Value = $value; Elapsed = $watch.ElapsedMilliseconds } }
        Start-Sleep -Milliseconds 50
    }
    return @{ Value = $null; Elapsed = $watch.ElapsedMilliseconds }
}

$previous = $env:__COMPAT_LAYER
$env:__COMPAT_LAYER = 'RunAsInvoker'
try {
    $arguments = if ($Console -eq 'empty') { @() } else { @((Join-Path $system "$Console.msc")) }
    $process = Start-Process -FilePath $mmc -ArgumentList $arguments -PassThru
} finally { $env:__COMPAT_LAYER = $previous }
$renderer = $null
try {
    Start-Sleep -Seconds $StartupSeconds
    $injected = & $injector inject $mmc --pid $process.Id 2>&1
    "inject: $($injected -join ' | ')"
    $projection = $null
    $deadline = [DateTime]::UtcNow.AddSeconds($ProjectionSeconds)
    while (-not $projection -and [DateTime]::UtcNow -lt $deadline) {
        $renderer = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($process.Id)" |
            Where-Object { $_.Name -eq 'FluentShell.Renderer.exe' } | Select-Object -First 1
        if ($renderer) { $projection = Find-Projection $renderer.ProcessId }
        if (-not $projection) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $projection) { throw "No committed projection within $ProjectionSeconds s" }
    "projection: renderer=$($renderer.ProcessId) title='$($projection.Current.Name)'"
    [void][DriveInput]::SetForegroundWindow([IntPtr]$projection.Current.NativeWindowHandle)
    # Let startup reconcile and the menu-bar read finish before the first gesture.
    Start-Sleep -Seconds 5
    foreach ($step in ($Steps -split ';' | Where-Object { $_ })) {
        Start-Sleep -Milliseconds 1000
        "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $step
        $verb, $argument = $step -split ':', 2
        # 'Verb:argument>file.png' captures the window while a menu is still open.
        $menuShot = $null
        if ($verb -ne 'Screenshot' -and $argument -match '^(.*)>(.+)$') { $argument = $Matches[1]; $menuShot = $Matches[2] }
        $projection = Find-Projection $renderer.ProcessId
        if (-not $projection) { "[$step] projection is gone (rolled back?)"; break }
        switch ($verb) {
            { $_ -in 'TreeContext', 'ListContext', 'TreeSelect' } {
                $type = if ($verb -eq 'ListContext') { [System.Windows.Automation.ControlType]::ListItem } else { [System.Windows.Automation.ControlType]::TreeItem }
                # 'Item/Menu/Submenu item' chooses that path from the opened menu.
                $path = @($argument -split '/')
                $argument = $path[0]
                $path = @($path | Select-Object -Skip 1)
                $item = Find-Named $projection $argument $type
                if (-not $item) {
                    # A report row's name joins its cells; match the first one.
                    $item = $projection.FindAll($scope::Descendants,
                        [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, $type)) |
                        Where-Object { $_.Current.Name.StartsWith($argument) } | Select-Object -First 1
                }
                if (-not $item) {
                    "[$step] item not found; list items: $((Get-ListItemNames $projection) -join ' | ')"
                    continue
                }
                $x, $y = Center $item
                [DriveInput]::Click($x, $y, $verb -ne 'TreeSelect')
                if ($verb -eq 'TreeSelect') { Start-Sleep -Milliseconds 1500; "[$step] clicked at $x,$y"; continue }
                $result = Wait-Until { $items = Get-OpenMenuItems $renderer.ProcessId; if ($items.Count) { ,$items } } 5000
                if ($result.Value) {
                    "[$step] menu after $($result.Elapsed) ms: $($result.Value -join ' | ')"
                    if ($menuShot) { Start-Sleep -Milliseconds 400; Save-Screenshot $projection $menuShot }
                }
                else { "[$step] no menu within $($result.Elapsed) ms"; continue }
                if ($path.Count -eq 0) {
                    [DriveInput]::Key(0x1B)
                    Start-Sleep -Milliseconds 800
                    continue
                }
                foreach ($name in $path) {
                    $entry = (Wait-Until {
                        $ae::RootElement.FindFirst($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
                            [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, $renderer.ProcessId),
                            [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem),
                            [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $name)))
                    } 3000).Value
                    if (-not $entry) { "[$step] menu entry '$name' not found"; [DriveInput]::Key(0x1B); break }
                    $x, $y = Center $entry
                    [DriveInput]::Click($x, $y, $false)
                    "[$step] chose '$name'"
                    Start-Sleep -Milliseconds 600
                }
                Start-Sleep -Milliseconds 1500
            }
            { $_ -in 'MenuBar', 'MenuInvoke' } {
                $title, $leaf = $argument -split '/', 2
                $root = Find-Named $projection $title ([System.Windows.Automation.ControlType]::MenuItem)
                if (-not $root) { "[$step] menu root not found"; continue }
                $x, $y = Center $root
                [DriveInput]::Click($x, $y, $false)
                $result = Wait-Until { $items = Get-OpenMenuItems $renderer.ProcessId | Where-Object { $_ -ne $title }; if ($items.Count) { ,$items } } 4000
                if ($result.Value) {
                    "[$step] items after $($result.Elapsed) ms: $($result.Value -join ' | ')"
                    if ($menuShot) { Start-Sleep -Milliseconds 400; Save-Screenshot $projection $menuShot }
                }
                else { "[$step] no menu items within $($result.Elapsed) ms" }
                if ($verb -eq 'MenuInvoke' -and $leaf) {
                    $condition = [System.Windows.Automation.AndCondition]::new(
                        [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, $renderer.ProcessId),
                        [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $leaf))
                    $target = $ae::RootElement.FindFirst($scope::Descendants, $condition)
                    if ($target) { $x, $y = Center $target; [DriveInput]::Click($x, $y, $false); "[$step] clicked '$leaf'" }
                    else { "[$step] '$leaf' not found"; [DriveInput]::Key(0x1B) }
                    Start-Sleep -Milliseconds 1500
                } else {
                    [DriveInput]::Key(0x1B)
                    Start-Sleep -Milliseconds 600
                }
            }
            'Splitter' {
                $condition = [System.Windows.Automation.PropertyCondition]::new($ae::ClassNameProperty, 'ProjectedPaneSplitter')
                $splitter = $projection.FindFirst($scope::Descendants, $condition)
                if (-not $splitter) { "[$step] no projected splitter"; continue }
                $before = $splitter.Current.BoundingRectangle
                $x, $y = Center $splitter
                $x = [int]($before.X + $before.Width / 2)
                $dx = [int]$argument
                $watch = [Diagnostics.Stopwatch]::StartNew()
                [DriveInput]::Down($x, $y)
                for ($move = 1; $move -le 24; ++$move) {
                    [DriveInput]::Move($x + [int]($dx * $move / 24), $y)
                    Start-Sleep -Milliseconds 16
                }
                $dragMs = $watch.ElapsedMilliseconds
                [DriveInput]::Up()
                $settled = Wait-Until {
                    $s = $projection.FindFirst($scope::Descendants, $condition)
                    if ($s) { $r = $s.Current.BoundingRectangle; if ([Math]::Abs(($r.X - $before.X) - $dx) -le 3) { $r } }
                } 4000
                $after = $splitter.Current.BoundingRectangle
                "[$step] drag $dragMs ms; native split settled after release in $($settled.Elapsed) ms; x $($before.X) -> $($after.X)"
            }
            'Screenshot' { Save-Screenshot $projection $argument; "[$step] saved" }
            default { "[$step] unknown step" }
        }
    }
} finally {
    if (-not $KeepOpen) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        if ($renderer) {
            Start-Sleep -Seconds 3
            Get-Process -Id $renderer.ProcessId -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -like '*FluentShell.Renderer.exe' } | Stop-Process -Force
        }
    }
}
