# Read-only MSHTML and embedded System Monitor Automation evidence for one PID.
# Never invokes HTML click/script functions or System Monitor mutation methods.
param([Parameter(Mandatory = $true)][int] $ProcessId)
$ErrorActionPreference = 'Stop'

Add-Type -Namespace NativeHtmlProbe -Name Win32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder text, int count);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
[DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
[DllImport("oleacc.dll")] public static extern int ObjectFromLresult(IntPtr result, ref Guid iid, IntPtr wparam, [MarshalAs(UnmanagedType.Interface)] out object document);
public delegate bool EnumProc(IntPtr window, IntPtr param);
'@

function Read-ComProperty([object] $Object, [string] $Name, [object[]] $Arguments = @()) {
    if ($null -eq $Object) { throw "Property $Name has no COM object." }
    # Keep COM collections intact; PowerShell otherwise enumerates document.all
    # into Object[] before its length and item properties can be inspected.
    return ,($Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::GetProperty, $null, $Object, $Arguments))
}

$target = Get-Process -Id $ProcessId
$started = $target.StartTime.ToUniversalTime().ToString('o')
$windows = [Collections.Generic.List[IntPtr]]::new()
$collect = [NativeHtmlProbe.Win32+EnumProc] {
    param($window, $parameter)
    $owner = [uint32]0
    [void][NativeHtmlProbe.Win32]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -eq $ProcessId) {
        $name = [Text.StringBuilder]::new(128)
        [void][NativeHtmlProbe.Win32]::GetClassName($window, $name, $name.Capacity)
        if ($name.ToString() -eq 'Internet Explorer_Server') { $windows.Add($window) }
    }
    return $true
}
$enumerate = [NativeHtmlProbe.Win32+EnumProc] {
    param($window, $parameter)
    $owner = [uint32]0
    [void][NativeHtmlProbe.Win32]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -eq $ProcessId) { [void][NativeHtmlProbe.Win32]::EnumChildWindows($window, $collect, [IntPtr]::Zero) }
    return $true
}
[void][NativeHtmlProbe.Win32]::EnumWindows($enumerate, [IntPtr]::Zero)
$records = @()
foreach ($window in $windows) {
    $result = [IntPtr]::Zero
    $message = [NativeHtmlProbe.Win32]::RegisterWindowMessage('WM_HTML_GETOBJECT')
    if ([NativeHtmlProbe.Win32]::SendMessageTimeout($window, $message, [IntPtr]::Zero, [IntPtr]::Zero, 2, 1500, [ref]$result) -eq [IntPtr]::Zero) {
        $records += [pscustomobject]@{ Hwnd = ('0x{0:X}' -f $window.ToInt64()); Error = 'WM_HTML_GETOBJECT timed out or failed' }
        continue
    }
    $iid = [Guid]'332C4425-26CB-11D0-B483-00C04FD90119'
    $document = $null
    $status = [NativeHtmlProbe.Win32]::ObjectFromLresult($result, [ref]$iid, [IntPtr]::Zero, [ref]$document)
    if ($status -ne 0 -or $null -eq $document) {
        $records += [pscustomobject]@{ Hwnd = ('0x{0:X}' -f $window.ToInt64()); Error = "ObjectFromLresult: $status" }
        continue
    }
    try {
        $body = Read-ComProperty $document 'body'
        $markup = [string](Read-ComProperty $body 'outerHTML')
        $objects = @()
        $all = Read-ComProperty $document 'all'
        $count = [int](Read-ComProperty $all 'length')
        for ($index = 0; $index -lt [Math]::Min($count, 256); $index++) {
            $element = Read-ComProperty $all 'item' @($index, $null)
            if ((Read-ComProperty $element 'tagName') -ne 'OBJECT') { continue }
            $objectRecord = [ordered]@{ Id = [string](Read-ComProperty $element 'id'); ClassId = [string](Read-ComProperty $element 'classid') }
            try {
                $automation = Read-ComProperty $element 'object'
                foreach ($property in @('ReadOnly', 'DisplayType', 'ReportValueType', 'ShowToolbar', 'ShowLegend', 'ShowValueBar', 'GraphTitle')) {
                    try { $objectRecord[$property] = Read-ComProperty $automation $property }
                    catch { $objectRecord[$property] = $_.Exception.Message }
                }
                $counters = Read-ComProperty $automation 'Counters'
                $counterCount = [int](Read-ComProperty $counters 'Count')
                $objectRecord['CounterCount'] = $counterCount
                $rows = @()
                for ($counterIndex = 1; $counterIndex -le [Math]::Min($counterCount, 256); $counterIndex++) {
                    $counter = Read-ComProperty $counters 'Item' @($counterIndex)
                    $row = [ordered]@{}
                    foreach ($property in @('Path', 'Value', 'Visible', 'Selected')) {
                        try { $row[$property] = Read-ComProperty $counter $property }
                        catch { $row[$property] = $_.Exception.Message }
                    }
                    $rows += [pscustomobject]$row
                }
                $objectRecord['Counters'] = $rows
            } catch { $objectRecord['Error'] = $_.Exception.Message }
            $objects += [pscustomobject]$objectRecord
        }
        $records += [pscustomobject]@{
            Hwnd = ('0x{0:X}' -f $window.ToInt64())
            Url = [string](Read-ComProperty $document 'URL')
            ReadyState = [string](Read-ComProperty $document 'readyState')
            ElementCount = $count
            Body = $markup.Substring(0, [Math]::Min($markup.Length, 262144))
            BodyTruncated = $markup.Length -gt 262144
            Objects = $objects
        }
    } catch {
        $records += [pscustomobject]@{ Hwnd = ('0x{0:X}' -f $window.ToInt64()); Error = $_.Exception.Message }
    } finally {
        if ($null -ne $document -and [Runtime.InteropServices.Marshal]::IsComObject($document)) {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($document)
        }
    }
}
[pscustomobject]@{ ProcessId = $ProcessId; StartTimeUtc = $started; Image = $target.Path; Documents = $records } | ConvertTo-Json -Depth 10
