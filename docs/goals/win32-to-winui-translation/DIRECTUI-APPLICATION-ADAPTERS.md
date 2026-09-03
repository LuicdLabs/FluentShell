# DirectUI application adapters

The DirectUI translation engine is profile-driven and has two admission lanes.
Exact application pages are declarative `DirectUiWindowProfile` rows in
`DirectUiProfiles.cpp`. Other Microsoft-signed canonical System32 surfaces
generate a sealed per-surface semantic profile from native A, UIA U, native B
evidence. The engine contains no executable-name or page-name behavior branches.

## Profiles

| adapterId | page | executable | fixed version |
|---|---|---|---|
| `microsoft.mdsched.directui` | initial | System32\MdSched.exe | 10.0.26100.7309 |
| `microsoft.recoverydrive.directui` | first | System32\RecoveryDrive.exe | 10.0.26100.33296 |

MdSched projects its initial dialog: virtual MainIcon (trusted 32x32
RT_GROUP_ICON 5000 pixels from the admitted signed image), MainInstruction,
ContentText, two command links, and Cancel. All three buttons dispatch through
restore-before-click handoff.

RecoveryDrive's exact profile projects its first wizard page: disabled virtual
back button, wizard title/header texts, the native page explanatory text, the
native id-1000 checkbox (projected and toggled in place through the native click
state machine), Next, and Cancel. `wizardicon` has no trusted pixel source and is
not projected; its semantic slot stays declared so an unexpected mutation still
rejects the surface. Next uses a restore-before-action handoff.

Later RecoveryDrive pages, and every other built-in DirectUI page, are admitted
through the capability-derived lane below rather than by adding
executable-specific rows. That is deliberate: a page row pins one localized build
of one executable, so per-page rows can never cover the system's DirectUI surface
area, while a capability the adapter registry already proves holds on every page
that uses the same control.

## The capability-derived lane

`adapterId = microsoft.windows.directui.semantic.v1`, `pageId = semantic-v1`.
Each admitted UIA element becomes exactly one slot, and its projected kind comes
from the registered Win32 adapter that backs it (`ControlAdapters.cpp`), so the
lane inherits proven typed contracts instead of re-deriving one per application.

| UIA control type | native backing | projected kind / variant | routes |
|---|---|---|---|
| Text | none (provider) | `static` / standard | — |
| Text, Pane | `Static` | `static` / standard | — |
| Separator | none (provider) | `separator` / standard | — |
| Image, Pane | `Static` (SS_ICON) | `staticIcon` / standard | — |
| Text, Image, Pane | `BitmapDisplayClass` | `staticIcon` / bitmapDisplay | — |
| Image, Pane | `MonitorPaletteClass` | `staticIcon` / monitorPalette | — |
| Group | `Button` (BS_GROUPBOX) | `groupBox` / standard | — |
| ProgressBar | `msctls_progress32` | `progressBar` / standard | — |
| StatusBar | `msctls_statusbar32` | `statusBar` / standard | — |
| Button | none (AeroWizard page host) | `button` / standard | `invoke` |
| Button, Pane | `Button` (push, default push) | `button` / standard | `invoke` |
| Button | `Button` (BS_COMMANDLINK) | `button` / commandLink | `invoke` |
| Hyperlink | `SysLink` | `sysLink` / standard | `invoke` |
| CheckBox, Pane | `Button` (check, auto check) | `checkBox` / standard | `setCheck` |
| CheckBox | `Button` (BS_3STATE) | `threeState` / standard | `setCheck` |
| RadioButton, Pane | `Button` (radio) | `radioButton` / standard | `setCheck` |
| RadioButton, Pane | `BitmapSwitchClass` | `radioButton` / bitmapSwitch | `setCheck` |
| Edit | `Edit` | `edit` / standard | `setText` |
| Edit | `Edit` (ES_PASSWORD) | `password` / password | `setText` |
| ComboBox | `ComboBox` | `comboBox` / standard | `select` (+ `setText`) |
| List | `ListBox` | `listBox` / standard | `select` |
| List, DataGrid, Table | `SysListView32` | `listView` / standard | `setSelection` (+ `setItemCheck`) |
| Tab | `SysTabControl32` | `tabControl` / standard | `select` |
| ToolBar | `ToolbarWindow32` | `toolbar` / standard | `toolbarCommand` |

