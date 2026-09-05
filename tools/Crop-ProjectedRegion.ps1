# Captures a projected window and writes cropped regions at full resolution, so a
# rendering defect can be inspected without the whole frame being downscaled.
# Regions are given in physical client pixels as "name=x,y,w,h" separated by ';'.
#
# Diagnostic only.
param(
    [string] $ProcessName = 'FluentShell.Renderer',
    [Parameter(Mandatory = $true)][string] $OutputPrefix,
    [Parameter(Mandatory = $true)][string] $Regions
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace CropProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref POINT point);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
'

# Physical pixels only: a virtualized caller would crop the wrong rectangle.
[void][CropProbe.Win32]::SetThreadDpiAwarenessContext([IntPtr](-4))

$process = Get-Process -Name $ProcessName -ErrorAction Stop |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if ($null -eq $process) { 'no window found'; exit 1 }
$window = $process.MainWindowHandle

$client = New-Object CropProbe.Win32+RECT
[void][CropProbe.Win32]::GetClientRect($window, [ref] $client)
$origin = New-Object CropProbe.Win32+POINT
[void][CropProbe.Win32]::ClientToScreen($window, [ref] $origin)
"client {0}x{1} at screen {2},{3}" -f ($client.Right - $client.Left),
    ($client.Bottom - $client.Top), $origin.X, $origin.Y

foreach ($region in ($Regions -split ';')) {
    if ([string]::IsNullOrWhiteSpace($region)) { continue }
    $parts = $region -split '='
    $name = $parts[0].Trim()
    $values = @($parts[1].Trim() -split ',' | ForEach-Object { [int] $_.Trim() })
    $bitmap = New-Object System.Drawing.Bitmap($values[2], $values[3])
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen(
        ($origin.X + $values[0]), ($origin.Y + $values[1]), 0, 0,
        (New-Object System.Drawing.Size($values[2], $values[3])))
    $graphics.Dispose()
    $path = "$OutputPrefix-$name.png"
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    "wrote $path"
}
