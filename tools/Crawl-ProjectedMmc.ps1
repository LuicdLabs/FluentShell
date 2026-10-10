<#
Census of which MMC views project. Starts a disposable console, injects the Release
Bridge, then walks the native scope tree breadth-first: each node is selected and
(up to -MaxDepth) expanded through the tree's own messages (TVM_SELECTITEM,
TVM_EXPAND), exactly as native navigation would. After each step it records whether
the window is projected (DWM reports the native frame cloaked), how long that took,
and what the Bridge logged for this process (rejections, rollbacks, retries).

Navigation is read-only, but a snap-in may run its own work when a node is selected
(enumerating disks, querying services). A dialog that appears is recorded and closed
with WM_CLOSE (its cancel path). Use disposable consoles only; the console is closed
at the end. Output: <OutputDirectory>\<console>-crawl.jsonl and a printed summary.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Console = 'compmgmt',
    [ValidateRange(1, 400)][int]$MaxNodes = 60,
    [ValidateRange(1, 8)][int]$MaxDepth = 3,
    # Longer than the Bridge's reconcile hold (3 s) on a refused view, so a stale
    # projection held while a view loads is not counted as projected.
    [ValidateRange(500, 20000)][int]$SettleMs = 4500,
    # How long a node may take to (re)project after its view settles.
    [ValidateRange(0, 60)][int]$ProjectSeconds = 8,
    [ValidateRange(5, 120)][int]$StartupSeconds = 45,
    [string]$OutputDirectory = '',
    [switch]$RunAsInvoker
)

