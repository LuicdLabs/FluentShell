# MMC validation matrix

The requested scope includes the empty console and every installed MMC console,
including Group Policy, Local Security Policy, and Computer Management. A shared
adapter regression passing does not establish that every view in a snap-in works.

## Current implementation and evidence

- Deferred accessible-island actions are agent-owned and token-addressed. Source-
  thread tests cover cancellation, duplicate delivery, busy refusal, current MSAA
  enabledness, and disabled or hidden HWNDs. Every uncloak invalidates queued work.
- MDI caption style bits no longer become dialog tab stops while a modal disables
  the owner. Real HWND tests cover separate MDI traversal scopes and restoration.
- TreeView and ListView icons are compacted from their referenced native indexes,
  including selected TreeView images. The protocol permits up to 8,192 entries
  within a 1 MiB decoded-pixel budget. Tests cover 140 native entries, distinct
  pixels, changed references, callback providers, and resource bounds. TreeView's
  zero-extended `I_IMAGENONE` value is normalized before validating indexes.
- Built-in Static frames and fills have an explicit inert `staticDecoration`
  contract. Frame interiors remain transparent; native edge styles are retained.
  Tests cover the Add/Remove Columns `0x50001007 / 0x00020004` shape, seven drawing
  types, action refusal, protocol checks, and presentation updates.
- `AMCCustomTab` now uses its MSAA page-tab list, labels, canonical selection and
  native `Switch` action. The renderer exposes Tab/TabItem selection semantics.
  The real 73/70-pixel tabs overlap by 8 pixels at their slanted edges; that
  geometry is retained. Tests cover mixed roles, stale identity, disabled items,
  selected-state updates, bounds, and the actual edge-overlap layout.
- MMC's `mmcndmgr.dll/views.htm` extended-view resource has a bounded read-only
  text adapter. Other documents, interactive details, unsupported embedded
  objects, and unrepresented scrolling remain outside this adapter. Typography,
  selected long descriptions and DPI still need presentation validation.
- Discovery excludes native tooltip and MSHTML OLE-channel helper HWNDs.
- ListView captures report, large-icon, small-icon, and list modes. Non-report
  views retain native item rectangles and their normal/small image lists without
  synthetic report columns. Item activation uses stable native IDs, a canonical
  fingerprint, and deferred source-thread MSAA actions. Unmanifested legacy
  controls retain selection/display support when stable activation IDs are absent.
- Toolbar CHECKGROUP buttons have explicit radio-group identity and native
  exclusive selection before command notification.
- The source-command guard defers owned island, toolbar-menu and ListView tokens
  when providers pump messages during capture. Failed commands cancel their tokens.
- These protocol additions use minor 23. Bridge and Renderer must be deployed
  together.

These changes address specific failures recorded in earlier runs. Startup CLI
probes have exercised the real installed snap-ins, but this revision has not yet
received live click-through validation on their views and dialogs. The
available tools in this chat do not expose the Computer Use `node_repl` runtime.
Build and regression results must be kept separate from that missing evidence.

Before the page-tab work, verified on 2026-09-28: 339 Renderer tests passed; the full native protocol suite
passed; Release Bridge/Injector built and Renderer published; `test.ps1` reported
all dependency, export, CLI, and production-layout gates passed. The transcript is
`build/mmc-expanded-final-gates.log`.

The page-tab revision passed 345 Renderer tests and the native suite. Additional
HTML guards, disabled-action handling, viewport scope and menu identity work must
be rebuilt and checked together before those results describe the final revision.

## Startup observations

`tests/Probe-MmcStartup.ps1 -Consoles all` starts disposable MMC processes and
invokes the production Injector. It records startup projection, later rollback,
and per-console logs without navigating or changing configuration. The current
probe requires process-tagged Bridge logs; do not run it with older binaries.
Run native tests and startup probes sequentially to avoid timing interference.
Use `-ShowConsole` for the empty console: it honors a hidden launch and otherwise
never supplies a visible projection candidate. That option displays and closes
the disposable interactive console used by the probe.

