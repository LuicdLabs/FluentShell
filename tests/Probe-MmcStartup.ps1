<#
Starts disposable MMC instances and exercises the production Injector CLI.
This records startup admission only; it does not validate navigation or dialogs.
It never sends pointer/keyboard input or changes snap-in configuration.
Use -Repetitions to retain separate logs for each startup/shutdown cycle.
Renderer children that survive the shutdown deadline are reported, not killed.
StableStartupAndCleanup is true only when the bounded observations and cleanup
checks pass; null means the probe could not establish the required evidence.
#>
[CmdletBinding()]
param(
    [string[]]$Consoles = @('gpedit', 'secpol', 'compmgmt'),
    [ValidateRange(0, 30)][int]$StartupSeconds = 3,
    [ValidateRange(3, 60)][int]$ObserveSeconds = 8,
    [ValidateRange(1, 20)][int]$Repetitions = 1,
    [switch]$AccessibilityDiagnostics,
    # Keep both MMC and the Injector at the caller's integrity level. Some MMC
    # manifests otherwise request elevation before a read-only startup probe.
    [switch]$RunAsInvoker,
    # Empty MMC honors SW_HIDE, so explicitly show the interactive test window
    # when validating its visible projection. It is closed by this probe.
    [switch]$ShowConsole
)

