# Win32 Whole-Window Translation Evidence

## Evidence Status

The architecture and automated contract tests are implemented. The first
acceptance slice is **implemented but unproven on the current final payload**.

The prior full runtime audit hit the committed UIA isolation gate and restored
the native window. The post-change smoke below no longer reproduces the
immediate rollback, but the goal must not be marked complete yet.

After the committed-gate retry change, a clean Release payload was injected
into a freshly started `LegacyDialogHost.exe` on 2026-08-15. The bridge log
recorded:

```text
Projected native window: AnyFluent Win32 Translation Oracle
```

No committed UIA gate failure or immediate native restore was recorded during
the four-second smoke window. This is a projection smoke result, not complete
target-perspective acceptance evidence; the UIA interaction and rollback
scenarios below still need to be repeated.

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

- Release native build and renderer publish completed successfully.
- `FluentShell.Renderer.Tests`: `79 passed, 0 failed`.
- `FluentShell.Native.Tests`: passed.
- Bridge dependency, export, and Injector CLI cutover gates: passed.
- The Bridge import gate found no `Microsoft.UI.*`, `Microsoft.WindowsAppRuntime`, or `CoreMessagingXP` imports.
- Production layout and Bootstrap/Island cutover checks passed in the test script.

The post-change runtime smoke command also completed successfully:

```powershell
$target = (Resolve-Path '.\build\bin\x64\Release\LegacyDialogHost.exe').Path
Start-Process $target
.\build\bin\x64\Release\FluentShell.Injector.exe inject $target
```

The fixture suite covers the fixed 32-byte frame codec, payload/depth/node
limits, nonce and peer identity, version mismatch, sequence/revision ordering,
malformed input, typed ViewModel binding, tab-index validation, and presenter
action coordination.

## Runtime Audit

### Post-change projection smoke

A clean Release `LegacyDialogHost.exe` was injected with the matching Release
Bridge and Injector. The target remained running after injection, and the log
recorded `Projected native window` without a committed UIA gate failure or
immediate restore during the four-second observation window.

On 2026-08-15 the oracle was expanded with a native `BS_GROUPBOX` and a
determinate `msctls_progress32` whose value changes every second. A matching
Release Bridge and renderer opened the 17-node surface, committed it, and
continued applying native-authoritative patches:

```text
window.open ... revision=1 nodes=17
surface prepared ... uia=True ...
Projected native window: AnyFluent Win32 Translation Oracle
window.patch applied rev=3 ...
window.patch applied rev=4 ...
```

This proves runtime projection and reconciliation for the first expanded
control tranche. It is not a substitute for final Computer Use/UIA capture.

### Menu and Dialer projection

The oracle was expanded with a nested textual HMENU. Windows UI Automation
expanded `File -> Tools`, invoked `Reset progress`, and the native host recorded:

```text
menu command reset progress
```

The renderer returned an accepted action and continued canonical patches. This
proves the menu action crossed WinUI UIA, renderer IPC, source-thread
`WM_COMMAND`, and native recapture.

After adding bounded textual HMENU and editable `CBS_DROPDOWN` support, a fresh
Release payload was injected into `C:\Windows\System32\dialer.exe`. The main
window committed after one transient UIA deadline retry:

```text
Committed UIA gate attempt 1 failed: UIA validation deadline expired
Projected native window: Phone Dialer
```

An owned startup `#32770` top-level remained native because owned-window graph
translation is not implemented. This is main-window projection evidence, not a
claim that every Dialer top-level is translated.

### Partial projection session

A freshly injected `LegacyDialogHost.exe` (PID `1632`) created an authenticated
renderer (`FluentShell.Renderer.exe`, PID `14808`) and the renderer applied
hundreds of ordered `window.patch` messages and accepted presenter actions. The
native capture log showed focusable nodes with unique tab indexes `0..8`.
This proves pipe/session/revision activity, but the same session did not yield a
current Computer Use UIA tree that unambiguously belonged to the renderer.

### Latest committed-surface failure

The earlier audit recorded:

```text
14:57:47.568 window.open ... revision=1 nodes=15
14:57:47.963 surface prepared ... uia=True bounds=182,182,720x500
14:57:48.318 Committed UIA gate failed: screen hit-test does not resolve to the proxy tree
14:57:48.318 Projection gate rejected surface: committed UIA validation failed
```

That failure motivated the bounded post-commit UIA retries. The smoke result
above no longer reproduced an immediate rollback, but full UIA proof remains
outstanding.

Native fallback remains the required fail-safe result when the bounded gate
cannot validate the committed proxy.

### Historical target-perspective evidence

Earlier evidence for PIDs `7220`/`8012` captured the main window, MessageBox, static TaskDialog,
native timer binding, unsupported-child fallback, renderer termination recovery,
and Chrome isolation. It remains useful as regression history, but the recorded
screenshots and process IDs are stale after the latest source changes.

The following must be repeated on one clean, single-owner build before they can
be counted as final acceptance:

- `FrameworkId=XAML` proxy-only UIA tree and focus order.
- Edit/check/ComboBox/ListBox round trips and native timer patches.
- MessageBox result and owner modality.
- TaskDialog verification checkbox followed by `Yes` (`verification=true`).
- Native close veto, unsupported-child whole-window fallback, and renderer kill recovery.

## Required Fixes Before Completion

1. **Reprove the committed UIA gate.** Capture a proxy-only Computer Use/UIA tree on the final payload and confirm the bounded retry does not hide a persistent native or foreign-process hit.
2. **Reprove TaskDialog verification.** The latest historical attempt closed with `button=6, verification=false`; select the modal separately and verify the checkbox action precedes the button action and yields `verification=true`.
3. **Synchronize source and production output.** Run one clean, non-concurrent build/test and record hashes for Bridge, Injector, host, and renderer artifacts.
4. **Repeat target-perspective evidence.** Determine whether native app identity observations represent fallback, window ownership, or UIA de-duplication before claiming isolation.
5. **Add cancellation-race integration coverage.** Prove timeout/Stop cannot issue a late `WM_CLOSE`, `BM_CLICK`, text, or selection mutation.

## Implemented But Unproven Gates

- 150% and 200% DPI, DPI changes, and cross-monitor transitions.
- Named-pipe spoof attempt against a live randomized pipe.
- Owning UI-thread stall and the two-second source-thread acknowledgement timeout.
- Renderer termination before `surface.ready`, after cloak/before commit, during active input, and during modal close.
- Three-heartbeat-miss timing as distinct from immediate process/pipe failure.
- Full Alt-Tab/taskbar ordering across multiple physical monitors.
- Final WER/Application Error query for the latest runtime audit.
- Final Chrome isolation pass against the same rebuilt payload.

## Review Outcome

The implementation remains aligned with the approved plan's truth owner,
protocol boundary, and production cutover. The bounded committed UIA retry
reduces false rollback caused by compositor/UIA propagation timing, and the
post-change smoke test projected the target window successfully. The goal is
still **partially aligned at the evidence gate** until the full target-perspective
UIA, interaction, modal, rollback, and browser-isolation scenarios are repeated.