$ErrorActionPreference = 'Stop'
# Node names are localized: emit UTF-8 so a redirected log stays readable.
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }
$repository = Split-Path -Parent $PSScriptRoot
$injector = Join-Path $repository 'build\bin\x64\Release\FluentShell.Injector.exe'
$system = [Environment]::SystemDirectory
$mmc = Join-Path $system 'mmc.exe'
$bridgeLog = Join-Path $env:TEMP 'FluentShell.log'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repository ('build\mmc-crawl-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$report = Join-Path $OutputDirectory "$Console-crawl.jsonl"
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class CrawlNative {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtrW(IntPtr hwnd, int index);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, IntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr process, IntPtr address, IntPtr size, uint type);
    [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr written);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

    static string ClassOf(IntPtr h) { var b = new StringBuilder(128); GetClassNameW(h, b, 128); return b.ToString(); }
    // Messages to the target are bounded: a snap-in can block its UI thread.
    static IntPtr Send(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam) {
        IntPtr result; SendMessageTimeoutW(hwnd, message, wParam, lParam, 0x0002 /* ABORTIFHUNG */, 15000, out result);
        return result;
    }

    public static IntPtr Frame(uint pid) {
        IntPtr frame = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner == pid && ClassOf(h) == "MMCMainFrame") { frame = h; return false; }
            return true;
        }, IntPtr.Zero);
        return frame;
    }
    public static IntPtr Tree(IntPtr frame) {
        IntPtr tree = IntPtr.Zero;
        if (frame == IntPtr.Zero) return tree;
        EnumChildWindows(frame, (h, l) => {
            if (ClassOf(h) == "SysTreeView32" && IsWindowVisible(h)) { tree = h; return false; }
            return true;
        }, IntPtr.Zero);
        return tree;
    }
    public static bool Cloaked(IntPtr frame) {
        int value; return DwmGetWindowAttribute(frame, 14 /* DWMWA_CLOAKED */, out value, 4) == 0 && (value & 1 /* DWM_CLOAKED_APP */) != 0;
    }
    public static IntPtr Next(IntPtr tree, int relation, IntPtr item) { return Send(tree, 0x110A, (IntPtr)relation, item); }
    public static void Select(IntPtr tree, IntPtr item) { Send(tree, 0x110B, (IntPtr)0x9, item); }
    public static void Expand(IntPtr tree, IntPtr item) { Send(tree, 0x1102, (IntPtr)0x2 /* TVE_EXPAND */, item); }
    public static string Label(IntPtr tree, IntPtr item) {
        uint pid; GetWindowThreadProcessId(tree, out pid);
        IntPtr process = OpenProcess(0x0008 | 0x0010 | 0x0020, false, pid);
        if (process == IntPtr.Zero) return null;
        IntPtr remote = VirtualAllocEx(process, IntPtr.Zero, (IntPtr)1024, 0x3000, 0x04);
        try {
            var tvitem = new byte[56];
            BitConverter.GetBytes(0x0001 | 0x0010).CopyTo(tvitem, 0);          // TVIF_TEXT | TVIF_HANDLE
            BitConverter.GetBytes(item.ToInt64()).CopyTo(tvitem, 8);           // hItem
            BitConverter.GetBytes(remote.ToInt64() + 64).CopyTo(tvitem, 24);   // pszText
            BitConverter.GetBytes(256).CopyTo(tvitem, 32);                     // cchTextMax
            IntPtr io;
            WriteProcessMemory(process, remote, tvitem, (IntPtr)56, out io);
            WriteProcessMemory(process, remote + 64, new byte[512], (IntPtr)512, out io);
            if (Send(tree, 0x113E /* TVM_GETITEMW */, IntPtr.Zero, remote) == IntPtr.Zero) return null;
            var text = new byte[512];
            ReadProcessMemory(process, remote + 64, text, (IntPtr)512, out io);
            var label = Encoding.Unicode.GetString(text);
            int end = label.IndexOf('\0');
            return end >= 0 ? label.Substring(0, end) : label;
        } finally { VirtualFreeEx(process, remote, IntPtr.Zero, 0x8000); CloseHandle(process); }
    }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    public struct RECT { public int Left, Top, Right, Bottom; }
    // Visible top-level windows of a process other than the one covering `frame`'s
    // rectangle (the projected frame's proxy): the renderer's dialogs.
    public static List<IntPtr> OtherTopLevels(uint pid, IntPtr frame) {
        var found = new List<IntPtr>();
        RECT frameRect; GetWindowRect(frame, out frameRect);
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid || !IsWindowVisible(h)) return true;
            RECT rect; GetWindowRect(h, out rect);
            if (rect.Right - rect.Left <= 0 || rect.Bottom - rect.Top <= 0) return true;
            bool proxy = Math.Abs(rect.Left - frameRect.Left) <= 16 && Math.Abs(rect.Top - frameRect.Top) <= 16 &&
                Math.Abs(rect.Right - frameRect.Right) <= 16 && Math.Abs(rect.Bottom - frameRect.Bottom) <= 16;
            if (!proxy) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // Visible dialogs of the process: #32770, or a captioned window with an owner.
    // Helper windows (OLE channels, IME, tooltips) are never touched.
    public static List<string> Dialogs(uint pid, bool close) {
        var found = new List<string>();
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner != pid || !IsWindowVisible(h)) return true;
            var cls = ClassOf(h);
            const long WS_CAPTION = 0x00C00000;
            bool dialog = cls == "#32770" ||
                ((GetWindowLongPtrW(h, -16).ToInt64() & WS_CAPTION) == WS_CAPTION && GetWindow(h, 4) != IntPtr.Zero);
            if (!dialog || cls == "MMCMainFrame") return true;
            var title = new StringBuilder(256); GetWindowTextW(h, title, 256);
            found.Add(cls + " '" + title + "'");
            if (close) PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$script:logOffset = if (Test-Path $bridgeLog) { (Get-Item $bridgeLog).Length } else { 0 }
