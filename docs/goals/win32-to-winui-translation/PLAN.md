# Win32 Whole-Window Translation Implementation Plan

**Intent:** Render eligible Win32 top-level windows with real WinUI 3 controls in a separate renderer process.
**Current Behavior:** The injected Bridge links WinAppSDK and can enter in-process Island/overlay paths that are unsafe in third-party processes.
**Expected Outcome:** Explicitly selected x64 Win32 targets project supported windows through a native-authoritative, versioned IPC contract and restore native UI on every failure.
**Target-Perspective Output:** LegacyDialogHost's main window and supported modal dialogs expose XAML UI Automation, round-trip pointer and keyboard input to native state, and survive renderer termination.
**Truth Owner:** The native HWND/control tree and the original Win32 API arguments/results.
**Contract Boundary:** A single authenticated full-duplex named pipe using the versioned `FLSH` frame and JSON schemas under `src/Protocol`.
**Cutover:** Capture and validate the complete native tree, create a hidden proxy, validate `surface.ready` against the latest revision, cloak the native root, then reveal the proxy.
**Displaced Path:** Production Bootstrap, in-process WinUI/Island Strategy C, dialog repaint overlays, broad system injection, and name-only injection.
**Value Density:** The first slice translates the LegacyDialogHost main window, MessageBox, and static TaskDialog with live binding and all-or-nothing fallback.
**Evidence Gate:** Release build, protocol tests, dependency inspection, Computer Use/UIA interaction and rollback evidence, and Chrome isolation evidence.
**Acceptance Evidence:** Recorded in `EVIDENCE.md` with commands, screenshots, UIA output, native oracle state, renderer-kill recovery, and WER checks.
**Evidence Lane:** Local x64 Release build on Windows, explicit LegacyDialogHost injection, Computer Use for native UI, Chrome for unrelated-process isolation.
**Kill Criteria:** Production Bridge has no WinUI/WinAppSDK/CoreMessaging imports; Strategy C, Bootstrap entry, system watch, and hybrid window surfaces are absent from the production path.
**Architecture Slice:** Bridge captures and mutates native state only on owning UI threads; Renderer owns WinUI projection and transient input state; protocol/schema is the sole boundary.
**Plan Review Gate:** PRE review aligned before execution; POST, correctness, maintainability, and verifier gates are required before completion.

## Contracts

- One renderer process per target process and one proxy per supported top-level window.
- Per-window state is `Native -> Scanning -> SurfaceReady -> Projected -> Restoring -> Native`.
- The Bridge is the pipe server and authenticates the exact renderer PID, process creation time, current logon SID, and a 128-bit nonce.
- Frame header is the fixed 32-byte little-endian v1 layout specified by `protocol-v1.schema.json`; payload limits and ordering violations fail native.
- Every native object has a generation ID; sequence is monotonic per pipe direction and revision is monotonic per top-level window.
- Property actions carry an expected revision and are rejected as stale when it no
  longer matches. Request-semantic actions -- `invoke`, `close`, `move`, and
  `resize` -- are rebased onto the current revision instead, because the user's
  pointer, not a snapshot revision, is the truth for whether they still apply.
  Geometry is coalesced latest-wins by the renderer and emitted once per move/size
  gesture rather than once per frame.
- Native reads and writes run on the owning GUI thread. The IPC thread only queues commands.
- Renderer ViewModels use typed binding. Native patches are canonical; matching event IDs suppress only their own echoes.
- Any unsupported visible descendant or runtime capability change restores the entire native top-level window.

## Implementation Tasks

1. Add protocol schema, shared fixtures, native codec tests, renderer protocol tests, and a test runner.
2. Add the unpackaged self-contained x64 .NET 8 / Windows App SDK 2.3.1 renderer with typed ViewModels, control factory, and pipe client.
3. Replace Bridge Island dependencies with the pipe server, capture/adapters, source-thread dispatcher, renderer session, and surface state machine.
4. Direct-load Bridge from Injector; require canonical absolute target path; remove system/name-only/watch production modes.
5. Integrate renderer restore/publish into the solution and build, isolate all WinUI runtime files below `Renderer`, and demote IslandDemo to diagnostics.
6. Expand LegacyDialogHost into the acceptance oracle and update project documentation for the new production architecture.
7. Run protocol, build, dependency, UIA, interaction, DPI, modal, crash recovery, unsupported-control, and browser-isolation gates.

## V1 Support Boundary

Supported: standard rectangular overlapped/dialog windows; bounded textual HMENU
command bars; Static text/separators; push/default/check/three-state/radio Button;
noninteractive GroupBox; standard Edit including multiline/read-only/password;
string dropdown-list and editable dropdown ComboBox; string ListBox;
determinate horizontal ProgressBar; standard MessageBox; static TaskDialog.

Native fallback: owner/custom draw, RichEdit, virtual data, foreign/custom HWND,
tree/tab/list-view, owner-draw/callback/MDI menus, toolbar/status, MDI,
ActiveX/OLE, browser/XAML children, layered/nonrectangular/custom nonclient
windows, custom accelerator/focus/IME behavior.

The staged expansion program and the non-goals of universal pixel translation
are defined in `CONTROL-ADAPTER-ROADMAP.md`.

## Verification

- `build.ps1 -Configuration Release`
- `test.ps1 -Configuration Release`
- Bridge import-table inspection for forbidden runtime dependencies
- Computer Use UIA plus click/type/toggle/select/keyboard/DPI/modal/rollback scenarios
- Chrome interaction before and after explicit LegacyDialogHost injection
- POST plan, correctness, maintainability, and final evidence reviews

## Forbidden Moves

- Do not modify `ref_src`.
- Do not copy GPL-3.0 code from `win32-ui-modernizer.wh.cpp`.
- Do not initialize WinUI in the target process.
- Do not keep native and WinUI controls active in a hybrid translated surface.
- Do not mark the goal complete without target-perspective evidence.
