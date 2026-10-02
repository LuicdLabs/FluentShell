# MMC validation matrix

The requested scope includes the empty console and every installed MMC console,
including Group Policy, Local Security Policy, and Computer Management. A shared
adapter regression passing does not establish that every view in a snap-in works.

## Current implementation and evidence

- Bounded `LVS_OWNERDATA` ListViews now read text with `LVM_GETITEMW` through
  the application's display-info callback. Report, icon, small-icon and list
  modes retain canonical text, images, selection, focus and viewport geometry.
  Native label-edit notifications decide whether renaming succeeds. The existing
  4,096-item, string and aggregate-text limits still apply; unsupported callback
  state images, overlays and virtual checkboxes keep the whole surface native.
- Indexed virtual-list actions carry the published native fingerprint. A fresh
  capture that observes a different ordering refuses the action and publishes the
  canonical patch; the old index is not automatically replayed against new rows.
- Protocol minor 26 sends stable ListView IDs as canonical decimal strings.
  Delayed activation waits for selection and focus of the original native ID.
  Replaced, removed or moved items cancel that intention even when labels match.
  Owner-data lists have no stable-ID activation contract and do not offer it.
- Delayed ListView activation follows actual pending selection/focus events.
  Definitive rejection, an unchanged completed prerequisite, or a later input
  gesture cancels the intention; the allowed stale-replay gap remains pending.
  New event IDs protect a new gesture from late old results. Eleven coordinator
  regressions use the real view-model pending and canonical snapshot paths.
- A ListView provider returning `DISP_E_MEMBERNOTFOUND` can use the native
  control's double-click handler when its current MSAA metadata matches the
  standard provider and the exact selected/focused native item still matches.
  The control's own label/icon hit test authorizes the point. Other failures,
  custom actions, state changes and cancellation do not use this route. The
  operation runs outside the bounded capture command and preserves foreground.
- Plain report ListViews inherit WinUI's virtualizing `ItemsStackPanel` instead
  of locally assigning a pre-theme null `ItemsPanel`. Checkable reports retain
  full realization until their UIA checkbox census has stable row identity.
  Services' 279 rows now realize 20 cached rows at startup; successful Services
  projection still requires the remaining native/gate failures to be resolved.
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
- TreeView state images and composited overlay images are supported, including
  empty trees. ListView focus and native scrolling use the minor 24 routes;
  TreeView imagery uses minor 25 and activation identity uses minor 26.
  Bridge and Renderer must be deployed together.

These changes address specific failures recorded in earlier runs. Startup CLI
probes have exercised the real installed snap-ins, but this revision has not yet
received complete live click-through validation on their views and dialogs.
Startup, regression and interactive evidence are tracked separately below.

The earlier 2026-10-02 minor-26 implementation passed all **395 Renderer tests** and
the full native suite, including the negative-index and provider-failure cases.
The final Release build/publish and production import, export, CLI and payload
layout gates passed. Evidence:

- `build/mmc-20261002-final-renderer-lifecycle-tests.log`
- `build/mmc-20261002-final-activation-native.log`
- `build/mmc-20261002-final-lifecycle-build.log`
- `build/mmc-20261002-final-lifecycle-gates.log`

The earlier 384-test revision also passed the native suite and production gates
before the real-console probes described below. Startup results and subsequent
interaction/lifecycle checks remain separate evidence.

The later native double-click and report virtualization revision also passed
all **395 Renderer tests**, the complete native Release suite, and production
build/publish and architecture gates. The native suite includes **24 fallback
cases** and **8 stock label/icon hit-test cases**. Final evidence:

- `build/mmc-20261002-combined-renderer-tests.log`
- `build/mmc-20261002-native-lifecycle-final-tests.log`
- `build/mmc-20261002-combined-release-build.log`
- `build/mmc-20261002-final-clean-bridge-build.log`
- `build/mmc-20261002-combined-production-gates.log`

## Startup observations

`tests/Probe-MmcStartup.ps1 -Consoles all` starts disposable MMC processes and
invokes the production Injector. It records startup projection, later rollback,
and per-console logs without navigating or changing configuration. The current
probe requires process-tagged Bridge logs; do not run it with older binaries.
Run native tests and startup probes sequentially to avoid timing interference.
Use `-ShowConsole` for the empty console: it honors a hidden launch and otherwise
never supplies a visible projection candidate. That option displays and closes
the disposable interactive console used by the probe.
Use `-RunAsInvoker` to keep MMC at the caller's integrity level when its manifest
would otherwise request elevation. This option grants no administrator rights;
permission-error dialogs can prevent privileged snap-ins from reaching their
normal pages. Launch failures are retained in `results.json` with no Injector
result, and both environment overrides are restored after the probe.

