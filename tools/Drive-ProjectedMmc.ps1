<#
Drives a projected MMC console with real pointer input and reports what the WinUI
projection showed. Starts a disposable console at the caller's integrity level,
injects the Release Bridge, waits for the committed projection, then runs one or
more steps:

  TreeContext:<item name>   right-clicks a projected tree item, lists the menu, Escape
  ListContext:<item name>   right-clicks a projected list item (or #n, the nth row), lists the menu, Escape
  ...Context:<item>/<a>/<b> chooses menu entry a, then submenu entry b (read-only verbs only);
                            an entry written ~Prefix matches the first entry starting with Prefix
  CloseDialogs              cancels every projected dialog but the console window (Cancel/Close/No/OK, never Apply)
  Any step:<arg>><file.png> also captures the window while its menu is open
  MenuRoots                 lists the projected menu bar's top-level items
  MenuBar:<title>           opens a projected menu-bar root, lists its items, Escape
  MenuInvoke:<title>/<item> opens a menu-bar root and clicks one item (read-only verbs only)
  TreeSelect:<item name>    left-clicks a projected tree item
  Splitter:<dx>             drags the first vertical splitter by dx pixels in 24 moves
  ListSelect:<name or #n>   left-clicks a projected list row
  ListOpen:<name or #n>     double-clicks a projected list row (its default verb: usually Properties)
  TreeOpen:<item name>      double-clicks a projected tree item
  Select:<name>             selects a projected tab or row by name through UIA (SelectionItem)
  Dialogs                   lists the console's and the Renderer's visible top-level windows
  Invoke:<name>             presses a projected button (or menu item) by name through UIA
  Wait:<ms>                 sleeps (a view that loads slowly)
  Expand:<item name>        expands a projected tree item through UIA (ExpandCollapse)
  Collapse:<item name>      collapses a projected tree item through UIA (ExpandCollapse)
  ExpandAny:<name>          expands (CollapseAny: collapses) any projected element of that name that supports
                            ExpandCollapse, in any Renderer window: a list group heading of a dialog
  WheelTree:<notches>       turns the mouse wheel over the largest projected tree (negative = down)
  PointClick:<dx>:<dy>      left-clicks that offset (px) from the projected window's top-left corner
  ScreenClick:<x>:<y>       left-clicks absolute physical screen coordinates (DumpDialog prints those)
  ClickEdit:<n>             clicks the nth projected edit of the topmost Renderer window holding one, reports focus
  Focused                   reports the element that holds keyboard focus
  Type:<text>               types the text into the focused element (Unicode key events), then lists the edit values
  DumpDialog                lists every UIA descendant (type, name, state, bounds) of the topmost dialog
  DumpWindow[:<text>]       the same for the console's own projected window, limited to elements whose
                            type, class or name contains <text>
  ClickWindow:<name>:<f>    clicks the named element of the console's own window at fraction f (0..1) of
                            its width (a toolbar button, a split button's arrow, a legend check box)
  Interact                  types into every editable edit, toggles every check box and selects every radio button
                            of the topmost dialog with real input and reports which did not respond (cancel after)
  Key:<name>                presses Right/Left/Up/Down/Home/End/PageUp/PageDown/Enter/Escape/Tab
  SpinStep:<up|down>        presses the upper/lower half of the first projected spin pair (NumericUpDown arrows)
  EditValues                lists the value of every projected edit
  NativeClick:<class>:<dx>:<dy>   clicks that offset (96-dpi px) inside the console's largest visible
  NativeDouble:<class>:<dx>:<dy>  window whose class contains <class> (the -Native oracle's aim)
  WheelList:<notches>       turns the mouse wheel over the largest projected list (negative = down)
  ListState                 reports the rows the largest projected list has realized
  Items                     lists the realized tree items and the first list rows by name
  Screenshot:<file>         captures the projected window
  ScreenshotWindows:<stem>  captures every Renderer top-level window (dialogs too) to <stem>-<n>.png
  ScreenshotScreen:<file>   captures the whole virtual screen (what is really on top)

Pointer input really moves the cursor: do not use the machine while it runs. No
configuration is changed by the built-in steps; MenuInvoke must name a read-only
command (View/Refresh/Help). The console and its Renderer are closed at the end.

-Native runs the same steps against the console's own windows with no Bridge injected:
the oracle for what a gesture does natively (does a double-click open a dialog at all?).
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Console = 'compmgmt',
    # Steps separated by ';' (powershell.exe -File cannot pass an array).
    [string]$Steps = 'TreeContext:System Tools',
    [ValidateRange(1, 60)][int]$StartupSeconds = 4,
    [ValidateRange(5, 120)][int]$ProjectionSeconds = 40,
    [switch]$KeepOpen,
    # Act on the console's own windows with the same gestures (no Bridge): what native does.
    [switch]$Native
)

$ErrorActionPreference = 'Stop'
# Window titles and node names are localized: emit UTF-8 so a redirected log stays readable.
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }
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
    [DllImport("user32.dll")] static extern uint GetDpiForSystem();
    public static double DpiScale() { return GetDpiForSystem() / 96.0; }
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
    public static void DoubleClick(int x, int y) {
        Move(x, y);
        Mouse(0x0002u); Mouse(0x0004u);
        System.Threading.Thread.Sleep(60);
        Mouse(0x0002u); Mouse(0x0004u);
    }
    public static void Down(int x, int y) { Move(x, y); Mouse(0x0002); }
    public static void Up() { Mouse(0x0004); }
    public static void Wheel(int delta) {
        var input = new INPUT { type = 0 };
        input.u.mi.dwFlags = 0x0800;
        input.u.mi.mouseData = unchecked((uint)delta);
        SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Key(ushort vk) {
        var down = new INPUT { type = 1 }; down.u.ki.vk = vk;
        var up = new INPUT { type = 1 }; up.u.ki.vk = vk; up.u.ki.flags = 2;
        SendInput(2, new[] { down, up }, Marshal.SizeOf(typeof(INPUT)));
    }
    // Types text as Unicode key events (KEYEVENTF_UNICODE), the way an IME commits characters.
    public static void Text(string text) {
        foreach (var ch in text) {
            var down = new INPUT { type = 1 }; down.u.ki.scan = ch; down.u.ki.flags = 4;
            var up = new INPUT { type = 1 }; up.u.ki.scan = ch; up.u.ki.flags = 4 | 2;
            SendInput(2, new[] { down, up }, Marshal.SizeOf(typeof(INPUT)));
            System.Threading.Thread.Sleep(30);
        }
    }
}
'@
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class DriveWindows {
    delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);
    struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr param);
    // The visible window (a top-level one or any descendant) of a process with the largest area
    // whose class contains `classPart`: {left, top, width, height} in screen pixels, or empty.
    public static int[] LargestChild(uint pid, string classPart) {
        int[] best = new int[0];
        long bestArea = 0;
        EnumProc consider = (h, p) => {
            if (!IsWindowVisible(h)) return true;
            var cls = new StringBuilder(128); GetClassNameW(h, cls, 128);
            if (cls.ToString().IndexOf(classPart, StringComparison.OrdinalIgnoreCase) < 0) return true;
            RECT r; GetWindowRect(h, out r);
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = new[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top }; }
            return true;
        };
        EnumWindows((top, p) => {
            uint owner; GetWindowThreadProcessId(top, out owner);
            if (owner != pid) return true;
            consider(top, IntPtr.Zero);
            EnumChildWindows(top, consider, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return best;
    }
    // Visible top-level windows of a process: class, title, size and whether DWM cloaks it.
    public static string[] List(uint pid) {
        var found = new List<string>();
        EnumWindows((h, p) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid || !IsWindowVisible(h)) return true;
            var cls = new StringBuilder(128); GetClassNameW(h, cls, 128);
            var title = new StringBuilder(256); GetWindowTextW(h, title, 256);
            RECT r; GetWindowRect(h, out r);
            int cloaked; DwmGetWindowAttribute(h, 14, out cloaked, 4);
            found.Add(string.Format("{0} '{1}' {2}x{3}{4}", cls, title, r.Right - r.Left, r.Bottom - r.Top, cloaked != 0 ? " (cloaked)" : ""));
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
}
'@
# UIA bounding rectangles are physical pixels; keep SetCursorPos in the same space.
[void][DriveInput]::SetThreadDpiAwarenessContext([IntPtr](-4))

$ae = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]
function Find-Projection([int]$RendererId) {
    $condition = [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, $RendererId)
    if ($Native) {
        # The native oracle: the console's own frame stands where the projection would.
        foreach ($window in $ae::RootElement.FindAll($scope::Children, $condition)) {
            if ($window.Current.ClassName -eq 'MMCMainFrame') { return $window }
        }
        return $null
    }
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
    # An item outside its scroller's viewport has no usable rectangle; scroll it into view.
    if ([double]::IsInfinity($r.X) -or [double]::IsNaN($r.X) -or $r.Width -le 0 -or $r.Height -le 0) {
        try {
            $element.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView()
            Start-Sleep -Milliseconds 700
        } catch { }
        $r = $element.Current.BoundingRectangle
    }
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
# Every window of the Renderer: its top-level windows and, nested under their owner's element,
# the owned ones (a modal dialog's proxy), one entry per HWND, topmost first as UIA lists them.
function Get-RendererWindows([int]$RendererId) {
    $process = [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, $RendererId)
    $window = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
    $found = @($ae::RootElement.FindAll($scope::Children, $process))
    $nested = @($ae::RootElement.FindAll($scope::Descendants, [System.Windows.Automation.AndCondition]::new($process, $window)))
    $seen = [System.Collections.Generic.HashSet[int]]::new()
    return @(foreach ($element in ($found + $nested)) {
        $handle = [int]$element.Current.NativeWindowHandle
        if ($handle -ne 0 -and $seen.Add($handle)) { $element }
    })
}
# The text of every projected edit of every Renderer window: what a spin pair's buddy reads.
function Get-EditValues([int]$RendererId) {
    $edit = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
    return @(foreach ($window in (Get-RendererWindows $RendererId)) {
        foreach ($element in $window.FindAll($scope::Descendants, $edit)) {
            try { $element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { }
        }
    })
}
function Get-ListItemNames($root) {
    $condition = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    return @($root.FindAll($scope::Descendants, $condition) | ForEach-Object { $_.Current.Name })
}
# The result list is the largest projected List; its realized rows are what a virtualizing
# panel keeps alive, so first/last show where a windowed list is looking.
function Find-LargestList($root) {
    $lists = $root.FindAll($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
        $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::List))
    return $lists | Sort-Object { $r = $_.Current.BoundingRectangle; $r.Width * $r.Height } -Descending |
        Select-Object -First 1
}
function Describe-List($list) {
    if (-not $list) { return 'no projected list' }
    $names = Get-ListItemNames $list
    if (-not $names.Count) { return 'list has no realized rows' }
    return '{0} realized rows; first="{1}" last="{2}"' -f $names.Count, $names[0], $names[-1]
}
function Save-Screenshot($window, [string]$path) {
    $r = $window.Current.BoundingRectangle
    # A window that was just closed or rolled back reports empty bounds.
    if ($r.IsEmpty -or [int]$r.Width -le 0 -or [int]$r.Height -le 0) { return }
    $bitmap = [System.Drawing.Bitmap]::new([int]$r.Width, [int]$r.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen([int]$r.X, [int]$r.Y, 0, 0, $bitmap.Size)
    $bitmap.Save($path)
    $graphics.Dispose(); $bitmap.Dispose()
}
# The cancel path of a dialog's buttons: Cancel, Close, No, then OK -- never Apply, Yes or any verb.
# A Win32 button name may carry its mnemonic: "Close(C)" / "<close>(C)".
function Get-CancelButton($buttons) {
    $cancelWord = -join ([char[]](0x53D6, 0x6D88))
    $closeWord = -join ([char[]](0x95DC, 0x9589))
    $noWord = [string][char]0x5426
    $okWord = -join ([char[]](0x78BA, 0x5B9A))
    foreach ($pattern in "^($cancelWord|Cancel)", "^($closeWord|Close)(\(.\))?$", "^($noWord|No)\b", "^($okWord|OK)\b") {
        $choice = $buttons | Where-Object { $_.Current.Name -match $pattern -and $_.Current.IsEnabled -and $_.Current.AutomationId -ne 'Close' } | Select-Object -First 1
        if ($choice) { return $choice }
    }
    return $null
}
# The native window handles of every Renderer window, to tell what an interaction opened.
function Get-WindowHandles([int]$RendererId) {
    return @(Get-RendererWindows $RendererId | ForEach-Object { [int]$_.Current.NativeWindowHandle })
}
# Answers through the cancel path every Renderer window that was not among $Known (a prompt a click
# raised); returns what it answered, one entry per window.
function Dismiss-NewWindows([int]$RendererId, [int[]]$Known) {
    $buttonType = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
    $answered = @()
    foreach ($window in (Get-RendererWindows $RendererId)) {
        if ($Known -contains [int]$window.Current.NativeWindowHandle) { continue }
        $choice = Get-CancelButton @($window.FindAll($scope::Descendants, $buttonType))
        if ($choice) {
            $choice.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            $answered += "'$($window.Current.Name)' -> '$($choice.Current.Name)'"
            Start-Sleep -Milliseconds 1000
        } else { $answered += "'$($window.Current.Name)' (no cancel button)" }
    }
    return $answered
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
    # An empty ArgumentList is a parameter-validation error: the empty console has no arguments.
    $process = if ($arguments.Count) { Start-Process -FilePath $mmc -ArgumentList $arguments -PassThru } else { Start-Process -FilePath $mmc -PassThru }
} finally { $env:__COMPAT_LAYER = $previous }
$renderer = $null
try {
    Start-Sleep -Seconds $StartupSeconds
    if ($Native) {
        # The oracle: no Bridge, no Renderer; the steps act on the console's own windows. The
        # "renderer" id is the console's, so Find-Projection and Get-RendererWindows see it.
        $renderer = [pscustomobject]@{ ProcessId = $process.Id }
        $projection = $null
        $deadline = [DateTime]::UtcNow.AddSeconds($ProjectionSeconds)
        while (-not $projection -and [DateTime]::UtcNow -lt $deadline) {
            $projection = Find-Projection $process.Id
            if (-not $projection) { Start-Sleep -Milliseconds 500 }
        }
        if (-not $projection) { throw "No native console frame within $ProjectionSeconds s" }
        "native: console=$($process.Id) title='$($projection.Current.Name)'"
    } else {
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
        $mainTitle = $projection.Current.Name
        "projection: renderer=$($renderer.ProcessId) title='$mainTitle'"
    }
    [void][DriveInput]::SetForegroundWindow([IntPtr]$projection.Current.NativeWindowHandle)
    # Let startup reconcile and the menu-bar read finish before the first gesture.
    Start-Sleep -Seconds 5
    foreach ($step in ($Steps -split ';' | Where-Object { $_ })) {
        Start-Sleep -Milliseconds 1000
        "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $step
        $verb, $argument = $step -split ':', 2
        # 'Verb:argument>file.png' captures the window while a menu is still open. Only a .png names a
        # capture: a control called "Add >>" is an argument.
        $menuShot = $null
        if ($verb -ne 'Screenshot' -and $argument -match '^(.*)>(.+\.png)$') { $argument = $Matches[1]; $menuShot = $Matches[2] }
        $projection = Find-Projection $renderer.ProcessId
        if (-not $projection) { "[$step] projection is gone (rolled back?)"; break }
        switch ($verb) {
            { $_ -in 'TreeContext', 'ListContext', 'TreeSelect', 'ListSelect', 'TreeOpen', 'ListOpen' } {
                $type = if ($verb -in 'ListContext', 'ListSelect', 'ListOpen') { [System.Windows.Automation.ControlType]::ListItem } else { [System.Windows.Automation.ControlType]::TreeItem }
                # 'Item/Menu/Submenu item' chooses that path from the opened menu.
                $path = @($argument -split '/')
                $argument = $path[0]
                $path = @($path | Select-Object -Skip 1)
                $item = $null
                if ($verb -in 'ListContext', 'ListSelect', 'ListOpen' -and $argument -match '^#(\d+)$') {
                    # '#n' is the nth realized row of the largest projected list.
                    $row = [int]$Matches[1]
                    $list = Find-LargestList $projection
                    $rows = if ($list) { @($list.FindAll($scope::Descendants,
                        [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, $type))) } else { @() }
                    if ($rows.Count -gt $row) { $item = $rows[$row] }
                } else { $item = Find-Named $projection $argument $type }
                if (-not $item -and $verb -in 'ListContext', 'ListSelect', 'ListOpen') {
                    # A report row's name joins its cells; match the first one. (A tree
                    # item is matched exactly: a prefix would pick a sibling's longer name.)
                    $item = $projection.FindAll($scope::Descendants,
                        [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, $type)) |
                        Where-Object { $_.Current.Name.StartsWith($argument) } | Select-Object -First 1
                }
                if (-not $item) {
                    "[$step] item not found; list items: $((Get-ListItemNames $projection) -join ' | ')"
                    continue
                }
                # Asking for the rectangle of a virtualized item can make its list scroll it into view,
                # animated: let that settle and aim where the item really is (a double-click's second
                # press must meet the same row as its first).
                $null = Center $item
                Start-Sleep -Milliseconds 800
                $x, $y = Center $item
                if ($verb -like 'Tree*') {
                    # A deep row's own rectangle includes its indentation and expander, so its
                    # middle can be the chevron: aim at the label side instead.
                    $r = $item.Current.BoundingRectangle
                    $x = [int]($r.X + $r.Width * 0.8)
                }
                if ($verb -in 'TreeOpen', 'ListOpen') {
                    # A double-click runs the item's default verb (properties, or navigation).
                    [DriveInput]::DoubleClick($x, $y)
                    Start-Sleep -Milliseconds 2500
                    "[$step] double-clicked at $x,$y"
                    continue
                }
                [DriveInput]::Click($x, $y, ($verb -notin @('TreeSelect', 'ListSelect')))
                if ($verb -in 'TreeSelect', 'ListSelect') {
                    Start-Sleep -Milliseconds 1500
                    # What the pointer really landed on: a click that selects nothing usually hit a cover.
                    $under = $ae::FromPoint([System.Windows.Point]::new($x, $y))
                    "[$step] clicked at $x,$y; under the pointer: $($under.Current.ControlType.ProgrammaticName) '$($under.Current.Name)' class=$($under.Current.ClassName)"
                    continue
                }
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
                    # '~Prefix' picks the first menu entry whose name starts with Prefix (accelerator
                    # letters differ between snap-ins: Properties is "(R)" in one and "(P)" in another).
                    $prefix = if ($name.StartsWith('~')) { $name.Substring(1) } else { $null }
                    $entry = (Wait-Until {
                        if ($prefix) {
                            $ae::RootElement.FindAll($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
                                [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.ProcessId),
                                [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem))) |
                                Where-Object { $_.Current.Name.StartsWith($prefix) } | Select-Object -First 1
                        } else {
                            $ae::RootElement.FindFirst($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
                                [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.ProcessId),
                                [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem),
                                [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $name)))
                        }
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
                        [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.ProcessId),
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
            'Wait' { Start-Sleep -Milliseconds ([int]$argument); "[$step] waited" }
            'MenuRoots' {
                $bars = @($projection.FindAll($scope::Descendants,
                    [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuBar)))
                if (-not $bars.Count) { "[$step] no projected menu bar"; continue }
                foreach ($bar in $bars) {
                    $roots = @($bar.FindAll([System.Windows.Automation.TreeScope]::Children,
                        [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)))
                    "[$step] menu bar: " + (($roots | ForEach-Object { "{0}{1}" -f $_.Current.Name, $(if ($_.Current.IsEnabled) { '' } else { ' (disabled)' }) }) -join ' | ')
                }
            }
            'Select' {
                $target = $ae::RootElement.FindFirst($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
                    [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.ProcessId),
                    [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $argument),
                    [System.Windows.Automation.PropertyCondition]::new($ae::IsSelectionItemPatternAvailableProperty, $true)))
                if (-not $target) { "[$step] nothing selectable named '$argument'"; continue }
                $target.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
                Start-Sleep -Milliseconds 2500
                "[$step] selected"
            }
            'Dialogs' {
                "[$step] console windows:"
                foreach ($line in [DriveWindows]::List([uint32]$process.Id)) { "    $line" }
                "[$step] renderer windows:"
                foreach ($line in [DriveWindows]::List([uint32]$renderer.ProcessId)) { "    $line" }
            }
            'Invoke' {
                # Any renderer window: a projected dialog is its own top-level window.
                $target = $ae::RootElement.FindFirst($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
                    [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.ProcessId),
                    [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $argument),
                    [System.Windows.Automation.PropertyCondition]::new($ae::IsInvokePatternAvailableProperty, $true)))
                if (-not $target) { "[$step] nothing invokable named '$argument'"; continue }
                $target.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                Start-Sleep -Milliseconds 1500
                "[$step] invoked"
            }
            'Expand' {
                $item = Find-Named $projection $argument ([System.Windows.Automation.ControlType]::TreeItem)
                if (-not $item) { "[$step] tree item not found"; continue }
                $item.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
                Start-Sleep -Milliseconds 2500
                "[$step] expanded"
            }
            { $_ -in 'ExpandAny', 'CollapseAny' } {
                # Any Renderer window: a list group heading of a projected dialog, a tree item, a drop-down.
                $target = $ae::RootElement.FindFirst($scope::Descendants, [System.Windows.Automation.AndCondition]::new(
                    [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.ProcessId),
                    [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $argument),
                    [System.Windows.Automation.PropertyCondition]::new($ae::IsExpandCollapsePatternAvailableProperty, $true)))
                if (-not $target) { "[$step] nothing expandable named '$argument'"; continue }
                $pattern = $target.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
                if ($verb -eq 'ExpandAny') { $pattern.Expand() } else { $pattern.Collapse() }
                Start-Sleep -Milliseconds 2500
                "[$step] now $($pattern.Current.ExpandCollapseState)"
            }
            'ClickEdit' {
                # ClickEdit:<n> clicks the nth projected edit of the topmost Renderer window that has one and
                # reports which element then holds keyboard focus (a focus handler that moves focus shows here).
                $edit = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
                $target = $null
                foreach ($window in (Get-RendererWindows $renderer.ProcessId)) {
                    $edits = @($window.FindAll($scope::Descendants, $edit) | Where-Object { -not $_.Current.IsOffscreen })
                    if ($edits.Count -gt [int]$argument) { $target = $edits[[int]$argument]; break }
                }
                if (-not $target) { "[$step] no such projected edit"; continue }
                $x, $y = Center $target
                [DriveInput]::Click($x, $y, $false)
                Start-Sleep -Milliseconds 800
                $focused = $ae::FocusedElement
                "[$step] clicked '$($target.Current.Name)' at $x,$y; focus is on $($focused.Current.ControlType.ProgrammaticName) '$($focused.Current.Name)' class=$($focused.Current.ClassName) (target had focus: $($target.Current.HasKeyboardFocus)); runtime ids target=$($target.GetRuntimeId() -join '.') focus=$($focused.GetRuntimeId() -join '.')"
            }
            'Type' {
                [DriveInput]::Text($argument)
                Start-Sleep -Milliseconds 800
                $now = $ae::FocusedElement
                "[$step] typed '$argument'; focus runtime id $($now.GetRuntimeId() -join '.'); edits: $((Get-EditValues $renderer.ProcessId) -join ' | ')"
            }
            'Focused' {
                $focused = $ae::FocusedElement
                "[$step] focus: $($focused.Current.ControlType.ProgrammaticName) '$($focused.Current.Name)' class=$($focused.Current.ClassName)"
            }
            'ClickControl' {
                # ClickControl:<name>:<fraction> clicks the control of the topmost dialog with that UIA name at the
                # given fraction (0..1) of its width, at mid-height: where on a wide button a pointer lands matters.
                $name, $fraction = $argument -split ':(?=[0-9.]+$)'
                $menuBar = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuBar)
                $dialog = @(Get-RendererWindows $renderer.ProcessId | Where-Object { -not $_.FindFirst($scope::Descendants, $menuBar) }) | Select-Object -First 1
                if (-not $dialog) { "[$step] no dialog"; continue }
                $target = Find-Named $dialog $name $null
                if (-not $target) { "[$step] '$name' not found"; continue }
                $r = $target.Current.BoundingRectangle
                $fractionValue = if ($fraction) { [double]$fraction } else { 0.5 }
                $cx = [int]($r.X + $r.Width * $fractionValue); $cy = [int]($r.Y + $r.Height / 2)
                [DriveInput]::Click($cx, $cy, $false)
                Start-Sleep -Milliseconds 1500
                "[$step] clicked '$name' at $cx,$cy (fraction $fractionValue of $([int]$r.Width) px)"
            }
            'ClickWindow' {
                # ClickWindow:<name>:<fraction> is ClickControl for the console's own projected window: a toolbar
                # button, a legend check box. The fraction (0..1) is where along its width the pointer lands, so a
                # split button's arrow (the right part) can be told from its body.
                $name, $fraction = $argument -split ':(?=[0-9.]+$)'
                # A button's hover tooltip carries the button's name, so a tooltip (and the text inside it)
                # is never the element meant.
                $target = @($projection.FindAll($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $name)) |
                    Where-Object { $_.Current.ControlType -ne [System.Windows.Automation.ControlType]::ToolTip -and
                        $_.Current.ClassName -notin @('ToolTip', 'TextBlock') }) | Select-Object -First 1
                if (-not $target) { "[$step] '$name' not found"; continue }
                $r = $target.Current.BoundingRectangle
                $fractionValue = if ($fraction) { [double]$fraction } else { 0.5 }
                $cx = [int]($r.X + $r.Width * $fractionValue); $cy = [int]($r.Y + $r.Height / 2)
                [DriveInput]::Click($cx, $cy, $false)
                Start-Sleep -Milliseconds 1500
                "[$step] clicked '$name' at $cx,$cy (fraction $fractionValue of $([int]$r.Width) px)"
            }
            'ScreenClick' {
                # ScreenClick:<x>:<y> left-clicks absolute physical screen coordinates (as DumpDialog prints them).
                $sx, $sy = $argument -split ':'
                [DriveInput]::Click([int]$sx, [int]$sy, $false)
                Start-Sleep -Milliseconds 1500
                "[$step] clicked at $sx,$sy"
            }
            'PointClick' {
                # PointClick:<dx>:<dy> clicks that offset from the projected window's top-left corner,
                # with no UIA lookup (which can itself scroll a virtualized list).
                $dx, $dy = $argument -split ':'
                $r = $projection.Current.BoundingRectangle
                [DriveInput]::Click([int]($r.X + [double]$dx), [int]($r.Y + [double]$dy), $false)
                Start-Sleep -Milliseconds 1500
                "[$step] clicked at $([int]($r.X + [double]$dx)),$([int]($r.Y + [double]$dy))"
            }
            'Collapse' {
                $item = Find-Named $projection $argument ([System.Windows.Automation.ControlType]::TreeItem)
                if (-not $item) { "[$step] tree item not found"; continue }
                $item.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
                Start-Sleep -Milliseconds 2500
                "[$step] collapsed"
            }
            'WheelTree' {
                # WheelTree:<notches> turns the wheel over the largest projected tree (negative = down).
                $tree = $projection.FindAll($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
                    $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Tree)) |
                    Sort-Object { $r = $_.Current.BoundingRectangle; $r.Width * $r.Height } -Descending | Select-Object -First 1
                if (-not $tree) { "[$step] no projected tree"; continue }
                $r = $tree.Current.BoundingRectangle
                [DriveInput]::Move([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
                $notches = [int]$argument
                for ($tick = 0; $tick -lt [Math]::Abs($notches); ++$tick) {
                    [DriveInput]::Wheel($(if ($notches -lt 0) { -120 } else { 120 }))
                    Start-Sleep -Milliseconds 25
                }
                Start-Sleep -Milliseconds 1500
                "[$step] wheeled $notches over the tree"
            }
            'Key' {
                $codes = @{ Right = 0x27; Left = 0x25; Up = 0x26; Down = 0x28; Home = 0x24; End = 0x23
                    PageUp = 0x21; PageDown = 0x22; Enter = 0x0D; Escape = 0x1B; Tab = 0x09 }
                if (-not $codes.ContainsKey($argument)) { "[$step] unknown key"; continue }
                [DriveInput]::Key([uint16]$codes[$argument])
                Start-Sleep -Milliseconds 1200
                "[$step] pressed"
            }
            'WheelList' {
                $list = Find-LargestList $projection
                if (-not $list) { "[$step] no projected list"; continue }
                "[$step] before: $(Describe-List $list)"
                $r = $list.Current.BoundingRectangle
                [DriveInput]::Move([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
                $notches = [int]$argument
                for ($tick = 0; $tick -lt [Math]::Abs($notches); ++$tick) {
                    [DriveInput]::Wheel($(if ($notches -lt 0) { -120 } else { 120 }))
                    Start-Sleep -Milliseconds 25
                }
                Start-Sleep -Milliseconds 2000
                $projection = Find-Projection $renderer.ProcessId
                "[$step] after:  $(Describe-List (Find-LargestList $projection))"
            }
            { $_ -in 'NativeClick', 'NativeDouble' } {
                # NativeClick:<class>:<dx>:<dy> clicks the offset (96-dpi px from the top-left corner)
                # of the console's largest visible window whose class contains <class>: a native
                # Windows Forms list or tree publishes no UIA items, so the -Native oracle aims by geometry.
                $part, $dx, $dy = $argument -split ':'
                $box = [DriveWindows]::LargestChild([uint32]$process.Id, $part)
                if (-not $box.Length) { "[$step] no visible window of class *$part*"; continue }
                $scale = [DriveInput]::DpiScale()
                $x = [int]($box[0] + [double]$dx * $scale); $y = [int]($box[1] + [double]$dy * $scale)
                if ($verb -eq 'NativeDouble') { [DriveInput]::DoubleClick($x, $y); Start-Sleep -Milliseconds 2500 }
                else { [DriveInput]::Click($x, $y, $false); Start-Sleep -Milliseconds 1500 }
                "[$step] at $x,$y in a $($box[2])x$($box[3]) window at $($box[0]),$($box[1])"
            }
            'SpinStep' {
                # SpinStep:up|down presses the upper or lower half of the first projected spin pair of any
                # Renderer window (a dialog's NumericUpDown arrows) and reports the edit values before and after.
                $spinner = $null
                foreach ($window in (Get-RendererWindows $renderer.ProcessId)) {
                    $spinner = $window.FindFirst($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
                        $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Spinner))
                    if ($spinner) { break }
                }
                if (-not $spinner) { "[$step] no projected spin pair"; continue }
                $r = $spinner.Current.BoundingRectangle
                $before = Get-EditValues $renderer.ProcessId
                $x = [int]($r.X + $r.Width / 2)
                $y = [int]($r.Y + $(if ($argument -eq 'down') { $r.Height * 0.75 } else { $r.Height * 0.25 }))
                [DriveInput]::Click($x, $y, $false)
                Start-Sleep -Milliseconds 2500
                "[$step] pressed '$argument' at $x,$y (spinner $([int]$r.Width)x$([int]$r.Height)); edits before: $($before -join ' | ') ; after: $((Get-EditValues $renderer.ProcessId) -join ' | ')"
            }
            'Interact' {
                # Exercises every projected edit, check box and radio button of the topmost dialog (not the
                # console window) with real pointer and keyboard input and reports each that did not respond.
                # Nothing is applied: the dialog is cancelled afterwards by CloseDialogs.
                $menuBar = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuBar)
                $dialog = @(Get-RendererWindows $renderer.ProcessId | Where-Object { -not $_.FindFirst($scope::Descendants, $menuBar) }) | Select-Object -First 1
                if (-not $dialog) { "[$step] no dialog"; continue }
                $typeOf = { param($t) [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, $t) }
                $control = [System.Windows.Automation.ControlType]
                $found = 0; $failed = 0
                foreach ($element in @($dialog.FindAll($scope::Descendants, (& $typeOf $control::Edit)))) {
                    $c = $element.Current
                    if ($c.IsOffscreen -or -not $c.IsEnabled -or $c.BoundingRectangle.IsEmpty) { continue }
                    try { $value = $element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern) } catch { continue }
                    if ($value.Current.IsReadOnly) { continue }
                    ++$found
                    $before = $value.Current.Value
                    $x, $y = Center $element
                    [DriveInput]::Click($x, $y, $false); Start-Sleep -Milliseconds 400
                    [DriveInput]::Text('7'); Start-Sleep -Milliseconds 500
                    $after = $value.Current.Value
                    if ($after -ne $before) { "[$step] edit '$($c.Name)' [$($c.AutomationId)] '$before' -> '$after' ok" }
                    else {
                        # The element this step holds may have been replaced while the text was typed: compare it with
                        # what the dialog offers now (same AutomationId, current runtime id and value).
                        $now = @($dialog.FindAll($scope::Descendants, (& $typeOf $control::Edit)) | Where-Object { $_.Current.AutomationId -eq $c.AutomationId })
                        $current = if ($now.Count) { try { $now[0].GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } catch { '?' } } else { '(gone)' }
                        $liveValue = if ($current -ne $before) { 'typing DID take effect on the replaced element' } else { 'typing did not take effect' }
                        if ($current -ne $before) { "[$step] edit '$($c.Name)' [$($c.AutomationId)] held element went stale; the dialog's current element holds '$current' ($liveValue)" }
                        else { ++$failed; "[$step] edit '$($c.Name)' [$($c.AutomationId)] class=$($c.ClassName) rect=$($c.BoundingRectangle) did not take typing ('$before'); focus is on $($ae::FocusedElement.Current.ControlType.ProgrammaticName) '$($ae::FocusedElement.Current.Name)'" }
                    }
                }
                foreach ($element in @($dialog.FindAll($scope::Descendants, (& $typeOf $control::CheckBox)))) {
                    $c = $element.Current
                    if ($c.IsOffscreen -or -not $c.IsEnabled) { continue }
                    try { $toggle = $element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern) } catch { continue }
                    ++$found
                    $before = $toggle.Current.ToggleState
                    $x, $y = Center $element
                    $known = @(Get-WindowHandles $renderer.ProcessId)
                    [DriveInput]::Click($x, $y, $false)
                    # The state comes back through the native control (poll for it rather than guess), and
                    # a toggle may raise a confirmation prompt of its own (Task Scheduler's "Hidden").
                    $null = Wait-Until { if ($toggle.Current.ToggleState -ne $before -or @(Get-WindowHandles $renderer.ProcessId).Count -gt $known.Count) { $true } } 3000
                    $prompts = @(Dismiss-NewWindows $renderer.ProcessId $known)
                    $after = $toggle.Current.ToggleState
                    if ($prompts.Count) { "[$step] check '$($c.Name)' raised a prompt, answered: $($prompts -join '; ')" }
                    elseif ($after -ne $before) { "[$step] check '$($c.Name)' $before -> $after ok"; [DriveInput]::Click($x, $y, $false); Start-Sleep -Milliseconds 600 }
                    else { ++$failed; "[$step] check '$($c.Name)' did not toggle ($before)" }
                }
                foreach ($element in @($dialog.FindAll($scope::Descendants, (& $typeOf $control::RadioButton)))) {
                    $c = $element.Current
                    if ($c.IsOffscreen -or -not $c.IsEnabled) { continue }
                    try { $radio = $element.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern) } catch { continue }
                    if ($radio.Current.IsSelected) { continue }
                    ++$found
                    $x, $y = Center $element
                    $known = @(Get-WindowHandles $renderer.ProcessId)
                    [DriveInput]::Click($x, $y, $false)
                    $null = Wait-Until { if ($radio.Current.IsSelected -or @(Get-WindowHandles $renderer.ProcessId).Count -gt $known.Count) { $true } } 3000
                    $prompts = @(Dismiss-NewWindows $renderer.ProcessId $known)
                    if ($prompts.Count) { "[$step] radio '$($c.Name)' raised a prompt, answered: $($prompts -join '; ')" }
                    elseif ($radio.Current.IsSelected) { "[$step] radio '$($c.Name)' selected ok" }
                    else { ++$failed; "[$step] radio '$($c.Name)' did not select" }
                }
                "[$step] '$($dialog.Current.Name)': $found interactive control(s), $failed unresponsive"
            }
            'DumpDialog' {
                # Every UIA descendant of the topmost dialog (not the console window): type, name, state, bounds.
                $menuBar = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuBar)
                $dialog = @(Get-RendererWindows $renderer.ProcessId | Where-Object { -not $_.FindFirst($scope::Descendants, $menuBar) }) | Select-Object -First 1
                if (-not $dialog) { "[$step] no dialog"; continue }
                "[$step] '$($dialog.Current.Name)'"
                foreach ($element in @($dialog.FindAll($scope::Descendants, [System.Windows.Automation.Condition]::TrueCondition) | Select-Object -First 400)) {
                    $c = $element.Current
                    $state = ''
                    try { $state += " selected=$($element.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected)" } catch { }
                    try { $state += " toggle=$($element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState)" } catch { }
                    try { $state += " value='$($element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value)'" } catch { }
                    "[$step]   $($c.ControlType.ProgrammaticName -replace 'ControlType\.','') '$($c.Name)' [$($c.AutomationId)] off=$($c.IsOffscreen) en=$($c.IsEnabled) rect=$($c.BoundingRectangle)$state"
                }
            }
            'DumpWindow' {
                # The console's own projected window: every UIA descendant (type, class, name, state, bounds),
                # or only those whose type, class or name contains the argument.
                $shown = 0
                foreach ($element in @($projection.FindAll($scope::Descendants, [System.Windows.Automation.Condition]::TrueCondition))) {
                    $c = $element.Current
                    $type = $c.ControlType.ProgrammaticName -replace 'ControlType\.',''
                    if ($argument -and ("$type $($c.ClassName) $($c.Name)" -notlike "*$argument*")) { continue }
                    if (++$shown -gt 300) { break }
                    $state = ''
                    try { $state += " selected=$($element.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected)" } catch { }
                    try { $state += " toggle=$($element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState)" } catch { }
                    "[$step]   $type '$($c.Name)' class=$($c.ClassName) [$($c.AutomationId)] off=$($c.IsOffscreen) en=$($c.IsEnabled) rect=$($c.BoundingRectangle)$state"
                }
                "[$step] $shown element(s)"
            }
            'EditValues' { "[$step] $((Get-EditValues $renderer.ProcessId) -join ' | ')" }
            'ListState' { "[$step] $(Describe-List (Find-LargestList $projection))" }
            'Items' {
                # What the projection offers to click: every realized tree item and the first rows.
                $trees = @($projection.FindAll($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
                    $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::TreeItem)) | ForEach-Object { $_.Current.Name })
                $rows = @(Get-ListItemNames $projection)
                "[$step] tree items: $($trees -join ' | ')"
                "[$step] list rows ($($rows.Count)): $((@($rows | Select-Object -First 12)) -join ' | ')"
            }
            'Screenshot' { Save-Screenshot $projection $argument; "[$step] saved" }
            'ScreenshotScreen' {
                Add-Type -AssemblyName System.Windows.Forms
                $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
                $bitmap = [System.Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
                $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
                $graphics.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bitmap.Size)
                $bitmap.Save($argument)
                $graphics.Dispose(); $bitmap.Dispose()
                "[$step] saved ($($bounds.Width)x$($bounds.Height))"
            }
            'CloseDialogs' {
                # Answers every projected dialog but the console's own window through its cancel
                # path (Cancel, Close, No, then OK) -- never Apply -- and reports each.
                $cancelWord = -join ([char[]](0x53D6, 0x6D88))
                $closeWord = -join ([char[]](0x95DC, 0x9589))
                $noWord = [string][char]0x5426
                $okWord = -join ([char[]](0x78BA, 0x5B9A))
                $buttonType = [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
                $tops = @(Get-RendererWindows $renderer.ProcessId)
                $closed = 0
                foreach ($top in $tops) {
                    # The console's own window has the menu bar (its title follows the selection).
                    if ($top.FindFirst($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
                            $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuBar))) { continue }
                    $buttons = @($top.FindAll($scope::Descendants, $buttonType))
                    $choice = $null
                    # A Win32 button name may carry its mnemonic: "Close(C)" / "<close>(C)".
                    foreach ($pattern in "^($cancelWord|Cancel)", "^($closeWord|Close)(\(.\))?$", "^($noWord|No)\b", "^($okWord|OK)\b") {
                        $choice = $buttons | Where-Object { $_.Current.Name -match $pattern -and $_.Current.IsEnabled -and $_.Current.AutomationId -ne 'Close' } | Select-Object -First 1
                        if ($choice) { break }
                    }
                    if (-not $choice) { "[$step] '$($top.Current.Name)': no cancel button among: $(($buttons | ForEach-Object { $_.Current.Name }) -join ' | ')"; continue }
                    $choice.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                    "[$step] '$($top.Current.Name)' -> '$($choice.Current.Name)'"
                    ++$closed
                    Start-Sleep -Milliseconds 1200
                }
                if (-not $closed) { "[$step] nothing to close" }
            }
            'ScreenshotWindows' {
                $tops = @(Get-RendererWindows $renderer.ProcessId)
                $n = 0
                foreach ($top in $tops) {
                    $file = '{0}-{1}.png' -f $argument, $n++
                    Save-Screenshot $top $file
                    "[$step] $($top.Current.Name) -> $file"
                }
            }
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
