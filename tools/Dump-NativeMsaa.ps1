# Probes the MSAA (IAccessible) contract of a native window's accessible tree.
# UIA's LegacyIAccessible pattern being absent does not mean the provider has no
# IAccessible: DirectUI answers WM_GETOBJECT directly, and accDoDefaultAction is a
# documented action route where UIA exposes none.
param(
    [Parameter(Mandatory = $true)][string] $ProcessName,
    # Optional with -Hwnd, which names the window itself (hex, as Dump-WindowTree prints it).
    [string] $ClassName = '',
    # Inspect exactly this window instead of searching the main window's children by class:
    # a dialog is its own top-level window, which the class search never reaches.
    [string] $Hwnd = '',
    [int] $MaxNodes = 40,
    [int] $ProcessId = 0,
    [switch] $AllMatches,
    [int] $MaxDepth = 12
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName Accessibility
Add-Type -Namespace MsaaProbe -Name Win32 -MemberDefinition '
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll")] public static extern IntPtr GetClassLongPtrW(IntPtr window, int index);
[DllImport("oleacc.dll")] public static extern int AccessibleObjectFromWindow(IntPtr window, uint objectId, ref System.Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object accessible);
[DllImport("oleacc.dll")] public static extern int AccessibleChildren([MarshalAs(UnmanagedType.IUnknown)] object container, int start, int count, [Out, MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.Struct)] object[] children, out int obtained);
[DllImport("oleacc.dll", CharSet = CharSet.Unicode)] public static extern uint GetRoleText(uint role, System.Text.StringBuilder text, uint max);
public delegate bool EnumProc(IntPtr window, IntPtr param);
'
Add-Type -Namespace MsaaProbe -Name Accessible -ReferencedAssemblies Accessibility -IgnoreWarnings -MemberDefinition '
public static int[] Location(object node, object child) {
    int x, y, width, height;
    ((Accessibility.IAccessible)node).accLocation(out x, out y, out width, out height, child);
    return new[] { x, y, width, height };
}
public static object Focus(object node) { return ((Accessibility.IAccessible)node).accFocus; }
public static object Selection(object node) { return ((Accessibility.IAccessible)node).accSelection; }
'

function Get-Class([IntPtr] $window) {
    $builder = New-Object System.Text.StringBuilder 256
    [void][MsaaProbe.Win32]::GetClassName($window, $builder, $builder.Capacity)
    $builder.ToString()
}

$process = if ($ProcessId -ne 0) { Get-Process -Id $ProcessId -ErrorAction Stop } else {
    $matches = @(Get-Process -Name $ProcessName -ErrorAction Stop)
    if ($matches.Count -ne 1) { throw 'More than one matching process exists; pass -ProcessId.' }
    $matches[0]
}
if ($MaxNodes -lt 1 -or $MaxNodes -gt 4096 -or $MaxDepth -lt 0 -or $MaxDepth -gt 32) {
    throw 'MaxNodes must be 1..4096 and MaxDepth must be 0..32.'
}
$script:targets = [Collections.Generic.List[IntPtr]]::new()
$find = [MsaaProbe.Win32+EnumProc] {
    param($window, $param)
    if ((Get-Class $window) -eq $ClassName -and [MsaaProbe.Win32]::IsWindowVisible($window)) {
        $script:targets.Add($window)
        return [bool]$AllMatches
    }
    return $true
}
if ($Hwnd) {
    $script:targets.Add([IntPtr][Convert]::ToInt64($Hwnd.Replace('0x', ''), 16))
} else {
    if (-not $ClassName) { throw 'Pass -ClassName or -Hwnd.' }
    [void][MsaaProbe.Win32]::EnumChildWindows($process.MainWindowHandle, $find, [IntPtr]::Zero)
}
if ($script:targets.Count -eq 0) { "no visible '$ClassName'"; exit 1 }

function Role-Text([object] $value) {
    if ($value -isnot [int]) { return "$value" }
    $builder = New-Object System.Text.StringBuilder 128
    [void][MsaaProbe.Win32]::GetRoleText([uint32] $value, $builder, [uint32] $builder.Capacity)
    "$($builder.ToString())($value)"
}

