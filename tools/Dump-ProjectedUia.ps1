# Dumps the projected proxy's UIA elements (AutomationId + BoundingRectangle) while a
# surface is committed.  Diagnostic only: it reads the renderer's UIA tree and never
# drives it.  Run it concurrently with an injection; the committed gate keeps the proxy
# visible for a few seconds.
param(
    [int] $TimeoutSeconds = 20
)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$root = [System.Windows.Automation.AutomationElement]::RootElement
$viewportCondition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'FluentShell.ContentViewport')

while ([DateTime]::UtcNow -lt $deadline) {
    $viewport = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $viewportCondition)
    if ($null -ne $viewport) {
        $rect = $viewport.Current.BoundingRectangle
        "VIEWPORT bounds={0},{1} {2}x{3}" -f $rect.X, $rect.Y, $rect.Width, $rect.Height
        $window = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($viewport)
        while ($null -ne $window -and $window.Current.ControlType.ProgrammaticName -ne 'ControlType.Window') {
            $window = [System.Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($window)
        }
        if ($null -ne $window) {
            $windowRect = $window.Current.BoundingRectangle
            "WINDOW   bounds={0},{1} {2}x{3} name='{4}'" -f `
                $windowRect.X, $windowRect.Y, $windowRect.Width, $windowRect.Height, $window.Current.Name
        }
        $nodes = $viewport.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        foreach ($node in $nodes) {
            $id = $node.Current.AutomationId
            if (-not $id.StartsWith('FluentShell.Node.')) { continue }
            $nodeRect = $node.Current.BoundingRectangle
            "NODE {0,-28} type={1,-24} bounds={2},{3} {4}x{5} name='{6}'" -f `
                $id, $node.Current.ControlType.ProgrammaticName, $nodeRect.X, $nodeRect.Y,
                $nodeRect.Width, $nodeRect.Height, $node.Current.Name
        }
        return
    }
    Start-Sleep -Milliseconds 150
}
'no projected viewport observed'
