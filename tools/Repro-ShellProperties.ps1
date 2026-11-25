#requires -Version 5
<#
    Projects the shell's folder Properties sheet without Explorer and walks every page.

    SHObjectProperties shows the same sheet Explorer shows for a folder (same pages, same thread
    model: a shell thread of its own, owned by a hidden StubWindow32), in whatever process calls
    it.  A plain powershell.exe host shows it, and powershell.exe is not on the injection deny
    list, so plain `inject` projects it.  This runs the application-scope Bridge, not the Explorer
    guest; the page adapters and the renderer are the same.

    The script selects each tab on the projected window through UIA (the gesture the user's click
    emits), screenshots the proxy, and finally presses the proxy's Cancel.  Nothing is changed on
    disk: the folder is a disposable one under %TEMP%.

      .\tools\Repro-ShellProperties.ps1 -Configuration Debug -OutDir <folder for screenshots>
#>
param(
    [string] $Configuration = 'Debug',
    [string] $OutDir = (Join-Path $env:TEMP 'fs-props-repro'),
    # Also toggle a check box on the General page (twice, so nothing changes) and open
    # Security > Edit, toggle a permission there and Cancel it, timing how long the
    # projected Cancel takes to close the dialog.  Nothing is applied: every dialog is
    # cancelled, and the folder is a disposable one.
    [switch] $Interact,
    # Tab indexes the walk does not visit.  Previous Versions (3) with a version listed keeps
    # the sheet native by design, which would end the walk before -Interact runs.
    [int[]] $SkipTab = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\bin\x64\$Configuration"
$injector = Join-Path $out 'FluentShell.Injector.exe'
if (-not (Test-Path $injector)) { throw "no injector at $injector" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type -Namespace RP -Name W -MemberDefinition @'
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'@
function Get-Class([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 128; [void][RP.W]::GetClassNameW($h, $sb, 128); $sb.ToString() }
function Get-Text([IntPtr] $h) { $sb = New-Object System.Text.StringBuilder 512; [void][RP.W]::GetWindowTextW($h, $sb, 512); $sb.ToString() }
function Save-Shot([IntPtr] $h, [string] $name) {
    [void][RP.W]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 600
    $r = New-Object RP.W+RECT
    [void][RP.W]::GetWindowRect($h, [ref] $r)
    $w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
    if ($w -le 0 -or $hgt -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $path = Join-Path $OutDir $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "  SHOT $path"
}

# --- a disposable folder and a host that opens its Properties ---------------
$work = Join-Path $env:TEMP 'fs-props-repro-host'
$folder = Join-Path $work 'dev'
New-Item -ItemType Directory -Force -Path (Join-Path $folder 'sub') | Out-Null
Set-Content -Path (Join-Path $folder 'a.txt') -Value 'probe' -Encoding ascii
$hostFile = Join-Path $work 'host.ps1'
Set-Content -Path $hostFile -Encoding ascii -Value @'
Add-Type -Namespace RH -Name S -MemberDefinition '[DllImport("shell32.dll", CharSet=CharSet.Unicode)] public static extern bool SHObjectProperties(IntPtr h, uint type, string name, string page);'
# Wait for the Bridge: a sheet shown on a hooked thread wakes discovery at once, but the sheet's
# own thread is new, so discovery's periodic pass finds it.
Start-Sleep -Seconds 10
[void][RH.S]::SHObjectProperties([IntPtr]::Zero, 2, $env:FS_PROPS_FOLDER, $null)
Start-Sleep -Seconds 150
'@
foreach ($log in 'FluentShell.log', 'FluentShell.Renderer.log') {
    $p = Join-Path $env:TEMP $log
    if (Test-Path $p) { Move-Item $p "$p.before-props" -Force }
}
$env:FS_PROPS_FOLDER = $folder
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$hostProc = Start-Process $psExe -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $hostFile)
Write-Output "HOST pid=$($hostProc.Id)"
Start-Sleep -Seconds 2
& $injector inject $psExe --pid $hostProc.Id 2>&1 | ForEach-Object { "  injector: $_" }
Write-Output "INJECT exit=$LASTEXITCODE"

try {
    # --- wait for the projected sheet -----------------------------------------
    $proxy = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline -and $proxy -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 400
        $script:hit = [IntPtr]::Zero
        $cb = [RP.W+EnumProc] {
            param([IntPtr] $h, [IntPtr] $p)
            if ([RP.W]::IsWindowVisible($h) -and (Get-Class $h) -eq 'WinUIDesktopWin32WindowClass' -and
                (Get-Text $h) -match '^dev - ') { $script:hit = $h; return $false }
            return $true
        }
        [void][RP.W]::EnumWindows($cb, [IntPtr]::Zero)
        $proxy = $script:hit
    }
    if ($proxy -eq [IntPtr]::Zero) { Write-Output 'RESULT: the sheet was never projected (see the Bridge log)'; return }
    Write-Output ("PROXY 0x{0:X} '{1}'" -f $proxy.ToInt64(), (Get-Text $proxy))
    Start-Sleep -Seconds 2

    $A = [System.Windows.Automation.AutomationElement]
    $el = $A::FromHandle($proxy)
    $tabCondition = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::TabItem)
    $tabs = @($el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $tabCondition))
    Write-Output ("TABS: " + (($tabs | ForEach-Object { $_.Current.Name }) -join ' | '))
    Save-Shot $proxy 'page-0.png'
    for ($index = 1; $index -lt $tabs.Count; $index++) {
        if ($SkipTab -contains $index) { Write-Output "SKIP tab $index '$($tabs[$index].Current.Name)'"; continue }
        $el = $A::FromHandle($proxy)
        $tab = @($el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $tabCondition))[$index]
        if (-not $tab) { Write-Output "  tab $index vanished (rolled back?)"; break }
        Write-Output "SELECT tab $index '$($tab.Current.Name)'"
        $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        Start-Sleep -Seconds 4
        if (-not [RP.W]::IsWindowVisible($proxy)) { Write-Output '  the proxy is gone: the sheet rolled back to native'; break }
        Save-Shot $proxy "page-$index.png"
    }

    if ($Interact) {
        $toggleCondition = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::CheckBox)
        $buttonCondition = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)
        function Select-Tab([int] $index) {
            $tab = @(($A::FromHandle($proxy)).FindAll([System.Windows.Automation.TreeScope]::Descendants, $tabCondition))[$index]
            if ($tab) { $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
            Start-Sleep -Seconds 2
        }
        # --- a check box on General: toggle, watch it settle, toggle back --------
        Select-Tab 0
        $box = @(($A::FromHandle($proxy)).FindAll([System.Windows.Automation.TreeScope]::Descendants, $toggleCondition)) |
            Where-Object { $_.Current.IsEnabled } | Select-Object -First 1
        if ($box) {
            $toggle = $box.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
            $before = $toggle.Current.ToggleState
            $toggle.Toggle()
            $seen = @()
            $clock = [Diagnostics.Stopwatch]::StartNew()
            foreach ($at in 40, 120, 300, 700, 1500) {
                while ($clock.ElapsedMilliseconds -lt $at) { Start-Sleep -Milliseconds 10 }
                $seen += "$($at)ms=$($toggle.Current.ToggleState)"
            }
            Write-Output "CHECKBOX '$($box.Current.Name)' before=$before after: $($seen -join ' ')"
            Save-Shot $proxy 'checkbox-toggled.png'
            # A three-state box (Read-only on a folder) cycles through all three states.
            for ($step = 0; $step -lt 3 -and "$($toggle.Current.ToggleState)" -ne "$before"; $step++) {
                $toggle.Toggle()
                Start-Sleep -Seconds 1
            }
            Write-Output "CHECKBOX back to $($toggle.Current.ToggleState) (was $before)"
        } else { Write-Output 'CHECKBOX none enabled on General' }

        # --- Security > Edit: the permission editor over the sheet --------------
        Select-Tab 2
        $editPrefix = -join [char[]](0x7DE8, 0x8F2F)
        $edit = @(($A::FromHandle($proxy)).FindAll([System.Windows.Automation.TreeScope]::Descendants, $buttonCondition)) |
            Where-Object { $_.Current.Name.StartsWith($editPrefix) -or $_.Current.Name.StartsWith('Edit') } | Select-Object -First 1
        if ($edit) {
            Write-Output "INVOKE '$($edit.Current.Name)'"
            $edit.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            $permissionsTitle = -join [char[]](0x6B0A, 0x9650)
            $dialog = [IntPtr]::Zero
            $deadline = (Get-Date).AddSeconds(15)
            while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
                Start-Sleep -Milliseconds 200
                $script:hit = [IntPtr]::Zero
                $cb = [RP.W+EnumProc] {
                    param([IntPtr] $h, [IntPtr] $p)
                    if ([RP.W]::IsWindowVisible($h) -and (Get-Class $h) -eq 'WinUIDesktopWin32WindowClass' -and
                        ((Get-Text $h).Contains($permissionsTitle) -or (Get-Text $h).StartsWith('Permissions'))) { $script:hit = $h; return $false }
                    return $true
                }
                [void][RP.W]::EnumWindows($cb, [IntPtr]::Zero)
                $dialog = $script:hit
            }
            Write-Output ("SHEET proxy still visible while the editor is open: " + [RP.W]::IsWindowVisible($proxy))
            if ($dialog -ne [IntPtr]::Zero) {
                Write-Output ("PERMISSIONS proxy 0x{0:X} '{1}'" -f $dialog.ToInt64(), (Get-Text $dialog))
                Start-Sleep -Seconds 2
                Save-Shot $dialog 'permissions.png'
                $cells = @(($A::FromHandle($dialog)).FindAll([System.Windows.Automation.TreeScope]::Descendants, $toggleCondition)) |
                    Where-Object { $_.Current.IsEnabled }
                Write-Output "PERMISSIONS check boxes enabled: $($cells.Count)"
                $cell = $cells | Where-Object { $_.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq 'Off' } | Select-Object -First 1
                if ($cell) {
                    $cellToggle = $cell.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
                    $cellToggle.Toggle()
                    $states = foreach ($at in 50, 300, 1000) { Start-Sleep -Milliseconds $at; "$($cellToggle.Current.ToggleState)" }
                    Write-Output "PERMISSION '$($cell.Current.Name)' toggled: $($states -join ' -> ')"
                    Save-Shot $dialog 'permissions-toggled.png'
                }
                $cancelLabel = -join [char[]](0x53D6, 0x6D88)
                $dialogCancel = @(($A::FromHandle($dialog)).FindAll([System.Windows.Automation.TreeScope]::Descendants, $buttonCondition)) |
                    Where-Object { $_.Current.Name -eq $cancelLabel -or $_.Current.Name -eq 'Cancel' } | Select-Object -First 1
                if ($dialogCancel) {
                    $clock = [Diagnostics.Stopwatch]::StartNew()
                    $dialogCancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                    while ([RP.W]::IsWindowVisible($dialog) -and $clock.ElapsedMilliseconds -lt 5000) { Start-Sleep -Milliseconds 20 }
                    Write-Output "PERMISSIONS Cancel closed the projected dialog after $($clock.ElapsedMilliseconds) ms"
                }
            } else { Write-Output 'PERMISSIONS the editor was never projected' }
            Start-Sleep -Seconds 2
            Write-Output ("SHEET proxy visible after the editor closed: " + [RP.W]::IsWindowVisible($proxy))
        } else { Write-Output 'EDIT button not found on Security' }
    }

    # --- cancel through the projection ----------------------------------------
    $el = $A::FromHandle($proxy)
    $buttons = @($el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button))))
    # Windows PowerShell reads a BOM-less script as ANSI, so the localized label is built from its code points.
    $cancelLabel = -join [char[]](0x53D6, 0x6D88)
    $cancel = $buttons | Where-Object { $_.Current.Name -eq $cancelLabel -or $_.Current.Name -eq 'Cancel' } | Select-Object -First 1
    if ($cancel) {
        Write-Output "INVOKE '$($cancel.Current.Name)'"
        $cancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Start-Sleep -Seconds 3
        Write-Output ("AFTER CANCEL proxy visible=" + [RP.W]::IsWindowVisible($proxy))
    }
} finally {
    Write-Output ''
    Write-Output '=== Bridge log: gate, reconcile, rollback ==='
    $bridge = Join-Path $env:TEMP 'FluentShell.log'
    if (Test-Path $bridge) {
        Select-String -Path $bridge -Pattern 'gate rejected|Capture rejected|Reconcile capture|Restoring native|rolled|projected|Projection|scope|verified after|action rejected|same button|Native press:' |
            ForEach-Object { $_.Line } | Select-Object -Last 30
    }
    $hostProc | Stop-Process -Force -EA SilentlyContinue
    Get-Process FluentShell.Renderer -EA SilentlyContinue | Where-Object { $_.Parent.Id -eq $hostProc.Id } |
        Stop-Process -Force -EA SilentlyContinue
    Remove-Item Env:FS_PROPS_FOLDER -EA SilentlyContinue
}
