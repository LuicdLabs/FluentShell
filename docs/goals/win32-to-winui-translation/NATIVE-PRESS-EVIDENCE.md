# The native press and Explorer's security warning: what the dialog actually gates on

Status: evidence, 2026-10-22. It contradicts the premise the working tree's
`NativePressActivation` change was written against, so it is recorded before the
code is changed again.

## The target

`開啟檔案 - 安全性警告` ("Open File - Security Warning") is **shdocvw's dialog
template 4416**, shown by `shdocvw!SafeOpenPromptForShellExec`. Its controls are
fixed and known:

| control | id   |
|---------|------|
| 執行 / Run (the template's 開啟)  | **4426** |
| 儲存 / Save                      | 4427 |
| 取消 / Cancel                    | **2** |
| 開啟這個檔案前一定要先詢問 (check) | 4428 |

The title string lives in `urlmon.dll.mui`; the template is in `shdocvw.dll`.
`tools\Dump-DialogResources.ps1 -Path C:\Windows\System32\shdocvw.dll` prints it.

Neither `shdocvw.dll` nor `urlmon.dll` imports `GetCurrentInputMessageSource`,
`GetAsyncKeyState`, `GetMessageExtraInfo`, `GetMessageTime`, `GetMessagePos` or
`GetLastInputInfo`, so the dialog has no way to ask whether the click came from
real hardware. (`shell32.dll` and `explorerframe.dll` do import them; this
dialog is not theirs.)

## The experiment

The dialog is reproduced outside any projection: an inert `probe.bat`
(`@exit /b 0`) carrying a mark-of-the-web (`Zone.Identifier`, `ZoneId=3`) is run
through `ShellExecute`, which shows the prompt in the calling process. A second
process then posts `BM_CLICK` to control 4426 — exactly what the Bridge's
`PressNativeButton` does — and reports whether the dialog acted.

A DWM application cloak only takes effect when the owning process sets it, so
the child that shows the dialog cloaks it from a thread of its own while its
main thread sits in the dialog's modal loop. That reproduces a projection's
cloak faithfully; setting `DWMWA_CLOAK` cross-process silently does nothing
(`DWMWA_CLOAKED` stays 0), which is worth knowing on its own.

Scripts: `Probe-SecurityWarning.ps1`, `Isolate-RunGate.ps1`, `Test-CloakedRun.ps1`
(session scratchpad).

## The results

| state of the dialog when `BM_CLICK` is posted to 4426 | outcome |
|---|---|
| foreground, uncloaked (a plain native dialog) | **Run accepted**, file runs, dialog closes |
| foreground, **DWM application-cloaked** (`DWMWA_CLOAKED=0x1`) | **Run accepted** |
| foreground, cloaked, **focus taken first** (`WM_NEXTDLGCTL` to 4426) | **Run accepted** |
| foreground, uncloaked, **re-cloaked immediately after the post** | **Run accepted** |
| foreground, uncloaked, **covered by a topmost window** | accepted (one refusal in 3 runs — not reproducible) |
| posted from **another process** rather than the dialog's own thread | **Run accepted** |

Every combination was repeated; only the "covered" row ever refused, once, and
it did not reproduce on repeat or with the same window placed beside the dialog
instead of over it. Treat that single refusal as noise, not a gate.

## What this means

The premises the working tree is built on are **not** what this dialog does:

- it does **not** require the window to be on screen — a cloaked window's Run
  button runs the file;
- it does **not** refuse a click delivered re-entrantly or require the click to
  come from the application's own message loop — a posted *and* a sent click are
  both accepted;
- it does **not** require the borrowed state to outlive the press — re-cloaking
  immediately after the post changes nothing;
- it does **not** check the input source, and it has no imported API with which
  it could.

So `NativePressActivation`'s uncloak, the foreground hold across dispatch, the
`CompleteNativePress` observation, the 2 s deadline and the `SourceHook` requeue
do not address the reason the projected Run button does nothing. The comments in
`NativePressActivation.h`/`.cpp` and the matching paragraph in `CLAUDE.md`
("runs the file only while ... a window that is on screen", "the shell refuses a
click that arrives that way") state a rule the dialog does not have.

## What is also ruled out

- **The dialog procedure did not crash.** No `Application Error` or
  `Windows Error Reporting` entry for `explorer.exe` around the failing presses
  (05:16 on 2026-10-22); the only Bridge faults in the log are in other
  processes (`cc-switch.exe`, `WindowsTerminal.exe`, both
  `FluentShell.Bridge.dll+0x2bddbc` — a separate bug worth its own look).
- **The wrong control is not being pressed.** The proxy is at
  `720,348 480x336`; a two-button right-aligned layout puts 執行 at x≈981..1075
  and 取消 at x≈1084..1178. The failing presses logged `cursor=1014,552` and the
  working one `cursor=1117,550`, both `overButton=yes` against the *native*
  button's rectangle. The user's clicks landed on the projected 執行 and the
  Bridge pressed the native control underneath it.

## The bisection: projecting the dialog is what breaks Run

The dialog can be reproduced under a real projection without Explorer and without
global apply: shdocvw shows it in whatever process calls `ShellExecute` on a
mark-of-the-web file, and `powershell.exe` is not on the injection deny list
(`src/Common/ProcessPolicy.h`). So a hidden `powershell.exe` host is injected with
`FluentShell.Injector.exe inject`, the host then shows the dialog, and the Run
button is pressed either through the proxy (UIA invoke, what the user's click
does) or straight at native control 4426 with the Bridge uninvolved.
`Bisect-ProjectedRun.ps1` does this; the probe file records that it ran, so "the
file ran" is observable independently of the dialog's window.

| setup | how Run was pressed | result |
|---|---|---|
| no Bridge at all (harness control) | native `BM_CLICK` on 4426 | **dialog closed, file ran** |
| Bridge injected, held **dormant** (never projects) | native `BM_CLICK` on 4426 | **dialog closed, file ran** |
| Bridge injected, dialog **projected** | proxy's Run invoked through UIA | dialog stayed open, file did not run |
| Bridge injected, dialog **projected** | native `BM_CLICK` on 4426, Bridge uninvolved | dialog stayed open, file did not run |
| projected, then rollback signalled, native window uncloaked | native `BM_CLICK` on 4426 | dialog stayed open, file did not run |

What this exonerates, by measurement rather than by argument:

- **the press path** — the native press fails identically when the Bridge has
  nothing to do with the press;
- **injection, the Detours hooks and anything `dllmain` installs** — a dormant
  Bridge is injected, hooked and running, and Run works;
- **the shell's own rule** — the same press on the same control in the same host
  works until the window is projected.

So the cause is in what `OpenSurface` does to that window, and the instrumented
press confirms the shell is reached and declines: the log line reads
`Native press state (handled, the dialog got BN_CLICKED for id=4426)` with
`id=4426 enabled=yes visible=yes style=0x50010000`, `onScreen=yes`, `layered=no`,
the native dialog in the foreground and the focus on the button — every state the
old theory said was needed — and the dialog still does not act.

Narrowing it further means bisecting what projection does to the window
(the subclasses, the capture's reads and owner-draw painting, the UIA gate, the
thread-focus handover, the taskbar-tab removal), one at a time, against this
repro.

Loose ends this turned up, each worth its own look:

- **Rollback leaves the proxy alive.** Signalling
  `Local\FluentShell.Rollback.<pid>.<hwnd>` uncloaks the native window, but the
  proxy window was still present 20 s later in every run.
- **A Bridge fault in unrelated processes.** `FluentShell.Bridge.dll+0x2bddbc`,
  exception `0xc0000005`, in `cc-switch.exe` and `WindowsTerminal.exe`
  (Application Error, 2026-10-22 04:51 and 05:00).

## The cause: the click has to arrive late

With the dialog projected, the press was given every piece of state in turn and
measured. These runs all have the native window in the foreground, its own thread
active, the Run button focused and carrying the default-button bit, `id=4426`
enabled and visible, no input-queue join, and the proxy not raised:

| what differed | result |
|---|---|
| click posted by the Bridge from the control's own thread, immediately | refused |
| click posted by the Bridge from another thread, immediately | refused |
| click posted by the Bridge from another thread, 150 ms later | refused |
| click posted by another process ~700 ms later | **accepted** |
| click posted by the Bridge from another thread, **700 ms** later | **accepted**, repeatedly |
| the same, with the window left cloaked instead of uncloaked | **accepted** |
| delivered as `WM_COMMAND`/`BN_CLICKED` to the dialog instead of `BM_CLICK` | refused |

So it is neither the state, nor the delivery mechanism, nor which thread or
process posts the click. It is **how long after the window came forward the click
arrives**: shdocvw's prompt guards its Run button against a click that lands too
soon after its window was activated — click-through protection — and guards only
that button. That is why Cancel always worked and Run never did, and why no
amount of matching state helped.

A projection leaves the native window's thread with no active window at all
(`GetGUIThreadInfo` reports `hwndActive = 0`, `hwndFocus = 0`), so every press has
to bring it forward first, and every press therefore met the guard.

### What the fix is

- The renderer hands the foreground over before sending the action
  (`GiveForegroundToNativeForPress`); it owns the foreground, so no grant and no
  input-queue join are needed. The join does not survive being released — the
  active window reverts to the proxy the moment the queues are unjoined — so it
  is now only a last resort.
- The click waits out the remainder of `kActivationSettleMs` (700 ms) on a thread
  of its own, because the control's own thread has to stay free to pump.
- The settling time is counted from when the window actually came forward
  (`SourceThreadAgent::nativeForegroundSince_`), so a press that follows one still
  holding the foreground pays none of it.
- The press is held until the application has finished reacting — the window being
  destroyed, or `kNativePressHoldMs` — rather than until the click was merely
  observed. Run's work happens after its `WM_COMMAND` returns, so ending the press
  at `BN_CLICKED` took the foreground away mid-flight.
- The renderer's own take-back no longer runs on the action result (which arrives
  while the press is still in flight); it waits past the Bridge's hold and only
  acts if the native window really still has the foreground.

Verified: 執行 closes the dialog and runs the file through the WinUI projection on
repeated runs, and 取消 closes it without running the file.

## Where the cause was looked for first

The dialog accepts the press in isolation and refuses it under the projection,
so the difference is on our side of the boundary, not in the shell's rule. The
log cannot currently distinguish the two remaining possibilities, because
`Native press state (dispatched)` is written from two places and the
`RootSubclassProc` one fires on *any* `BN_CLICKED` that arrives while a press is
pending — including one from a different control. The next live run needs the
Bridge to say:

1. the pressed control's **dialog control id**, style bits and enabled/visible
   state at the moment of the press (is it really 4426, and is it enabled?);
2. whether the dialog received `WM_COMMAND`/**`BN_CLICKED` for that same control
   id** after the press — which separates "the button swallowed the click" from
   "the dialog got the command and declined".

Until that evidence exists, any further change to the activation machinery is a
guess.
