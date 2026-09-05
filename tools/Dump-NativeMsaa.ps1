# Probes the MSAA (IAccessible) contract of a native window's accessible tree.
# UIA's LegacyIAccessible pattern being absent does not mean the provider has no
# IAccessible: DirectUI answers WM_GETOBJECT directly, and accDoDefaultAction is a
# documented action route where UIA exposes none.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    [Parameter(Mandatory = $true)][string] $ClassName,
    [int] $MaxNodes = 40
)

$ErrorActionPreference = 'Stop'
Add-Type -Namespace MsaaProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("oleacc.dll")] public static extern int AccessibleObjectFromWindow(IntPtr window, uint objectId, ref System.Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object accessible);
[DllImport("oleacc.dll")] public static extern int AccessibleChildren([MarshalAs(UnmanagedType.IUnknown)] object container, int start, int count, [Out, MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.Struct)] object[] children, out int obtained);
[DllImport("oleacc.dll", CharSet = CharSet.Unicode)] public static extern uint GetRoleText(uint role, System.Text.StringBuilder text, uint max);
public delegate bool EnumProc(IntPtr window, IntPtr param);
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][MsaaProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$script:target = [IntPtr]::Zero
$find = [MsaaProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq $ClassName -and [MsaaProbe.Win32]::IsWindowVisible($window)) {
        $script:target = $window
        return $false
    }
    return $true
}
[void][MsaaProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)
if ($script:target -eq [IntPtr]::Zero) { "no visible '$ClassName'"; exit 1 }

$iid = [System.Guid]::Parse('618736e0-3c3d-11cf-810c-00aa00389b71')  # IID_IAccessible
$accessible = $null
$hr = [MsaaProbe.Win32]::AccessibleObjectFromWindow($script:target, [uint32]::MaxValue - 3, [ref] $iid, [ref] $accessible)
if ($hr -ne 0 -or $null -eq $accessible) { "AccessibleObjectFromWindow failed: 0x{0:X}" -f $hr; exit 1 }

function Role-Text([object] $value) {
    if ($value -isnot [int]) { return "$value" }
    $builder = New-Object System.Text.StringBuilder 128
    [void][MsaaProbe.Win32]::GetRoleText([uint32] $value, $builder, [uint32] $builder.Capacity)
    "$($builder.ToString())($value)"
}

$script:count = 0
function Show-Node([object] $node, [object] $child, [int] $depth) {
    if ($script:count -ge $MaxNodes) { return }
    ++$script:count
    $type = $node.GetType()
    function Get-Prop([string] $name) {
        try { return $type.InvokeMember($name, 'GetProperty', $null, $node, @($child)) }
        catch { return $null }
    }
    $name = Get-Prop 'accName'
    $role = Get-Prop 'accRole'
    $state = Get-Prop 'accState'
    $action = Get-Prop 'accDefaultAction'
    $value = Get-Prop 'accValue'
    $childCount = 0
    if ($child -eq 0) {
        try { $childCount = $type.InvokeMember('accChildCount', 'GetProperty', $null, $node, @()) } catch { }
    }
    '{0}role={1} name="{2}" state={3} children={4} defaultAction="{5}" value="{6}"' -f
        (' ' * ($depth * 2)), (Role-Text $role), $name, $state, $childCount, $action, $value

    if ($child -ne 0) { return }
    if ($childCount -le 0) { return }
    $children = New-Object object[] $childCount
    $obtained = 0
    try {
        $hr = [MsaaProbe.Win32]::AccessibleChildren($node, 0, $childCount, $children, [ref] $obtained)
        if ($hr -ne 0) { "{0}  AccessibleChildren failed: 0x{1:X}" -f (' ' * ($depth * 2)), $hr }
    }
    catch { "{0}  AccessibleChildren threw: {1}" -f (' ' * ($depth * 2)), $_.Exception.Message; return }
    for ($index = 0; $index -lt $obtained; ++$index) {
        $entry = $children[$index]
        if ($entry -is [int]) { Show-Node $node $entry ($depth + 1) }
        elseif ($null -ne $entry) { Show-Node $entry 0 ($depth + 1) }
    }
}

Show-Node $accessible 0 0
