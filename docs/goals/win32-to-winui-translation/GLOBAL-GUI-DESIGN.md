# Global apply, GUI, and sticky rollback: design

Status: implemented on branch `feature/global-apply-gui`, with these exceptions: the live global run against the desktop's other applications is not yet done (it needs the user's go-ahead, since it injects into every window), and the authenticated control pipe is out of scope for this slice.

Built so far: the x86 lane; dormancy; per-window rollback events; the owner-group fix for translated dialogs; the injector's shared core with handle-pinned checks; `watch` (consent-gated, manual start, panic hotkey, opt-outs, `--dry-run`); the Bridge's watcher lease; the WinForms GUI (`Gui\FluentShell.Gui.exe`); and the guardrail gates in `test.ps1`.

Changes to the decisions below, made during implementation:

- **Restore-all does not clear per-window rollback.** The text under "Reconciled decisions" says a rolled-back window's re-projection waits for the resume path. This slice matches that: the only way a rolled-back window projects again is an application restart. Clearing it on dormancy reset would let any same-session process re-project a window through the shared event, and this slice keeps the forced-native direction unauthenticated and the projecting direction authenticated.
- **Autostart is off.** The watcher runs only while the GUI runs it. Closing the GUI, the panic hotkey, or Stop ends it. The Bridges' lease then restores every window.
- **Stop and panic are the same path.** The GUI sets the dormancy event before it stops the watcher, so Stop restores every window.

Scope comes from the user's 2026-10-14 decisions:

- Keep Folder Options native (explorer.exe stays hard-denied).
- Build the x86 lane.
- Build per-PID global apply as designed: same integrity level, each PID injected through the injector path, elevated windows skipped and shown as skipped, HKCU Run autostart only, a risk warning, a restore-all panic control, and a persistent opt-out list.
- Never use UAC or elevation. Never use global `SetWindowsHookEx`, AppInit_DLLs, or IFEO.

## What no-UAC means for "whole Windows"

The watcher and the GUI run non-elevated. A non-elevated process cannot inject into an elevated process. Global apply therefore covers same-integrity processes only, which in practice means the logged-on user's ordinary apps. Elevated windows appear in the GUI as **skipped**. This is a hard limit of the no-UAC rule, not a gap to close later.

## x86 lane (implemented)