$script:count = 0
function Show-Node([object] $node, [object] $child, [int] $depth) {
    if ($script:count -ge $MaxNodes -or $depth -gt $MaxDepth) { return }
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
    $bounds = 'unavailable'
    try {
        # Calling accLocation through late-bound reflection without explicit
        # by-ref argument metadata leaves all four local values at zero.
        $location = [MsaaProbe.Accessible]::Location($node, $child)
        $bounds = '{0},{1} {2}x{3}' -f $location[0], $location[1], $location[2], $location[3]
    } catch { $bounds = 'unavailable (' + $_.Exception.GetBaseException().Message + ')' }
    $childCount = 0
    if ($child -eq 0) {
        try { $childCount = $type.InvokeMember('accChildCount', 'GetProperty', $null, $node, @()) } catch { }
    }
    '{0}child={1} role={2} name="{3}" state={4} children={5} defaultAction="{6}" value="{7}" bounds={8}' -f
        (' ' * ($depth * 2)), $child, (Role-Text $role), $name, $state, $childCount, $action, $value, $bounds
    if ($child -eq 0) {
        try { '{0}  focus={1}' -f (' ' * ($depth * 2)), [MsaaProbe.Accessible]::Focus($node) }
        catch { '{0}  focus=unavailable ({1})' -f (' ' * ($depth * 2)), $_.Exception.GetBaseException().Message }
        try { '{0}  selection={1}' -f (' ' * ($depth * 2)), [MsaaProbe.Accessible]::Selection($node) }
        catch { '{0}  selection=unavailable ({1})' -f (' ' * ($depth * 2)), $_.Exception.GetBaseException().Message }
    }

    if ($child -ne 0) { return }
    if ($childCount -le 0) { return }
    $requested = [Math]::Min([int]$childCount, $MaxNodes - $script:count)
    if ($requested -le 0) { return }
    $children = New-Object object[] $requested
    $obtained = 0
    try {
        $hr = [MsaaProbe.Win32]::AccessibleChildren($node, 0, $requested, $children, [ref] $obtained)
        if ($hr -ne 0) { "{0}  AccessibleChildren failed: 0x{1:X}" -f (' ' * ($depth * 2)), $hr }
    }
    catch { "{0}  AccessibleChildren threw: {1}" -f (' ' * ($depth * 2)), $_.Exception.Message; return }
    for ($index = 0; $index -lt $obtained; ++$index) {
        $entry = $children[$index]
        if ($entry -is [int]) { Show-Node $node $entry ($depth + 1) }
        elseif ($null -ne $entry) { Show-Node $entry 0 ($depth + 1) }
    }
}

foreach ($target in $script:targets) {
    $ownerId = [uint32]0
    $threadId = [MsaaProbe.Win32]::GetWindowThreadProcessId($target, [ref]$ownerId)
    if ($ownerId -ne $process.Id) { throw 'The HWND changed process identity during inspection.' }
    $moduleHandle = [MsaaProbe.Win32]::GetClassLongPtrW($target, -16)
    $module = $process.Modules | Where-Object { $_.BaseAddress -eq $moduleHandle } | Select-Object -First 1
    'HWND=0x{0:X} pid={1} thread={2} class={3} classModule={4}' -f $target.ToInt64(), $ownerId, $threadId, (Get-Class $target), $module.FileName
    $iid = [System.Guid]::Parse('618736e0-3c3d-11cf-810c-00aa00389b71')
    $accessible = $null
    $hr = [MsaaProbe.Win32]::AccessibleObjectFromWindow($target, [uint32]::MaxValue - 3, [ref]$iid, [ref]$accessible)
    if ($hr -ne 0 -or $null -eq $accessible) { 'AccessibleObjectFromWindow failed: 0x{0:X}' -f $hr; continue }
    $script:count = 0
    try { Show-Node $accessible 0 0 }
    finally { if ([Runtime.InteropServices.Marshal]::IsComObject($accessible)) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($accessible) } }
    if ($script:count -ge $MaxNodes) { 'TRUNCATED at MaxNodes={0}' -f $MaxNodes }
}
