# DirectUI application adapters

The DirectUI translation engine is generic and profile-driven. The engine in
`DirectUiEngine.cpp` contains no per-application branches; every supported page
is one declarative `DirectUiWindowProfile` row in `DirectUiProfiles.cpp`. Adding
a target means adding a profile row plus its slot table, never editing engine
logic.

## Profiles

| adapterId | page | executable | fixed version |
|---|---|---|---|
| `microsoft.mdsched.directui` | initial | System32\MdSched.exe | 10.0.26100.7309 |
| `microsoft.recoverydrive.directui` | first | System32\RecoveryDrive.exe | 10.0.26100.33296 |

MdSched projects its initial dialog: virtual MainIcon (trusted 32x32
RT_GROUP_ICON 5000 pixels from the admitted signed image), MainInstruction,
ContentText, two command links, and Cancel. All three buttons dispatch through
restore-before-click handoff.

RecoveryDrive projects its first wizard page: disabled virtual back button,
wizard title/header texts, the native page explanatory text, the native id-1000
checkbox (projected and toggled in place through the native click state
machine), Next, and Cancel.
`wizardicon` has no trusted pixel source and is not projected; its semantic
slot stays declared so an unexpected mutation still rejects the surface. Later
pages are never projected: Next and Cancel are handoff exits that restore the
native window first.

## Admission contract

A profile row pins: canonical System32 image path, exact fixed binary file
version, Microsoft Windows embedded or catalog Authenticode signer, root window class, an exact
census of implementation HWND classes below the DirectUI host (every count is
an equality), and a slot table where each slot declares its projection kind,
UIA automation id/control type/class/enabled/focusable/actionable contract,
native backing style/control id, action routing, and optional trusted icon
resource. Labels are captured as localized evidence and are never matched
against English text.

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

- `DirectUiAction::HandoffClick` — exit-style buttons. Under the canonical
  barrier the page and backing HWND generation are revalidated, the proxy is
  gated, the native root is uncloaked and verified visible, and only then is
  `BM_CLICK` posted. The projected surface closes; the next native page is
  never projected.
- `DirectUiAction::ToggleCheck` — projected checkboxes. The page is
  revalidated against fresh native evidence, the native checkbox is clicked
  through its own state machine until it reports the requested value, and a
  second capture accepts only that checkbox delta before publishing the next
  revision. `BM_SETCHECK` would bypass the application handler and is never
  used. UIA is not re-queried while the source is cloaked because DirectUI may
  disconnect its virtual provider when the renderer proxy takes activation.
- `DirectUiAction::None` — presentation-only slots; they never emit.

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
recreation, epoch change outside the tolerated retry, or UIA failure rejects
the whole window back to native. There is never a partially projected page.
After initial admission, projected reconcile performs a full native-evidence
comparison; any facet drift restores the native page rather than attempting a
partial patch from an unavailable cloaked UIA provider.
