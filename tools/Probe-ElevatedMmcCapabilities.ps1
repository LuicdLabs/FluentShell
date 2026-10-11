# Read-only discovery. Run this script from an already elevated PowerShell.
# It does not request elevation, inject code, invoke controls, or change settings.
param(
    [ValidateSet('diskmgmt', 'perfmon')][string[]] $Consoles = @('diskmgmt', 'perfmon'),
    [string] $OutputDirectory = '',
    [ValidateRange(5, 60)][int] $ProbeTimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'
$probeIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$probePrincipal = [Security.Principal.WindowsPrincipal]::new($probeIdentity)
if (-not $probePrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This read-only probe requires an already elevated PowerShell. It never bypasses or requests UAC.'
}
$probeRepository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $probeRepository ('build/elevated-capabilities-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
}
$probeOutput = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($probeOutput)
$probeMmcPath = (Get-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/mmc.exe')).FullName
$probePowerShell = (Get-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe')).FullName

function Stop-OwnedProcess($process, [DateTime] $started, [string] $path) {
    $current = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
    if ($null -eq $current) { return }
    if ($current.StartTime.ToUniversalTime() -ne $started -or $current.Path -ine $path) {
        throw "Refusing cleanup: process $($process.Id) no longer has the recorded identity."
    }
    [void]$current.CloseMainWindow()
    if (-not $current.WaitForExit(3000)) { Stop-Process -Id $current.Id -ErrorAction Stop }
}

function Invoke-BoundedProbe([string] $name, [string] $script, [string[]] $arguments) {
    $stdout = Join-Path $probeOutput ($name + '.log')
    $stderr = Join-Path $probeOutput ($name + '.error.log')
    $quotedScript = '"' + (Join-Path $PSScriptRoot $script) + '"'
    $worker = Start-Process -FilePath $probePowerShell -ArgumentList (@('-NoProfile', '-File', $quotedScript) + $arguments) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $workerStarted = $worker.StartTime.ToUniversalTime()
    try {
        if (-not $worker.WaitForExit($ProbeTimeoutSeconds * 1000)) {
            Add-Content -LiteralPath $stderr -Value "Probe timed out after $ProbeTimeoutSeconds seconds."
            return
        }
        Add-Content -LiteralPath $stdout -Value ('ProbeExitCode=' + $worker.ExitCode)
    }
    finally { Stop-OwnedProcess $worker $workerStarted $probePowerShell }
}

foreach ($console in $Consoles) {
    $msc = (Get-Item -LiteralPath (Join-Path $env:SystemRoot ('System32/' + $console + '.msc'))).FullName
    $start = [Diagnostics.ProcessStartInfo]::new($probeMmcPath, ('"' + $msc + '"'))
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $probeRepository
    $target = [Diagnostics.Process]::Start($start)
    $targetStarted = $target.StartTime.ToUniversalTime()
    try {
        [void]$target.WaitForInputIdle(10000)
        Start-Sleep -Seconds 3
        $target.Refresh()
        if ($target.HasExited) { throw "$console exited before its native controls became available." }
        $actualPath = $target.Path
        if ($actualPath -ine $probeMmcPath) { throw 'The started process does not match the canonical MMC executable.' }
        [pscustomobject]@{
            Console = $console; ProcessId = $target.Id; ImagePath = $actualPath
            StartTimeUtc = $targetStarted.ToString('o'); Elevated = $true; ReadOnly = $true
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $probeOutput ($console + '-identity.json')) -Encoding UTF8
        $pidArguments = @('-ProcessName', 'mmc', '-ProcessId', [string]$target.Id)
        Invoke-BoundedProbe ($console + '-windows') 'Dump-WindowTree.ps1' $pidArguments
        if ($console -eq 'diskmgmt') {
            Invoke-BoundedProbe ($console + '-msaa') 'Dump-NativeMsaa.ps1' ($pidArguments +
                @('-ClassName', 'AfxWnd42u', '-AllMatches', '-MaxNodes', '256', '-MaxDepth', '12'))
        }
        else {
            Invoke-BoundedProbe ($console + '-msaa-html') 'Dump-NativeMsaa.ps1' ($pidArguments +
                @('-ClassName', 'Internet Explorer_Server', '-AllMatches', '-MaxNodes', '256', '-MaxDepth', '12'))
            Invoke-BoundedProbe ($console + '-dom') 'Dump-NativeHtml.ps1' @('-ProcessId', [string]$target.Id)
        }
        Invoke-BoundedProbe ($console + '-uia') 'Dump-NativeUia.ps1' ($pidArguments + @('-MaxDepth', '12', '-MaxNodes', '512'))
    }
    finally { Stop-OwnedProcess $target $targetStarted $probeMmcPath }
}
Write-Output ('Read-only capability evidence: ' + $probeOutput)
