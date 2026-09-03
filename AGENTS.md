# AGENTS.md

This file provides guidance to Any AI Coding Agents (Codex, OpenCode, Pi) when working with code in this repository.

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
.\build.ps1 -Configuration Debug -CoreOnly   # skip the renderer publish; native projects only
.\build.ps1 -RestoreOnly                # restore and stop
.\test.ps1  -Configuration Release      # C# tests, native tests, then the production gates
```

`build.ps1` wipes the configuration's output directory first and fails if renderer or Bootstrap
runtime files escape `build\bin\x64\<Config>\Renderer\`. `test.ps1` runs both suites and then the
gates that keep the architecture honest: Bridge import-table inspection (no `Microsoft.UI`,
`Microsoft.WindowsAppRuntime`, or `CoreMessagingXP`), Bridge export presence, forbidden-payload
checks, and Injector CLI surface checks.

**`test.ps1` leaves a nonzero exit code even when everything passes.** Its last gate runs the
Injector with no arguments (which exits 1 by design) and the script never resets `$LASTEXITCODE`.
Read the printed gate lines, not the exit code.

### Faster inner loop

MSBuild lives under the VS install; resolve it with
`vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath` then
`MSBuild\Current\Bin\amd64\MSBuild.exe`.

```powershell
# Native only (Bridge pulls in Core + detours)
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
  applies.
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
  nothing beyond the common facets). Indexed by enum value, sized from the last `ControlKind`
  enumerator; a kind declared past `StatusBar` falls outside and is rejected rather than silently
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
production entry point (plus the one-shot `l0` DWM diagnostic). The target is resolved through a file
handle; the path read from the selected process must match that canonical path; `--pid` is mandatory
when several processes share the image; injector and target integrity levels must match. Shell,
security, XAML hosts, the renderer, and FluentShell's own tools are hard-denied in
`src/Common/ProcessPolicy.h`. `--signer` is deliberately rejected until implemented. There is no
process-name injection, system-wide discovery, or watch mode.

`dllmain.cpp` starts `BridgeWorker` from `DLL_PROCESS_ATTACH`: pin the module, refuse denied
targets and processes that already host XAML, start the `RendererSession`, install the
MessageBox/TaskDialog Detours hooks, then run the supervisor. Exports: `FluentShell_Start`,
`FluentShell_IsRendererReady`, `FluentShell_Ping`.

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

- `FluentShell.Island` (`src/IslandHost`) and `IslandDemo` are listed in the solution but **not
  built** in either configuration. They are source-level diagnostics; build `IslandDemo.vcxproj`
  explicitly with an `OutDir` outside the production folder if you ever need it.
- `src/Renderer.Dwm` is the cross-process DWM attribute layer behind the `l0` diagnostic; it is not
  part of the translation path.
- Source files are CRLF and carry localized comments/diagnostics; `Directory.Build.props` forces
  `/utf-8` so MSVC's character sets stay deterministic across locales.
- `ProgressBar` state changes and other native mutations are only observed for the messages listed in
  `RelevantMessage` (`SourceThreadAgent.cpp`); a new adapter that reacts to a new message must add it
  there or reconcile will only notice on the slow poll.
