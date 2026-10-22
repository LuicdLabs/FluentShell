#requires -Version 5
<#
    The dialog is projected, and then its Run button is pressed one of two ways:

    -Through Proxy   invoke the projected proxy's Run button through UIA, which
                     is what the user's click does: the renderer emits the
                     action and the Bridge presses the native control.
    -Through Native  post BM_CLICK straight to native control 4426 from this
                     script, bypassing the Bridge's press path entirely while
                     the window is still projected (cloaked, proxy on top).

    Native succeeding and Proxy failing puts the fault in the Bridge's press
    path.  Both failing puts it in the state the projection itself establishes,
    because an un-projected dialog accepts exactly this press
    (docs\goals\win32-to-winui-translation\NATIVE-PRESS-EVIDENCE.md).
#>
param(
    [ValidateSet('Proxy', 'Native', 'NativeActivated', 'NativeActivatedDetached',
                 'RollbackThenNative')] [string] $Through = 'Native',
    # The control for the harness itself: no Bridge, no projection, same host,
    # same probe file, same press, same way of judging the outcome.
    [switch] $NoInject,
    # Inject the Bridge but hold FluentShell dormant, so the Bridge runs in the
    # process and never projects anything.  Splits "injection broke it" from
    # "projecting it broke it".
    [switch] $Dormant,
    # Which control to press.  4426 is Run, 2 is Cancel: the asymmetry the user
    # reported is that Cancel works while Run does not.
    [int] $Control = 4426,
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\bin\x64\$Configuration"
$injector = Join-Path $out 'FluentShell.Injector.exe'

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -Namespace B -Name W -MemberDefinition @'
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint owner);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int cb);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO i);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [StructLayout(LayoutKind.Sequential)] public struct RECT2 { public int l, t, r, b; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public uint flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
        public RECT2 rcCaret; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
'@
function Get-Class([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 128; [void][B.W]::GetClassNameW($h, $sb, 128); $sb.ToString() }
function Get-Text([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 512; [void][B.W]::GetWindowTextW($h, $sb, 512); $sb.ToString() }
function Get-Cloaked([IntPtr] $h) { $v = 0; [void][B.W]::DwmGetWindowAttribute($h, 14, [ref] $v, 4); $v }

# The full window state of the dialog and its Run button, so the projected case
# can be diffed against the dormant one.  Whatever projection changes that the
# dialog's Run handler reads has to show up here.
function Dump-State([IntPtr] $dlg, [IntPtr] $btn, [string] $label) {
    $thread = [uint32] 0
    [void][B.W]::GetWindowThreadProcessId($dlg, [ref] $thread)
    $tid = [B.W]::GetWindowThreadProcessId($dlg, [ref] $thread)
    $gti = New-Object B.W+GUITHREADINFO
    $gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
    $haveGti = [B.W]::GetGUIThreadInfo($tid, [ref] $gti)
    Write-Output "--- state ($label) ---"
    Write-Output ("  dialog  0x{0:X} style=0x{1:X8} ex=0x{2:X8} enabled={3} visible={4} cloaked=0x{5:X}" -f
        $dlg.ToInt64(),
        [B.W]::GetWindowLongPtrW($dlg, -16).ToInt64(),
        [B.W]::GetWindowLongPtrW($dlg, -20).ToInt64(),
        [B.W]::IsWindowEnabled($dlg), [B.W]::IsWindowVisible($dlg), (Get-Cloaked $dlg))
    Write-Output ("  owner   0x{0:X}  parent 0x{1:X}  lastActivePopup 0x{2:X}" -f
        [B.W]::GetWindow($dlg, 4).ToInt64(), [B.W]::GetParent($dlg).ToInt64(),
        [B.W]::GetLastActivePopup($dlg).ToInt64())
    if ($btn -ne [IntPtr]::Zero) {
        Write-Output ("  button  0x{0:X} id={1} style=0x{2:X8} ex=0x{3:X8} enabled={4} visible={5}" -f
            $btn.ToInt64(), [B.W]::GetDlgCtrlID($btn),
            [B.W]::GetWindowLongPtrW($btn, -16).ToInt64(),
            [B.W]::GetWindowLongPtrW($btn, -20).ToInt64(),
            [B.W]::IsWindowEnabled($btn), [B.W]::IsWindowVisible($btn))
    }
    Write-Output ("  foreground 0x{0:X}" -f [B.W]::GetForegroundWindow().ToInt64())
    if ($haveGti) {
        Write-Output ("  GUI thread {0}: active=0x{1:X} focus=0x{2:X} capture=0x{3:X} menuOwner=0x{4:X} moveSize=0x{5:X} flags=0x{6:X}" -f
            $tid, $gti.hwndActive.ToInt64(), $gti.hwndFocus.ToInt64(), $gti.hwndCapture.ToInt64(),
            $gti.hwndMenuOwner.ToInt64(), $gti.hwndMoveSize.ToInt64(), $gti.flags)
    } else { Write-Output "  GUI thread ${tid}: GetGUIThreadInfo failed" }
}

$dir = Join-Path $env:TEMP 'fs-press-probe'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$probe = Join-Path $dir 'probe.bat'
# The probe records that it ran, so "the file ran" is observable even if the
# dialog's own window has already gone.
$ran = Join-Path $dir 'ran.txt'
Remove-Item $ran -Force -EA SilentlyContinue
Set-Content -Path $probe -Value "@echo ran > `"$ran`"" -Encoding ascii
Set-Content -Path "$probe`:Zone.Identifier" -Value "[ZoneTransfer]`r`nZoneId=3" -Encoding ascii

$hostFile = Join-Path $dir 'host.ps1'
Set-Content -Path $hostFile -Encoding ascii -Value @'
Start-Sleep -Seconds 14
Start-Process -FilePath $env:FS_PROBE -Wait
Start-Sleep -Seconds 8
'@

foreach ($log in 'FluentShell.log', 'FluentShell.Renderer.log') {
    $p = Join-Path $env:TEMP $log
    if (Test-Path $p) { Remove-Item $p -Force -EA SilentlyContinue }
}

$env:FS_PROBE = $probe
# The host's environment is inherited by the Bridge that is injected into it.
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$hostProc = Start-Process $psExe -PassThru -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass', '-File', $hostFile)
$dormancy = $null
if ($Dormant) {
    $created = $false
    $dormancy = New-Object System.Threading.EventWaitHandle($true, 'ManualReset', 'Local\FluentShell.Dormant', [ref] $created)
    [void]$dormancy.Set()
    Write-Output "DORMANT: Local\FluentShell.Dormant signalled (created=$created)"
}

Start-Sleep -Seconds 2
if (-not $NoInject) { & $injector inject $psExe --pid $hostProc.Id *> $null }

# --- wait until the dialog is projected -------------------------------------
$native = [IntPtr]::Zero; $proxy = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadline -and ($native -eq [IntPtr]::Zero -or $proxy -eq [IntPtr]::Zero)) {
    Start-Sleep -Milliseconds 400
    $script:n = [IntPtr]::Zero; $script:p = [IntPtr]::Zero
    $cb = [B.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $unused)
        $cls = Get-Class $h
        if ($cls -eq '#32770') {
            [uint32] $owner = 0; [void][B.W]::GetWindowThreadProcessId($h, [ref] $owner)
            if ($owner -eq $hostProc.Id) { $script:n = $h }
        } elseif ($cls -eq 'WinUIDesktopWin32WindowClass' -and [B.W]::IsWindowVisible($h)) {
            if ((Get-Text $h) -match '安全性警告|Security Warning') { $script:p = $h }
        }
        return $true
    }
    [void][B.W]::EnumWindows($cb, [IntPtr]::Zero)
    $native = $script:n; $proxy = $script:p
}

# With no Bridge, or with a dormant one, no proxy exists: the native dialog is
# all there is, so it stands in for the proxy in the readiness check below.
if ($NoInject -or $Dormant) { $proxy = $native }
if ($native -eq [IntPtr]::Zero -or $proxy -eq [IntPtr]::Zero) {
    Write-Output "RESULT: not projected (native=0x$($native.ToInt64().ToString('X')) proxy=0x$($proxy.ToInt64().ToString('X')))"
    $hostProc | Stop-Process -Force -EA SilentlyContinue
    Get-Process FluentShell.Renderer -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    return
}

Write-Output ("PROJECTED: native=0x{0:X} cloaked=0x{1:X}  proxy=0x{2:X}" -f
    $native.ToInt64(), (Get-Cloaked $native), $proxy.ToInt64())

Start-Sleep -Seconds 2

if ($Through -eq 'RollbackThenNative') {
    # Ask this Bridge to roll the root back to native.  Rollback uncloaks the
    # window, closes the proxy and runs DetachControlObservation, so everything
    # the projection installed on the dialog and its controls comes off.  The
    # press afterwards is the same posted BM_CLICK as the 'Native' run.
    $name = "Local\FluentShell.Rollback.$($hostProc.Id).$($native.ToInt64().ToString('x'))"
    try {
        $ev = [System.Threading.EventWaitHandle]::OpenExisting($name)
        [void]$ev.Set(); $ev.Dispose()
        Write-Output "ROLLBACK: signalled $name"
    } catch {
        Write-Output "ROLLBACK: could not open $name ($($_.Exception.Message))"
    }
    # Wait for the rollback to actually finish: the proxy is gone and the native
    # window is uncloaked.  Pressing while it is half done proves nothing.
    $rbDeadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $rbDeadline) {
        Start-Sleep -Milliseconds 400
        if (-not [B.W]::IsWindow($proxy) -and (Get-Cloaked $native) -eq 0) { break }
    }
    Start-Sleep -Seconds 1
    Write-Output ("  after rollback: cloaked=0x{0:X} proxyStillThere={1}" -f
        (Get-Cloaked $native), [B.W]::IsWindow($proxy))
}

