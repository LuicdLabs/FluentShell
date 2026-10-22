#requires -Version 5
<#
    Does the real security-warning dialog refuse its Run button while the window
    is DWM application-cloaked?

    That is the claim the working tree is built on.  A cloak only takes effect
    when the owning process sets it, so the child process that shows the dialog
    cloaks it itself, from a thread of its own, while its main thread sits in
    the dialog's modal loop.  This script then posts BM_CLICK to Run (id 4426)
    exactly as the Bridge does and reports whether the dialog acted.

    -Cloak      the dialog is cloaked for the press   (what a projection does)
    (omitted)   the dialog is not cloaked             (the control)
#>
param([switch] $Cloak, [switch] $FocusFirst)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace T -Name W -MemberDefinition @'
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetFocus();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint owner);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int cb);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

function Get-Class([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 128; [void][T.W]::GetClassNameW($h, $sb, 128); $sb.ToString() }
function Get-Text([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 512; [void][T.W]::GetWindowTextW($h, $sb, 512); $sb.ToString() }
function Get-Cloaked([IntPtr] $h) { $v = 0; [void][T.W]::DwmGetWindowAttribute($h, 14, [ref] $v, 4); $v }

$dir = Join-Path $env:TEMP 'fs-press-probe'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$probe = Join-Path $dir 'probe.bat'
Set-Content -Path $probe -Value '@exit /b 0' -Encoding ascii
Set-Content -Path "$probe`:Zone.Identifier" -Value "[ZoneTransfer]`r`nZoneId=3" -Encoding ascii

# --- the child: shows the dialog and, when asked, cloaks it itself ----------
$childSource = @'
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Threading;
public class Cloaker {
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int cb);
  delegate bool EnumProc(IntPtr h, IntPtr p);
  static IntPtr found;
  public static void Start() {
    var t = new Thread(() => {
      uint me = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
      for (int i = 0; i < 200; i++) {
        Thread.Sleep(100);
        found = IntPtr.Zero;
        EnumWindows((h, p) => {
          var sb = new StringBuilder(64); GetClassNameW(h, sb, 64);
          uint o; GetWindowThreadProcessId(h, out o);
          if (sb.ToString() == "#32770" && IsWindowVisible(h) && o == me) { found = h; return false; }
          return true; }, IntPtr.Zero);
        if (found != IntPtr.Zero) { int v = 1; DwmSetWindowAttribute(found, 13, ref v, 4); return; }
      }
    });
    t.IsBackground = true; t.Start();
  }
}
"@
if ($env:FS_CLOAK -eq '1') { [Cloaker]::Start() }
Start-Process -FilePath $env:FS_PROBE -Wait
'@
$childFile = Join-Path $dir 'child.ps1'
Set-Content -Path $childFile -Value $childSource -Encoding ascii

$env:FS_PROBE = $probe
$env:FS_CLOAK = if ($Cloak) { '1' } else { '0' }
$child = Start-Process powershell -PassThru -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass', '-File', $childFile)

# --- find the dialog --------------------------------------------------------
$dialog = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 200
    $script:found = [IntPtr]::Zero
    $cb = [T.W+EnumProc] {
        param([IntPtr] $h, [IntPtr] $p)
        if ((Get-Class $h) -eq '#32770') {
            [uint32] $owner = 0; [void][T.W]::GetWindowThreadProcessId($h, [ref] $owner)
            if ($owner -eq $child.Id) { $script:found = $h; return $false }
        }
        return $true
    }
    [void][T.W]::EnumWindows($cb, [IntPtr]::Zero)
    $dialog = $script:found
}
if ($dialog -eq [IntPtr]::Zero) {
    Write-Output 'RESULT: no dialog appeared'; $child | Stop-Process -Force -EA SilentlyContinue; return
}

# Give the child's thread time to apply the cloak.
Start-Sleep -Milliseconds 1200

$script:buttons = New-Object System.Collections.ArrayList
$cb2 = [T.W+EnumProc] {
    param([IntPtr] $h, [IntPtr] $p)
    if ((Get-Class $h) -eq 'Button') {
        [void] $script:buttons.Add([pscustomobject]@{ Handle = $h; Id = [T.W]::GetDlgCtrlID($h) })
    }
    return $true
}
[void][T.W]::EnumChildWindows($dialog, $cb2, [IntPtr]::Zero)
$run = $script:buttons | Where-Object { $_.Id -eq 4426 } | Select-Object -First 1
if (-not $run) { Write-Output 'RESULT: no Run button'; $child | Stop-Process -Force -EA SilentlyContinue; return }

[void][T.W]::SetForegroundWindow($dialog)
Start-Sleep -Milliseconds 400

$cloaked = Get-Cloaked $dialog
Write-Output ("MODE: {0}   dialog=0x{1:X} '{2}'" -f
    $(if ($Cloak) { 'CLOAKED' } else { 'not cloaked' }), $dialog.ToInt64(), (Get-Text $dialog))
Write-Output ("  cloaked flags=0x{0:X}  foreground=0x{1:X}  isForeground={2}" -f
    $cloaked, [T.W]::GetForegroundWindow().ToInt64(),
    ([T.W]::GetForegroundWindow() -eq $dialog))

if ($FocusFirst) {
    # What the Bridge does before the press: the focus is taken first, so the
    # activation BM_CLICK causes cannot cancel the press half-way.  WM_NEXTDLGCTL
    # is how that focus change is made from another process: the dialog's own
    # thread runs it, exactly as SetFocus would on the Bridge's path.
    [void][T.W]::SendMessageW($dialog, 0x0028, $run.Handle, [IntPtr] 1)  # WM_NEXTDLGCTL
    Start-Sleep -Milliseconds 250
    Write-Output '  focus taken first (WM_NEXTDLGCTL to the Run button)'
}

[void][T.W]::PostMessageW($run.Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)  # BM_CLICK

$gone = $false
$deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if (-not [T.W]::IsWindow($dialog)) { $gone = $true; break }
}
Write-Output ("RESULT: dialog {0}" -f $(if ($gone) { 'CLOSED  -> Run accepted' } else { 'STILL OPEN  -> Run REFUSED' }))

if ([T.W]::IsWindow($dialog)) {
    $cancel = $script:buttons | Where-Object { $_.Id -eq 2 } | Select-Object -First 1
    if ($cancel) { [void][T.W]::PostMessageW($cancel.Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
}
Start-Sleep -Milliseconds 800
$child | Stop-Process -Force -EA SilentlyContinue
Remove-Item Env:FS_CLOAK, Env:FS_PROBE -EA SilentlyContinue
