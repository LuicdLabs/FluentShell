# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

FluentShell translates supported Win32 top-level windows into **real WinUI 3 windows** in a
**separate process**. It is not a DWM recoloring layer, and it never initializes WinUI inside
an injected third-party process.

```text
native Win32 HWND/control tree  (canonical state, the truth owner)
        | snapshot / patch                 ^ semantic action / result
        v                                  |
FluentShell.Bridge.dll -- versioned pipe -- Renderer\FluentShell.Renderer.exe
   (injected, C++)                            (separate WinUI 3 process, C#)
```

## Build And Test

Requires Visual Studio with the x64 C++ toolchain, Windows SDK 10.0.26100, and the .NET 8+ SDK.
Everything is x64-only; `Debug` and `Release` are the only configurations.

```powershell
.\build.ps1 -Configuration Release      # nuget + dotnet restore, build solution, publish renderer, verify payload layout
.\build.ps1 -Configuration Debug -CoreOnly   # skip renderer restore/publish; native projects only
.\build.ps1 -RestoreOnly                # restore and stop
.\test.ps1  -Configuration Release      # C# tests, native tests, then the production gates
```

`build.ps1` wipes the configuration's output directory first and fails if renderer or Bootstrap
runtime files escape `build\bin\x64\<Config>\Renderer\`. `test.ps1` runs both suites and then the
gates that keep the architecture honest: Bridge import-table inspection (no `Microsoft.UI`,
`Microsoft.WindowsAppRuntime`, or `CoreMessagingXP`), Bridge export presence, forbidden-payload
checks, and Injector CLI surface checks.

`test.ps1` exits 0 only when every suite and gate passed; any failure throws first. (Its last gate
runs the Injector with no arguments, which exits 1 by design, so the script ends with `exit 0`.)

### Faster inner loop

MSBuild lives under the VS install; resolve it with
`vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath` then
`MSBuild\Current\Bin\amd64\MSBuild.exe`.

```powershell
# Native only (Bridge pulls in detours)
& $msbuild src\FluentShell.Bridge.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /v:m

# C# tests
dotnet test tests\FluentShell.Renderer.Tests\FluentShell.Renderer.Tests.csproj `
    --configuration Debug -p:FluentShellTestBuild=true

# One C# test or class
dotnet test tests\FluentShell.Renderer.Tests\FluentShell.Renderer.Tests.csproj `
    --configuration Debug -p:FluentShellTestBuild=true `
    --filter "FullyQualifiedName~ProtocolValidatorTests"

# Native tests: build the project, then run the exe from the repo root
& $msbuild tests\FluentShell.Native.Tests\FluentShell.Native.Tests.vcxproj /m /p:Configuration=Debug /p:Platform=x64
.\build\tests\x64\Debug\FluentShell.Native.Tests.exe
```

