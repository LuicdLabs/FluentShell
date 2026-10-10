<#
Visual survey of the projection: one window screenshot per scope-tree node of each console.

The structural gates say a view projected; they cannot say it looks right (a stray splitter bar and
background patches in a dialog were found by eye). For each console this reads the node names of a
navigation census (Crawl-ProjectedMmc.ps1 output), then starts a disposable console through
Drive-ProjectedMmc.ps1, selects the nodes breadth-first (expanding their ancestors) and saves a
screenshot of the projected window after each. Nothing is changed. The pointer really moves: do not
use the machine while it runs.

Output: <OutputDirectory>\<console>-<n>-<node>.png and <console>.txt (the Drive log).
#>
[CmdletBinding()]
param(
    # Consoles to shoot, comma separated ('all' is every console in the census).
    [string]$Consoles = 'all',
    # A census directory holding <console>-crawl.jsonl files (the latest build\mmc-crawl-census-* when empty).
    [string]$Census = '',
    [ValidateRange(1, 6)][int]$MaxDepth = 2,
    [ValidateRange(1, 40)][int]$PerConsole = 6,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }
$repository = Split-Path -Parent $PSScriptRoot
$drive = Join-Path $PSScriptRoot 'Drive-ProjectedMmc.ps1'
if (-not $Census) {
    $Census = (Get-ChildItem -LiteralPath (Join-Path $repository 'build') -Directory -Filter 'mmc-crawl-census-*' |
        Sort-Object Name | Select-Object -Last 1).FullName
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repository ('build\shots-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$files = @(Get-ChildItem -LiteralPath $Census -Filter '*-crawl.jsonl')
if ($Consoles -ne 'all') {
    $wanted = $Consoles -split ','
    $files = @($files | Where-Object { $wanted -contains $_.Name.Replace('-crawl.jsonl', '') })
}
foreach ($file in $files) {
    $console = $file.Name.Replace('-crawl.jsonl', '')
    $paths = @([System.IO.File]::ReadAllLines($file.FullName, [System.Text.Encoding]::UTF8) |
        Where-Object { $_.Trim().Length -gt 0 } |
        ForEach-Object { ($_.TrimStart([char]0xFEFF) | ConvertFrom-Json).path } |
        Where-Object { $_ -ne '(startup)' })
    # Breadth first, shallow nodes first, as the census recorded them.
    $nodes = @($paths | Where-Object { @($_ -split ' / ').Count -le ($MaxDepth + 1) } | Select-Object -First $PerConsole)
    $steps = New-Object System.Collections.Generic.List[string]
    $steps.Add('Wait:3000'); $steps.Add('CloseDialogs'); $steps.Add('Wait:1500'); $steps.Add('CloseDialogs')
    $steps.Add("Screenshot:$OutputDirectory\$console-0-start.png")
    $expanded = New-Object System.Collections.Generic.HashSet[string]
    $number = 0
    foreach ($path in $nodes) {
        ++$number
        $segments = @($path -split ' / ')
        for ($depth = 0; $depth -lt $segments.Count - 1; ++$depth) {
            $ancestor = ($segments[0..$depth] -join ' / ')
            if ($expanded.Add($ancestor)) { $steps.Add("Expand:$($segments[$depth])"); $steps.Add('Wait:1200') }
        }
        $safe = ($segments[-1] -replace '[^\w]', '_')
        if ($safe.Length -gt 24) { $safe = $safe.Substring(0, 24) }
        $steps.Add("TreeSelect:$($segments[-1])"); $steps.Add('Wait:5000')
        $steps.Add("Screenshot:$OutputDirectory\$console-$number-$safe.png")
    }
    "[$console] $($nodes.Count) node(s)"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $drive -Console $console -Steps ($steps -join ';') 2>&1 |
        ForEach-Object { "$_" } | Set-Content -LiteralPath (Join-Path $OutputDirectory "$console.txt") -Encoding UTF8
}
"screenshots: $OutputDirectory"
