#pragma once

#include <Windows.h>

#include <atomic>
#include <memory>

namespace FluentShell::Bridge::Translation {

// A native press has to reach its control the way a native click does: the
// window that owns the control holds the foreground, its own thread is the
// active one, and the control has the focus.  A projected surface keeps the
// WinUI proxy in the foreground instead, which leaves the native window's thread
// with no active window and no focus at all -- and in that state the dialog
// ignores a press on *any* of its buttons.  So the press borrows the foreground.
//
// Measured, against the real dialog this was written for
// (docs\goals\win32-to-winui-translation\NATIVE-PRESS-EVIDENCE.md): Explorer's
// "Open File - Security Warning" is shdocvw's template 4416, Run is control 4426
// and Cancel is 2.  Two things follow, and both correct what this file used to
// say.
//
// **The click has to arrive late.**  That is what the Run button needed and the
// Cancel button never did, and it is not about any state: with the cloak, the
// foreground, the thread's active window, the focus and the default-button bit
// all identical, a click delivered within about 150 ms of the window coming
// forward is declined, and the same click at about 700 ms runs the file.  A
// security prompt guards its dangerous action against a click that arrives too
// soon after its window was activated -- click-through protection -- and guards
// only that action, which is exactly why Cancel always worked.  So the click
// waits out the rest of that settling time, on a thread of its own because the
// control's own thread has to stay free to pump.  A press that follows one still
// holding the foreground pays nothing: the window never went away.
//
// The press is also held until the application has finished reacting -- until the
// window is gone, or a short deadline.  Cancel is finished inside its own
// WM_COMMAND, but Run only *starts* there: the dialog ends its modal loop, the
// loop unwinds, and shdocvw's caller then runs the file, all after the click's
// handler has returned.  Giving the foreground back as soon as the click was
// seen took the state away in the middle of that.
//
// The control does not care about the cloak.  The press uncloaks the window for a
// reason of our own -- a cloaked window does not keep the keyboard focus -- and
// puts the proxy back on top for the duration, so the uncloaked native window is
// never the one the user sees, and it is cloaked again before the press lets go.
//
// The renderer owns the foreground and grants this process the right to take it.
// The native window is cloaked throughout, so the user keeps seeing only the
// projected copy, and the foreground goes back to the proxy when the press ends.
class NativePressActivation final {
public:
    // Takes the foreground for `root` and puts it on screen.  `proxy` is the WinUI
    // proxy that covers the native window; it stays above it while the native
    // window is on screen.  Runs on the native window's own GUI thread.
    NativePressActivation(HWND root, HWND proxy) noexcept;
    ~NativePressActivation();

    NativePressActivation(const NativePressActivation&) = delete;
    NativePressActivation& operator=(const NativePressActivation&) = delete;

    // Ends the press: the native window is cloaked again and the foreground goes
    // back to the proxy that covers it.  Calling it twice does nothing.
    void Release() noexcept;

    // Whether the native window was already the foreground window when the press
    // began -- the renderer had handed it over, or an earlier press is still
    // holding it.  Nothing about the window just changed then, so a prompt that
    // guards its dangerous action against a click arriving too soon after its
    // window came forward has nothing to guard against, and the click needs no
    // settling time.
    bool AlreadyHadForeground() const noexcept { return alreadyForeground_; }

    // Whether the native window is the foreground window while the press runs.
    bool ForegroundHeld() const noexcept { return foregroundHeld_; }

    // Whether the press has already ended.
    bool Released() const noexcept { return released_; }

    // The window the press is delivered to.
    HWND Root() const noexcept { return root_; }

    // Whether the click is still waiting out its settling time on its own thread.  A
    // second press of the same button in that window is the user clicking again because
    // nothing has happened yet; it must not become a second click.
    bool ClickStillWaiting() const noexcept {
        return delayedClickPosted_ && !delayedClickPosted_->load(std::memory_order_acquire);
    }
    void TrackDelayedClick(std::shared_ptr<std::atomic<bool>> posted) noexcept {
        delayedClickPosted_ = std::move(posted);
    }

private:
    std::shared_ptr<std::atomic<bool>> delayedClickPosted_;
    HWND root_ = nullptr;
    HWND proxy_ = nullptr;
    // The foreground thread whose input queue this one is joined to, while the
    // press holds the foreground that way.  Zero when the renderer's grant made
    // the take stand on its own.
    DWORD joinedThread_ = 0;
    bool alreadyForeground_ = false;
    bool uncloaked_ = false;
    bool foregroundHeld_ = false;
    bool released_ = false;
};

// How the click reaches the control.
enum class PressDelivery {
    // The application's own message loop dispatches the click, the way a real
    // click arrives.  A control of the calling thread has to be posted: a sent
    // click would be delivered re-entrantly, inside the message the application
    // was already handling, and the shell refuses a click that arrives that
    // way.  A control of another thread is sent, because the owning thread's
    // own loop dispatches a sent click -- it is not delivered re-entrantly --
    // and the caller learns the outcome.
    ThroughTheApplicationLoop,
    // The click is delivered re-entrantly and its effect is observable when the
    // call returns.  Only a walk that reads the control's state right after the
    // click needs this, and the application's own loop has to be free to
    // dispatch the click for the state to move at all.
    Synchronous,
};

// Presses a native button the way a native click does.  `press` holds the state
// the press needs: the native window that owns the button is the foreground
// window and is on screen.  The call runs on the button's own GUI thread.
//
// A click the application's own message loop dispatches is posted, and the
// application dispatches it after this call returns: the state the press needs
// is still in place then, and `press` keeps it until the application has
// dispatched the click.  A click that is delivered here -- a control of another
// thread, or the synchronous delivery a state walk needs -- has been handled
// when the call returns, and the press ends with the call.
//
// Returns whether the native window held the foreground and was on screen while
// the press was delivered.
// `settleMs` is how long the click waits after the native window came forward
// before it is delivered; zero delivers it at once.  A security prompt guards its
// dangerous action against a click that arrives too soon after its window was
// activated, so the caller passes what is left of that settling time -- measured
// from when the foreground was actually acquired, which is not the same as this
// press having found the window already in front.
bool PressNativeButton(NativePressActivation& press, HWND button,
    PressDelivery delivery = PressDelivery::ThroughTheApplicationLoop,
    DWORD settleMs = 0) noexcept;

// How long a click waits after the native window came forward.  See
// docs\goals\win32-to-winui-translation\NATIVE-PRESS-EVIDENCE.md.
DWORD NativePressActivationSettleMs() noexcept;

// Records the state the application saw when it acted on a native press, and
// which observation saw it (`phase`).  The two are not the same evidence: the
// control's own BM_CLICK says the click was delivered, while the dialog's
// BN_CLICKED for that control id says the dialog was told about it.  A press
// that shows the first and never the second reached the button and was swallowed
// there.
void LogPressDispatchState(HWND root, HWND button, const wchar_t* phase) noexcept;

} // namespace FluentShell::Bridge::Translation
