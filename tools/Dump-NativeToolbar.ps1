# Reports what a native ToolbarWindow32 says about itself: its button table, the
# image list ids its buttons reference, and how many images each of those lists owns.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [int] $Index = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -Namespace TbProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("comctl32.dll")] public static extern int ImageList_GetImageCount(IntPtr list);
public delegate bool EnumProc(IntPtr window, IntPtr param);
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][TbProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$script:found = New-Object System.Collections.Generic.List[IntPtr]
$find = [TbProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq 'ToolbarWindow32' -and [TbProbe.Win32]::IsWindowVisible($window)) {
        $script:found.Add($window)
    }
    return $true
}
[void][TbProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)
if ($script:found.Count -le $Index) { "no visible toolbar at index $Index"; exit 1 }
$toolbar = $script:found[$Index]

$TB_BUTTONCOUNT = 0x0418
$TB_GETBUTTON = 0x0417
$TB_GETIMAGELIST = 0x0431
$count = [int][TbProbe.Win32]::SendMessage($toolbar, $TB_BUTTONCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
"toolbar 0x{0:X} id={1} buttons={2}" -f [int64] $toolbar, [TbProbe.Win32]::GetDlgCtrlID($toolbar), $count

for ($listId = 0; $listId -lt 4; ++$listId) {
    $list = [TbProbe.Win32]::SendMessage($toolbar, $TB_GETIMAGELIST, [IntPtr] $listId, [IntPtr]::Zero)
    if ($list -ne [IntPtr]::Zero) {
        "  imageList[{0}] = 0x{1:X} images={2}" -f $listId, [int64] $list,
            [TbProbe.Win32]::ImageList_GetImageCount($list)
    }
}

# TBBUTTON on x64: int iBitmap; int idCommand; byte fsState; byte fsStyle; byte[6] pad; IntPtr dwData; IntPtr iString
$size = 32
$buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size)
try {
    for ($index = 0; $index -lt $count; ++$index) {
        [System.Runtime.InteropServices.Marshal]::Copy((New-Object byte[] $size), 0, $buffer, $size)
        $ok = [TbProbe.Win32]::SendMessage($toolbar, $TB_GETBUTTON, [IntPtr] $index, $buffer)
        if ($ok -eq [IntPtr]::Zero) { "  button $index read failed"; continue }
        $iBitmap = [System.Runtime.InteropServices.Marshal]::ReadInt32($buffer, 0)
        $idCommand = [System.Runtime.InteropServices.Marshal]::ReadInt32($buffer, 4)
        $fsState = [System.Runtime.InteropServices.Marshal]::ReadByte($buffer, 8)
        $fsStyle = [System.Runtime.InteropServices.Marshal]::ReadByte($buffer, 9)
        $dwData = [System.Runtime.InteropServices.Marshal]::ReadIntPtr($buffer, 16)
        "  button {0}: iBitmap={1} (list={2} image={3}) id={4} state=0x{5:X2} style=0x{6:X2} data=0x{7:X}" -f
            $index, $iBitmap, ($iBitmap -shr 16), ($iBitmap -band 0xFFFF), $idCommand, $fsState, $fsStyle, [int64] $dwData
    }
}
finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
}
