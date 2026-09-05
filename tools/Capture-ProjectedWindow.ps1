# Captures the projected proxy window to a PNG so the Fluent chrome can be
# inspected as evidence.  Diagnostic only.
param(
    [Parameter(Mandatory = $true)][string] $OutputPath,
    [string] $Title = '',
    # Captures a native window of another process instead of the projected proxy,
    # which is how the native original and its projection can be compared.
    [string] $ProcessName = 'FluentShell.Renderer'
)

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace ShotProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'

$renderer = Get-Process $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } |
    Where-Object { $Title -eq '' -or $_.MainWindowTitle -eq $Title } |
    Select-Object -First 1
if ($null -eq $renderer) { "no $ProcessName window"; exit 1 }

$hwnd = $renderer.MainWindowHandle
[void][ShotProbe.Win32]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 700
$rect = New-Object ShotProbe.Win32+RECT
[void][ShotProbe.Win32]::GetWindowRect($hwnd, [ref] $rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { 'projected window has no bounds'; exit 1 }

$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
$graphics.Dispose()
$bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
"captured '{0}' {1}x{2} -> {3}" -f $renderer.MainWindowTitle, $width, $height, $OutputPath
