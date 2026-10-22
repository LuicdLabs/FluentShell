#requires -Version 5
<#
    Which single piece of state makes shdocvw's security-warning dialog refuse
    its Run button?

    The dialog is the real one (shdocvw template 4416, shown in-process by
    ShellExecute for a file carrying a mark-of-the-web).  A child process shows
    it; this script puts the dialog into one state and posts BM_CLICK to Run
    (id 4426), then reports whether the dialog acted.

    -State
      Baseline          foreground, uncloaked, on top        (a native click)
      Cloaked           foreground, DWM application-cloaked  (what a projection does)
      Occluded          foreground, uncloaked, covered by a topmost window
      Background        uncloaked, another window in the foreground
      ReCloakAfterPost  foreground + uncloaked for the post, cloaked again
                        before the application dispatches the click

    Every state is restored and the dialog is cancelled afterwards.
#>
param(
    [ValidateSet('Baseline', 'Cloaked', 'Occluded', 'OccludedBeside', 'OccludedNotTopmost',
                 'OccludedVerified', 'Background', 'ReCloakAfterPost')]
    [string] $State = 'Baseline'
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Q -Name W -MemberDefinition @'
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint owner);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int v, int cb);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int cb);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
'@

$DWMWA_CLOAK = 13; $DWMWA_CLOAKED = 14
function Get-Text([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 512; [void][Q.W]::GetWindowTextW($h, $sb, 512); $sb.ToString() }
function Get-Class([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 128; [void][Q.W]::GetClassNameW($h, $sb, 128); $sb.ToString() }
function Set-Cloak([IntPtr] $h, [bool] $on) { $v = [int]$on; [void][Q.W]::DwmSetWindowAttribute($h, $DWMWA_CLOAK, [ref] $v, 4) }
function Get-Cloaked([IntPtr] $h) { $v = 0; [void][Q.W]::DwmGetWindowAttribute($h, $DWMWA_CLOAKED, [ref] $v, 4); $v }

# --- the probe file ---------------------------------------------------------
$dir = Join-Path $env:TEMP 'fs-press-probe'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$probe = Join-Path $dir 'probe.bat'
Set-Content -Path $probe -Value '@exit /b 0' -Encoding ascii
Set-Content -Path "$probe`:Zone.Identifier" -Value "[ZoneTransfer]`r`nZoneId=3" -Encoding ascii

$child = Start-Process powershell -PassThru -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-Command', "Start-Process -FilePath '$probe' -Wait")

# --- find the dialog --------------------------------------------------------
$dialog = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 200
    $script:found = [IntPtr]::Zero
    $cb = [Q.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $p)
        if ((Get-Class $h) -eq '#32770' -and [Q.W]::IsWindowVisible($h)) {
            [uint32] $owner = 0; [void][Q.W]::GetWindowThreadProcessId($h, [ref] $owner)
            if ($owner -eq $child.Id) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][Q.W]::EnumWindows($cb, [IntPtr]::Zero)
    $dialog = $script:found
}
if ($dialog -eq [IntPtr]::Zero) {
    Write-Output 'RESULT: no dialog appeared'; $child | Stop-Process -Force -EA SilentlyContinue; return
}

$script:buttons = New-Object System.Collections.ArrayList
$cb2 = [Q.W+EnumProc] {
    param([IntPtr] $h, [IntPtr] $p)
    if ((Get-Class $h) -eq 'Button') {
        [void] $script:buttons.Add([pscustomobject]@{ Handle = $h; Id = [Q.W]::GetDlgCtrlID($h) })
    }
    return $true
}
[void][Q.W]::EnumChildWindows($dialog, $cb2, [IntPtr]::Zero)
$run = $script:buttons | Where-Object { $_.Id -eq 4426 } | Select-Object -First 1
if (-not $run) { Write-Output 'RESULT: no Run button'; $child | Stop-Process -Force -EA SilentlyContinue; return }

Write-Output ("STATE: {0}   dialog=0x{1:X}  run=0x{2:X}" -f $State, $dialog.ToInt64(), $run.Handle.ToInt64())

# --- put the dialog into the requested state --------------------------------
[void][Q.W]::SetForegroundWindow($dialog)
Start-Sleep -Milliseconds 400

$cover = $null
switch ($State) {
    'Cloaked' { Set-Cloak $dialog $true; Start-Sleep -Milliseconds 300 }
    'OccludedVerified' {
        # The projection's actual geometry: a window of another process exactly
        # over the dialog, raised above it *after* the dialog has the foreground,
        # which is the order NativePressActivation uses (it foregrounds the
        # native window and then puts the proxy back on top).  The occlusion is
        # then verified with WindowFromPoint over the Run button, so a run that
        # failed to cover is not counted as evidence.
        $r = New-Object Q.W+RECT; [void][Q.W]::GetWindowRect($dialog, [ref] $r)
        Add-Type -AssemblyName System.Windows.Forms
        $cover = New-Object System.Windows.Forms.Form
        $cover.FormBorderStyle = 'None'; $cover.ShowInTaskbar = $false
        $cover.StartPosition = 'Manual'
        $cover.Left = $r.left; $cover.Top = $r.top
        $cover.Width = $r.right - $r.left; $cover.Height = $r.bottom - $r.top
        $cover.BackColor = 'DarkSlateBlue'
        $cover.Show(); $cover.Refresh()
        [void][Q.W]::SetForegroundWindow($dialog)
        Start-Sleep -Milliseconds 300
        # HWND_TOPMOST, no activation: the dialog keeps the foreground and the
        # cover is above it, exactly as the proxy is.
        [void][Q.W]::SetWindowPos($cover.Handle, [IntPtr](-1), 0, 0, 0, 0, 0x0013)
        Start-Sleep -Milliseconds 400
    }
    { $_ -in 'Occluded', 'OccludedBeside', 'OccludedNotTopmost' } {
        # A window over the dialog, the way a projection's proxy covers it.
        # 'OccludedBeside' is the control: the same window, next to the dialog
        # instead of over it, so what is being tested is the covering and not
        # the existence of another window.
        $r = New-Object Q.W+RECT; [void][Q.W]::GetWindowRect($dialog, [ref] $r)
        Add-Type -AssemblyName System.Windows.Forms
        $cover = New-Object System.Windows.Forms.Form
        $cover.FormBorderStyle = 'None'; $cover.ShowInTaskbar = $false
        $cover.TopMost = ($State -ne 'OccludedNotTopmost')
        $cover.StartPosition = 'Manual'
        $offset = if ($State -eq 'OccludedBeside') { $r.right - $r.left + 40 } else { 0 }
        $cover.Left = $r.left + $offset; $cover.Top = $r.top
        $cover.Width = $r.right - $r.left; $cover.Height = $r.bottom - $r.top
        $cover.BackColor = 'DarkSlateBlue'
        $cover.Show(); $cover.Refresh()
        if ($State -eq 'OccludedNotTopmost') {
            # Above the dialog in z-order without being topmost.
            [void][Q.W]::SetWindowPos($cover.Handle, [IntPtr]0, 0, 0, 0, 0, 0x0013)  # TOP|NOMOVE|NOSIZE|NOACTIVATE
        }
        # The dialog keeps the foreground; only the pixels are covered.
        [void][Q.W]::SetForegroundWindow($dialog)
        Start-Sleep -Milliseconds 400
    }
    'Background' {
        Add-Type -AssemblyName System.Windows.Forms
        $cover = New-Object System.Windows.Forms.Form
        $cover.Text = 'foreground thief'; $cover.TopMost = $true
        $cover.Width = 300; $cover.Height = 120
        $cover.Show(); $cover.Activate(); $cover.Refresh()
        Start-Sleep -Milliseconds 400
    }
}

# Is the Run button actually the window on screen at its own centre?
$rb = New-Object Q.W+RECT; [void][Q.W]::GetWindowRect($run.Handle, [ref] $rb)
$pt = New-Object Q.W+POINT
$pt.x = [int](($rb.left + $rb.right) / 2); $pt.y = [int](($rb.top + $rb.bottom) / 2)
$onTop = [Q.W]::WindowFromPoint($pt)
Write-Output ("  before press: foreground=0x{0:X} cloaked={1} windowAtRunButton=0x{2:X} ({3})" -f
    [Q.W]::GetForegroundWindow().ToInt64(), (Get-Cloaked $dialog), $onTop.ToInt64(),
    $(if ($onTop -eq $run.Handle) { 'the Run button -> NOT occluded' } else { 'something else -> occluded' }))

[void][Q.W]::PostMessageW($run.Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)  # BM_CLICK

if ($State -eq 'ReCloakAfterPost') {
    # The click is still on the queue.  This is a press that gave the state back
    # before the application dispatched the click.
    Set-Cloak $dialog $true
    Write-Output '  re-cloaked immediately after the post'
}

$gone = $false
$deadline = (Get-Date).AddSeconds(4)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if (-not [Q.W]::IsWindow($dialog) -or -not [Q.W]::IsWindowVisible($dialog)) { $gone = $true; break }
}
Write-Output ("RESULT [{0}]: dialog {1}" -f $State, $(if ($gone) { 'CLOSED  -> Run accepted' } else { 'STILL OPEN  -> Run REFUSED' }))

# --- cleanup ----------------------------------------------------------------
if ([Q.W]::IsWindow($dialog)) { Set-Cloak $dialog $false }
if ($cover) { $cover.Close(); $cover.Dispose() }
if ([Q.W]::IsWindow($dialog)) {
    $cancel = $script:buttons | Where-Object { $_.Id -eq 2 } | Select-Object -First 1
    if ($cancel) { [void][Q.W]::SetForegroundWindow($dialog); [void][Q.W]::PostMessageW($cancel.Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
}
Start-Sleep -Milliseconds 600
$child | Stop-Process -Force -EA SilentlyContinue