- The x64 injector hands WOW64 targets to `x86\FluentShell.Injector.exe` with the same validated request and returns that process's exit code.
- `x86\` holds a Win32 Injector and a Win32 Bridge. The renderer stays in the root `Renderer\` (x64) and is shared. A Bridge in `x86\` resolves the renderer from the parent directory.
- The Win32 Bridge exports its entry points through `src\Bridge\FluentShell.Bridge.def`. Without it, a 32-bit DLL exports `_FluentShell_Start@4`, and the injector's `GetProcAddress("FluentShell_Start")` fails. The test gate now requires exact export names.
- `build.ps1` builds the Win32 detours, Bridge, and Injector, and copies two files into `x86\`. It fails if `x86\` holds anything else.
- Verified on a 32-bit target: the Bridge loads, the renderer connects, and the window projects (`Projected native window: FluentShell Win32 Translation Oracle`).

## Reconciled decisions

The three design passes disagreed on some points. These are the resolutions.

| Question | Decision | Why |
|---|---|---|
| Panic / restore-all trigger | One **global** auto-reset event, `Local\FluentShell.RestoreAll`. Every Bridge waits on it in `RunSupervisor`. No new export. | One signal reaches every target. An exported flag would need a remote thread per target. |
| Per-root rollback and resume | The authenticated control pipe (below). | Resume must not be a forgeable signal. |
| Watcher storage | `HKCU\Software\FluentShell\Watch`: `Enabled`, `AcceptedRiskSha256`, `Hotkey`, and subkey `OptOut` with one value per canonical path. | The injector is C++ and has no JSON parser. The GUI reads the registry too. |
| x86 targets in the watcher | Handled. The watcher uses the same WOW64 delegation as the CLI. | The watcher design predates the x86 lane. |
| GUI location | **`Gui\FluentShell.Gui.exe`** (user decision, 2026-10-14): a non-WinUI .NET 8 WinForms app beside `Renderer\`. | Keeps the App SDK out of the GUI's publish. This adds a third payload folder, so the root invariant and `build.ps1` change with it. |
| Re-arm semantics | **Recommendation adopted** (user decision): **restore-all** makes every root native and the Bridges dormant. **Resume** clears dormancy. Per-root suppressions survive resume until the user allows that window again. | Keeps a deliberate per-window rollback sticky without making panic irreversible. |

## Sticky per-window rollback (Bridge)

- **Suppression ledger.** `suppressedRoots_` maps a root HWND to a stamp set with `SetPropW` on first sighting, the way `NodeGeneration` works. A recreated HWND has no matching stamp, so it projects normally. The ledger is guarded by `surfacesMutex_`.
- **One gate.** `ClassifyTopLevel` returns Skip for a suppressed root before anything else. `OpenNativeWindow` re-checks before `OpenSurface`. The four bare erases of `discoveryAttempts_` go through `PermitFreshAttempt`, which refuses under suppression.
- **Owner group.** `RestoreOwnerGroup(root, reason)` restores the root and every projected surface owned by it, deepest first, under one reason. The current `HandOwnerGraphToNative` restores only ancestors, which leaves a projected sibling dialog under a native owner. Closing that gap is part of this change.
- **Dormancy.** `std::atomic<bool> dormant_`. While dormant, discovery and reconcile do nothing. Heartbeat keeps running, because the renderer drops its pipe after 6000 ms without one. Dormancy must not go through `Fail`, which is permanent and leads to `Stop`.
- **Trigger.** The global panic event above, and a per-target request over the control pipe.

## Control channel and GUI

- **Server.** The Bridge, one control pipe per target: `\\.\pipe\FluentShell.Control.<pid>`. The listener starts once at module start. It does not depend on `RendererSession`.
- **Protocol.** Separate from the renderer protocol. Magic `FLCT`, a `controlMinor` in Hello, its own message types. The renderer protocol does not change, so `kProtocolMinor` stays 41.
- **Authentication.** Identity, not a secret. The Bridge checks the client's logon-SID DACL, medium integrity label, PID and creation time, and image path (`<root>\Gui\FluentShell.Gui.exe`). The GUI checks the server's PID, its creation time, and the canonical Bridge module in the target. Nothing secret goes on argv.
- **Threading.** The control thread only enqueues work and signals the supervisor. It never touches native state or takes `canonicalMutex`.
- **Asymmetric commands.** Restore-all needs no authentication: a squatter can only force native, never project. Resume and per-root rollback require the authenticated pipe.
- **Rejected.** The renderer pipe (single instance, closed dispatch, a minor bump), a GUI-as-server design (every target would depend on a singleton), and a secret file.

## Global watcher

- **Host.** A `watch` subcommand of `FluentShell.Injector.exe`, started from HKCU Run. It is single-instance, guarded by the mutex `Local\FluentShell.Watch`.
- **Discovery.** Poll `EnumWindows` once per second, with the Bridge's own window filter. Rejected: out-of-context `SetWinEventHook` (session-wide and chatty, with unproven delivery for elevated windows), WMI process traces, and `SetWindowsHookEx`.
- **Identity and the PID race.** Key each target by (PID, creation time). Open the handle first and read session, integrity, WOW64 state, and image path from it. The current injector reads session, integrity, and architecture by PID, then opens the handle, then rechecks only the image. A PID reused in that window passes. The refactor fixes this for both the CLI and the watcher.
- **Shared core.** Move the injector's helpers into `src\injector\InjectorCore.{h,cpp}`. `InjectProcess(handle, pid, image, sha256)` is the single checked path.
- **Eligibility.** Same session, same integrity, visible top-level window, not on `ProcessPolicy`'s deny list, not opted out, not already handled. Each target lands in a state: Injected, Skipped(reason), Failed(retry), or Panicked. Failed targets retry with 2 to 30 s backoff, 12 attempts at most.
- **Default off.** `watch` exits with code 6 unless `Enabled` is 1 and `AcceptedRiskSha256` matches the SHA-256 of the compiled-in warning text. `watch enable` prints the warning and requires an exact typed phrase. `watch disable` removes the Run entry and the flags.
- **Panic.** A `RegisterHotKey` on the watcher's thread queue. If registration fails, the watcher refuses to start. Panic sets `Local\FluentShell.RestoreAll`.
- **Lease.** A Bridge started while the watcher's mutex exists checks it every tick. If the mutex disappears, the Bridge panics. That returns projected windows to native if the watcher is killed.
- **Risks.** Each injected target gets its own renderer process, so ten targets means ten renderers. A Bridge fault takes down its target app. Name-based denial is weak: a renamed copy of a denied executable passes. These are accepted under the warning, but they belong in it.

## Test plan (not yet run)

- Native unit tests for the eligibility table, the identity refusal, the suppression ledger, group ordering, and HWND reuse. `RendererSession.cpp` is not compiled into the native test project, so pure logic goes in headers such as `RootSuppression.h`.
- C# codec tests for the control protocol (`tests\ProtocolFixtures\control-*.json`).
- Live, non-elevated, against `LegacyDialogHost.exe` in both bitnesses: projection, per-window rollback with no re-projection, restore-all, resume, and the watcher lease. The renderer must exit when its target exits.
- Test gates: `test.ps1` extended so the watcher and the Bridge never reference `SetWindowsHookEx` or `SetWinEventHook`, and so the control pipe refuses a forged server.

## Status of the sticky-rollback slice

Implemented in the Bridge. **Dormancy is verified live** (projected, dormant and native for 16 s, reset, projected again within 6 s, renderer exits with its target). The suppression ledger is unit-tested natively; per-window suppression still needs its trigger, the control pipe.

- `src/Bridge/Translation/RootSuppression.h`: the ledger. A suppression applies only while its window carries the stamp it was made with, so a destroyed window or a reused HWND drops it. Covered by `TestRootSuppressionLedger` in the native tests.
- `ClassifyTopLevel` refuses a suppressed root and every window it owns. `OpenNativeWindow` re-checks. Reprojection never schedules for the `userRestore` reason.
- `RestoreOwnerGroup` restores a root and its projected owned windows, deepest first, as one unit. `SuppressRoot` ties rollback to the ledger.
- Dormancy: the session-wide manual-reset event `Local\FluentShell.Dormant`, created with the renderer pipe's DACL. While it is signaled every Bridge restores its windows once and runs no discovery. Resetting it lifts dormancy, and suppressed roots stay native.

**Not yet wired:** nothing calls `SuppressRoot` yet. The per-window trigger is the control pipe, which comes next with the GUI. Until then the only live control is dormancy.

**Known gap:** `RestoreOwnerGroup` finds windows through their `SourceThreadAgent`. A virtual dialog (a MessageBox or TaskDialog projection) is matched by its `dialogHost` instead, which the group does not check yet. A projected virtual dialog under a rolled-back root therefore stays projected until its own restore. Fixing this is part of the next slice.

## Open decisions for the user

None outstanding in this document. The GUI location and the re-arm semantics were decided on 2026-10-14.

## Invariants to change deliberately

When the watcher lands, CLAUDE.md's "no watch mode", "no system-wide discovery", and "broad system injection" statements change, along with the matching `PLAN.md` displaced-path entry and the `test.ps1` gates. The x64-only build and the three-file root rule also change for the x86 lane. This document does not change them; the commit that implements each piece does.
