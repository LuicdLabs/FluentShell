# Reports the exact UIA pattern availability and legacy accessibility contract of
# every element inside a native window's UIA subtree.  GetSupportedPatterns can be
# lossy for some providers, so each pattern is asked for by property as well.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [Parameter(Mandatory = $true)][string] $RootClass,
    [int] $MaxNodes = 40
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$root = [System.Windows.Automation.AutomationElement]::RootElement
$window = $root.FindFirst([System.Windows.Automation.TreeScope]::Children,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)))
if ($null -eq $window) { "no UIA window for $ProcessName"; exit 1 }

$host_ = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, $RootClass)))
if ($null -eq $host_) { "no UIA element with class '$RootClass'"; exit 1 }

$probes = @(
    @{ Name = 'Invoke';         Property = [System.Windows.Automation.AutomationElement]::IsInvokePatternAvailableProperty },
    @{ Name = 'Toggle';         Property = [System.Windows.Automation.AutomationElement]::IsTogglePatternAvailableProperty },
    @{ Name = 'ExpandCollapse'; Property = [System.Windows.Automation.AutomationElement]::IsExpandCollapsePatternAvailableProperty },
    @{ Name = 'Value';          Property = [System.Windows.Automation.AutomationElement]::IsValuePatternAvailableProperty },
    @{ Name = 'Selection';      Property = [System.Windows.Automation.AutomationElement]::IsSelectionPatternAvailableProperty },
    @{ Name = 'SelectionItem';  Property = [System.Windows.Automation.AutomationElement]::IsSelectionItemPatternAvailableProperty },
    @{ Name = 'RangeValue';     Property = [System.Windows.Automation.AutomationElement]::IsRangeValuePatternAvailableProperty },
    @{ Name = 'Text';           Property = [System.Windows.Automation.AutomationElement]::IsTextPatternAvailableProperty },
    @{ Name = 'Legacy';         Property = [System.Windows.Automation.AutomationElement]::IsLegacyIAccessiblePatternAvailableProperty }
)

$elements = @($host_) + @($host_.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false))))

$index = 0
foreach ($element in $elements) {
    if ($index -ge $MaxNodes) { break }
    ++$index
    $info = $element.Current
    $available = @()
    foreach ($probe in $probes) {
        try {
            if ($element.GetCurrentPropertyValue($probe.Property) -eq $true) { $available += $probe.Name }
        }
        catch { }
    }
    $legacy = ''
    try {
        $pattern = $element.GetCurrentPattern(
            [System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
        if ($null -ne $pattern) {
            $current = $pattern.Current
            $legacy = " legacyRole=$($current.Role) legacyState=$($current.State) defaultAction='$($current.DefaultAction)'"
        }
    }
    catch { }
    $rect = $info.BoundingRectangle
    '{0} name="{1}" class="{2}" id="{3}" hwnd=0x{4:X} enabled={5} keyboard={6} rect={7},{8} {9}x{10} patterns={11}{12}' -f
        $info.ControlType.ProgrammaticName, $info.Name, $info.ClassName, $info.AutomationId,
        [int64] $info.NativeWindowHandle, $info.IsEnabled, $info.IsKeyboardFocusable,
        [int] $rect.X, [int] $rect.Y, [int] $rect.Width, [int] $rect.Height,
        ($available -join ','), $legacy
}