The 2026-10-02 minor-25 baseline is in
`build/mmc-startup-20261002-193648/`: 12 of 22 targets completed startup projection
and Renderer cleanup, nine stayed native, and `services` failed its committed
UIA gate and Renderer heartbeat. Two isolated services repeats in
`build/mmc-startup-20261002-194231/` reproduced that failure. These probes used
`-RunAsInvoker -ShowConsole -AccessibilityDiagnostics` after the Release build,
374 Renderer tests, full native suite and production gates passed. They precede
the virtual-list and stable-ID changes described above.

The same 22 targets were rerun with minor 26 in
`build/mmc-startup-20261002-200307/` after the owner-data and stable-ID changes.
The result remained 12 stable startups with Renderer cleanup, nine capture
rejections and one `services` committed-gate/heartbeat failure. This is startup
coverage, not a claim that every nested view or command works.

The latest all-console probe after native double-click support and report
virtualization is `build/mmc-startup-20261002-221525/`. It again recorded **12
stable startups**, **9 capture rejections**, and **1 Services rollback**; Services
now stopped at source-thread capture-and-cloak acknowledgement before provisional
commit. All 22 exact Renderer children exited within the cleanup deadline. The
command output is `build/mmc-20261002-final-all-consoles.log`.

Three isolated `services` enumeration experiments in
`build/mmc-startup-20261002-200845/`, `build/mmc-startup-20261002-201119/` and
`build/mmc-startup-20261002-201744/` did not resolve the failure. All changes to
`UiAutomationValidator.cpp` from those experiments were reverted and the
production Bridge rebuilt. No timeout increase or gate reduction was retained.

The 2026-09-29 checks in `build/mmc-startup-20260929-041651/` and
`build/mmc-startup-20260929-044654/` confirmed startup projection for `secpol`,
`devmgmt`, and `compmgmt`. They exposed the legacy HTML BODY's 16-pixel blank
overflow and Component Services' CHECKGROUP toolbar after ListView capture passed.
Those startup failures were no longer observed in the 2026-10-02 all-console runs.

The 2026-09-28 batch is in `build/mmc-startup-20260928-195729/`. It predates the
latest HTML guards, viewport and menu fixes. Native tests overlapped the
`DevModeRunAsUserConfig` observation; the 2026-10-02 sequential runs supersede it.

| Console | Last observed startup result |
| --- | --- |
| Empty MMC, `gpedit`, `compmgmt`, `certlm`, `certmgr`, `fsmgmt`, `lusrmgr`, `printmanagement` | Stable startup and Renderer cleanup on 2026-10-02 |
| `secpol`, `devmgmt`, `comexp`, `DevModeRunAsUserConfig` | Stable startup and Renderer cleanup on 2026-10-02 |
| `services` | Latest extended/standard attempts stop at source-thread capture-and-cloak acknowledgement; earlier builds timed out in committed UIA enumeration and Renderer heartbeat |
| `azman`, `WmiMgmt` | ATL document/information pane rejected; MSAA exposes a document with text and graphic children |
| `diskmgmt` | Ordinary-integrity launch has an owned startup dialog, so owner-graph admission stops before the normal disk view; older elevated evidence also names the custom disk view/legend |
| `eventvwr`, `taskschd`, `WF` | Embedded Windows Forms child belongs to another UI thread |
| `perfmon` | HTML host mixes semantic content and a native performance report control |
| `rsop` | Ordinary-integrity startup shows an owned Group Policy error dialog; owner graph remains native |
| `tpm` | ATL pane, disabled Actions entries, and owner-drawn Static need coverage |

Individual projection logs, including `build/mmc-gpedit-tabs-html.log`, establish
that the committed gate succeeded. They do not establish every nested page or
command. Keep failures explicit until a fresh observation confirms the fix.

## Manual interaction evidence on 2026-10-02

Two disposable `compmgmt` processes exercised the published minor-26 build. Logs
are `build/mmc-20261002-manual-bridge.log`,
`build/mmc-20261002-manual-renderer.log` and
`build/mmc-20261002-activation-manual-{bridge,renderer}.log`.

- Root ListView selection succeeded. A focus request raced the selection revision,
  replayed once after its canonical patch and succeeded.