`-p:FluentShellTestBuild=true` is required for the renderer under test: it redirects renderer
output to `build\tests\...\RendererUnderTest\` so a test build cannot contaminate the production
payload. The native test exe has no filter mechanism — it compiles the real
`WindowSnapshot.cpp`/`WindowCapture.cpp`/`ControlAdapters.cpp`/codec sources, creates real HWNDs,
and runs every `Test*()` registered in `wmain`. It must be run from the repo root because it loads
fixtures from `tests\ProtocolFixtures\`.

### Manual acceptance (the translation oracle)

`LegacyDialogHost.exe` is the acceptance target, not a demo. From the production output directory:

```powershell
$target = (Resolve-Path .\LegacyDialogHost.exe).Path
$process = Start-Process $target -PassThru
.\FluentShell.Injector.exe inject $target --pid $process.Id
```

Logs: Bridge → `%TEMP%\FluentShell.log`, renderer → `%TEMP%\FluentShell.Renderer.log`,
oracle → `%TEMP%\FluentShell.LegacyDialogHost.log`. Every projection attempt writes its capture,
gate, commit, UIA, and rollback result to the Bridge log; `Projection gate rejected surface:` names
the stage that refused.

### Driving projected MMC consoles (`tools\`)

MMC is the large real-world target (`docs\goals\win32-to-winui-translation\MMC-VALIDATION-MATRIX.md`).
These scripts start a disposable console (`__COMPAT_LAYER=RunAsInvoker`), inject the Release Bridge and
drive the projection with real pointer/keyboard input plus UIA, so nothing else may use the machine or
rebuild `build\bin\x64\Release` while they run, and a console left open (`-KeepOpen`) must be stopped
before the next build (a live renderer locks `clrjit.dll`):

- `Drive-ProjectedMmc.ps1 -Console <name> -Steps 'Verb:arg;...'` — one scripted session (tree/list
  select/open/context menu, menu bar, `Dialogs`, `CloseDialogs`, screenshots, `Interact` for the topmost
  dialog, `DumpDialog`/`DumpWindow` for UIA evidence, `ClickWindow` for a toolbar button or its arrow);
  `-Native` runs the same steps with no Bridge, the oracle for what a gesture does natively.
- `Crawl-ProjectedMmc.ps1` — navigation census: which scope-tree nodes of a console project.
- `Survey-ProjectedDialogs.ps1` — dialog census over `dialog-survey.json`: whether the dialog each flow
  opens is projected, stays native, or rolled the console back. Cancels everything; changes nothing.
- `Dump-WindowTree.ps1` / `Dump-NativeMsaa.ps1 -Hwnd` — native HWND and MSAA evidence for a refused control.
- `Dump-TypeLib.ps1 <file> [filter]` — a type library's interfaces, dispatch ids and vtable slots
  (how `SysmonInterfaces.h` was written; sysmon.ocx is read by vtable, never by IDispatch).
- `Dump-DialogResources.ps1 -Path <module> [-Id n,...]` — the dialog templates (RT_DIALOG) a module
  carries, control by control (class, id, style bits, rectangle, text): which controls a native dialog
  will need before it is ever opened. Static; the module is mapped as data.
- `Sample-ThreadStacks.ps1 -ProcessId <pid> [-Registers]` — where a hung or busy thread is: instruction
  pointer, registers (with the state of the memory each points into) and an approximate stack, with no
  debugger. Use it on a spinning `mmc.exe` or on the renderer's UI thread.
- `FLUENTSHELL_DIAG_CAPTURE=1` in the target's environment makes capture log every stage it is about to
  run (`capture classify/identity/facets/detail`, ListView groups and rows), so a control whose capture
  never returns is named by the last line of the Bridge log. Off by default (about a hundred lines a pass).
- `FLUENTSHELL_DIAG_RECONCILE=1` makes every reconcile republish (and every pixels-only change it holds
  back) log what moved -- the window field, menu, or first differing node, with the size of a chrome
  pixel change -- and dumps the first changing chrome regions to `%TEMP%\FluentShell.chrome-N-before/after.bmp`.
  A console that republishes while idle shows up here.

A target process that dies during a projection attempt did not necessarily hang: look in the Application
event log first (`Get-WinEvent -FilterHashtable @{LogName='Application'}`, providers Application Error and
Windows Error Reporting) for the faulting module and offset. A suspended-looking GUI thread is often one
sitting in WER after an unhandled exception.

A dialog's refusal is in the Bridge log (`Capture rejected child:`, `Owned modal dialog could not be
projected`). The injector refuses a target at a different integrity level, so from a non-elevated shell
the consoles that need elevation (`gpedit`, `secpol`, `diskmgmt`) cannot be driven: verify them from an
elevated session.

## Architecture

### Truth ownership

The native HWND tree and the original Win32 API arguments/results are canonical. The renderer owns
only WinUI projection and transient input state. Any failure — unsupported control, protocol fault,
renderer crash, capture timeout — restores the **entire** native top-level window. There is never a
hybrid surface with some native and some XAML controls.

### Per-window state machine

`Native -> Scanning -> SurfaceReady -> Projected -> Restoring -> Closed`, held in
`RendererSession::Surface` (`src/Bridge/Translation/RendererSession.cpp`). Two mutexes per surface:

- `canonicalMutex` — the **native-operation barrier**. Projection, revision resync, actions,
  reconcile, and rollback all take it, so no source-thread call can land after rollback begins.
  Reconcile takes it with `try_to_lock` and skips rather than blocking.
- `mutex` — guards the snapshot, ready record, and state fields.

`canonicalMutex` is always acquired before that surface's own `mutex`. `surfacesMutex_` is held only
for short lookups in the surface map and is never held while acquiring `canonicalMutex` — though
`ResolveOwnerProxy` does take it while `canonicalMutex` is already held, so the reverse order is not
available.

### The projection gate

`RendererSession::OpenSurface` is a sequence of named stages, each of which logs its own rejection
and returns false. The order is load-bearing: the proxy is validated **while hidden and cloaked**,
the native window is cloaked only against a revision the renderer already confirmed, and renderer
input is released last.

```
ResolveOwnerProxy -> PublishAndAwaitReady -> RunUiaGate("Prepared")
  -> SynchronizeNativeRevision -> CommitProvisional -> AwaitProxyVisible
  -> EstablishProxyZOrder -> VerifyNativeCloaked -> RunCommittedUiaGate -> CommitInteractive
