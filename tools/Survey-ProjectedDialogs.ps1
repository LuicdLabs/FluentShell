<#
Dialog census: which dialogs a console opens are projected, and which stay native.

Navigation (Crawl-ProjectedMmc.ps1) says whether a console's views project. A dialog is a
separate boundary: an owned dialog the Bridge cannot project hands the whole owner graph back
to native. For each flow of dialog-survey.json this starts a disposable console through
Drive-ProjectedMmc.ps1, runs the flow's steps (which open one dialog), and records from the
Drive output whether a Renderer window carrying the expected title existed (PROJECTED), only the
native dialog did (NATIVE), or neither did (MISSING). Optional tabs are selected one by one and
checked again, because a tab can bring a control the first page did not have. Each page is then
exercised with real input (Drive's Interact step: type into every editable edit, toggle every check
box, select every radio button); the Interaction column reads controls/unresponsive. The dialog
is cancelled or closed; nothing is changed. The pointer really moves: do not use the machine.

Flow fields: id, console, open (Drive steps that open the dialog), expect (a title fragment),
optional tabs (names selected through UIA after the dialog opens), optional note.
Output: <OutputDirectory>\survey.md, one row per flow (and per tab), and a window screenshot
of every dialog.
#>
[CmdletBinding()]
param(
    # Defaults to dialog-survey.json beside this script ($PSScriptRoot is empty inside a param default
    # of Windows PowerShell 5.1 under -File, so the default is applied below).
    [string]$Flows = '',
    # Only flows whose id contains this text.
    [string]$Filter = '',
    [string]$OutputDirectory = '',
    [ValidateRange(4, 60)][int]$DialogWaitSeconds = 12
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }
$repository = Split-Path -Parent $PSScriptRoot
if (-not $Flows) { $Flows = Join-Path $PSScriptRoot 'dialog-survey.json' }
$drive = Join-Path $PSScriptRoot 'Drive-ProjectedMmc.ps1'
$bridgeLog = Join-Path $env:TEMP 'FluentShell.log'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repository ('build\dialog-survey-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
# Windows PowerShell 5.1 emits a JSON array as ONE object: wrapping the pipeline in @() would nest it.
$parsed = Get-Content -LiteralPath $Flows -Raw -Encoding UTF8 | ConvertFrom-Json
# (Variable names are case-insensitive: $Flows is the typed [string] parameter, so the list has its own name.)
$flowList = if ($parsed -is [System.Array]) { $parsed } else { @($parsed) }
if ($Filter) { $flowList = @($flowList | Where-Object { $_.id -like "*$Filter*" }) }

# The windows a "Dialogs" step listed, in order: each entry is the list of renderer-window titles
# and the list of console-window titles at that moment.
function Read-DialogListings([string[]]$Lines) {
    $listings = @()
    $current = $null
    $mode = ''
    foreach ($line in $Lines) {
        if ($line -match '^\[Dialogs\] console windows:') { $current = @{ Console = @(); Renderer = @() }; $listings += , $current; $mode = 'console'; continue }
        if ($line -match '^\[Dialogs\] renderer windows:') { $mode = 'renderer'; continue }
        if ($line -match '^\[Dialogs\] projection is gone') { $listings += , @{ Console = @(); Renderer = @(); Gone = $true }; $mode = ''; continue }
        if ($current -and $mode -and $line -match "^\s+\S+ '(.*)' \d+x\d+") {
            if ($mode -eq 'console') { $current.Console += $Matches[1] } else { $current.Renderer += $Matches[1] }
            continue
        }
        if ($line -match '^\[') { $mode = '' }
    }
    return $listings
}

function Get-Outcome($Listing, [string]$Expect) {
    if ($Listing.Gone) { return 'ROLLED-BACK' }
    # "=Title" matches a window title exactly: a dialog named "Certificate" would otherwise match
    # the console window "certmgr - [Certificates - ...]".
    $matches = if ($Expect.StartsWith('=')) { { param($title) $title -eq $Expect.Substring(1) } } else { { param($title) $title -like "*$Expect*" } }
    $projected = @($Listing.Renderer | Where-Object { & $matches $_ }).Count -gt 0
    $native = @($Listing.Console | Where-Object { & $matches $_ }).Count -gt 0
    if ($projected) { return 'PROJECTED' }
    if ($native) { return 'NATIVE' }
    return 'MISSING'
}

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('| Flow | Console | Dialog | Page | Outcome | Interaction | Escape | Evidence |')
$rows.Add('| --- | --- | --- | --- | --- | --- | --- | --- |')
foreach ($flow in $flowList) {
    $shots = Join-Path $OutputDirectory $flow.id
    $steps = "$($flow.open);Wait:$($DialogWaitSeconds * 1000);Dialogs;ScreenshotWindows:$shots;Interact"
    foreach ($tab in @($flow.tabs)) { if ($tab) { $steps += ";Select:$tab;Wait:3500;Dialogs;ScreenshotWindows:$shots-$tab;Interact" } }
    # Escape is how a native dialog is cancelled from the keyboard: after the last page the dialog is asked to
    # close that way, and whatever it leaves behind is cancelled through its buttons.
    $steps += ';Key:Escape;Wait:2500;Dialogs;CloseDialogs;Wait:2500;Dialogs'
    $mark = (Get-Item -LiteralPath $bridgeLog -ErrorAction SilentlyContinue).Length
    if (-not $mark) { $mark = 0 }
    $started = Get-Date
    "[$($flow.id)] $($flow.console)"
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $drive -Console $flow.console -Steps $steps 2>&1 | ForEach-Object { "$_" })
    $output | Set-Content -LiteralPath (Join-Path $OutputDirectory "$($flow.id).txt") -Encoding UTF8
    $listings = @(Read-DialogListings $output)
    # Listings: 0 = the dialog once opened, 1.. = after each tab, last = after closing.
    $pages = @('(first page)') + @($flow.tabs | Where-Object { $_ })
    $evidence = ''
    # The Bridge's last refusal during this flow explains a NATIVE outcome. Only what the log gained since the
    # flow started counts: an older refusal from another run says nothing about this one.
    if (Test-Path -LiteralPath $bridgeLog) {
        $stream = [System.IO.File]::Open($bridgeLog, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            if ($mark -lt $stream.Length) { [void]$stream.Seek($mark, [System.IO.SeekOrigin]::Begin) }
            $fresh = ([System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)).ReadToEnd() -split "`r?`n"
        } finally { $stream.Dispose() }
        $reasons = @($fresh |
            Where-Object { $_ -match 'Capture rejected child|Owned modal dialog could not|Projection gate rejected|Committed UIA gate attempt \d+ failed' })
        if ($reasons.Count) { $evidence = ($reasons[-1] -replace '^.*?\[FluentShell\] ', '') }
        if ($evidence.Length -gt 160) { $evidence = $evidence.Substring(0, 160) }
        $evidence = $evidence -replace '\|', '/'
    }
    # One Interact result per page, in order: "controls/unresponsive", or "-" when no dialog answered.
    $interactions = @($output | Where-Object { $_ -match '^\[Interact\] (no dialog|.*: \d+ interactive control)' } | ForEach-Object {
        if ($_ -match ': (\d+) interactive control\(s\), (\d+) unresponsive') { "$($Matches[1])/$($Matches[2])" } else { '-' } })
    $unresponsive = @($output | Where-Object { $_ -match '^\[Interact\] (edit|check|radio) .*did not' } | ForEach-Object { ($_ -replace '^\[Interact\] ', '') -replace '\|', '/' })
    # Listings: one per page, then the one after Escape, then the final one after cancelling.
    $escape = '-'
    if ($listings.Count -ge $pages.Count + 2 -and -not $listings[$pages.Count].Gone) {
        $escape = if ((Get-Outcome $listings[$pages.Count] $flow.expect) -eq 'MISSING') { 'closes' } else { 'stays open' }
    }
    for ($index = 0; $index -lt $pages.Count; ++$index) {
        $outcome = if ($index -lt $listings.Count - 2) { Get-Outcome $listings[$index] $flow.expect } else { 'MISSING' }
        $interaction = if ($index -lt $interactions.Count) { $interactions[$index] } else { '-' }
        $detail = if ($outcome -eq 'PROJECTED') { '' } else { $evidence }
        if ($outcome -eq 'PROJECTED' -and $interaction -match '/[1-9]') { $detail = ($unresponsive -join '; ') }
        if ($detail.Length -gt 200) { $detail = $detail.Substring(0, 200) }
        $rows.Add("| $($flow.id) | $($flow.console) | $($flow.expect) | $($pages[$index]) | $outcome | $interaction | $(if ($index -eq 0) { $escape } else { '' }) | $detail |")
        "    $($pages[$index]): $outcome (interaction $interaction$(if ($index -eq 0) { ", escape $escape" }))"
    }
    # Whatever a flow left running must not leak into the next one.
    Get-Process -Name mmc, FluentShell.Renderer -ErrorAction SilentlyContinue |
        Where-Object { $_.StartTime -ge $started.AddSeconds(-5) } | Stop-Process -Force -ErrorAction SilentlyContinue
}
$report = Join-Path $OutputDirectory 'survey.md'
$rows | Set-Content -LiteralPath $report -Encoding UTF8
''
$rows
"report: $report"
