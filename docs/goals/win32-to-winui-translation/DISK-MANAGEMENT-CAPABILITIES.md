# Disk Management graphical view capability evidence

The graphical disk view remains unverified until its running accessibility tree is
captured at the same integrity level as Disk Management. No adapter may derive
disk actions from painted coordinates or replace the MMC view with an independent
WMI/storage query: the existing native view remains the state owner.

## Observed host shape

The recorded startup capture in
`build/mmc-startup-20261003-171847/diskmgmt-bridge.log` rejects two visible
`AfxWnd42u` windows: a scrolling private container (`style=1344274432`) and a
childless private surface (`style=1342177280`). Earlier runs also encountered
zero-sized instances while the snap-in was loading. These logs establish the HWND
boundary, but contain no complete MSAA tree or item action evidence.

## Installed native contracts inspected on 2026-10-04

Read-only PE import inspection and `LoadTypeLibEx(REGKIND_NONE)` found:

| System component | Version | Evidence |
| --- | --- | --- |
| `dmview.ocx` | 10.0.26100.1882 | Imports `AccessibleObjectFromWindow` and `LresultFromObject` from OLEACC. Its `_DDMView2` dispatch interface is `aeb84c81-95dc-11d0-b7fc-b61140119c4a`; the control class is `aeb84c83-95dc-11d0-b7fc-b61140119c4a`. |
| `dmocx.dll` | 10.0.26100.8737 | Imports `CreateStdAccessibleProxyW` and `LresultFromObject`. Its `_DTV` dispatch interface has no automation members. |
| `dmdskmgr.dll` | 10.0.26100.5074 | Exports private MFC cache/component methods, including disk-view refresh and native object pointers; these are not a supported snapshot or action contract. |

`_DDMView2` exposes `LoadData`, `Init`, `AddRow`, `ChangeRow`, `DeleteRow`,
`SetViewType`, `RefreshDiskView`, `GetListViewsWidths`, `UIStateChange`,
`ResetLoadState`, `SysColorChange`, and `ReloadData`. These are feed/update methods,
with no property getters or returned semantic disk/partition collection. None was
invoked. The OLEACC imports make the live MSAA provider the next supported contract
to inspect; import presence alone does not prove that it exposes usable items.

## Required next observation

From an already elevated PowerShell, use the exact PID of the fresh `diskmgmt.msc`
MMC instance. The diagnostic reads state and never invokes an accessible action:

```powershell
.\tools\Dump-WindowTree.ps1 -ProcessName mmc -ProcessId $diskManagementPid
.\tools\Dump-NativeMsaa.ps1 -ProcessName mmc -ProcessId $diskManagementPid `
    -ClassName AfxWnd42u -AllMatches -MaxNodes 256 -MaxDepth 12
```

Record every matching HWND, owning GUI thread, child role, name/value, state,
screen bounds, default action, and whether the output reached its explicit cap.
Both the scroll container and childless painted surface matter. The existing
MSAA diagnostic now selects the exact PID and all class matches instead of the
first unrelated MMC process or first matching container.

Admission requires complete disk/partition membership and identity, visible labels,
selection and focus, scrolling, and a supported command route. If MSAA exposes only
a generic client/proxy object or omits any of those facets, keep the whole native
window and report the missing facet. The installed automation type library does
not fill that gap. Actual command execution and projection/UIA acceptance remain
separate verification steps after read-only capability discovery.
