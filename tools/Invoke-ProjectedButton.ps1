# Clicks a projected button by name and reports what the projection shows
# afterwards, so a translated MessageBox or TaskDialog can be driven from outside
# the target process.
param(
    [Parameter(Mandatory = $true)][string] $ButtonName,
    [string] $Title = 'FluentShell Win32 Translation Oracle',
    [int] $SettleMs = 1500
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

function Find-Window([string] $name) {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $name)
    $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
}

$window = Find-Window $Title
if ($null -eq $window) { "no projected window '$Title'"; exit 1 }

$button = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)),
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, $ButtonName)))))
if ($null -eq $button) { "no projected button '$ButtonName'"; exit 1 }

$button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
Start-Sleep -Milliseconds $SettleMs

# Report every top-level window this process now shows, plus its buttons, which is
# how a translated dialog proves it arrived as a real projected surface.
$root = [System.Windows.Automation.AutomationElement]::RootElement
$processId = $window.Current.ProcessId
$root.FindAll([System.Windows.Automation.TreeScope]::Children,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $processId))) |
    ForEach-Object {
        $buttons = $_.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                (New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                    [System.Windows.Automation.ControlType]::Button))) |
            ForEach-Object { $_.Current.Name }
        "window '$($_.Current.Name)' class=$($_.Current.ClassName) buttons=$($buttons -join ', ')"
    }