The 2026-09-29 checks in `build/mmc-startup-20260929-041651/` and
`build/mmc-startup-20260929-044654/` confirmed startup projection for `secpol`,
`devmgmt`, and `compmgmt`. They exposed the legacy HTML BODY's 16-pixel blank
overflow and Component Services' CHECKGROUP toolbar after ListView capture passed.
The subsequent clipping/group fixes require a fresh production probe.

The 2026-09-28 batch is in `build/mmc-startup-20260928-195729/`. It predates the
latest HTML guards, viewport and menu fixes. Native tests overlapped the
`DevModeRunAsUserConfig` observation, so its failure needs an isolated repeat.

| Console | Last observed startup result |
| --- | --- |
| Empty MMC | Hidden launch produced no visible projection candidate |
| `gpedit`, `services` | Projected, including extended-view text and page tabs |
| `compmgmt`, `certlm`, `certmgr`, `fsmgmt`, `lusrmgr`, `printmanagement` | Projected |
| `secpol` | Projected, then restored after dynamic menu command identity failure |
| `devmgmt` | Capture passed; committed UIA viewport scope validation failed |
| `comexp` | Non-report `SysListView32` rejected |
| `azman`, `WmiMgmt` | ATL document/information pane rejected |
| `diskmgmt` | Custom graphical disk view/legend rejected |
| `eventvwr`, `taskschd`, `WF` | Embedded Windows Forms child belongs to another UI thread |
| `perfmon` | HTML host mixes semantic content and a native performance report control |
| `rsop` | Startup dialog source-thread acknowledgement timed out |
| `DevModeRunAsUserConfig` | Repeat required in isolation |
| `tpm` | ATL pane, disabled Actions entries, and owner-drawn Static need coverage |

Individual projection logs, including `build/mmc-gpedit-tabs-html.log`, establish
that the committed gate succeeded. They do not establish every nested page or
command. Keep failures explicit until a fresh observation confirms the fix.

## Installed console coverage

Inventory: 21 top-level `C:\Windows\System32\*.msc` files on 2026-09-28.
Language copies and DriverStore files are excluded. The empty console is an
additional target. Every row remains pending live validation for this revision.

| Console | Views that must be exercised | Live validation |
| --- | --- | --- |
| Empty `mmc.exe` | Menus, toolbars, tree, Actions pane, Add/Remove Snap-in, Add/Remove Columns, Open/Cancel | Pending |
| `gpedit.msc` | Computer/User Configuration, policy categories, details, policy properties and Cancel | Pending |
| `secpol.msc` | Security categories, policy lists, properties and Cancel | Pending |
| `compmgmt.msc` | System Tools, Storage, Services and Applications; each embedded snap-in | Pending |
| `azman.msc` | Authorization tree, details, properties | Pending |
| `certlm.msc` | Computer certificate stores, certificate list and viewer | Pending |
| `certmgr.msc` | User certificate stores, certificate list and viewer | Pending |
| `comexp.msc` | Component Services hierarchy and details | Pending |
| `devmgmt.msc` | Device hierarchy, categories and property sheets | Pending |
| `DevModeRunAsUserConfig.msc` | Available local pages and dialogs | Pending |
| `diskmgmt.msc` | Volume list, graphical disk view and property dialogs | Pending |
| `eventvwr.msc` | Log tree, event lists, event details and filters | Pending |
| `fsmgmt.msc` | Shares, Sessions, Open Files and properties | Pending |
| `lusrmgr.msc` | Users, Groups and property dialogs | Pending |
| `perfmon.msc` | Monitoring graph, collector sets and reports | Pending |
| `printmanagement.msc` | Print servers, printer lists and properties | Pending |
| `rsop.msc` | Available policy results, details and properties | Pending |
| `services.msc` | Service list, extended/standard views and properties | Pending |
| `taskschd.msc` | Library, task lists, details, history and properties | Pending |
| `tpm.msc` | Status, information and available dialogs | Pending |
| `WF.msc` | Profiles, rules, monitoring and property dialogs | Pending |
| `WmiMgmt.msc` | Local WMI node and property pages | Pending |