- Selecting Shared Folders in the tree changed the result rows and Actions pane.
  Expanding Shared Folders exposed Shares, Sessions and Open Files.
- Double-clicking System Tools in the result list did not navigate. The provider
  advertises a double-click action but returns `0x80020003`
  (`DISP_E_MEMBERNOTFOUND`) from `accDoDefaultAction`. Diagnostics now record its
  HRESULT, HWND, node, native ID, index and MSAA child ID. A queued `accepted`
  action result alone does not establish that the deferred provider action ran.
- Selecting Sessions displayed its report columns, then opened an ordinary-
  integrity permission-error dialog. The first committed modal UIA validation
  failed eight times, restored that attempted surface, and a later discovery
  projected the dialog. The tree selection command timed out and was rejected.
  This path is not a successful navigation/modal-ownership acceptance result.
- Both exact disposable targets were terminated after identity checks, and their
  Renderer children exited. No configuration changes were made.

Earlier `S_FALSE` and `DISP_E_MEMBERNOTFOUND` provider regressions established
that failure must not synthesize keyboard or parent-activation notifications.
The later bounded native double-click contract below supersedes the original
missing-method behavior when the standard item metadata matches.

The final lifecycle build was checked again in a third disposable `compmgmt`
process. Logs are `build/mmc-20261002-lifecycle-manual-{bridge,renderer}.log`.
A double-click completed canonical selection plus one stale focus replay and
invoked the native provider exactly once; its missing-method failure remained.
A subsequent single-click on Storage completed selection/focus without invoking
the old action again. The exact target and its Renderer both exited after cleanup.

The native double-click implementation subsequently succeeded in disposable
`compmgmt` PID 13832 (Renderer 15840). Double-clicking System Tools in the root
result list navigated to it; double-clicking Shared Folders in the resulting list
navigated again. The title, tree, result rows and Actions pane followed canonical
native state without rollback. Logs:
`build/mmc-20261002-native-doubleclick-manual-{bridge,renderer}.log`.
Both exact processes were cleaned after identity checks. No configuration changed.

The final published build repeated both navigation steps successfully in PID
9176 (Renderer 8512), after all 22 startup probes. Its selection/focus revision
replay and native double-click logs are
`build/mmc-20261002-final-doubleclick-{bridge,renderer}.log`.
Both exact processes exited after cleanup. This manual launch injected
immediately after `WaitForInputIdle`: the Injector printed `Bridge restart entry
failed`, but Bridge subsequently passed both commit phases and projected the
window. The reason for that early CLI failure remains unconfirmed; it does not
change the successful interactive observation or the separate settled-startup
probe results. CLI evidence is `build/mmc-20261002-final-doubleclick-injector.log`.

The stock-control regression covers label and icon points in all four ListView
modes. With selection/focus already canonical, `WM_LBUTTONDBLCLK` followed by
`WM_LBUTTONUP` produces exactly one native `NM_DBLCLK` and `LVN_ITEMACTIVATE`,
without an extra `NM_CLICK`, keyboard event, foreground change or capture.
Fallback regressions cover provider refusal, changing metadata, native item
replacement, reentrancy, cancellation, restore and capture changes. A trailing
button-up is suppressed if the callback changes the admitted lifetime.

The owner-data action fixture now completes its first native paint before
publishing a baseline. A before/after `WM_PAINT` probe established that stock
ListView changes the fixture image's first-pixel alpha from 255 to 0 on that
paint, while later paints leave it stable. The raw `GetDIBits` bytes already
contain the changed alpha; Bridge's normalization and fingerprint were correct.
This fixes the fixture's premature baseline without weakening native mutation
checks or retrying an old indexed gesture. Evidence:
`build/mmc-20261002-ownerdata-paint-tests.log`.

### Services virtualization follow-up

The panel diagnostic in `build/mmc-startup-20261002-212249/` showed a null local
`ItemsPanel` and a `StackPanel` containing all 279 service rows. After the fix,
`build/mmc-startup-20261002-213504/` and
`build/mmc-startup-20261002-214634/` showed `ItemsStackPanel`, 20 children and
cache indexes 0 through 19. These attempts stopped before the provisional commit:
the source GUI thread did not acknowledge capture-and-cloak within two seconds.

