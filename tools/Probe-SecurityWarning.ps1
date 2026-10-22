#requires -Version 5
<#
    Does BM_CLICK press the Run button of the shell's "Open File - Security
    Warning" on a NATIVE, un-projected dialog?

    The dialog is shown in-process by ShellExecute when the file carries a
    mark-of-the-web (Zone.Identifier, ZoneId=3), so a child process runs the
    ShellExecute and this script drives the dialog it shows.  Nothing about a
    projection is involved: the dialog is the foreground window, it is not
    cloaked, and it is not layered.

    -Button Run | Cancel picks which button is pressed.  Cancel is the control:
    it is the button the projection can already press.

    The probe file is an inert batch file (@exit /b 0).
#>
param(
    [ValidateSet('Run', 'Cancel')] [string] $Button = 'Run',
    [switch] $UseSendInput
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace P -Name W -MemberDefinition @'
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int cb);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
        public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT {
        public uint type; public MOUSEINPUT mi; public long pad1, pad2; }
'@

function Get-Text([IntPtr] $h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][P.W]::GetWindowTextW($h, $sb, 512); $sb.ToString()
}
function Get-Class([IntPtr] $h) {
    $sb = New-Object System.Text.StringBuilder 128
    [void][P.W]::GetClassNameW($h, $sb, 128); $sb.ToString()
}

# --- the probe file ---------------------------------------------------------
$dir = Join-Path $env:TEMP 'fs-press-probe'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$probe = Join-Path $dir 'probe.bat'
Set-Content -Path $probe -Value '@exit /b 0' -Encoding ascii
# Mark of the web: this is what makes ShellExecute prompt.
Set-Content -Path "$probe`:Zone.Identifier" -Value "[ZoneTransfer]`r`nZoneId=3" -Encoding ascii
# A marker file the probe would create if it ever ran.  It does not: the batch
# file only exits.  The dialog closing is the signal, not the file.

# --- the child that shows the dialog ---------------------------------------
$child = Start-Process powershell -PassThru -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-Command',
    "Start-Process -FilePath '$probe' -Wait")

# --- find the dialog --------------------------------------------------------
$dialog = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 200
    $found = [IntPtr]::Zero
    $cb = [P.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $p)
        if ((Get-Class $h) -eq '#32770' -and [P.W]::IsWindowVisible($h)) {
            [uint32] $owner = 0; [void][P.W]::GetWindowThreadProcessId($h, [ref] $owner)
            if ($owner -eq $child.Id) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][P.W]::EnumWindows($cb, [IntPtr]::Zero)
    $dialog = $found
}
if ($dialog -eq [IntPtr]::Zero) {
    Write-Output 'RESULT: no dialog appeared (ShellExecute did not prompt)'
    $child | Stop-Process -Force -ErrorAction SilentlyContinue
    return
}

Write-Output ("DIALOG: 0x{0:X} '{1}'" -f $dialog.ToInt64(), (Get-Text $dialog))

# --- its buttons ------------------------------------------------------------
$buttons = New-Object System.Collections.ArrayList
$cb2 = [P.W+EnumProc] {
    param([IntPtr] $h, [IntPtr] $p)
    if ((Get-Class $h) -eq 'Button') {
        [void] $script:buttons.Add([pscustomobject]@{
            Handle = $h; Id = [P.W]::GetDlgCtrlID($h); Text = (Get-Text $h) })
    }
    return $true
}
[void][P.W]::EnumChildWindows($dialog, $cb2, [IntPtr]::Zero)
$buttons | ForEach-Object { Write-Output ("  BUTTON id={0} '{1}'" -f $_.Id, $_.Text) }

# shdocvw's dialog template 4416: 4426 is the Run/Open button, 2 is Cancel.
# Matched by control id, not by its localized label.
$wanted = if ($Button -eq 'Run') { 4426 } else { 2 }
$target = $buttons | Where-Object { $_.Id -eq $wanted } | Select-Object -First 1
if (-not $target) {
    Write-Output "RESULT: no $Button button found"
    $child | Stop-Process -Force -ErrorAction SilentlyContinue
    return
}
Write-Output ("TARGET: id={0} '{1}'" -f $target.Id, $target.Text)

# --- press it the way the Bridge does --------------------------------------
[void][P.W]::SetForegroundWindow($dialog)
Start-Sleep -Milliseconds 300
Write-Output ("FOREGROUND before press: 0x{0:X}" -f [P.W]::GetForegroundWindow().ToInt64())

if ($UseSendInput) {
    $r = New-Object P.W+RECT
    [void][P.W]::GetWindowRect($target.Handle, [ref] $r)
    $x = [int](($r.left + $r.right) / 2); $y = [int](($r.top + $r.bottom) / 2)
    [void][P.W]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 120
    $down = New-Object P.W+INPUT; $down.type = 0; $down.mi.dwFlags = 0x0002
    $up = New-Object P.W+INPUT;   $up.type = 0;   $up.mi.dwFlags = 0x0004
    $cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type]'P.W+INPUT')
    [void][P.W]::SendInput(2, @($down, $up), $cbSize)
    Write-Output "PRESS: SendInput real mouse click at $x,$y"
} else {
    [void][P.W]::PostMessageW($target.Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)  # BM_CLICK
    Write-Output 'PRESS: posted BM_CLICK'
}

# --- did the dialog act? ----------------------------------------------------
$gone = $false
$deadline = (Get-Date).AddSeconds(4)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if (-not [P.W]::IsWindow($dialog) -or -not [P.W]::IsWindowVisible($dialog)) { $gone = $true; break }
}
Write-Output ("RESULT: dialog {0} after the {1} press" -f
    $(if ($gone) { 'CLOSED' } else { 'STILL OPEN (refused)' }), $Button)

$child | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name cmd -ErrorAction SilentlyContinue |
    Where-Object { $_.StartTime -gt (Get-Date).AddMinutes(-1) } |
    Stop-Process -Force -ErrorAction SilentlyContinue
