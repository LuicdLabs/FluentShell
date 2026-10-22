#requires -Version 5
<#
    Reproduces the projected security-warning dialog without Explorer.

    shdocvw shows "Open File - Security Warning" in whatever process calls
    ShellExecute on a file carrying a mark-of-the-web, so a plain powershell.exe
    host shows the same dialog (same template 4416, same control ids) and
    powershell.exe is not on the injection deny list.  The Bridge is injected
    into that host, the host then shows the dialog, and the projected proxy's
    Run button is invoked through UIA -- the same action the user's click emits.

    The point is the Bridge log: which control received BM_CLICK, and whether the
    dialog was ever told about it (BN_CLICKED for that id).
#>
param([string] $Configuration = 'Debug')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\bin\x64\$Configuration"
$injector = Join-Path $out 'FluentShell.Injector.exe'
if (-not (Test-Path $injector)) { throw "no injector at $injector" }

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -Namespace R -Name W -MemberDefinition @'
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint owner);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
'@
function Get-Class([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 128; [void][R.W]::GetClassNameW($h, $sb, 128); $sb.ToString() }
function Get-Text([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 512; [void][R.W]::GetWindowTextW($h, $sb, 512); $sb.ToString() }

# --- probe file with a mark-of-the-web --------------------------------------
$dir = Join-Path $env:TEMP 'fs-press-probe'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$probe = Join-Path $dir 'probe.bat'
Set-Content -Path $probe -Value '@exit /b 0' -Encoding ascii
Set-Content -Path "$probe`:Zone.Identifier" -Value "[ZoneTransfer]`r`nZoneId=3" -Encoding ascii

# The host waits so the Bridge is in place before the dialog is shown: a dialog
# shown on a hooked GUI thread wakes discovery into its burst.
$hostFile = Join-Path $dir 'host.ps1'
Set-Content -Path $hostFile -Encoding ascii -Value @'
Start-Sleep -Seconds 14
Start-Process -FilePath $env:FS_PROBE -Wait
Start-Sleep -Seconds 5
'@

# --- fresh logs -------------------------------------------------------------
foreach ($log in 'FluentShell.log', 'FluentShell.Renderer.log') {
    $p = Join-Path $env:TEMP $log
    if (Test-Path $p) { Move-Item $p "$p.before-repro" -Force }
}

$env:FS_PROBE = $probe
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$hostProc = Start-Process $psExe -PassThru -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass', '-File', $hostFile)
Write-Output "HOST pid=$($hostProc.Id)"
Start-Sleep -Seconds 2

# --- inject -----------------------------------------------------------------
& $injector inject $psExe --pid $hostProc.Id 2>&1 | ForEach-Object { "  injector: $_" }
Write-Output "INJECT exit=$LASTEXITCODE"

# --- wait for the dialog to be projected ------------------------------------
$proxy = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadline -and $proxy -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 400
    $script:hit = [IntPtr]::Zero
    $cb = [R.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $p)
        if ([R.W]::IsWindowVisible($h) -and (Get-Class $h) -eq 'WinUIDesktopWin32WindowClass') {
            if ((Get-Text $h) -match '安全性警告|Security Warning') { $script:hit = $h; return $false }
        }
        return $true
    }
    [void][R.W]::EnumWindows($cb, [IntPtr]::Zero)
    $proxy = $script:hit
}

if ($proxy -eq [IntPtr]::Zero) {
    Write-Output 'RESULT: the dialog was never projected (see the Bridge log)'
} else {
    Write-Output ("PROXY: 0x{0:X} '{1}'" -f $proxy.ToInt64(), (Get-Text $proxy))
    Start-Sleep -Seconds 2
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($proxy)
    $buttons = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)))
    foreach ($b in $buttons) { Write-Output ("  PROXY BUTTON '{0}'" -f $b.Current.Name) }
    $run = $null
    foreach ($b in $buttons) { if ($b.Current.Name -match '執行|Run') { $run = $b; break } }
    if (-not $run) {
        Write-Output 'RESULT: no Run button on the proxy'
    } else {
        $pattern = $run.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        Write-Output ("INVOKING proxy button '{0}'" -f $run.Current.Name)
        $pattern.Invoke()
        Start-Sleep -Seconds 3
    }
}

Start-Sleep -Seconds 2
Write-Output ''
Write-Output '=== Bridge log: press lines ==='
$bridge = Join-Path $env:TEMP 'FluentShell.log'
if (Test-Path $bridge) {
    Select-String -Path $bridge -Pattern 'Native press|Projected native|Capture rejected|gate rejected|Discovery candidate' |
        ForEach-Object { $_.Line } | Select-Object -Last 25
}
Write-Output ''
Write-Output '=== Renderer log: actions ==='
$rend = Join-Path $env:TEMP 'FluentShell.Renderer.log'
if (Test-Path $rend) {
    Select-String -Path $rend -Pattern 'action\.|window\.open|surface\.commit' |
        ForEach-Object { $_.Line } | Select-Object -Last 15
}

$hostProc | Stop-Process -Force -EA SilentlyContinue
Get-Process FluentShell.Renderer -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Remove-Item Env:FS_PROBE -EA SilentlyContinue
