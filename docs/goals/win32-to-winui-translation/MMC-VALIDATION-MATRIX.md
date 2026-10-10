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

The 2026-10-03 all-console run after the minor-27 message-view adapter, the MMC
description bar, startup modal-dialog ordering and tab-stop progress bars is
`build/mmc-startup-20261003-122510/`: **17 stable startups with Renderer cleanup**
(up from 13 in `build/mmc-startup-20261003-113713/` the same morning) and 5 capture
rejections (`diskmgmt`, `eventvwr`, `perfmon`, `taskschd`, `tpm`). Rows below are
updated from those runs.

| Console | Last observed startup result |
| --- | --- |
| Empty MMC, `gpedit`, `compmgmt`, `certlm`, `certmgr`, `fsmgmt`, `lusrmgr`, `printmanagement` | Stable startup and Renderer cleanup on 2026-10-02 |
| `secpol`, `devmgmt`, `comexp`, `DevModeRunAsUserConfig` | Stable startup and Renderer cleanup on 2026-10-02 |
| `services` | Stable startup on 2026-10-03 (`build/mmc-startup-20261003-113713/`); earlier attempts stopped at source-thread capture-and-cloak acknowledgement, so it is intermittent rather than fixed |
| `azman`, `WmiMgmt` | Stable startup and Renderer cleanup on 2026-10-03 (`build/mmc-startup-20261003-115543/`) through the MMC message-view adapter (minor 27) |
| `diskmgmt` | The owner now projects before its startup dialog, which exposes the real blocker: the MFC graphical disk view (`AfxWnd42u`, scrolling custom paint) has no adapter |
| `eventvwr`, `taskschd`, `tpm` | Embedded Windows Forms child belongs to another UI thread |
| `WF` | Stable startup on 2026-10-03 (`build/mmc-startup-20261003-120242/`) at ordinary integrity: its access-denied message view and MMC's owner-drawn description bar (`Static` under an mmc.exe-registered `MMCViewWindow`, projected as its text) are both admitted. Later runs the same afternoon (`build/mmc-startup-20261003-172543/`) instead loaded the snap-in's Windows Forms view, which lives on another UI thread and keeps the window native like `eventvwr`. The elevated rule views are unverified |
| `perfmon` | HTML host mixes semantic content and a native performance report control |
| `rsop` | Stable startup and Renderer cleanup on 2026-10-03 (`build/mmc-startup-20261003-122436/`, two repetitions): the processing dialog projects, then its modal Group Policy error dialog on top. Later runs (`build/mmc-startup-20261003-172543/`) caught the processing dialog while its UI thread was still busy (even `WM_GETTEXT` timed out), so the source-thread attach timed out and it stayed native; the result depends on how long RSoP processing takes |

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

## 2026-10-06/07: context menus, splitters, menus, WinForms consoles

Protocol minor 32. All 545 Renderer tests, the full native suite and the production
gates pass (`build/session-20261007-test2.log`).

Interaction fixes, each verified live in a disposable console with
`tools/Drive-ProjectedMmc.ps1` (real pointer input against the projection):

