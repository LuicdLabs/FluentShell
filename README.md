# AnyFluent

AnyFluent translates supported Win32 top-level windows into real WinUI 3 Fluent windows. It is not a DWM recoloring layer and it does not initialize WinUI inside an injected third-party process.

## Architecture

```text
native Win32 HWND/control tree (canonical state)
        | snapshot / patch                 ^ semantic action / result
        v                                  |
FluentShell.Bridge.dll -- versioned pipe -- FluentShell.Renderer.exe
      target process                         separate WinUI 3 process
```

- `FluentShell.Bridge.dll` is the only injected production DLL. It captures supported native controls on their owning UI thread and owns rollback.
- `Renderer\FluentShell.Renderer.exe` is a self-contained, unpackaged x64 WinUI 3 process. It owns XAML windows, typed view models, binding, and temporary input state.
- The native HWND tree remains authoritative. A renderer or protocol failure restores the complete native top-level window.
- Unsupported controls cause whole-window fallback. AnyFluent does not mix a native subtree into a translated WinUI surface.

The current control boundary is bounded textual HMENU command bars, standard
Static, Button/check/radio/GroupBox, Edit, dropdown-list/editable dropdown
ComboBox, ListBox, and determinate horizontal ProgressBar controls plus
supported MessageBox and static TaskDialog shapes. Owner-draw, custom HWND
classes, RichEdit, virtual controls, embedded browser/XAML,
layered/nonrectangular windows, and custom non-client rendering remain native.
The staged adapter expansion is documented in
`docs/goals/win32-to-winui-translation/CONTROL-ADAPTER-ROADMAP.md`.

## Safety Boundary

Injection is explicit and path-bound:

- The target is an existing absolute executable image path, resolved through a file handle.
- The path read from the selected process must match that canonical path.
- If multiple processes use the same image, `--pid` is required.
- `--sha256` can pin the target file. Signer pinning is intentionally rejected until implemented.
- Shell, security, XAML hosts, the AnyFluent renderer, and AnyFluent tools are hard-denied.
- There is no process-name injection, system-wide discovery, or injection watch mode.

The one-shot `l0` command is the only diagnostic exposed by the production Injector. `IslandDemo` remains a source-level diagnostic and is excluded from the default production build.

## Build And Test

Requirements are Visual Studio with the x64 C++ toolchain, Windows SDK 10.0.26100, and the .NET 8 or newer SDK.

```powershell
.\build.ps1 -Configuration Release
.\test.ps1 -Configuration Release
```

Production output is isolated as follows:

```text
build\bin\x64\Release\
  FluentShell.Injector.exe
  FluentShell.Bridge.dll
  LegacyDialogHost.exe
  Renderer\
    FluentShell.Renderer.exe
    ... WinUI 3, Windows App SDK, and .NET runtime payload ...
```

The build clears this configuration's output directory before compiling, publishes the renderer only under `Renderer`, and fails if Bootstrap or renderer runtime files appear at the production root.

## Run The Translation Oracle

Open PowerShell in the production output directory:

```powershell
$target = (Resolve-Path .\LegacyDialogHost.exe).Path
$process = Start-Process $target -PassThru
.\FluentShell.Injector.exe inject $target --pid $process.Id
```

When there is exactly one matching process, `--pid` may be omitted. To pin the binary:

```powershell
$hash = (Get-FileHash $target -Algorithm SHA256).Hash
.\FluentShell.Injector.exe inject $target --pid $process.Id --sha256 $hash
```

`LegacyDialogHost` exposes Edit, CheckBox, ComboBox, ListBox, GroupBox, a
native-updated ProgressBar and timer value, MessageBox/TaskDialog result
reporting, a close-veto toggle, and a button that creates an unsupported custom
child. The log is `%TEMP%\FluentShell.LegacyDialogHost.log`; Bridge diagnostics
use `%TEMP%\FluentShell.log`.

Expected behavior:

- UI Automation identifies the projected controls as XAML and exposes their normal patterns.
- User edits and selections return to the native controls; native timer text patches the WinUI view.
- Dialog button IDs and verification state return to the native host and appear in its result field/log.
- Selecting close veto rejects a proxy close because the native `WM_CLOSE` handler keeps the HWND alive.
- Creating the custom child or terminating the renderer restores the whole native window without terminating the target.

## Diagnostics

```powershell
.\FluentShell.Injector.exe l0
```

The default solution lists `FluentShell.Island` and `IslandDemo` for manual research but does not build them. When that legacy diagnostic is needed, build `src\PoC\IslandDemo\IslandDemo.vcxproj` explicitly with an `OutDir` outside the production folder and run `IslandDemo.exe` there; it is not an Injector entry point.

Reference source under `ref_src` is read-only. WinUIShell informs the server/IPC/lifetime shape; its reflection RPC layer is not copied. The GPL-licensed modernizer sample is behavioral research only and contributes no copied implementation.
