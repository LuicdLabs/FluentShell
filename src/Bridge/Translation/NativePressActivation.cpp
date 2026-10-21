#include "NativePressActivation.h"

namespace FluentShell::Bridge::Translation {

NativePressActivation::NativePressActivation(HWND root) noexcept : root_(root) {
    if (!root_) return;
    previousForeground_ = GetForegroundWindow();
    // The renderer owns the foreground and granted this process the right to take
    // it, so the native window can be the foreground window while the press runs.
    if (SetForegroundWindow(root_) != FALSE && GetForegroundWindow() == root_) {
        foregroundHeld_ = true;
        return;
    }
    // The grant was not there.  Joining the input queues of this thread and of
    // the foreground window's thread is the other way to take the foreground
    // without it, and it costs nothing when the grant did arrive.
    const DWORD foreign = previousForeground_ ?
        GetWindowThreadProcessId(previousForeground_, nullptr) : 0;
    const DWORD thread = GetCurrentThreadId();
    if (!foreign || foreign == thread) return;
    if (AttachThreadInput(thread, foreign, TRUE) == FALSE) return;
    if (SetForegroundWindow(root_) != FALSE && GetForegroundWindow() == root_) {
        foregroundHeld_ = true;
    }
    AttachThreadInput(thread, foreign, FALSE);
}

NativePressActivation::~NativePressActivation() {
    // The foreground goes back to the window that held it: the WinUI proxy of the
    // projected surface, or whatever else owned it before the press.
    if (previousForeground_ && IsWindow(previousForeground_) &&
        GetForegroundWindow() != previousForeground_) {
        SetForegroundWindow(previousForeground_);
    }
}

bool PressNativeButton(HWND root, HWND button) noexcept {
    if (!button || !IsWindow(button)) return false;
    // BM_CLICK presses the button and focuses it between its button-down and
    // button-up.  The activation that focus causes cancels the press half-way:
    // the click is accepted and BN_CLICKED never comes.  Taking the focus first
    // lets the activation finish before the press starts, and it leaves the
    // native window as the active window of its own thread, which is the state a
    // native click leaves behind.
    if (GetFocus() != button) SetFocus(button);
    if (!IsWindow(button)) return false;
    // The native window holds the foreground while the press runs: Windows gates
    // some of its own controls on that, and a projected surface keeps the WinUI
    // proxy in the foreground instead.
    NativePressActivation activation(root);
    SendMessageW(button, BM_CLICK, 0, 0);
    return activation.ForegroundHeld();
}

} // namespace FluentShell::Bridge::Translation