function Read-BridgeLines([int]$TargetId) {
    if (-not (Test-Path $bridgeLog)) { return @() }
    $stream = [System.IO.File]::Open($bridgeLog, 'Open', 'Read', 'ReadWrite')
    try {
        if ($stream.Length -lt $script:logOffset) { $script:logOffset = 0 }
        [void]$stream.Seek($script:logOffset, 'Begin')
        $text = [System.IO.StreamReader]::new($stream).ReadToEnd()
        $script:logOffset = $stream.Length
    } finally { $stream.Dispose() }
    return @($text -split "`r?`n" | Where-Object { $_ -match "\[pid=$TargetId\]" })
}
function Summarize-Lines([string[]]$Lines) {
    $interesting = 'Capture rejected child|Restoring native window|remains untranslated|gate rejected|barrier|Retrying projection|Projected native window|surface fault|stays untranslated|heartbeat|session failed|missed its deadline|UIA gate attempt 8 failed|UIA gate failed|UIA validation deadline|Native action failed|Reconcile capture rejected|foreground handoff'
    return @($Lines | Where-Object { $_ -match $interesting } | ForEach-Object {
        $line = $_ -replace '^\S+ \[pid=\d+\] \[FluentShell\] ', ''
        $line.Substring(0, [Math]::Min(300, $line.Length))
    } | Select-Object -Unique)
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
# A prompt the snap-in raises while the frame is projected (MessageBox, TaskDialog)
# opens as the renderer's own window and has no native HWND for WM_CLOSE. It is
# answered through its cancel path -- Cancel, then No, then the only OK -- so the
# crawl never changes state and the next view is not measured behind a modal prompt.
function Close-RendererDialogs([int]$TargetId, [IntPtr]$Frame) {
    $closed = @()
    $renderers = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$TargetId" |
        Where-Object { $_.Name -eq 'FluentShell.Renderer.exe' })
    $buttonType = [Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Button)
    foreach ($renderer in $renderers) {
        foreach ($window in [CrawlNative]::OtherTopLevels([uint32]$renderer.ProcessId, $Frame)) {
            try {
                $element = [Windows.Automation.AutomationElement]::FromHandle($window)
                $buttons = @($element.FindAll([Windows.Automation.TreeScope]::Descendants, $buttonType))
                $choice = $null
                # Cancel / No / OK, also in zh-TW. Built from code points: Windows PowerShell 5.1
                # reads this BOM-less script in the ANSI code page, so no non-ASCII may sit in it.
                $cancelWord = -join ([char[]](0x53D6, 0x6D88))
                $noWord = [string][char]0x5426
                $okWord = -join ([char[]](0x78BA, 0x5B9A))
                foreach ($pattern in "^($cancelWord|Cancel)", "^($noWord|No)\b", "^($okWord|OK)\b") {
                    $choice = $buttons | Where-Object { $_.Current.Name -match $pattern -and $_.Current.AutomationId -ne 'Close' } |
                        Select-Object -First 1
                    if ($choice) { break }
                }
                if (-not $choice) { continue }
                $title = $element.Current.Name
                $choice.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
                $closed += "renderer '$title' -> '$($choice.Current.Name)'"
            } catch { $closed += "renderer dialog not answered: $($_.Exception.Message)" }
        }
    }
    return $closed
}
function Wait-Cloaked([IntPtr]$Frame, [int]$Seconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        if ([CrawlNative]::Cloaked($Frame)) { return [int]$watch.ElapsedMilliseconds }
        Start-Sleep -Milliseconds 250
    }
    return -1
}

