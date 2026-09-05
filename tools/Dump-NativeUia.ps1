# Dumps the UI Automation view of a native window subtree, which is how a private
# class or a DirectUI island reveals what it is made of: HWND-less DirectUI
# elements only exist in this view.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [string] $RootClass = '',
    [int] $MaxDepth = 6,
    [int] $MaxNodes = 200
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

$start = $window
if ($RootClass -ne '') {
    $start = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ClassNameProperty, $RootClass)))
    if ($null -eq $start) { "no UIA element with class '$RootClass'"; exit 1 }
}

$script:count = 0
function Show-Element($element, [int] $depth) {
    if ($script:count -ge $MaxNodes -or $depth -gt $MaxDepth) { return }
    ++$script:count
    $info = $element.Current
    $patterns = @()
    foreach ($pattern in $element.GetSupportedPatterns()) { $patterns += $pattern.ProgrammaticName }
    $rect = $info.BoundingRectangle
    '{0}{1} name="{2}" class="{3}" id="{4}" hwnd=0x{5:X} rect={6},{7} {8}x{9} patterns={10}' -f
        (' ' * ($depth * 2)), $info.ControlType.ProgrammaticName, $info.Name, $info.ClassName,
        $info.AutomationId, [int64] $info.NativeWindowHandle, [int] $rect.X, [int] $rect.Y,
        [int] $rect.Width, [int] $rect.Height, (($patterns -replace 'PatternIdentifiers\.Pattern', '') -join ',')
    $child = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetFirstChild($element)
    while ($null -ne $child) {
        Show-Element $child ($depth + 1)
        $child = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetNextSibling($child)
    }
}

Show-Element $start 0
