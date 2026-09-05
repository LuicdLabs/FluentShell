# Repeats the full inject -> rename -> veto cycle so an intermittent protocol
# fault around the label session has to show itself, and prints the Bridge and
# renderer evidence for any iteration that faults.
param(
    [int] $Iterations = 6
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'build\bin\x64\Release'
$target = (Resolve-Path (Join-Path $release 'LegacyDialogHost.exe')).Path
$injector = (Resolve-Path (Join-Path $release 'FluentShell.Injector.exe')).Path
$renameTool = Join-Path $PSScriptRoot 'Rename-ProjectedItem.ps1'
$bridgeLog = Join-Path $env:TEMP 'FluentShell.log'
$rendererLog = Join-Path $env:TEMP 'FluentShell.Renderer.log'
$faults = 0

for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
    Get-Process LegacyDialogHost, FluentShell.Renderer -ErrorAction SilentlyContinue |
        Stop-Process -Force
    Start-Sleep -Milliseconds 700
    Remove-Item $bridgeLog, $rendererLog -ErrorAction SilentlyContinue
    $process = Start-Process $target -PassThru
    Start-Sleep -Seconds 2
    & $injector inject $target --pid $process.Id | Out-Null
    Start-Sleep -Seconds 7

    $names = @(
        @{ Target = 'tree'; From = 'Event Viewer';  To = 'Renamed node' },
        @{ Target = 'tree'; From = 'Renamed node';  To = '!refused' },
        @{ Target = 'list'; From = 'Recovery';      To = 'Recovery volume' },
        @{ Target = 'list'; From = 'Removable';     To = '!nope' }
    )
    foreach ($rename in $names) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $renameTool `
            -Target $rename.Target -ItemName $rename.From -NewText $rename.To |
            Out-Null
        Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 2

    $bridge = Get-Content $bridgeLog -ErrorAction SilentlyContinue
    $renderer = Get-Content $rendererLog -ErrorAction SilentlyContinue
    $bad = @($bridge | Select-String -Pattern 'Restoring|fault|rejected surface')
    $bad += @($renderer | Select-String -Pattern 'refused|fault')
    if ($bad.Count -gt 0) {
        ++$faults
        "--- iteration $iteration faulted ---"
        $bad | ForEach-Object { "  $($_.Line)" }
    }
    else {
        "iteration $iteration clean"
    }
}

Get-Process LegacyDialogHost, FluentShell.Renderer -ErrorAction SilentlyContinue |
    Stop-Process -Force
"faulted iterations: $faults of $Iterations"