$previous = $env:__COMPAT_LAYER
if ($RunAsInvoker) { $env:__COMPAT_LAYER = 'RunAsInvoker' }
try {
    $arguments = if ($Console -eq 'empty') { @() } else { @((Join-Path $system "$Console.msc")) }
    $launched = Start-Process -FilePath $mmc -ArgumentList $arguments -PassThru
} finally { $env:__COMPAT_LAYER = $previous }
$target = $null
$results = [System.Collections.Generic.List[object]]::new()
try {
    Start-Sleep -Seconds 4
    $target = Get-Process mmc | Where-Object { $_.StartTime -ge $launched.StartTime.AddSeconds(-1) } |
        Sort-Object StartTime | Select-Object -First 1
    [void](& $injector inject $mmc --pid $target.Id 2>&1)
    $frame = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ($frame -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
        $frame = [CrawlNative]::Frame([uint32]$target.Id); Start-Sleep -Milliseconds 250
    }
    $startupMs = Wait-Cloaked $frame $StartupSeconds
    $results.Add([pscustomobject]@{ path = '(startup)'; projected = $startupMs -ge 0; projectMs = $startupMs
        dialogs = [CrawlNative]::Dialogs([uint32]$target.Id, $false); log = Summarize-Lines (Read-BridgeLines $target.Id) })
    "{0} (startup) {1} ms" -f $(if ($startupMs -ge 0) { 'OK  ' } else { 'NAT ' }), $startupMs
    $tree = [CrawlNative]::Tree($frame)
    if ($tree -eq [IntPtr]::Zero) { throw 'no visible scope tree' }
    $queue = [System.Collections.Generic.Queue[object]]::new()
    for ($item = [CrawlNative]::Next($tree, 0, [IntPtr]::Zero); $item -ne [IntPtr]::Zero; $item = [CrawlNative]::Next($tree, 1, $item)) {
        $queue.Enqueue([pscustomobject]@{ Item = $item; Path = [string][CrawlNative]::Label($tree, $item); Depth = 1 })
    }
    $visited = 0
    while ($queue.Count -gt 0 -and $visited -lt $MaxNodes) {
        $entry = $queue.Dequeue()
        $item = [IntPtr]$entry.Item; $path = [string]$entry.Path; $depth = [int]$entry.Depth
        ++$visited
        [CrawlNative]::Select($tree, $item)
        Start-Sleep -Milliseconds $SettleMs
        $dialogs = @([CrawlNative]::Dialogs([uint32]$target.Id, $true)) + @(Close-RendererDialogs $target.Id $frame)
        $projectMs = Wait-Cloaked $frame $ProjectSeconds
        # A refused reconcile holds the projection for a few seconds before the window is
        # handed back, so a cloaked frame is only a projection once that hold has lapsed.
        $nodeLines = @(Read-BridgeLines $target.Id)
        if ($projectMs -ge 0 -and @($nodeLines | Where-Object { $_ -match 'holding the projection' }).Count -gt 0) {
            Start-Sleep -Milliseconds 5000
            $nodeLines += @(Read-BridgeLines $target.Id)
            if (-not [CrawlNative]::Cloaked($frame)) { $projectMs = -1 }
        }
        $children = 0
        if ($depth -lt $MaxDepth) {
            [CrawlNative]::Expand($tree, $item)
            Start-Sleep -Milliseconds 600
            for ($child = [CrawlNative]::Next($tree, 4, $item); $child -ne [IntPtr]::Zero; $child = [CrawlNative]::Next($tree, 1, $child)) {
                $queue.Enqueue([pscustomobject]@{ Item = $child
                    Path = "$path / $([CrawlNative]::Label($tree, $child))"; Depth = $depth + 1 })
                ++$children
            }
        }
        $nodeLines += @(Read-BridgeLines $target.Id)
        $results.Add([pscustomobject]@{ path = $path; projected = $projectMs -ge 0; projectMs = $projectMs
            dialogs = $dialogs; children = $children; log = Summarize-Lines $nodeLines })
        "{0} {1}{2}" -f $(if ($projectMs -ge 0) { 'OK  ' } else { 'NAT ' }), $path,
            $(if ($dialogs.Count) { "  [dialogs: $($dialogs -join ', ')]" } else { '' })
    }
} catch {
    "crawl stopped: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage)"
} finally {
    $results | ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 4 } | Set-Content -Encoding UTF8 $report
    if ($target) {
        $renderers = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($target.Id)" |
            Where-Object { $_.Name -eq 'FluentShell.Renderer.exe' })
        Stop-Process -Id $target.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
        foreach ($renderer in $renderers) {
            Get-Process -Id $renderer.ProcessId -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -like '*FluentShell.Renderer.exe' } | Stop-Process -Force
        }
    }
    $native = @($results | Where-Object { -not $_.projected }).Count
    "summary: $Console visited=$($results.Count) native=$native report=$report"
}