Temporary capture/dispatch traces in `build/mmc-startup-20261002-214825/` and
`build/mmc-startup-20261002-215223/` measured the first two native captures at
66–83 ms; the next command was not consumed until roughly four seconds later.
Waiting eight seconds before injection did not fix this. The `WM_NULL` wake
experiment in `build/mmc-startup-20261002-215448/` also failed. All temporary
tracing and wake changes were removed. The production rejection now includes
the source command's error, with no timeout or UIA gate changes.

The published build also failed at the same native barrier after switching a
disposable Services window to Standard view before injection (PID 4516, Renderer
18860). This captured 11 HWND nodes and realized 22 of 279 report rows, without
the extended HTML pane. Logs:
`build/mmc-20261002-services-standard-{bridge,renderer}.log`.
Both processes exited after cleanup. The remaining barrier failure is therefore
not confined to the extended view's HTML capture.

## Installed console coverage

Inventory: 21 top-level `C:\Windows\System32\*.msc` files, reconfirmed on 2026-10-02.
Language copies and DriverStore files are excluded. The empty console is an
additional target. Rows remain pending or partial until their full view/dialog scope is verified.

| Console | Views that must be exercised | Live validation |
| --- | --- | --- |
| Empty `mmc.exe` | Menus, toolbars, tree, Actions pane, Add/Remove Snap-in, Add/Remove Columns, Open/Cancel | Pending |
| `gpedit.msc` | Computer/User Configuration, policy categories, details, policy properties and Cancel | Pending |
| `secpol.msc` | Security categories, policy lists, properties and Cancel | Pending |
| `compmgmt.msc` | System Tools, Storage, Services and Applications; each embedded snap-in | Partial: Shared Folders tree navigation, list selection/focus, and result-list double-click through System Tools into Shared Folders |
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

Current evidence is in the `build/mmc-20261002-*` logs and the startup directories above. Keep this
matrix pending until target observations, including nested dialogs and recovery,
justify marking individual rows complete.

## Remaining interaction and architecture work

### Stability follow-up on 2026-09-30

Implemented before this follow-up; compilation and baseline regression checks
were completed on 2026-10-02:

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
included automatically by the SDK project. These changes are covered by the
2026-10-02 builds and suites recorded above.

The earlier Windows process initialization failure `0xC0000142` is no longer
blocking builds. The 2026-10-02 baseline build and gates passed; the per-console
results above still show why that is not a full-MMC stability pass.

For subsequent changes, build/test first, then run probes sequentially:

```powershell
# From the repository root. Do not overlap native tests and startup probes.
.\build.ps1 -Configuration Release
.\test.ps1 -Configuration Release
.\tests\Probe-MmcStartup.ps1 -Consoles @('empty','gpedit','secpol','compmgmt','services','devmgmt','comexp') -ShowConsole -RunAsInvoker -Repetitions 3 -AccessibilityDiagnostics
.\tests\Probe-MmcStartup.ps1 -Consoles all -ShowConsole -RunAsInvoker -AccessibilityDiagnostics
```

Check projection, later rollback, and Renderer exit results separately. Even
successful repeated startups do not establish navigation, dialogs, DPI transitions,
or all unsupported snap-in adapters.

### Still pending

- Verify the new virtual-list and stable-ID contracts through nested MMC pages,
  including refusal followed by a new explicit user action.
- Exercise the native result-list double-click contract in additional snap-ins
  and property dialogs; the current live success covers two Computer Management
  navigation steps.
- Investigate `services` native source-command starvation, then repeat committed
  UIA enumeration and dispatcher-liveness checks with report virtualization.
- Diagnose the immediate-launch Injector failure report followed by successful
  Bridge startup; record the failing module/export/thread step before changing
  its retry policy.
- Resolve the Shared Folders permission-dialog modal UIA mismatch and source-
  thread tree selection timeout observed during the Sessions navigation.
- Generic TreeView context menus and MMC HTML taskpads still need semantic
  adapters. Other `mmcndmgr.dll` taskpad resources contain click handlers, ActiveX
  task icons and embedded native views; broadening the read-only HTML allowlist
  would silently drop those behaviors.
- Owned deferred actions have a bounded-command guard. Older raw HWND message
  routes still need review for providers that pump messages during final capture.
- Event Viewer, Task Scheduler and Firewall require coordinated capture across
  their GUI threads. Removing the ownership guard is insufficient: reads, actions,
  subclass lifecycle and commit validation must run on each HWND's owner thread,
  with worker-side coordination and whole-window rollback.
- HTML typography, padding, selected-description wrapping and DPI parity remain
  pending even when the native text fits its captured rectangles.