For each row, record native capture/admission, committed WinUI projection, tree
selection/expansion, list columns and sorting, menus/toolbars/Actions, keyboard
navigation, modal ownership, resize/DPI behavior, and restore/unload. An Injector
connection alone is not a successful projection. A rejected view must record its
control class, native styles, and capture/validation error before adding an adapter.

Tests should inspect and cancel configuration dialogs without changing policies,
services, accounts, disks, certificates, firewall rules, or scheduled tasks. A
configuration write is not necessary to validate the UI translation contract.

Logs for the current implementation are under `build/mmc-expanded-*`. Keep this
matrix pending until target observations, including nested dialogs and recovery,
justify marking individual rows complete.

## Remaining interaction and architecture work

### Stability follow-up on 2026-09-30

Implemented, but awaiting compilation and runtime verification:

- Source hooks and subclass callbacks retain shared agent ownership through
  native/modal callbacks, including a timed-out command that pumps nested shutdown.
- Toolbar-menu actions carry their canonical binding generation. Refresh followed
  by failed recapture no longer publishes the old snapshot against new bindings.
- Renderer presentation lifetimes detach old view-model subscriptions and block
  stale control/menu/popup callbacks after rebuild, DPI changes and retirement.
- ListView range selection is bounded after item removal. Toolbar commands restore
  canonical checked state after refusal or an unchanged acknowledgement.
- The startup probe supports repetitions and tracks exit of the exact Renderer
  children created by each disposable MMC process, with PID/creation/path checks.

New native tests are integrated from `SourceThreadStabilityRegressionTests.h` and
`ToolbarRadioGroupTests.h`. New Renderer lifecycle and canonical-toolbar tests are
included automatically by the SDK project. The prior Renderer revision passed
362 tests; that result does **not** verify these latest changes.

Validation is currently blocked by Windows process initialization failure
`0xC0000142` (`-1073741502`). PowerShell, Windows PowerShell and cmd all fail before
executing a command. The last native/Bridge build exposed an incorrectly placed
toolbar radio-group hash and a missing WinRT collections include; both were fixed
in source, but could not be rebuilt after the environment failure. No current
native test or full-MMC stability pass is claimed.

After process launch recovers, build/test first, then run probes sequentially:

```powershell
# From E:\FluentShell. Do not overlap native tests and interactive startup probes.
.\build.ps1 -Configuration Release
.\test.ps1 -Configuration Release
.\tests\Probe-MmcStartup.ps1 -Consoles @('empty','gpedit','secpol','compmgmt','services','devmgmt','comexp') -ShowConsole -Repetitions 3 -AccessibilityDiagnostics
.\tests\Probe-MmcStartup.ps1 -Consoles all -ShowConsole -AccessibilityDiagnostics
```

Check projection, later rollback, and Renderer exit results separately. Even
successful repeated startups do not establish navigation, dialogs, DPI transitions,
or all unsupported snap-in adapters.

### Still pending

- Non-report proxy scrolling still needs a native scroll action. Ctrl/Shift list
  navigation does not yet synchronize native focus separately from selection.
- A new selection followed immediately by activation can race revisions; the
  activation is safely refused. An unsent activation intention needs stable item
  identity before it can wait for selection acknowledgement without retargeting.
- Owned deferred actions have a bounded-command guard. Older raw HWND message
  routes still need review for providers that pump messages during final capture.
- Event Viewer, Task Scheduler and Firewall require coordinated capture across
  their GUI threads. Removing the ownership guard is insufficient: reads, actions,
  subclass lifecycle and commit validation must run on each HWND's owner thread,
  with worker-side coordination and whole-window rollback.
- HTML typography, padding, selected-description wrapping and DPI parity remain
  pending even when the native text fits its captured rectangles.
