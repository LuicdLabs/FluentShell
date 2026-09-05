# Drives a projected surface through UI Automation and reports what the native
# oracle observed, so an action's round trip can be checked from outside both
# processes.
param(
    [int] $SliderValue = 15,
    [string] $SelectTreeItem = 'Event Viewer',
    [string] $ExpandTreeItem = 'Services and Applications'
)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$viewportCondition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'FluentShell.ContentViewport')
$viewport = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $viewportCondition)
if ($null -eq $viewport) { 'no projected viewport'; exit 1 }

function Find-ByType([string] $type) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::LookupById([int] $type))
    return $viewport.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Get-Texts {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Text)
    $viewport.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition) |
        ForEach-Object { $_.Current.Name }
}

$slider = Find-ByType 50015  # UIA_SliderControlTypeId
if ($null -ne $slider) {
    $range = $slider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    "slider before: value=$($range.Current.Value) min=$($range.Current.Minimum) max=$($range.Current.Maximum)"
    $range.SetValue([double] $SliderValue)
    Start-Sleep -Milliseconds 1500
    $range = $slider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    "slider after:  value=$($range.Current.Value)"
} else { 'no projected slider' }

$tree = Find-ByType 50023  # UIA_TreeControlTypeId
if ($null -ne $tree) {
    $items = $tree.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::TreeItem)))
    "tree items: " + (($items | ForEach-Object { $_.Current.Name }) -join ' | ')
    foreach ($item in $items) {
        if ($item.Current.Name -eq $ExpandTreeItem) {
            $expand = $item.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
            if ($null -ne $expand) {
                $expand.Expand()
                Start-Sleep -Milliseconds 1500
                "expanded '$ExpandTreeItem'"
            }
        }
    }
    $items = $tree.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::TreeItem)))
    "tree items after expand: " + (($items | ForEach-Object { $_.Current.Name }) -join ' | ')
    foreach ($item in $items) {
        if ($item.Current.Name -eq $SelectTreeItem) {
            $selection = $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
            if ($null -ne $selection) {
                $selection.Select()
                Start-Sleep -Milliseconds 1500
                "selected '$SelectTreeItem'"
            }
        }
    }
} else { 'no projected tree' }

'--- projected text after actions ---'
Get-Texts | Where-Object { $_ -match 'trackbar|Tree|result' }
