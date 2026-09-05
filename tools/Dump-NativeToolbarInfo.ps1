# Reports what a native ToolbarWindow32 says about each button through
# TB_GETBUTTONINFOW, which avoids marshalling the TBBUTTON layout by hand: the style
# bits are what decide whether a click sends WM_COMMAND or asks the owner for a menu.
#
# Diagnostic only.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [int] $Index = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -Namespace TbInfo -Name Win32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, ref TBBUTTONINFO info);
public delegate bool EnumProc(IntPtr window, IntPtr param);
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct TBBUTTONINFO {
    public uint cbSize;
    public uint dwMask;
    public int idCommand;
    public int iImage;
    public byte fsState;
    public byte fsStyle;
    public ushort cx;
    public IntPtr lParam;
    public IntPtr pszText;
    public int cchText;
}
'@

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][TbInfo.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
$script:found = New-Object System.Collections.Generic.List[IntPtr]
$find = [TbInfo.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq 'ToolbarWindow32' -and [TbInfo.Win32]::IsWindowVisible($window)) {
        $script:found.Add($window)
    }
    return $true
}
[void][TbInfo.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)
if ($script:found.Count -le $Index) { "no visible toolbar at index $Index"; exit 1 }
$toolbar = $script:found[$Index]

$TB_BUTTONCOUNT = 0x0418
$TB_GETBUTTONINFOW = 0x0441
$TB_GETEXTENDEDSTYLE = 0x0455
$TBIF_IMAGE = 0x01
$TBIF_STATE = 0x04
$TBIF_STYLE = 0x08
$TBIF_COMMAND = 0x20
$TBIF_TEXT = 0x02
$TBIF_BYINDEX = [uint32] 2147483648

$count = [int][TbInfo.Win32]::SendMessage($toolbar, $TB_BUTTONCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
$exStyle = [int][TbInfo.Win32]::SendMessage($toolbar, $TB_GETEXTENDEDSTYLE, [IntPtr]::Zero, [IntPtr]::Zero)
"toolbar 0x{0:X} id={1} buttons={2} extendedStyle=0x{3:X}" -f `
    $toolbar.ToInt64(), [TbInfo.Win32]::GetDlgCtrlID($toolbar), $count, $exStyle

$buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal(512)
try {
    for ($index = 0; $index -lt $count; $index++) {
        $info = New-Object TbInfo.Win32+TBBUTTONINFO
        $info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
        $info.dwMask = ([uint32]($TBIF_IMAGE -bor $TBIF_STATE -bor $TBIF_STYLE -bor $TBIF_COMMAND -bor
            $TBIF_TEXT)) -bor $TBIF_BYINDEX
        $info.pszText = $buffer
        $info.cchText = 200
        $result = [TbInfo.Win32]::SendMessage($toolbar, $TB_GETBUTTONINFOW, [IntPtr] $index, [ref] $info)
        if ($result.ToInt64() -lt 0) { "  button $index read failed"; continue }
        $text = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($buffer)
        $styles = New-Object System.Collections.Generic.List[string]
        if ($info.fsStyle -band 0x01) { $styles.Add('BUTTON/SEP?') }
        if ($info.fsStyle -band 0x02) { $styles.Add('CHECK') }
        if ($info.fsStyle -band 0x04) { $styles.Add('GROUP') }
        if ($info.fsStyle -band 0x08) { $styles.Add('DROPDOWN') }
        if ($info.fsStyle -band 0x10) { $styles.Add('AUTOSIZE') }
        if ($info.fsStyle -band 0x20) { $styles.Add('NOPREFIX') }
        if ($info.fsStyle -band 0x40) { $styles.Add('SHOWTEXT') }
        if ($info.fsStyle -band 0x80) { $styles.Add('WHOLEDROPDOWN') }
        "  [{0}] cmd={1,-5} image={2,-6} state=0x{3:X2} style=0x{4:X2} ({5}) text='{6}'" -f `
            $index, $info.idCommand, $info.iImage, $info.fsState, $info.fsStyle,
            ($styles -join '|'), $text
    }
}
finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
}
