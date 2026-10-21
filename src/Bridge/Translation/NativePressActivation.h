#pragma once

#include <Windows.h>

namespace FluentShell::Bridge::Translation {

// A native press has to reach its control the way a native click does: the window
// that owns the control is the foreground window while the press is delivered.
// Windows gates some of its own controls on exactly that.  Explorer's "Open File
// - Security Warning" runs the file only while its dialog is the foreground
// window; a synthesized press on a dialog that is not in the foreground is
// accepted by the button and refused by the dialog, so the file never runs.
//
// A projected surface keeps the WinUI proxy in the foreground instead, so the
// native window has to borrow the foreground for the press.  The handoff gives it
// back afterwards.  The native window stays application-cloaked, so none of it is
// drawn: only the activation state changes, and only while the press runs.
class NativePressActivation final {
public:
    // Takes the foreground for `root`.  Runs on the caller's GUI thread.
    explicit NativePressActivation(HWND root) noexcept;
    ~NativePressActivation();

    NativePressActivation(const NativePressActivation&) = delete;
    NativePressActivation& operator=(const NativePressActivation&) = delete;

    // Whether the native window really became the foreground window.  A press that
    // could not borrow it may still be refused by the control's owner.
    bool ForegroundHeld() const noexcept { return foregroundHeld_; }

private:
    HWND root_ = nullptr;
    HWND previousForeground_ = nullptr;
    HWND previousActive_ = nullptr;
    DWORD foreignThread_ = 0;
    bool attached_ = false;
    bool foregroundHeld_ = false;
};

// Presses a native button the way a native click does.  `root` is the window that
// owns the button; the call runs on the button's own GUI thread.  Returns whether
// the native window held the foreground while the press was delivered.
bool PressNativeButton(HWND root, HWND button) noexcept;

} // namespace FluentShell::Bridge::Translation
