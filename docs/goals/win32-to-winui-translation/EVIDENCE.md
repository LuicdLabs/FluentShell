# Win32 Whole-Window Translation Evidence

## Evidence Status

The architecture and automated contract tests are implemented. The first
acceptance slice is **implemented but unproven on the current final payload**.
The latest runtime audit hit the committed UIA isolation gate and restored the
native window, so this goal must not be marked complete yet.

Evidence was collected on 2026-07-23 on the local x64 Windows desktop at 96
DPI. The old screenshots and the earlier PID pair in this directory are
historical evidence only; they predate the latest tab-order and modal-lifetime
changes and are not proof for the current binary.

## Automated Evidence

The following commands were run successfully:

```powershell
.\build.ps1 -Configuration Release
.\test.ps1 -Configuration Release
```

Recorded results:

- Release native build and renderer publish completed successfully (latest
  publish completed around `14:45:28`).
- `FluentShell.Renderer.Tests`: `64 passed, 0 failed`.
- `FluentShell.Native.Tests`: passed.
- Bridge dependency, export, and Injector CLI cutover gates: passed.
- The Bridge import gate found no `Microsoft.UI.*`,
  `Microsoft.WindowsAppRuntime`, or `CoreMessagingXP` imports.
- Production layout and Bootstrap/Island cutover checks passed in the test
  script.

The fixture suite covers the fixed 32-byte frame codec, payload/depth/node
limits, nonce and peer identity, version mismatch, sequence/revision ordering,
malformed input, typed ViewModel binding, tab-index validation, and presenter
action coordination.

## Runtime Audit

### Partial projection session

A freshly injected `LegacyDialogHost.exe` (PID `1632`) created an authenticated
renderer (`FluentShell.Renderer.exe`, PID `14808`) and the renderer applied
hundreds of ordered `window.patch` messages and accepted presenter actions. The
native capture log showed focusable nodes with unique tab indexes `0..8`.
This proves pipe/session/revision activity, but the same session did not yield a
current Computer Use UIA tree that unambiguously belonged to the renderer.

### Latest committed-surface failure

In the latest audit, the renderer successfully sent `surface.ready`:

```text
14:57:47.568 window.open ... revision=1 nodes=15
14:57:47.963 surface prepared ... uia=True bounds=182,182,720x500
```

The Bridge then rejected the commit:

```text
14:57:48.318 Committed UIA gate failed: screen hit-test does not resolve to the proxy tree
14:57:48.318 Projection gate rejected surface: committed UIA validation failed
```

Native fallback was therefore selected. This is the required fail-safe result,
but it is also a failed projection acceptance gate: the final binary did not
prove that Computer Use/UIA sees only the XAML proxy after commit.

### Historical target-perspective evidence

Earlier evidence captured the main window, MessageBox, static TaskDialog,
native timer binding, unsupported-child fallback, renderer termination recovery,
and Chrome isolation. It remains useful as regression history, but the recorded
PIDs (`7220`/`8012`) and screenshots are stale after the latest source changes.
The following must be repeated on one clean, single-owner build before they
can be counted as final acceptance:

- `FrameworkId=XAML` proxy-only UIA tree and focus order.
- Edit/check/ComboBox/ListBox round trips and native timer patches.
- MessageBox result and owner modality.
- TaskDialog verification checkbox followed by `Yes` (`verification=true`).
- Native close veto, unsupported-child whole-window fallback, and renderer kill
  recovery.

## Required Fixes Before Completion

1. **Committed UIA gate is intermittent/failing.** Diagnose why the screen
   hit-test does not resolve to the renderer proxy after `surface.ready`; fix
   z-order, activation, cloak/visibility timing, or validator assumptions, then
   reproduce a proxy-only Computer Use/UIA tree on the final payload.
2. **TaskDialog verification is not reproven.** The latest attempted checkbox
   interaction closed the dialog with `button=6, verification=false`. Re-run it
   using a separately selected modal window and verify the checkbox action is
   routed to the verification node before the button action. Treat the behavior
   as a bug until `verification=true` is observed on the rebuilt binary.
3. **Synchronize source and production output.** A prior diagnostic build left
   high-volume `Captured focusable node` messages in `%TEMP%\FluentShell.log`,
   while the current source no longer contains that logging. Run one clean,
   non-concurrent build/test after all edits, then record hashes for Bridge,
   Injector, host, and renderer artifacts.
4. **Repeat target-perspective evidence after the clean build.** Current
   `list_windows()` observations sometimes return the native app identity even
   while a renderer process is alive; determine whether this is fallback,
   window ownership, or UIA de-duplication before claiming isolation.
5. **Add integration coverage for cancellation races.** The source-thread
   agent checks cancellation around native actions, but there is no test proving
   that a timeout/Stop cannot issue a late `WM_CLOSE`, `BM_CLICK`, text, or
   selection mutation. Keep the whole-window restore path as the outcome.

## Implemented But Unproven Gates

- 150% and 200% DPI, DPI changes, and cross-monitor transitions.
- Named-pipe spoof attempt against a live randomized pipe.
- Owning UI-thread stall and the two-second source-thread acknowledgement
  timeout.
- Renderer termination before `surface.ready`, after cloak/before commit,
  during active input, and during modal close.
- Three-heartbeat-miss timing as distinct from immediate process/pipe failure.
- Full Alt-Tab/taskbar ordering across multiple physical monitors.
- Final WER/Application Error query for the latest runtime audit.
- Final Chrome isolation pass against the same rebuilt payload. The earlier
  Chrome tab evidence is preserved and must not be overwritten.

## Review Outcome

The implementation remains aligned with the approved plan's truth owner,
protocol boundary, and production cutover. It is **partially aligned at the
evidence gate** because the current committed UIA validation failed and the
modal verification result was not reproduced. POST-plan, correctness,
maintainability, and verifier reviews must record the fixes or explicitly keep
these blockers open before completion.
