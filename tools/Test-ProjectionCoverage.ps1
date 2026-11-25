# Runs the expanded shapes against production Bridge and WinUI payloads.
# This invocation inspects and closes only the oracle and renderer it creates.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string] $Configuration = 'Release',
    [string] $PayloadDirectory = '',
    [ValidateRange(5, 180)][int] $TimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
if (-not $PayloadDirectory) {
    $PayloadDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) "build\bin\x64\$Configuration"
}
$PayloadDirectory = (Resolve-Path -LiteralPath $PayloadDirectory).Path
$oraclePath = Join-Path $PayloadDirectory 'LegacyDialogHost.exe'
$injectorPath = Join-Path $PayloadDirectory 'FluentShell.Injector.exe'
$rendererPath = Join-Path $PayloadDirectory 'Renderer\FluentShell.Renderer.exe'
foreach ($path in @($oraclePath, $injectorPath, $rendererPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Production payload missing: $path" }
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
if (-not ('CoverageOracleNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class CoverageOracleNative {
    delegate bool EnumCallback(IntPtr hwnd, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int show);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, UIntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint message,
        UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    public static IntPtr Find(uint pid) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((hwnd, data) => {
            uint owner; GetWindowThreadProcessId(hwnd, out owner);
            if (owner!=pid) return true;
            var name=new StringBuilder(256); GetClassName(hwnd, name, name.Capacity);
            if (name.ToString()!="FluentShell.ProjectionCoverageOracle") return true;
            found=hwnd; return false;
        }, IntPtr.Zero);
        return found;
    }
    public static long Query(IntPtr hwnd, uint pid, uint field) {
        uint owner; GetWindowThreadProcessId(hwnd, out owner);
        if (owner!=pid) throw new InvalidOperationException("The oracle HWND belongs to another process.");
        UIntPtr result;
        if (SendMessageTimeout(hwnd, 0x8000+411, new UIntPtr(field), IntPtr.Zero, 0x22, 2000, out result)==IntPtr.Zero)
            throw new InvalidOperationException("The native oracle did not answer before its deadline.");
        return unchecked((long)result.ToUInt64());
    }
}
'@
}

$ae = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]
function Wait-Coverage([scriptblock] $Read, [string] $Failure, [int] $Seconds = $TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        try { $value = & $Read; if ($value) { return $value } }
        catch [System.Windows.Automation.ElementNotAvailableException] { }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Failure
}
function Find-CoverageName($Parent, [string] $Name) {
    return $Parent.FindFirst($scope::Descendants,
        [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, $Name))
}
function Find-CoverageType($Parent, $Type) {
    return $Parent.FindFirst($scope::Descendants,
        [System.Windows.Automation.PropertyCondition]::new($ae::ControlTypeProperty, $Type))
}
function Assert-Coverage([bool] $Passed, [string] $Failure) {
    if (-not $Passed) { throw $Failure }
}

