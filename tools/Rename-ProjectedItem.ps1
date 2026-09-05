# Drives an in-place rename on a projected tree or list through real keyboard
# input: F2 opens the editor, the text is typed, Enter commits.  The native
# oracle's log is the proof of what the application decided.
param(
    [ValidateSet('tree', 'list')][string] $Target = 'tree',
    [Parameter(Mandatory = $true)][string] $NewText,
    [string] $ItemName = ''
)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$viewport = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'FluentShell.ContentViewport')))
if ($null -eq $viewport) { 'no projected viewport'; exit 1 }

$containerType = if ($Target -eq 'tree') {
    [System.Windows.Automation.ControlType]::Tree
} else {
    [System.Windows.Automation.ControlType]::List
}
$itemType = if ($Target -eq 'tree') {
    [System.Windows.Automation.ControlType]::TreeItem
} else {
    [System.Windows.Automation.ControlType]::ListItem
}

$containers = $viewport.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $containerType))) |
    Where-Object { $_.Current.AutomationId -like 'FluentShell.Node.*' }
# A projected ListBox and a projected report ListView are both UIA Lists, so the
# one that carries renamable labels is the one this drives.
$container = $containers |
    Where-Object {
        $null -ne $_.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
            (New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'ProjectedItemLabel')))
    } |
    Select-Object -First 1
if ($null -eq $container) { $container = $containers | Select-Object -First 1 }
if ($null -eq $container) { "no projected $Target"; exit 1 }

$items = $container.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $itemType)))
"items before: " + (($items | ForEach-Object { $_.Current.Name }) -join ' | ')

# The projected item label carries the Value pattern whenever the native control
# admits renaming, which is the same path assistive technology uses natively.
$labels = $container.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'ProjectedItemLabel')))
"renamable labels: " + (($labels | ForEach-Object { $_.Current.Name }) -join ' | ')
$targetLabel = $labels | Where-Object { $_.Current.Name -eq $ItemName } | Select-Object -First 1
if ($null -eq $targetLabel) { $targetLabel = $labels | Select-Object -First 1 }
if ($null -eq $targetLabel) { 'no renamable projected label'; exit 1 }
"renaming '{0}' to '{1}'" -f $targetLabel.Current.Name, $NewText
$value = $targetLabel.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
if ($null -eq $value) { 'projected label exposes no Value pattern'; exit 1 }
"read-only: {0}" -f $value.Current.IsReadOnly
$value.SetValue($NewText)
Start-Sleep -Milliseconds 2000

$items = $container.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $itemType)))
"items after:  " + (($items | ForEach-Object { $_.Current.Name }) -join ' | ')