A parenthesized second route is advertised only when that revision's own typed
state says the control accepts it: `setText` when the combo box is `CBS_DROPDOWN`
and not read-only, `setItemCheck` when the ListView carries
`LVS_EX_CHECKBOXES`. An `ES_READONLY` edit box is projected with no route at all,
and a disabled slot advertises nothing. Structural Image/Pane/Group/Window
providers are accounted and retained as evidence without being projected. An
unknown role, or a structural provider that turns out to be actionable, rejects
the whole surface.

Composite slots own their UIA item descendants instead of letting each item
become a slot of its own: list and data items, tab items, headers and header
items, tool bar buttons and separators, status bar panes, an editable combo box's
edit/list/drop-down children, and any scroll chrome. Two independent observations
then have to agree. The provider must publish at least one element per native
item — an inequality, because headers, per-column text runs, and scroll chrome
only ever add — and the adapter's own item count is pinned and held to an
equality across the A/B bracket and across every later revision. A collection
that grew or shrank is reported as a topology change, so the page is
re-evaluated from scratch rather than pinned to a stale census.

Generated slot styles are pinned with a full 64-bit mask, which is what keeps
`ES_READONLY`, `CBS_DROPDOWN`, and `LVS_EX_CHECKBOXES` from flipping under a
projected page: a route set derived from typed state stays true for the life of
the projection. The two deliberate alternates are a Button's default-push bit and
a radio group's `WS_TABSTOP`, both of which move between members by design.

`TreeView` (`SysTreeView32`) and `Slider` (`msctls_trackbar32`) are outside the
lane. Neither has a row in `kClassAdapters`, an entry in `kCaptureTable`, or a
name in the protocol's `kind` enum, so a DirectUI page that contains one stays
native rather than projecting a control the boundary cannot describe.

## Admission contract

A fixed profile row pins: canonical System32 image path, exact fixed binary file
version, Microsoft Windows embedded or catalog Authenticode signer, root window class, an exact
census of implementation HWND classes below the DirectUI host (every count is
an equality), and a slot table where each slot declares its projection kind,
UIA automation id/control type/class/enabled/focusable/actionable contract,
native backing style/control id, action routing, and optional trusted icon
resource. Labels are captured as localized evidence and are never matched
against English text. A generated profile instead pins the canonical System32
path and Microsoft Windows signature, root class, exact visible/hidden HWND
class inventory, every native style/control ID, UIA role/class/framework/pattern
shape, AutomationId, geometry scope, enabled/focus state, action route, and — for
a composite — the native item count its absorbed UIA subtree must keep matching.

Admission is a native A, MTA UIA U, native B transaction. A and B run on the
source GUI thread; U runs on a Bridge-owned MTA worker while that thread pumps
messages. Only copied values leave U. Root, DirectUI anchor, and every backing
slot's HWND identity, lifecycle generation, geometry, styles, visibility,
enabled state, DPI, title, and the source mutation epoch must be identical
across the bracket; the only tolerated retry is a single first-provider
activation that bumps the epoch without any native evidence change. A UIA
timeout abandons at most one worker and poisons the agent for DirectUI
admission; there is no unbounded join.

## Action routing

Nine routes plus `None` map onto the seven protocol action names. They split into
two families by `IsDirectUiInPlaceAction`.

Handoff routes hand the *page* over to the application rather than the *window*.
`DirectUiNavigationMayStayProjected` is the one definition of which of them only
replace the page inside the same top-level window: Back, Next, and Finish on a
property sheet, and any plain click or link that does not declare itself a cancel.

Those keep the projection. Under the canonical barrier the page and backing HWND
generation are revalidated, the renderer input gate is re-armed while the proxy
keeps the screen, and the native message is posted **while the native root stays
cloaked** (`NavigateDirectUiProjected`). Reconcile then owns the surface until the
replacement page is admitted in place: it waits for the departing page's contract
to stop capturing, places the native root behind the proxy, uncloaks it only long
enough for the admission contract to walk a real UIA subtree, re-cloaks it, and
publishes the new page as one full-snapshot `window.patch` followed by the
interactive commit. The native window is never composited, so there is no visible
native frame between pages. The wait is bounded by `kDirectUiSwapWindowMs`; a
window still on the same page when it expires means the application refused the
navigation, and the projection is simply handed back interactive.

Terminal handoff routes — cancel-declared clicks and `PSBTN_CANCEL` — give the
window up, because they end it rather than navigate it. The proxy is hidden, the
native root is uncloaked and verified visible, the message is posted, the
projected surface closes, and discovery evaluates whatever the application leaves
behind. Every rejection inside either route clears the one-shot discovery record
so the page the application settled on gets its own attempt instead of the window
being stranded native for the life of the process.

