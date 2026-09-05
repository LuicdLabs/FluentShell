# Renders a single native child window to a PNG through PrintWindow, which is how
# a private class reveals whether it paints content of its own or only frames its
# children.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [Parameter(Mandatory = $true)][string] $ClassName,
    [Parameter(Mandatory = $true)][string] $OutputPath,
    [int] $Index = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace PrintProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
public delegate bool EnumProc(IntPtr window, IntPtr param);
public struct RECT { public int Left, Top, Right, Bottom; }
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][PrintProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$script:matches = New-Object System.Collections.Generic.List[IntPtr]
$find = [PrintProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq $ClassName -and [PrintProbe.Win32]::IsWindowVisible($window)) {
        $script:matches.Add($window)
    }
    return $true
}
[void][PrintProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)
if ($script:matches.Count -le $Index) { "no visible '$ClassName' at index $Index"; exit 1 }
$target = $script:matches[$Index]

$rect = New-Object PrintProbe.Win32+RECT
[void][PrintProbe.Win32]::GetWindowRect($target, [ref] $rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { 'window has no bounds'; exit 1 }

$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$dc = $graphics.GetHdc()
# PW_RENDERFULLCONTENT (2) also renders windows that are cloaked or composited.
$printed = [PrintProbe.Win32]::PrintWindow($target, $dc, 2)
$graphics.ReleaseHdc($dc)
$graphics.Dispose()
$bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)

# Report how uniform the surface is: a container that only frames its children
# prints as a single colour.
$colours = @{}
for ($y = 0; $y -lt $height; $y += [Math]::Max(1, [int]($height / 64))) {
    for ($x = 0; $x -lt $width; $x += [Math]::Max(1, [int]($width / 64))) {
        $key = $bitmap.GetPixel($x, $y).ToArgb()
        $colours[$key] = $true
    }
}
$bitmap.Dispose()
"printed={0} 0x{1:X} '{2}' {3}x{4} distinctSampledColours={5} -> {6}" -f
    $printed, [int64] $target, $ClassName, $width, $height, $colours.Count, $OutputPath