$oracle = $null
$renderer = $null
$injector = $null
$native = [IntPtr]::Zero
$tag = [Guid]::NewGuid().ToString('N')
$injectOut = Join-Path ([IO.Path]::GetTempPath()) "FluentShell.Coverage.$tag.out.log"
$injectError = Join-Path ([IO.Path]::GetTempPath()) "FluentShell.Coverage.$tag.err.log"
try {
    $oracle = Start-Process -FilePath $oraclePath -ArgumentList '--coverage' -WorkingDirectory $PayloadDirectory `
        -WindowStyle Hidden -PassThru
    $native = Wait-Coverage {
        if ($oracle.HasExited) { throw 'The coverage oracle exited before creating its window.' }
        $hwnd = [CoverageOracleNative]::Find([uint32]$oracle.Id)
        if ($hwnd -ne [IntPtr]::Zero) { $hwnd }
    } 'The coverage oracle did not create its native window.'
    [void][CoverageOracleNative]::ShowWindow($native, 4) # owned window, no activation
    Assert-Coverage ([CoverageOracleNative]::Query($native, $oracle.Id, 4) -eq 10000) 'The oracle did not create 10000 stored rows.'
    Assert-Coverage ([CoverageOracleNative]::Query($native, $oracle.Id, 6) -eq 1024) 'The oracle did not create 1024 hidden HWNDs.'
    $injector = Start-Process -FilePath $injectorPath -WorkingDirectory $PayloadDirectory `
        -ArgumentList @('inject', ('"' + $oraclePath + '"'), '--pid', $oracle.Id) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $injectOut -RedirectStandardError $injectError
    # Retain the process handle before this short-lived command exits. Windows
    # PowerShell can otherwise expose a null ExitCode after a successful wait.
    [void]$injector.Handle
    $child = Wait-Coverage {
        $matches = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($oracle.Id)" |
            Where-Object { $_.ExecutablePath -eq $rendererPath -and
                $_.CommandLine -match ("--parent-pid\s+" + $oracle.Id + "(?:\s|$)") })
        if ($matches.Count -gt 1) { throw 'The oracle unexpectedly created several matching renderers.' }
        if ($matches.Count -eq 1) { $matches[0] }
    } 'The injected oracle did not start its renderer.'
    $renderer = Get-Process -Id $child.ProcessId -ErrorAction Stop
    $window = Wait-Coverage {
        $ae::RootElement.FindFirst($scope::Children, [System.Windows.Automation.AndCondition]::new(
            [System.Windows.Automation.PropertyCondition]::new($ae::ProcessIdProperty, [int]$renderer.Id),
            [System.Windows.Automation.PropertyCondition]::new($ae::NameProperty, 'FluentShell Projection Coverage Oracle')))
    } 'The renderer did not expose the projected oracle window.'
    $viewport = Wait-Coverage {
        $window.FindFirst($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
            $ae::AutomationIdProperty, 'FluentShell.ContentViewport'))
    } 'The renderer did not expose its FluentShell viewport.'
    Assert-Coverage ($viewport.Current.ProcessId -eq $renderer.Id) 'The viewport belongs to another process.'
    if (-not $injector.WaitForExit(20000)) { throw 'The injector did not finish its readiness check.' }
    if ($injector.ExitCode -ne 0) {
        throw ("Injector failed: " + (Get-Content -Raw -LiteralPath $injectOut) + (Get-Content -Raw -LiteralPath $injectError))
    }
    $list = Wait-Coverage { Find-CoverageType $viewport ([System.Windows.Automation.ControlType]::List) } `
        'The 10000-row stored report did not project.'
    $tabs = Wait-Coverage { Find-CoverageType $viewport ([System.Windows.Automation.ControlType]::Tab) } `
        'The fixed-width tabs did not project.'
    Assert-Coverage ($list.Current.FrameworkId -eq 'XAML' -and $tabs.Current.FrameworkId -eq 'XAML') `
        'Projected list and tabs must be genuine XAML controls.'
    $bars = @($viewport.FindAll($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
        $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::ProgressBar)))
    Assert-Coverage ($bars.Count -eq 2) 'Both vertical progress shapes must project.'
    $range = $null
    $indeterminate = 0
    foreach ($bar in $bars) {
        $rect = $bar.Current.BoundingRectangle
        Assert-Coverage ($rect.Height -gt $rect.Width * 3) 'The vertical progress layout became horizontal.'
        $pattern = $null
        if ($bar.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$pattern)) { $range = $pattern }
        else { ++$indeterminate }
    }
    Assert-Coverage ($null -ne $range -and $indeterminate -eq 1) 'Determinate and marquee capabilities are wrong.'
    Assert-Coverage ($range.Current.Minimum -eq 10 -and $range.Current.Maximum -eq 210 -and $range.Current.IsReadOnly) `
        'The determinate progress lost its native read-only range.'

    # The window is discoverable during provisional commit. Wait for a later
    # canonical timer patch before issuing input; normal reconcile runs only
    # after the Bridge has completed the interactive commit.
    $tick = [CoverageOracleNative]::Query($native, $oracle.Id, 0)
    [void](Wait-Coverage {
        $current = [CoverageOracleNative]::Query($native, $oracle.Id, 0)
        if ($current -gt $tick) { Find-CoverageName $viewport "Native tick=$current" }
    } 'Native timer text did not patch back to XAML.' 15)
    'PASS native timer: canonical text updates patched back to XAML.'

    $tab = Wait-Coverage { Find-CoverageName $tabs 'Fixed tab C' } 'The third fixed-width tab was missing.'
    $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    [void](Wait-Coverage {
        if ([CoverageOracleNative]::Query($native, $oracle.Id, 1) -eq 2) { Find-CoverageName $viewport 'Selected tab=2' }
    } 'Projected tab selection did not round-trip to native.' 15)
    'PASS fixed-width tabs: projected selection returned to native tab 2.'

    [void][CoverageOracleNative]::PostMessage($native, 0x8000 + 410, [UIntPtr]::Zero, [IntPtr]::Zero)
    [void](Wait-Coverage {
        if ([CoverageOracleNative]::Query($native, $oracle.Id, 1) -eq 1 -and
            [CoverageOracleNative]::Query($native, $oracle.Id, 2) -eq 7 -and
            (Find-CoverageName $viewport 'Selected row=7') -and (Find-CoverageName $viewport 'Selected tab=1')) {
            Find-CoverageName $list 'Native row update 1 Ready'
        }
    } 'Native row text, selection and tabs did not patch together.' 15)
    'PASS native update: stored row text, selection and tabs patched back.'

    $scroll = $list.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern)
    Assert-Coverage $scroll.Current.VerticallyScrollable 'The stored report did not expose vertical scrolling.'
    "scroll before: percent=$($scroll.Current.VerticalScrollPercent) viewSize=$($scroll.Current.VerticalViewSize) nativeTop=$([CoverageOracleNative]::Query($native, $oracle.Id, 3))"
    $scroll.SetScrollPercent(-1, 50)
    "scroll after request: percent=$($scroll.Current.VerticalScrollPercent) nativeTop=$([CoverageOracleNative]::Query($native, $oracle.Id, 3))"
    [void](Wait-Coverage {
        if ([CoverageOracleNative]::Query($native, $oracle.Id, 3) -gt 4000) { $true }
    } 'Projected list scroll did not reach the native viewport.' 15)
    $row = Wait-Coverage {
        $items = $list.FindAll($scope::Descendants, [System.Windows.Automation.PropertyCondition]::new(
            $ae::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem))
        foreach ($item in $items) {
            if (-not $item.Current.IsOffscreen -and $item.Current.Name -match '^Row ([4-9][0-9]{3}) Ready$') { return $item }
        }
    } 'The new native row window did not patch back to the renderer.' 15
    $rowIndex = [int]([regex]::Match($row.Current.Name, '^Row ([0-9]+) ').Groups[1].Value)
    $row.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    [void](Wait-Coverage {
        if ([CoverageOracleNative]::Query($native, $oracle.Id, 2) -eq $rowIndex) {
            Find-CoverageName $viewport "Selected row=$rowIndex"
        }
    } 'Projected selection did not preserve the absolute native row index.' 15)
    "PASS stored list: native viewport scrolled and row $rowIndex selected by absolute index."
    'PASS hidden HWNDs: all expanded shapes projected with 1024 hidden children.'
    'Projection coverage acceptance passed.'
} finally {
    if ($oracle) {
        if (-not $oracle.HasExited) {
            [uint32]$owner = 0
            if ($native -ne [IntPtr]::Zero) { [void][CoverageOracleNative]::GetWindowThreadProcessId($native, [ref]$owner) }
            if ($owner -eq $oracle.Id) { [void][CoverageOracleNative]::PostMessage($native, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) }
            if (-not $oracle.WaitForExit(5000)) { $oracle.Kill(); $oracle.WaitForExit() }
        }
        $oracle.Dispose()
    }
    if ($renderer) {
        if (-not $renderer.HasExited -and -not $renderer.WaitForExit(5000)) { $renderer.Kill(); $renderer.WaitForExit() }
        $renderer.Dispose()
    }
    if ($injector) {
        if (-not $injector.HasExited) { $injector.Kill(); $injector.WaitForExit() }
        $injector.Dispose()
    }
    foreach ($log in @($injectOut, $injectError)) {
        if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    }
}