- `HandoffClick` — exit-style buttons and command links. Posts `BM_CLICK`.
- `HandoffPropertySheetButton` — virtual AeroWizard Back/Next/Finish/Cancel.
  Only a generated `NativeHWNDHost` surface with a native property-sheet page
  host and an exact known AutomationId can select this route; the source GUI
  thread posts the corresponding standard `PSM_PRESSBUTTON` command.
- `HandoffLinkClick` — `SysLink` hyperlinks. Routes the control's own `NM_CLICK`
  to its parent, so the application decides what the link opens. It is
  deliberately **not** an in-place route: a link may navigate or spawn a modal.

In-place routes keep the surface projected. The page is revalidated against fresh
native evidence, the control is driven through its own registered adapter and its
own notification path, and a second capture accepts only that one control's
mutable delta (`MatchDirectUiInPlaceMutation`) before publishing the next
revision. Messages that would bypass the application's handler — `BM_SETCHECK`,
`WM_SETTEXT` — are never used. UIA is not re-queried while the source is cloaked
because DirectUI may disconnect its virtual provider when the renderer proxy
takes activation.

- `ToggleCheck` — checkboxes and three-state boxes. Clicks through the native
  state machine until the control reports the requested value.
- `SelectRadio` — radio buttons, including `BitmapSwitchClass`. Posts `BM_CLICK`
  and requires exclusive `WS_TABSTOP` to have moved to the clicked member.
- `SetEditText` — `Edit`, and the edit child of an editable combo box.
  `EM_SETSEL` + `EM_REPLACESEL`, so `EN_CHANGE` still reaches the application.
- `SelectListItem` — `ComboBox`, `ListBox`, `SysTabControl32`, `SysListView32`.
  Native selection plus that control's own selection notification. Advertised as
  `setSelection` for a ListView and `select` for the others.
- `SetItemCheck` — one `LVS_EX_CHECKBOXES` ListView item state, in place.
- `ToolbarCommand` — posts the toolbar's own `WM_COMMAND` to its owner.
- `None` — presentation-only slots; they never emit.

A slot may declare a secondary route, and `DirectUiSecondaryActionApplies` gates
it on this revision's typed state rather than on the declaration: an editable
combo box advertises `select` and `setText`, a checkbox ListView advertises
`setSelection` and `setItemCheck`, and the same rows advertise only the primary
route when the style bits say otherwise. The declared route is the ceiling, never
the guarantee.

Whole-window moves remain projected. The source GUI thread validates current
native evidence, moves the cloaked root, and accepts the next revision only when
the root, DirectUI anchor, and every backing HWND changed by one identical
translation. These admitted dialogs are non-resizable; any size change restores
the native surface rather than inventing resized virtual-element geometry.

Window close, Alt+F4, and Escape route to the profile-declared cancel binding
through the same handoff path, or report `closeRejected` when no cancel
binding exists.

## Failure

Any census drift, UIA shape change, signer/version/path mismatch, backing HWND
recreation, epoch change outside the tolerated retry, composite item-count
change, or UIA failure rejects the whole window back to native. There is never a
partially projected page. An in-place mutation that touches anything but the
acting slot's own mutable state — a bystander slot's selection, a style bit, a
detail shape change, the title — is refused the same way. After initial
admission, projected reconcile recaptures canonical native-backed state without
requiring virtual UIA providers that DirectUI disconnects while cloaked. Stable
generated topology publishes progress, check, text, selection, item-check,
geometry, and enabled-state revisions. An HWND inventory or root-shape change no longer closes the proxy: it is what a
replaced page looks like from reconcile, so the surface keeps its projection and
the new page is re-admitted in place through the same contract initial admission
uses. A surface first admitted through a declarative row gives that row the first
look at the replacement and then degrades to the capability-derived lane, because
a page row describes exactly one page while a capability holds across all of them.
Only a page that lane cannot admit within the swap window falls back, and it
clears the one-shot discovery record on the way out.

Runtime acceptance currently covers MdSched, RecoveryDrive first/Please wait/
Connect USB surfaces including seamless Next and Back page transitions, and the
first ClearType Text Tuner surface (`cttune.exe`) without an exact application
profile. Two limits are known and deliberate: a page whose replacement changes the
top-level window's size can flash the uncovered edge during the re-admission
uncloak, because the proxy geometry is pinned until the new snapshot is published;
and a page that opens an owned modal instead of navigating is resolved by the
owner-graph deferral back to native rather than by the swap. Destructive
RecoveryDrive erase/format pages require dedicated disposable USB media before
they can be claimed as accepted.