- **TreeView/ListView right-click.** A new `contextMenu` action carries the item
  index and its label as a witness (request-semantic, refused if the index names a
  different item). The Bridge focuses the control, selects the item through the
  control's own messages, and sends the keyboard form of `WM_CONTEXTMENU`. MMC
  hit-tests the *cursor*, not the message's point, and routes the keyboard form to
  the focused pane (Task Scheduler's focus otherwise rests in its WinForms view);
  `tools/Probe-NativeContextMenu.ps1` records that behavior. The application's
  `TrackPopupMenu` is answered by a WinUI flyout at the click point. MMC tracks its
  menus with an owner outside the frame, so an armed agent on the tracking thread
  now claims the popup even when the owner's root differs. Verified in compmgmt
  (tree and list) and taskschd.
- **Popup dismissal.** The selection a context menu makes arrives in the same patch,
  and the WinUI tree/list then moves focus to the selected item a frame later, which
  light-dismissed the flyout. A close within 400 ms that leaves focus in the anchor
  reopens the flyout once instead of answering a dismissal.
- **Menu-bar reads no longer steal activation.** Every navigation re-reads MMC's
  toolbar menu bar; the read used to `SetForegroundWindow` the cloaked frame for
  about 2 s, closing any open flyout and sending keystrokes to the hidden window.
  Passive reads no longer activate it (command execution still does). Buttons that
  opened no popup last time (Favorites, SnapinMenu) wait 150 ms instead of 700 ms.
- **Splitter drags** track locally and make one native `setSplit` on release, as
  MMC's own tracking bar does: a 24-move drag produced exactly one request, accepted
  about 100 ms after release. Escape and lost capture abandon the drag.
- **Native test crash fixed.** `LVM_GETGROUPINFOBYINDEX` with `LVGF_SUBTITLE` or
  `LVGF_TASK` dereferences null in comctl32 10.0.26100 for a group that has neither;
  those fields are no longer requested (it would have crashed the target).

Console coverage additions:

- **Windows Forms controls** (taskschd, eventvwr, tpm): owner-draw Labels and
  Buttons are classified by their MSAA role and state; `TCS_RAGGEDRIGHT` and a
  single-line strip that overflows into its own up-down are admitted; a tab control
  is a container for the TabPage windows WinForms parents to it; leaf panels and
  picture boxes with no tab stop and no accessible action project as their own
  pixels; callback ListView icons are resolved as the control resolves them.
- **Scrolling containers** (`scrollBars` on `paneContainer`, action `setScroll`):
  children are clipped to the native viewport, WinUI scroll bars sit where the
  native ones are, and scrolling goes through the application's own line/page
  handling (WinForms ignores a synthetic `SB_THUMBPOSITION`).
- **DirectUI scroll chrome**: an overflowing Actions pane hosts a real ScrollBar in
  a `CtrlNotifySink`; it is absorbed as chrome and the elements scroll locally.
- **Multi-thread capture barrier**: our own capture reads (toolbar `TB_GETBUTTON`,
  which aliases `TBM_SETLINESIZE`, and `WM_NOTIFY` display-info/custom-draw) no
  longer count as mutations; WM_USER-range codes are judged by the receiving
  window's class. Barrier failures now name the first difference.
- **Timing failures are retried**: a window whose GUI thread is busy (RSoP
  processing) is deferred instead of spending its one attempt; an attach timeout,
  an unstable barrier or a capture past its deadline is retried up to four times.

All-console startup (`build/mmc-startup-20261007-032922/`, elevated, 12 s observation):
**19 of 22 stable startups with Renderer cleanup**, up from 17 on 2026-10-03, with
taskschd, rsop and tpm newly stable. eventvwr projected in its dedicated run
(`build/mmc-startup-20261007-032646/`, after one transient retry while loading) but
in the all-console run its later surface open timed out waiting for the source UI
thread ("source UI thread did not acknowledge capture"), so it is intermittent, like
the earlier Services barrier. WF and perfmon stay native (below).

Known gaps from this pass: Event Viewer's overview section headers project as
captured pixels that do not match the native gradient, and their collapse icons
(clickable natively, but publishing no MSAA action) are inert in the projection.
WF still stays native: its collapsible section headers are tab stops and its links
are owner-draw WinForms LinkLabels, neither of which has an adapter. perfmon is
unchanged (HTML host plus the `SysmonReport` control).

## 2026-10-08: navigation census, recovery, and the remaining WinForms/HTML views

`tools/Crawl-ProjectedMmc.ps1` walks a disposable console's scope tree breadth-first
through the tree's own messages (`TVM_SELECTITEM`/`TVM_EXPAND`), waits past the
reconcile hold, and records per node whether the frame is projected (DWM reports the
native frame cloaked) with the Bridge lines for that process. Dialogs are cancelled:
a native one by `WM_CLOSE`, a renderer-hosted prompt (a snap-in's MessageBox while the
frame is projected) by its Cancel/No/OK button through UIA. The first census
(`build/mmc-crawl-census-20261008b/`, 21 consoles) visited 254 nodes, 209 projected
(82%). The same walk after this session's adapters
(`build/mmc-crawl-census-20261008g/`) visited the same 254 nodes and projected **237
(93%)**:

| Console | Nodes | Native before | Native after |
| --- | ---: | ---: | ---: |
| `WF` | 9 | 9 | 0 |
| `eventvwr` | 19 | 14 | 3 |
| `printmanagement` | 10 | 3 | 0 |
| `secpol` | 22 | 3 | 0 |
| `taskschd` | 5 | 2 | 0 |
| `compmgmt` | 14 | 1 | 1 (Performance) |
| `comexp` | 10 | 1 | 1 (Event Viewer Subscriptions prompt) |
| `perfmon` | 12 | 12 | 12 |
| the other thirteen consoles | 153 | 0 | 0 |

Every console but Performance Monitor now projects at every node except where it hands
the whole window to a prompt it cannot project (below).

Session-wide stability:

- **Heartbeat expiry from a blocked pipe reader.** `HandleAction` ran native-action
  admission on the pipe reader thread and could block on a surface's
  `canonicalMutex` while a reconcile held it, so heartbeats went unread and the whole
  session expired. Admission now runs on the action worker (`AdmitAction`); a
  renderer-scoped surface fault is queued there as an internal rollback.
- **Rollback is no longer permanent.** A projected window handed back to native
  because of what it showed is re-attempted with backoff (2 s doubling to 30 s, 12
  attempts, reset after a minute of stable projection), and a native navigation
  (child-window topology change) brings the attempt forward. A window that never
  projected waits for its view to change (the same topology signal, 1.5 s doubling to
  30 s) instead of keeping one failed attempt forever; a refused capture shows
  nothing, so these attempts cost no flash. Timing failures (an unacknowledged
  command, an unstable barrier, a capture past its deadline) retry on a timer for up
  to 90 s and then fall back to the topology watch.
- **Reconcile grace.** A reconcile capture refused while a view is still loading
  holds the projection for 3 s before rolling back.
- **Capture cost.** Painted pixels of Windows Forms controls (inert surfaces, picture
  buttons, glyph labels, the surface check) are rendered through WM_PRINT, which
  WinForms answers through `OnPrint`; only other windows pay a compositor round trip
  (`PW_RENDERFULLCONTENT`, now with one discarded first render, which DWM can answer
  before it holds a freshly painted window). Event Viewer's overview capture dropped
  from about 2 s to about 190 ms, and the console now projects at startup in 1.8 s.

Adapter work:

- **MMC busy shield.** `MMCWaitCursorWindow` (mmcndmgr.dll) is a transparent,
  unpainted child MMC raises over the frame while a snap-in holds the console (Event
  Viewer's Subscriptions prompt keeps it up). It is absorbed; the frame stays enabled
  and focusable as natively, and the session declines everything but the window
  frame's own commands while it is visible.
- **Windows Forms:** virtual ListViews keep `LVIS_STATEIMAGEMASK` in their callback
  mask (admitted when no state image list draws it); form-like panels whose own paint
  is only the dialog face or window background project as plain frames (no chrome,
  no splits between fields), as do panels whose children are all hidden; a read-only
  RichEdit with one size/colour, no emphasis, links or objects projects as a read-only
  text box (font binding of CJK/Latin runs is not formatting); LinkLabels project as
  one-link SysLinks activated through the link's accessible default action; an
  actionless focusable header strip (Firewall overview) projects as its pixels;
  textless picture buttons and symbol-font (Marlett) labels/buttons carry their
  painted faces with the face background keyed out.
- **HTML:** a `res://` image of a System32 module (Performance Monitor's
  `res://wdc.dll/perfmon.gif`, inside an image-only link) is decoded from the module's
  resource with WIC; refusals now carry the element and geometry evidence.
- **Covered hosts:** a window that a sibling above covers but for edges of at most
  8 px shows only those edges; unsupported content inside it drops the host instead
  of refusing the frame (Print Management lays MMC's result ListView over its
  frameset host). Supported covered content keeps its nodes.
- **Gate truthfulness:** container peers (MDI client, pane and dialog containers,
  accessible islands, native documents, the disk view) report the native enabled state,
  which MMC clears while a snap-in holds the console; SysLink gate failures name the
  mismatched property. A Windows Forms tab stop the dialog manager's walk never reaches
  (MMC 3.0 snap-in views) is still a tab stop and gets a tab index after the walk's.
- **UIA gate no longer poisons the session:** a validation worker abandoned at its
  deadline disables validation only until it finishes; a worker that was merely slow
  re-enables it, one stuck in a provider never does.
- **Windowed virtual lists (protocol minor 33):** an owner-data report ListView longer
  than 512 rows without checkboxes or editable labels (Event Viewer's logs: 5,000+
  events) is carried as a window of 192 rows around its native top row, with `itemCount`
  and `itemWindowStart`. Every index (selection, focus, `contextMenu`) stays absolute
  and is bounded by the full count; the projection shows the window between two spacers
  that keep the whole list's scroll extent and moves it with `setTopIndex`, which scrolls
  the native list so the requested row is its top row. At most one request is in flight,
  the viewport is re-checked after each window arrives, and the scroll offset is restored
  across the rebuild (a ListView rebuild resets it). Verified live against the Application
  log with real wheel input: rows from the previous days are reachable and the list
  returns to the top. A `setTopIndex` that does not land never rolls the window back.
- **Tools:** `Drive-ProjectedMmc.ps1` gained `Expand`, `Key`, `Wait`, `WheelList` and
  `ListState`; its tree lookups are exact (a prefix match had selected a sibling).

### Later on 2026-10-08: Performance Monitor, the menu bar, selected rows, item activation

Protocol minor 34. Everything below was driven live with `tools/Drive-ProjectedMmc.ps1`
(real pointer and keyboard input plus UIA) against disposable consoles, never changing a
policy, service, account or device; every dialog was cancelled or closed.

- **Performance Monitor.** The root and Computer Management's Performance node project
  (the counter collection is read through `Item(i)` with `_NewEnum` as the fallback). The
  chart view (toolbar, legend ListView, status strip, graph) is still native, and the
  other Performance Monitor nodes have not been re-walked since the census above.
- **Menu bar.** MMC publishes hidden menus as `STATE_SYSTEM_INVISIBLE` buttons; capture
  skips them. The MDI Window menu (ids from 0xFF00, which are ordinary commands) projects
  and invokes; only `0xF000`-`0xF1FF` are system commands.
- **Selected-row extended view (Services).** The HTML view's link ("Start") and its loose
  text node project together; the service properties dialog opens and is cancelled.
- **Item context menus.** Device Manager's menu opens from the Menu key (its OCX ignores a
  synthesized `WM_CONTEXTMENU(-1,-1)`) and its device Properties dialog opens; Event
  Viewer's event-list menu, which the frame thread creates asynchronously, is claimed and
  projected. The Local Users and Groups user Properties dialog opens and is cancelled.
- **Double-click and Enter (`openItem`).** Services' list double-click opens the service
  Properties dialog. Event Viewer's list opens the projected event dialog by double-click
  ("Event 10001, RestartManager") and by Enter on a selected row ("Event 16394,
  Security-SPP"). Device Manager's tree opens the device Properties dialog by double-click
  and by Enter on a selected leaf (the "ACPI x64-based PC" properties). Both gestures
  reach the Bridge as `openItem`; the Bridge selects the item and posts its control Enter.
- **Environment caveat.** The shell that ran these is non-elevated (Medium integrity,
  `-RunAsInvoker`): Device Manager shows its two standard-user notices (cancelled with
  OK) and `gpedit.msc` shows a Group Policy error dialog with only a Close button, so a
  policy double-click in `gpedit` needs an elevated session and is not verified.
- **Suites.** `.\test.ps1 -Configuration Release` passes: 596 Renderer tests, the full
  native suite and the production gates (Bridge dependency, Bridge export, Injector CLI).
- **Tools:** `Drive-ProjectedMmc.ps1` gained menu-bar steps (`MenuRoots`, `MenuBar`,
  `MenuInvoke`), item steps (`TreeSelect`, `ListSelect`, `ListOpen`, `TreeOpen`, `Select`,
  `Invoke`, `Items`), `Dialogs`/`CloseDialogs` (a button named with its mnemonic, such as
  `Close(C)`, is a cancel path), and window/screen screenshots. `Crawl-ProjectedMmc.ps1`
  re-checks stability after the reconcile hold.

### 2026-10-09: the dialogs MMC opens

Navigation projects; the dialogs a console opens are a separate boundary, because an owned
dialog that cannot be projected hands the whole owner graph back to native. A survey with
`tools/Drive-ProjectedMmc.ps1` (every dialog cancelled or closed, nothing changed) found
these, each now fixed and verified live on the Medium-integrity shell (protocol minor 35):

| Console | Dialog | Before | Cause and fix |
| --- | --- | --- | --- |
| empty | Add or Remove Snap-ins | projects | none needed |
| `WmiMgmt` | WMI Control Properties | projects | none needed |
| `certmgr` | Certificate (General, Details, Path) | whole console native | read-only RichEdit with bold runs: `richText` kind |
| `taskschd` | task Properties (six tabs) | double-click did nothing, then rolled back | Windows Forms list needs the pointer's double-click; 120 px CheckBox minimum; false form-row splitter; surface-coloured bands past the caps; Marlett up/down buttons |
| `eventvwr` | Actions menu commands | "owner window is gone" | per-call `ATL:` popup owner replaced on replay |
| `eventvwr` | Filter Current Log | stayed native | unowned `ShowDialog()` is parented to the desktop: modal-peer owner |
| `eventvwr` | Log Properties | stayed native | NumericUpDown arrows: spin-pair `upDown` adapter |
| `eventvwr` | Create Custom View, Find | project | none needed beyond the above |
| `eventvwr` | Create Basic Task wizard | stayed native | 1-pixel Panel rules (grouping role, full-height rule) |
| `perfmon` | Add Counters | ended `mmc.exe` | comctl32 null read on a group without footer text: group text read under a structured handler; a positioned list builds only its viewport; UIA gate scaled to the snapshot; `close` is `SC_CLOSE` |
| `perfmon` | chart Properties (five pages) | stayed native | pages are child dialogs with no `DS_CONTROL` (own tab scope); dialog-procedure owner-draw combos; owner-draw buttons; the Time Range bar as a picture |

Two interaction checks ride on these: the projected NumericUpDown arrows step the native
log-size field by its own increment (20480 to 20608 to 20544 in two presses up and one down),
and Event Viewer's list and Device Manager's tree open their item from a double-click and from
Enter.

### Later on 2026-10-09: Performance Monitor's chart and the Add Counters dialog

Protocol minor 36 (design notes: `CONTROL-ADAPTER-ROADMAP.md`, Tranche N). The chart view
(`perfmon.msc`, Monitoring Tools, Performance Monitor) was the one navigation-census node that
had stayed native. It projects now -- toolbar (with the graph-type drop-down arrow), the graph,
the value strip and the legend -- from the System Monitor control's own state, and was driven
live through the real pointer and UIA on a disposable, non-elevated console:

- the chart draws as the native one does (slots, the red time bar, time-axis labels; line and
  histogram, switched through the projected drop-down), and the legend's Show boxes and row
  selection act on the control;
- **Add Counters** (the chart's Add button, `Ctrl+A`) first ended `mmc.exe`: comctl32
  6.10.26100 dereferences a null string when a ListView group has no footer, which every group
  of that list is. Group text is now read under a structured handler. Then the dialog went back to native on the
  UIA gate (a couple of hundred group bands, 1.3 s a transaction): a positioned list builds only
  what its viewport shows and the gate's time grows with the snapshot. The dialog now projects;
  a counter set expands through UIA, a counter is selected, `Add >>` puts it in the right list,
  `OK` closes the dialog and the legend gains the row; Cancel and Escape close it (the
  Bridge's `close` is `SC_CLOSE`, which this dialog needs: it ignores `WM_CLOSE`);
- the chart's **Properties** dialog (`Ctrl+Q`; a property sheet whose five pages are sysmon.ocx child
  dialogs) projects on every page -- General, Source, Data, Graph and Appearance -- and every
  control answers real input (10 of 10 on General, 10 of 10 on Graph). Its pages are child dialogs
  with no `DS_CONTROL`, so each is a navigation scope of its own; the capture also had to admit
  owner-draw combos painted by a dialog procedure, owner-draw buttons, and the Time Range bar (a
  picture). Switching away from the Source page after choosing the Database radio raises the "no data
  source name" message box, which projects as a dialog of its own;
- the suites pass: `.\test.ps1 -Configuration Release` (656 Renderer tests, the native suite, the
  Bridge dependency and export gates, the Injector CLI gate);
- the navigation census over `perfmon` visits its three monitoring nodes and projects all of them
  (the data collector sets and reports need elevation).

Dialog survey (`tools/Survey-ProjectedDialogs.ps1`, every dialog cancelled): the Add Counters and
Properties flows are PROJECTED. Escape closes every Win32 dialog; the Windows Forms dialogs of Event
Viewer and Task Scheduler ignored it (`canCancel` was `#32770`-only) until a WinForms form that is
modal or has the dialog frame was treated as cancellable. Not carried: the chart's own pointer interactions (hover tooltips,
double-click properties) and report/area chart types.

The complete survey of the 19 flows (45 pages) over every console reachable without elevation ends
with every page PROJECTED except Shared Folders' share properties (MISSING by design: this machine
has only administrative shares, so the console shows its OK-only notice instead). Escape closes the
dialog in every flow but four, each explained: the Task Scheduler and Event Viewer Log Properties
dialogs raise a "discard changes?" prompt (which projects) because the survey had just edited their
fields; the chart Properties sheet raises the "no data source name" message box; and Event
Properties, a modeless Windows Forms window with a Close button, does not answer Escape (what the
native form does is not established: the scripted native run cannot open it). Local Users and Groups'
double-click flows passed in both runs.

Startup probe on the final build (`Probe-MmcStartup.ps1 -Consoles all -ShowConsole -RunAsInvoker
-AccessibilityDiagnostics`, a non-elevated shell, 22 consoles): 20 show stable startup and cleanup. The
other two do not: `diskmgmt` shows the modal "you do not have access to Disk
Management" box over a view whose lists are all empty, which capture refuses (`private container has no
visible children to frame`); `tpm` swaps its overview for an empty ATL host about two seconds after
start, so the capture barrier sees 55 nodes become 16 and the window goes back to native (3 of 3 runs
for each, also with a 15 s and a 45 s observation). Both projected in the 2026-10-07 runs; the probe
does not record the session's integrity level, so a session with access to Disk Management and the TPM
is the likeliest difference, and an elevated re-run is the check.

Presentation fixes that rode on these dialogs (all projected dialogs benefit): a Static draws like
DrawText (alignment, top-aligned wrapped text clipped to its rectangle, no-prefix and ellipsis
styles), native-sized combos no longer cut their labels in half, and check boxes and radio buttons use
compact templates (`CompactControlStyles.cs`): WinUI's 20 px glyph in a 32 px cell was clipped to a native
13 to 16 px control, so a radio button showed as an arc; it is now a whole circle with its dot, a check
box a whole box with its tick, in every state (checked, unchecked, disabled), on the chart Properties
pages, Task Scheduler's task Properties and the console Options dialog.

Stability found by the same survey: a property sheet MMC opens from a snap-in is owned by a hidden
`MMCDataWindow`, so the Bridge counts it as unowned and its proxy has no z-order tie to the console's.
About a second after it opened, the console's menu-bar re-read published a structural patch; the
console proxy rebuilt its controls and restored its remembered focus with `Focus(Programmatic)`, which
activates the window it is in, and the console came to the front over the dialog (a click meant for the
dialog's edit landed in the console's list; reproduced 5 of 5 on the Local Users and Groups group
properties). A window now restores a rebuild's focus only while it is the active one and keeps it for
its next activation otherwise; the same flow answers 4 of 4. This is the race the "z-order change to
`TranslatedWindow`" item below was waiting on for these dialogs.

### Later on 2026-10-09: dialogs opened from dialogs

The survey opens every dialog from a console. Opening one from a *dialog* (a modal chain) was a
separate boundary the owner graph had not crossed: it was one level deep, so the Color chooser the
chart Properties sheet's Appearance page opens (a dialog owned by a dialog owned by the console) dropped
the console, the sheet and the chooser to native, and nothing projected them again (the console's
re-attempt after two seconds ran while the chooser was still open, and failed the committed gate). Design
and the rules that came out of it: `CONTROL-ADAPTER-ROADMAP.md`, "Dialogs opened from dialogs".
Driven live on a disposable, non-elevated console:

- **Add Counters over the chart Properties sheet** (Data page, Add button): the whole chain projects
  (console, sheet, Add Counters: three WinUI windows, three cloaked natives); Escape closes only Add
  Counters and the sheet stays projected; Cancel on the sheet closes it. Survey flow
  `perfmon-nested-add-counters`.
- **Color chooser over the sheet** (Appearance page, Change under Color): projected at first, with blank
  palettes (comdlg32 paints them through a subclass of a plain Static), which would have left the
  dialog half dead. A Static that is a tab stop and has no text is refused as custom-drawn, so the
  chooser stays native and fully working while it is open, with the sheet and console native under it
  (held there for as long as it is open, with no re-attempt in between); about three seconds after it closes
  the console is projected, then the sheet, in one discovery pass.
- A console projected while a dialog is open natively (the console window is disabled) failed the
  committed gate on the chart: the three custom elements reported themselves enabled, and the graph
  stayed blank because it never drew to the extent it was given. Both fixed.
- **Nested dialogs that project** (survey flows `empty-snapin-advanced`, `empty-snapin-target-computer`,
  and by hand the Edit Extensions button): Add or Remove Snap-ins over the empty console, with Advanced, Extensions and the
  Choose Target Computer page of a snap-in's wizard over it: each projects as the third window of the
  chain, Escape closes only it, and the Add or Remove dialog stays projected.
- **Nested dialogs that cannot project** hand the graph back and get it again: Local Users and Groups'
  Select Users picker (`Link Window`, an editable RichEdit, a scroll bar) over a group's Properties went
  native with the sheet (the console stays projected where it is a separate root: these sheets have a
  hidden owner) and the sheet was projected again about four seconds after the picker closed; Task
  Scheduler's New Trigger did the same until it could project (below). Device Manager's Driver Details
  over a device's Properties raised a message box, which projects as a renderer prompt.
- **Task Scheduler's New Trigger** (nested over a task's Properties, `taskschd-new-trigger`) projects
  since protocol minor 37: its four `SysDateTimePick32` controls (start and expire, date and time) are
  `datePicker`/`timePicker` nodes, and its Windows Forms group boxes own their controls. Driven by real
  input: clicking the start date opens the projected calendar, day 20 sent `setValue`, the native picker
  took it and the field read 2026/10/20; the time field's flyout (AM, hour 9, Accept) read 上午 09:11:09
  with the native seconds kept. The dialog's eight check boxes and radio buttons all answer.
- The complete survey was repeated on the final build of the day: 35 flows, 64 pages, every page
  PROJECTED except Shared Folders' share properties (no ordinary share here). Escape closes every dialog
  that has something to cancel with; it stays open on the Task Scheduler and Event Viewer editors that
  prompt, Performance Monitor's sheet (the data-source message box), Event Properties (a modeless form) and
  Customize View (only OK, natively too). Task Scheduler's New Trigger and Create Task pages have dynamic
  panels, and the survey's blind `Interact` pass sometimes finds one control it picked before the panel
  changed (`radio '' did not select`, a check box that did not toggle): by hand every one answers.

### Later on 2026-10-09: the dialogs the consoles' own menus open

The survey's flows open dialogs through list items and tree nodes. The File, Action and View menus of
every console that projects a menu bar were listed (`MenuBar` steps) and each item that opens a dialog
on the root node was opened and cancelled (flows `menu-*` in `dialog-survey.json`). Of 16 candidates,
two kept their console native and are now projected, and one is another process:

| Console | Dialog | Before | Cause and fix |
| --- | --- | --- | --- |
| empty, `eventvwr` | View, Customize | native | MMC's Customize View has a group box with `WS_TABSTOP` (a stop on nothing: a group box never takes focus): capture drops it from the traversal. All nine check boxes answer; Escape closes nothing, natively too (the dialog has only OK) |
| `certmgr` | Action, Find Certificates | console rolled back | the search animation (`SysAnimate32`) is an inert picture of the frame it shows |
| `printmanagement` | Action, Migrate Printers | no dialog | the migration wizard is a process of its own, not a window of the console |

All of the others project on the first attempt: New Taskpad View wizard and Add/Remove Columns (empty
console, WMI Control), Select Computer (Event Viewer, Services, Computer Management; the Event Viewer one is
a Windows Forms form), Task Scheduler's Create Basic Task wizard, Create Task (five pages) and New Folder,
Certificates' View Options, Print Management's Add/Remove Servers, and Authorization Manager's Options.
Task Scheduler's wizard was walked page by page by real input (name, trigger type, Daily with its date and
time pickers, Start a program): every page projects, Next stays disabled until the native form enables it,
and the left-hand step list follows the page.

Two consoles gave no menu items to list: Device Manager (its startup notices are modal over the console)
and Windows Firewall (no projected menu bar when the survey ran, 4.5 s after the start).

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
| `devmgmt.msc` | Device hierarchy, categories and property sheets | Partial: tree expansion, Menu-key context menu, device Properties by double-click and Enter (non-elevated shell: two standard-user notices) |
| `DevModeRunAsUserConfig.msc` | Available local pages and dialogs | Pending |
| `diskmgmt.msc` | Volume list, graphical disk view and property dialogs | Pending |
| `eventvwr.msc` | Log tree, event lists, event details and filters | Partial: log tree, windowed event list, event-list context menu, event Properties by double-click and Enter; filters pending |
| `fsmgmt.msc` | Shares, Sessions, Open Files and properties | Pending |
| `lusrmgr.msc` | Users, Groups and property dialogs | Partial: user Properties dialog opened and cancelled |
| `perfmon.msc` | Monitoring graph, collector sets and reports | Partial: root, Monitoring Tools and the chart view project (toolbar, graph, value strip, legend, Add Counters and Properties dialogs); collector sets and reports need elevation |
| `printmanagement.msc` | Print servers, printer lists and properties | Pending |
| `rsop.msc` | Available policy results, details and properties | Pending |
| `services.msc` | Service list, extended/standard views and properties | Partial: list, selected-row extended view, service Properties by double-click |
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
- Exercise the result-list double-click and Enter contract (`openItem` for MMC views,
  native activation elsewhere) in the remaining snap-ins and property dialogs; the live
  successes cover Computer Management navigation, Services, Event Viewer and Device
  Manager. A `gpedit.msc` policy double-click needs an elevated session.
- Survey the dialog flows of the remaining consoles (Task Scheduler, Certificates, Disk
  Management, Print Management, Windows Firewall, Shared Folders, Local Security Policy,
  Component Services, the empty console's Add/Remove Snap-ins), cancel-only.
- Performance Monitor: the chart's log and SQL data sources, report and area chart types, and the
  data collector sets and reports (elevation); the Font chooser and the log-file picker the Properties
  pages open have not been driven (by the adapter registry's rules they stay native while open:
  `CBS_SIMPLE` combos, the shell's item dialog; the Color chooser was driven, see "dialogs opened from
  dialogs").
- Confirm that the z-order change to `TranslatedWindow` (no `AppWindow.Show` when the
  window is already visible, no presenter writes when nothing changed) removes the race
  that left a dialog behind its owner; and Event Viewer's scope-tree context menus under
  heavy navigation, which can still arrive late or be lost.
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