```

Two-phase commit: `CommitProvisional` shows the proxy so the committed UIA gate can see it but keeps
renderer input gated (`surface.commit` with `interactive: false`); `CommitInteractive` releases the
gate. While gated, `TranslatedWindow.ApplyPreInteractiveGate` swallows input and pins geometry.
Stages after the provisional commit reject through `RejectAfterCommit`, which restores the native
window first.

### The owner graph

A dialog the application runs modally against an owner (the dialog manager disables the owner) is
projected as a surface of its own whose proxy is owned by the owner's proxy, at any depth: console,
property sheet, Color chooser (`IsModalOwnedTopLevel`). Discovery decides roots first and then each depth of
owner in turn (`OwnerDepth`); an owned window waits without using its attempt (`ownedWaiting_`) until its
direct owner is a projection. A dialog that cannot be projected (the Color chooser's subclass-painted
palettes, a shell file dialog, a `CBS_SIMPLE` Font chooser) hands every projected owner back to native
together (`HandOwnerGraphToNative`): DWM cloaking is inherited down the owner chain, so a native dialog
under a cloaked owner could not be seen. That dialog (`refusedDialogs_`, or any modeless owned window)
holds the root native while it is open; after it closes the root projects again once the owner graph has
been quiet for two seconds (`ownerGraphDeferrals_`) and the dialogs on it are offered again in the same pass
(`ReleaseOwnedWindows`). The renderer closes proxies deepest-first (`WindowRegistry.CloseAll`, and the
Bridge's `RestoreAll` restores in that order).
### Threading rules

- **All native reads and writes happen on the HWND's own GUI thread.** `SourceThreadAgent` installs
  three thread hooks on it: `WH_GETMESSAGE` dispatches bounded commands posted by
  `PostThreadMessageW` and waited on with a deadline (2 s default), while `WH_CBT` and
  `WH_CALLWNDPROCRET` only set the dirty flag that gates reconcile. The IPC and supervisor threads
  never touch native state directly — they queue work.
- Source-thread command handlers (`ExecuteCapture`, `ExecuteInvoke`, `ExecuteCloak`,
  `ExecuteCaptureAndCloak`, `ExecuteRestore`, `ExecuteShutdown`) return `true` while the command is
  still running and `false` once `AbortIfCancelled` has finished it. A handler that returns `false`
  must never be touched again.
- Long-running native work (`WM_CLOSE`, `WM_COMMAND`, `BM_CLICK`) is **posted, not sent**, because
  the handler may enter an application-owned modal loop that would blow the command deadline.
- The supervisor loop (`RunSupervisor`) ticks every 250 ms: discovery every 1 s, reconcile every
  tick. A quiet surface is fully recaptured only every 4th tick (`kQuietReconcileTicks`); the
  dirty flag is set by subclass procs for the messages in `RelevantMessage`.
- A capture whose only change is the pixels of a container's chrome regions
  (`OnlyChromePixelsChanged`) is published only once two consecutive captures agree on it and at least
  3 s have passed since the last such publication. MMC paints its menu-bar MDI buttons (the strip right
  of the menu toolbar) so that DWM's copy of the cloaked window showed them present and absent from one
  capture to the next; every flip republished the whole surface about once a second, and a click that
  landed between a capture and its publication was refused as stale. Anything else that changed is
  published at once.

### Protocol boundary

`src/Protocol/protocol-v1.schema.json` is the contract; `tests/ProtocolFixtures/*.json` are shared
by both sides' tests. A 32-byte little-endian `FLSH` frame header wraps a JSON payload.

- Bridge is the pipe server and authenticates renderer PID, process creation time, current logon
  SID, and a 128-bit nonce.
- Revisions, generations, and IDs travel as **canonical decimal strings** and HWNDs as `0x`-prefixed
  hex, so a JSON reader can never round them through a double.
- Property actions carry an expected revision and are rejected `stale` on mismatch. Request-semantic
  actions — `invoke`, `close`, `move`, `resize` (`IsRequestSemanticAction`) — are rebased onto the
  current revision instead, because the pointer, not a snapshot, decides whether a drag still
  applies. A menu-bar `menuCommand` stays revision-gated (the Bridge also checks the menu binding),
  but a click that lost a race with a reconcile is not lost: the renderer keeps the clicked item
  (path, command, label) and re-sends a `stale` refusal once, when the resync patch still carries
  that same enabled command (`MenuActionReplayPolicy`).
- A protocol violation naming one surface faults only that surface
  (`WindowRegistry.SurfaceScopeOf` → non-fatal `error` frame → Bridge rolls that window back).
  A session-scoped violation faults the session.

Changing the wire format means updating the schema, both codecs, the fixtures, and
`kProtocolMinor`/`kProtocolMajor` in `src/Bridge/Ipc/Protocol.h`.

### Where the support boundary is defined

`src/Bridge/Translation/ControlAdapters.cpp` is the registry. Two tables:

- `kClassAdapters` — native class name → a `Probe` function that inspects style bits and either
  names the `ControlKind` or rejects with a specific reason.
- `kCaptureTable` — `ControlKind` → the typed-state reader for that kind (null when the kind adds
  nothing beyond the common facets). Indexed by enum value, sized from the `ControlKind::Count`
  sentinel (append a new kind before it); an index past the table is rejected rather than silently
  capturing nothing.

Adding a control means adding one registry row plus its probe and capture functions — never editing
a chain of class or kind comparisons. `WindowCapture.cpp` orchestrates
(`CaptureTopLevelFacets` → `CaptureChildNodes` → per-child `ClassifyControl` → `AssignNodeIdentity`
→ `CaptureCommonNodeFacets` → `CaptureControlDetail`) and appends HWND/class/style evidence to every
rejection. The renderer mirrors the boundary in three tables:
`ProtocolValidator.KindRules`, `ControlFactory.Create`'s kind switch, and
`ProtocolValidator.RequiredByFrame`. All of these must stay in sync, and
`docs/goals/win32-to-winui-translation/CONTROL-ADAPTER-ROADMAP.md` records the staged expansion plan
and why universal pixel translation is not a goal.

Node identity survives reconcile but never outlives its HWND: a bridge-owned window property
(`FluentShell.Bridge.NodeGeneration`) carries a lifecycle generation, so a control recreated at the
same address gets a fresh node ID.

### Renderer structure

`Program.cs` → `App` → `PipeClient` (frames, heartbeat, parent-process watch) →
`WindowRegistry` (surface lifetime, modal owner blocking, retired-surface tracking) →
`TranslatedWindow` (one WinUI window per surface) → `ControlFactory` (kind → real WinUI control with
bindings and UIA peers) → typed `WindowViewModel`/`ControlNodeViewModel`.

`ProtocolSerializer` only binds bytes to typed messages; every admission rule lives in
`ProtocolValidator`, which runs three gates in order: required raw-JSON fields → session identity
and canonical naming → per-message caps, ranges, and shape. Win32 interop for the proxy window is
isolated in `NativeWindowInterop.cs`/`WindowMessages`.

### Injection safety

`FluentShell.Injector.exe inject <absolute-image-path> [--pid <pid>] [--sha256 <hex>]` is the only
production entry point. The target is resolved through a file
handle; the path read from the selected process must match that canonical path; `--pid` is mandatory
when several processes share the image; injector and target integrity levels must match. Shell,
security, XAML hosts, the renderer, and FluentShell's own tools are hard-denied in
`src/Common/ProcessPolicy.h`. `--signer` is deliberately rejected until implemented. There is no
process-name injection, system-wide discovery, or watch mode.

`dllmain.cpp` starts `BridgeWorker` from `DLL_PROCESS_ATTACH`: pin the module, refuse denied
targets and processes that already host XAML, start the `RendererSession`, install the
MessageBox/MessageBoxEx/MessageBoxIndirect/TaskDialog/TaskDialogIndirect Detours hooks, then run the
supervisor. Exports: `FluentShell_Start`, `FluentShell_IsRendererReady`, `FluentShell_Ping`.

### Translated dialogs

A MessageBox or TaskDialog whose owner is a projection is answered by the Bridge as a virtual
surface (no native dialog is created); anything it cannot honour stays native. The layout is the
dialog's own (`DialogSnapshots.cpp`): text is measured with DirectWrite in Segoe UI with the system
fallback -- what XAML draws, CJK included -- and the dialog is sized to it, with the buttons
(user32's localized labels, `MB_GetString`) right-aligned in a command band the renderer shades
from the band's first row. A task dialog runs in `VirtualTaskDialog`: a hidden host window on the
caller's thread is the HWND its callback gets, every `TDN_*` notification runs on that thread, and
`TDM_*` messages (from the callback or any thread) and renderer actions (posted by the action worker)
rebuild the dialog and patch the projection. The host is the surface's `nativeHwnd`, so a MessageBox
the callback opens over it is owned by the task dialog's proxy. The hook depth guard counts only
native calls: a dialog opened from a translated dialog's callback is translated too.

## Hard Invariants

These are enforced by `test.ps1` gates or by `docs/goals/win32-to-winui-translation/PLAN.md`:

- The production Bridge must not import WinUI, Windows App SDK, or CoreMessaging, and must not
  initialize WinUI in the target process.
- Never mix native and WinUI controls in one translated surface.
- Do not modify anything under `ref_src` — it is read-only reference material. WinUIShell informs the
  server/IPC/lifetime shape only; its reflection RPC layer is not copied. The GPL-3.0
  `win32-ui-modernizer` sample is behavioral research and contributes no code.
- Do not reintroduce Bootstrap, in-process Island/Strategy C, dialog repaint overlays, broad system
  injection, or name-only injection.
- Renderer and Windows App SDK runtime files stay under `Renderer\`; nothing but
  `FluentShell.Injector.exe`, `FluentShell.Bridge.dll`, and `LegacyDialogHost.exe` at the
  production root.

## Repository Notes

- The native projects restore only CppWinRT (`src/packages.config`); WinUI and the Windows App SDK
  belong to the renderer's own `PackageReference`s and never to native code.
- Source files are CRLF and carry localized comments/diagnostics; `Directory.Build.props` forces
  `/utf-8` so MSVC's character sets stay deterministic across locales.
- `ProgressBar` state changes and other native mutations are only observed for the messages listed in
  `RelevantMessage` (`SourceThreadAgent.cpp`); a new adapter that reacts to a new message must add it
  there or reconcile will only notice on the slow poll.
- Performance Monitor's chart reads the System Monitor control (sysmon.ocx) through hand-declared
  vtable interfaces (`SysmonInterfaces.h`), never IDispatch: its dispatch dies process-wide when any
  instance is destroyed. A live chart keeps no readable history, so `Sysmon.cpp` subscribes to the
  control's `OnSampleCollected` event and records every counter's value (`PerformanceChart.cpp` lays
  the recording out as the graph's slots).
- comctl32 6.10.26100 copies a ListView group's header, footer, subtitle and task text into the caller's
  buffer without checking that the group has that text: a group inserted without a footer (every group of
  Performance Monitor's Add Counters list) is a null read that ends the whole application. Group text is
  therefore read one field per query through `QueryGroupInfo` (`ControlAdapters.cpp`), which calls the class
  window procedure directly under a structured handler -- an exception raised below `SendMessage` crosses
  a kernel callback and never reaches the sender -- and treats a null read as an absent text.
- A positioned (Canvas) ListView builds only the items within two viewports of the native viewport
  (`InBuildWindow` in `ControlFactory.cs`): the accessibility provider has to describe every element that
  exists, and a couple of hundred group bands kept it busy for seconds, longer than a UIA gate waits.
  Native scrolling recaptures the list, which builds the rest as the viewport reaches it.
- A child `#32770` with neither `DS_CONTROL` nor `WS_EX_CONTROLPARENT` (the pages of an OLE property
  frame, MFC form views) runs its own dialog manager, so it is a tab-order scope of its own, like an MDI
  child (`IsIndependentDialogPage` in `WindowCapture.cpp`); a scope's order starts at its first tab stop
  (`GetNextDlgTabItem` from a null control). A dialog procedure reports `WM_DRAWITEM` handled through
  `DWL_MSGRESULT`, which a sender reads as 0: owner-draw capture judges handling by painting too.
- XAML `Focus()` activates the window it is in. A proxy rebuild restores its remembered focus only while
  the window is active (`TranslatedWindow.RestoreFocus` defers it otherwise): doing it inactive lifted the
  console proxy over a dialog opened over it (MMC's property sheets are owned by a hidden helper window,
  so the dialog proxy has no z-order tie to the console's).
- The Bridge's `close` action is `WM_SYSCOMMAND`/`SC_CLOSE`, what the native close button sends: some
  dialogs (PDH's Add Counters) ignore a bare `WM_CLOSE`.
- The UIA gate's per-transaction timeout (1 s) and whole-pass budget (5 s) grow with the snapshot's size
  (`BudgetFor` in `UiAutomationValidator.cpp`; a small dialog keeps the tight bound that notices a stuck
  provider), because a XAML provider answers each element of a cross-process enumeration from its UI thread.
- WinUI's own check box and radio button templates assume a 32 px row (a 20 px glyph in a 32 px cell) and clip
  in a native-sized (13 to 16 px) control: a radio button showed as an arc. `CompactControlStyles.cs` supplies
  16 px-glyph templates for both (and `WrapWhenTall`: a label wraps only when the control has room for two
  lines, because the capture does not carry `BS_MULTILINE`). A style is used only if every theme resource it
  names exists; otherwise the control keeps WinUI's template and the renderer log says `compact ... style not
  used`.
- A date or time picker (`SysDateTimePick32`: kinds `datePicker` and `timePicker`, protocol minor 37) carries its
  value in the range fields every node has, so `setValue` needs no new shape: `position` is a date's day number
  (days since 0001-01-01, `DateOnly.DayNumber`, `DateTimePickerValue.h`) or a time's second of the day. A
  `setValue` sets `DTM_SETSYSTEMTIME` and then sends the parent `DTN_DATETIMECHANGE`, which is the only way a
  Windows Forms `DateTimePicker` learns its new `Value`. A Windows Forms `GroupBox` owns its controls as child
  windows, so a `groupBox` node is a container.