$ErrorActionPreference = 'Stop'
$mmcRepository = Split-Path -Parent $PSScriptRoot
$mmcInjector = Join-Path $mmcRepository 'build\bin\x64\Release\FluentShell.Injector.exe'
$mmcRenderer = [System.IO.Path]::GetFullPath((Join-Path $mmcRepository 'build\bin\x64\Release\Renderer\FluentShell.Renderer.exe'))
$mmcSystem = [Environment]::SystemDirectory
$mmcExecutable = Join-Path $mmcSystem 'mmc.exe'
$mmcLog = Join-Path $env:TEMP 'FluentShell.log'
$mmcReportDirectory = Join-Path $mmcRepository ('build\mmc-startup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (-not (Test-Path -LiteralPath $mmcInjector)) { throw "Build Release Injector first: $mmcInjector" }
if ($Consoles -contains 'all') {
    $Consoles = @('empty') + @(Get-ChildItem -LiteralPath $mmcSystem -Filter '*.msc' -File | Sort-Object Name | ForEach-Object BaseName)
}
foreach ($mmcName in $Consoles) {
    if ($mmcName -notmatch '^[a-zA-Z0-9_-]+$') { throw "Invalid console basename: $mmcName" }
    if ($mmcName -ne 'empty' -and -not (Test-Path -LiteralPath (Join-Path $mmcSystem "$mmcName.msc"))) {
        throw "Console is not installed: $mmcName"
    }
}
New-Item -ItemType Directory -Path $mmcReportDirectory | Out-Null

function Read-BridgeLog {
    if (-not (Test-Path -LiteralPath $mmcLog)) { return '' }
    $mmcStream = [System.IO.File]::Open($mmcLog, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $mmcReader = [System.IO.StreamReader]::new($mmcStream)
    try { return $mmcReader.ReadToEnd() } finally { $mmcReader.Dispose() }
}

# Capture only this MMC's child Renderer, and keep its process handle open so
# PID reuse cannot make the exit check observe an unrelated process.
function Add-OwnedRendererChildren {
    param([int]$ParentId, [datetime]$ParentStartedUtc, [datetime]$LatestStartedUtc,
        [System.Collections.IDictionary]$Children)
    $mmcChildren = Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId=$ParentId" `
        -Property ProcessId, ExecutablePath, CreationDate -OperationTimeoutSec 2
    foreach ($mmcChild in $mmcChildren) {
        if (-not $mmcChild.ExecutablePath -or -not $mmcChild.CreationDate -or
            -not [string]::Equals($mmcChild.ExecutablePath, $mmcRenderer, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $mmcChildCreated = ([datetime]$mmcChild.CreationDate).ToUniversalTime()
        if ($mmcChildCreated -lt $ParentStartedUtc -or $mmcChildCreated -gt $LatestStartedUtc) { continue }
        $mmcChildKey = '{0}/{1}' -f $mmcChild.ProcessId, $mmcChildCreated.Ticks
        if ($Children.Contains($mmcChildKey)) { continue }
        $mmcChildRecord = [pscustomobject]@{
            ProcessId = [int]$mmcChild.ProcessId; CreationTimeUtc = $mmcChildCreated.ToString('o')
            ExecutablePath = $mmcChild.ExecutablePath; ExitState = 'pending'; Error = $null
            WasAliveBeforeTargetCleanup = $null; ExitCode = $null; ExitTimeUtc = $null
            Process = $null
        }
        $Children.Add($mmcChildKey, $mmcChildRecord)
        $mmcChildProcess = $null
        try {
            $mmcChildProcess = [System.Diagnostics.Process]::GetProcessById($mmcChildRecord.ProcessId)
            # CIM timestamps have microsecond precision; Process.StartTime can
            # include sub-microsecond ticks. Retain the latter for diagnostics.
            $mmcChildStart = $mmcChildProcess.StartTime.ToUniversalTime()
            if ([Math]::Abs(($mmcChildStart - $mmcChildCreated).Ticks) -ge 10 -or
                -not [string]::Equals($mmcChildProcess.MainModule.FileName, $mmcRenderer, [StringComparison]::OrdinalIgnoreCase)) {
                $mmcChildRecord.ExitState = 'observation-error'
                $mmcChildRecord.Error = 'Renderer process identity changed between discovery and observation.'
                continue
            }
            $null = $mmcChildProcess.Handle
            $mmcChildRecord.CreationTimeUtc = $mmcChildStart.ToString('o')
            $mmcChildRecord.Process = $mmcChildProcess
            $mmcChildProcess = $null
        } catch [System.ArgumentException] {
            $mmcChildRecord.ExitState = 'exited-before-observation'
        } catch {
            $mmcChildError = $_.Exception.Message
            $mmcChildExited = $false
            if ($mmcChildProcess) {
                try { $mmcChildExited = $mmcChildProcess.HasExited } catch { }
            }
            if ($mmcChildExited) {
                $mmcChildRecord.ExitState = 'exited-before-observation'
            } else {
                $mmcChildRecord.ExitState = 'observation-error'
                $mmcChildRecord.Error = $mmcChildError
            }
        } finally {
            if ($mmcChildProcess) { $mmcChildProcess.Dispose() }
        }
    }
}

$mmcOldDiagnostics = $env:FLUENTSHELL_DIAG_ACCESSIBILITY
$env:FLUENTSHELL_DIAG_ACCESSIBILITY = if ($AccessibilityDiagnostics) { '1' } else { '0' }
$mmcResults = @()
try {
    for ($mmcIteration = 1; $mmcIteration -le $Repetitions; $mmcIteration++) {
        foreach ($mmcName in $Consoles) {
            $mmcLogName = if ($Repetitions -eq 1) { $mmcName } else { '{0}-run{1:D2}' -f $mmcName, $mmcIteration }
            $mmcBefore = (Read-BridgeLog).Length
            $mmcLaunch = @{ FilePath = $mmcExecutable; WindowStyle = $(if ($ShowConsole) { 'Normal' } else { 'Hidden' }); PassThru = $true }
            if ($mmcName -ne 'empty') { $mmcLaunch.ArgumentList = (Join-Path $mmcSystem "$mmcName.msc") }
            $mmcStartedUtc = [datetime]::UtcNow
            $mmcPreviousCompatibility = $env:__COMPAT_LAYER
            try {
                if ($RunAsInvoker) { $env:__COMPAT_LAYER = 'RunAsInvoker' }
                $mmcProcess = Start-Process @mmcLaunch
            } catch {
                # Launch happens before a Process handle exists. Retain a row for
                # this console instead of losing the entire batch to a UAC cancel
                # or an unavailable snap-in host. No injection was attempted.
                $mmcLaunchError = $_.Exception.Message
                Set-Content -LiteralPath (Join-Path $mmcReportDirectory "$mmcLogName-launch.log") `
                    -Value $mmcLaunchError -Encoding utf8
                $mmcResults += [pscustomobject]@{
                    Console = $mmcName; Iteration = $mmcIteration; ProcessId = $null; InjectorExit = $null
                    Outcome = 'launch-failed'; ProjectedTitles = @(); Error = $mmcLaunchError
                    OverallOutcome = 'launch-failed'; StableStartupAndCleanup = $null
                    TargetAliveAtObservationEnd = $null; TargetUnexpectedExitCode = $null
                    RendererExitStatus = 'not-launched'; RendererExitWithinTimeout = $null
                    RendererProcesses = @(); RendererObservationErrors = @()
                    RunAsInvoker = [bool]$RunAsInvoker
                }
                $mmcResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $mmcReportDirectory 'results.json') -Encoding utf8
                Write-Output "$mmcLogName`: launch-failed ($mmcLaunchError)"
                continue
            } finally { $env:__COMPAT_LAYER = $mmcPreviousCompatibility }
            $mmcPid = $mmcProcess.Id
            $mmcExitCode = -1
            $mmcInjectorAttempted = $false
            $mmcError = $null
            $mmcExited = $false
            $mmcTargetAliveAtObservationEnd = $null
            $mmcTargetUnexpectedExitCode = $null
            $mmcTargetCleanupRequestedUtc = $null
            $mmcRenderers = [ordered]@{}
            $mmcRendererErrors = @()
            $mmcRendererPrematureExit = $false
            $mmcRendererExitStatus = 'not-observed'
            $mmcRendererExitWithinTimeout = $null
            try {
                $mmcStartedUtc = $mmcProcess.StartTime.ToUniversalTime()
                $mmcProcess.WaitForInputIdle(5000) | Out-Null
                Start-Sleep -Seconds $StartupSeconds
                if ($mmcProcess.HasExited) { throw 'MMC exited before injection.' }
                $mmcInjectorAttempted = $true
                & $mmcInjector inject $mmcExecutable --pid $mmcPid 2>&1 |
                    Set-Content -LiteralPath (Join-Path $mmcReportDirectory "$mmcLogName-injector.log") -Encoding utf8
                $mmcExitCode = $LASTEXITCODE
                Start-Sleep -Seconds $ObserveSeconds
            } catch { $mmcError = $_.Exception.Message }
            finally {
                try {
                    Add-OwnedRendererChildren -ParentId $mmcPid -ParentStartedUtc $mmcStartedUtc `
                        -LatestStartedUtc ([datetime]::UtcNow) -Children $mmcRenderers
                } catch { $mmcRendererErrors += $_.Exception.Message }
                foreach ($mmcChildRecord in $mmcRenderers.Values) {
                    try {
                        if ($mmcChildRecord.Process) {
                            $mmcChildRecord.WasAliveBeforeTargetCleanup = -not $mmcChildRecord.Process.HasExited
                        } elseif ($mmcChildRecord.ExitState -eq 'exited-before-observation') {
                            $mmcChildRecord.WasAliveBeforeTargetCleanup = $false
                        }
                        if ($mmcChildRecord.WasAliveBeforeTargetCleanup -eq $false) {
                            $mmcRendererPrematureExit = $true
                        }
                    } catch {
                        $mmcChildRecord.ExitState = 'observation-error'
                        $mmcChildRecord.Error = $_.Exception.Message
                    }
                }
                try {
                    $mmcTargetAliveAtObservationEnd = -not $mmcProcess.HasExited
                    if ($mmcTargetAliveAtObservationEnd) {
                        # Stop the saved Process object, rather than resolving
                        # its numeric PID again during cleanup.
                        $mmcTargetCleanupRequestedUtc = [datetime]::UtcNow
                        $mmcProcess.Kill()
                    } else {
                        $mmcTargetUnexpectedExitCode = $mmcProcess.ExitCode
                    }
                    $mmcExited = $mmcProcess.WaitForExit(3000)
                    $mmcStoppedUtc = [datetime]::UtcNow
                    if ($mmcExited) {
                        $mmcStoppedUtc = $mmcProcess.ExitTime.ToUniversalTime()
                        if ($null -ne $mmcTargetCleanupRequestedUtc -and $mmcStoppedUtc -lt $mmcTargetCleanupRequestedUtc) {
                            $mmcTargetAliveAtObservationEnd = $false
                            $mmcTargetUnexpectedExitCode = $mmcProcess.ExitCode
                        }
                    }
                    else { $mmcError = 'MMC did not exit within three seconds during cleanup.' }
                } catch { $mmcError = $_.Exception.Message }
                finally { $mmcProcess.Dispose() }
                # One shared five-second deadline covers all Renderer children.
                # A surviving child is reported and preserved for diagnosis.
                $mmcRendererWait = [System.Diagnostics.Stopwatch]::StartNew()
                try {
                    if ($mmcExited) {
                        try {
                            Add-OwnedRendererChildren -ParentId $mmcPid -ParentStartedUtc $mmcStartedUtc `
                                -LatestStartedUtc $mmcStoppedUtc -Children $mmcRenderers
                        } catch { $mmcRendererErrors += $_.Exception.Message }
                        foreach ($mmcChildRecord in $mmcRenderers.Values) {
                            if (-not $mmcChildRecord.Process) { continue }
                            $mmcRemaining = [Math]::Max(0, 5000 - [int]$mmcRendererWait.ElapsedMilliseconds)
                            try {
                                $mmcChildRecord.ExitState = if ($mmcChildRecord.Process.WaitForExit($mmcRemaining)) { 'exited' } else { 'timeout' }
                                if ($mmcChildRecord.ExitState -eq 'exited') {
                                    $mmcChildRecord.ExitCode = $mmcChildRecord.Process.ExitCode
                                    $mmcChildExitUtc = $mmcChildRecord.Process.ExitTime.ToUniversalTime()
                                    $mmcChildRecord.ExitTimeUtc = $mmcChildExitUtc.ToString('o')
                                    if ($null -ne $mmcTargetCleanupRequestedUtc -and $mmcChildExitUtc -lt $mmcTargetCleanupRequestedUtc) {
                                        $mmcRendererPrematureExit = $true
                                    }
                                }
                            } catch {
                                $mmcChildRecord.ExitState = 'observation-error'
                                $mmcChildRecord.Error = $_.Exception.Message
                            }
                        }
                    }
                } catch { $mmcRendererErrors += $_.Exception.Message }
                finally {
                    $mmcRendererWait.Stop()
                    foreach ($mmcChildRecord in $mmcRenderers.Values) {
                        if ($mmcChildRecord.Process) { $mmcChildRecord.Process.Dispose() }
                    }
                }
                # Read after stopping MMC so rollback/fault records emitted
                # immediately before cleanup cannot fall outside the log slice.
                try {
                    $mmcText = Read-BridgeLog
                    $mmcUnfiltered = if ($mmcText.Length -ge $mmcBefore) { $mmcText.Substring($mmcBefore) } else { $mmcText }
                    $mmcPidMarker = '[pid=' + $mmcPid + ']'
                    $mmcSlice = ($mmcUnfiltered -split "`r?`n" | Where-Object { $_.Contains($mmcPidMarker) }) -join "`r`n"
                    if (-not $mmcSlice -and $mmcUnfiltered) {
                        $mmcError = 'No process-tagged log records; rebuild Bridge with process log attribution.'
                    }
                } catch { $mmcError = $_.Exception.Message; $mmcSlice = '' }
                $mmcRendererStates = @($mmcRenderers.Values | ForEach-Object ExitState)
                $mmcRendererExitStatus = if (-not $mmcExited) { 'mmc-exit-timeout' }
                    elseif ($mmcRendererStates -contains 'timeout') { 'timeout' }
                    elseif ($mmcRendererErrors.Count -gt 0 -or $mmcRendererStates -contains 'observation-error' -or $mmcRendererStates -contains 'pending') { 'observation-error' }
                    elseif ($mmcRenderers.Count -eq 0) { 'no-child-observed' }
                    else { 'exited' }
                if ($mmcRendererExitStatus -eq 'exited') { $mmcRendererExitWithinTimeout = $true }
                elseif ($mmcRendererExitStatus -eq 'timeout') { $mmcRendererExitWithinTimeout = $false }
                if ($mmcRendererExitStatus -eq 'timeout') {
                    Write-Warning "$mmcLogName`: owned Renderer did not exit within five seconds; process and evidence preserved."
                }
                Set-Content -LiteralPath (Join-Path $mmcReportDirectory "$mmcLogName-bridge.log") -Value $mmcSlice -Encoding utf8
                $mmcProjected = @([regex]::Matches($mmcSlice, 'Projected native window: ([^\r\n]+)') | ForEach-Object { $_.Groups[1].Value })
                $mmcRestored = $mmcSlice -match 'Restoring native window:|Renderer session failed:'
                $mmcOutcome = if ($mmcTargetAliveAtObservationEnd -eq $false) { 'target-exited-unexpectedly' }
                    elseif ($mmcInjectorAttempted -and $mmcExitCode -ne 0) { 'injector-failed' }
                    elseif ($mmcError) { 'probe-error' }
                    elseif ($mmcProjected.Count -gt 0 -and -not $mmcRestored) { 'startup-projected' }
                    elseif ($mmcRestored) { 'restored-or-renderer-failed' }
                    elseif ($mmcSlice -match 'Initial source capture failed|Native window remains untranslated') { 'capture-rejected' }
                    else { 'no-projection-observed' }
                $mmcRendererLifetimeVerified = $mmcRenderers.Count -gt 0 -and
                    @($mmcRenderers.Values | Where-Object { $_.WasAliveBeforeTargetCleanup -ne $true }).Count -eq 0
                $mmcRendererFailedExit = @($mmcRenderers.Values | Where-Object { $null -ne $_.ExitCode -and $_.ExitCode -ne 0 }).Count -gt 0
                $mmcOverallOutcome = if ($mmcOutcome -eq 'target-exited-unexpectedly' -or $mmcOutcome -eq 'injector-failed') { $mmcOutcome }
                    elseif (-not $mmcExited) { 'target-cleanup-failed' }
                    elseif ($mmcOutcome -ne 'startup-projected') { $mmcOutcome }
                    elseif ($mmcRendererPrematureExit) { 'renderer-exited-before-cleanup' }
                    elseif ($mmcRendererExitStatus -eq 'timeout') { 'renderer-cleanup-timeout' }
                    elseif ($mmcRendererFailedExit) { 'renderer-cleanup-failed' }
                    elseif ($mmcTargetAliveAtObservationEnd -ne $true -or -not $mmcRendererLifetimeVerified -or
                        $mmcRendererExitStatus -ne 'exited') { 'stability-inconclusive' }
                    else { 'stable-startup-and-cleanup' }
                $mmcStableStartupAndCleanup = if ($mmcOverallOutcome -eq 'stable-startup-and-cleanup') { $true }
                    elseif ($mmcOverallOutcome -eq 'stability-inconclusive' -or $mmcOverallOutcome -eq 'probe-error') { $null }
                    else { $false }
                $mmcResults += [pscustomobject]@{
                    Console = $mmcName; Iteration = $mmcIteration; ProcessId = $mmcPid; InjectorExit = $mmcExitCode
                    Outcome = $mmcOutcome; ProjectedTitles = $mmcProjected; Error = $mmcError
                    OverallOutcome = $mmcOverallOutcome; StableStartupAndCleanup = $mmcStableStartupAndCleanup
                    TargetAliveAtObservationEnd = $mmcTargetAliveAtObservationEnd; TargetUnexpectedExitCode = $mmcTargetUnexpectedExitCode
                    RendererExitStatus = $mmcRendererExitStatus; RendererExitWithinTimeout = $mmcRendererExitWithinTimeout
                    RendererProcesses = @($mmcRenderers.Values | Select-Object ProcessId, CreationTimeUtc, ExecutablePath, WasAliveBeforeTargetCleanup, ExitState, ExitCode, ExitTimeUtc, Error)
                    RendererObservationErrors = $mmcRendererErrors
                    RunAsInvoker = [bool]$RunAsInvoker
                }
                $mmcResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $mmcReportDirectory 'results.json') -Encoding utf8
                Write-Output "$mmcLogName`: $mmcOverallOutcome (Startup $mmcOutcome; Injector exit $mmcExitCode; Renderer exit $mmcRendererExitStatus)"
            }
        }
    }
} finally { $env:FLUENTSHELL_DIAG_ACCESSIBILITY = $mmcOldDiagnostics }
Write-Output "Reports: $mmcReportDirectory"
