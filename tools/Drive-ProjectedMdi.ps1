# Drives a projected MDI surface through UI Automation: clicks a child's own
# button, then exercises the projected caption commands.  Everything is observed
# from outside both processes, so what the native oracle logs is the proof.
param(
    [string] $ChildTitle = 'Document 1',
    [string] $CaptionCommand = 'Maximize'
)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$viewport = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'FluentShell.ContentViewport')))
if ($null -eq $viewport) { 'no projected viewport'; exit 1 }

$windows = $viewport.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Window)))
"projected MDI children: " + (($windows | ForEach-Object { $_.Current.Name }) -join ' | ')

$child = $windows | Where-Object { $_.Current.Name -eq $ChildTitle } | Select-Object -First 1
if ($null -eq $child) { "no projected MDI child named '$ChildTitle'"; exit 1 }

$buttons = $child.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)))
"buttons inside '$ChildTitle': " + (($buttons | ForEach-Object { $_.Current.Name }) -join ' | ')

$ping = $buttons | Where-Object { $_.Current.Name -eq 'Ping this child' } | Select-Object -First 1
if ($null -ne $ping) {
    $ping.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Milliseconds 1500
    $texts = $child.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Text)))
    "child text after invoke: " + (($texts | ForEach-Object { $_.Current.Name }) -join ' | ')
}

$caption = $buttons | Where-Object { $_.Current.Name -eq $CaptionCommand } | Select-Object -First 1
if ($null -ne $caption) {
    $caption.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Milliseconds 2000
    "invoked caption command '$CaptionCommand'"
} else {
    "caption command '$CaptionCommand' is not offered"
}

$windows = $viewport.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Window)))
foreach ($window in $windows) {
    $rect = $window.Current.BoundingRectangle
    "after: '{0}' bounds={1},{2} {3}x{4}" -f $window.Current.Name, $rect.X, $rect.Y, $rect.Width, $rect.Height
}