if ($Through -in 'NativeActivated', 'NativeActivatedDetached') {
    # Give the native dialog a *sustained* activation, rather than the momentary
    # one NativePressActivation establishes and then gives straight back.  The
    # cloak cannot be changed from outside the owning process, and it does not
    # need to be: a cloaked dialog whose own thread is active accepts the press
    # (NATIVE-PRESS-EVIDENCE.md).  Joining the two input queues is what lets this
    # process name another process's window as the foreground one.
    $hostThread = [uint32] 0
    $tid = [B.W]::GetWindowThreadProcessId($native, [ref] $hostThread)
    $me = [B.W]::GetCurrentThreadId()
    $attached = [B.W]::AttachThreadInput($me, $tid, $true)
    [void][B.W]::SetForegroundWindow($native)
    Start-Sleep -Milliseconds 400

    $script:btn = [IntPtr]::Zero
    $cbA = [B.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $unused)
        if ((Get-Class $h) -eq 'Button' -and [B.W]::GetDlgCtrlID($h) -eq $Control) { $script:btn = $h; return $false }
        return $true
    }
    [void][B.W]::EnumChildWindows($native, $cbA, [IntPtr]::Zero)
    # The dialog's own thread moves the focus, the way the Bridge's SetFocus does.
    if ($script:btn -ne [IntPtr]::Zero) {
        [void][B.W]::SendMessageW($native, 0x0028, $script:btn, [IntPtr] 1)  # WM_NEXTDLGCTL
        Start-Sleep -Milliseconds 250
    }
    # 'Detached' releases the input-queue join *before* the click is posted, which
    # is what NativePressActivation does: it joins only for the handover.  The
    # foreground still reads as the dialog afterwards, so this isolates whether
    # the join itself has to last through the dispatch.
    if ($Through -eq 'NativeActivatedDetached' -and $attached) {
        [void][B.W]::AttachThreadInput($me, $tid, $false)
        $attached = $false
        Start-Sleep -Milliseconds 200
        Write-Output 'DETACHED: input queues unjoined before the press (what the Bridge does)'
    }
    Write-Output "ACTIVATED: attachThreadInput=$attached"
    Dump-State $native $script:btn "projected + activation ($Through)"

    if ($script:btn -ne [IntPtr]::Zero) {
        Write-Output ("PRESS: posting BM_CLICK to native 0x{0:X} (id {1}), activation held" -f
            $script:btn.ToInt64(), $Control)
        [void][B.W]::PostMessageW($script:btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    # Hold the activation while the dialog acts, then let go.
    Start-Sleep -Seconds 4
    if ($attached) { [void][B.W]::AttachThreadInput($me, $tid, $false) }
}

if ($Through -in 'Native', 'RollbackThenNative') {
    $script:btn = [IntPtr]::Zero
    $cb2 = [B.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $unused)
        if ((Get-Class $h) -eq 'Button' -and [B.W]::GetDlgCtrlID($h) -eq $Control) { $script:btn = $h; return $false }
        return $true
    }
    [void][B.W]::EnumChildWindows($native, $cb2, [IntPtr]::Zero)
    Dump-State $native $script:btn $(if ($Dormant) { 'dormant, never projected' } elseif ($Through -eq 'RollbackThenNative') { 'after rollback' } else { 'projected' })
    if ($script:btn -eq [IntPtr]::Zero) { Write-Output "RESULT: no native control $Control"; }
    else {
        Write-Output ("PRESS: posting BM_CLICK straight to native 0x{0:X} (id {1}), Bridge not involved" -f $script:btn.ToInt64(), $Control)
        [void][B.W]::PostMessageW($script:btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    }
} else {
    $el = [System.Windows.Automation.AutomationElement]::FromHandle($proxy)
    $buttons = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)))
    $wanted = if ($Control -eq 2) { '取消|Cancel' } else { '執行|Run' }
    $run = $null
    foreach ($b in $buttons) { if ($b.Current.Name -match $wanted) { $run = $b; break } }
    if (-not $run) { Write-Output 'RESULT: no Run button on the proxy' }
    else {
        # The native Run button, only so its state can be reported alongside.
        $script:btn = [IntPtr]::Zero
        $cbN = [B.W+EnumProc] {
            param([IntPtr] $h, [IntPtr] $unused)
            if ((Get-Class $h) -eq 'Button' -and [B.W]::GetDlgCtrlID($h) -eq 4426) { $script:btn = $h; return $false }
            return $true
        }
        [void][B.W]::EnumChildWindows($native, $cbN, [IntPtr]::Zero)
        Dump-State $native $script:btn 'projected, before the proxy invoke'
        Write-Output ("PRESS: invoking the proxy's '{0}' through UIA (the user's click)" -f $run.Current.Name)
        $run.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Start-Sleep -Milliseconds 700
        if ([B.W]::IsWindow($native)) { Dump-State $native $script:btn 'projected, just after the proxy invoke' }
    }
}

# --- did the dialog act? ----------------------------------------------------
$gone = $false
$deadline = (Get-Date).AddSeconds(6)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    if (-not [B.W]::IsWindow($native)) { $gone = $true; break }
}
Start-Sleep -Milliseconds 800
$fileRan = Test-Path $ran
Write-Output ("RESULT [{0}]: native dialog {1}; the file {2}" -f $Through,
    $(if ($gone) { 'CLOSED' } else { 'STILL OPEN' }),
    $(if ($fileRan) { 'RAN' } else { 'did NOT run' }))

Write-Output ''
Select-String -Path (Join-Path $env:TEMP 'FluentShell.log') -Pattern 'Native press' |
    ForEach-Object { ($_.Line -split '\] ')[-1] } | Select-Object -Last 6

$hostProc | Stop-Process -Force -EA SilentlyContinue
Get-Process FluentShell.Renderer -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Remove-Item Env:FS_PROBE -EA SilentlyContinue
