#include "NativePressActivation.h"
#include "HostScope.h"

#include <dwmapi.h>

#include <memory>

#include "../../Common/FluentShell.h"

namespace FluentShell::Bridge::Translation {
namespace {

// How long the click waits after the native window was given the foreground.
//
// This is the whole reason Explorer's security warning used to refuse its Run
// button, and it is not about any of the state the press establishes: with the
// cloak, the foreground, the thread's active window, the focus and the
// default-button bit all identical, a click delivered within ~150 ms of the
// activation is declined and the same click at ~700 ms is honoured.  A security
// prompt guards its dangerous action against a click that arrives too soon after
// its window came forward -- click-through protection -- and it guards only that
// action, which is exactly why Cancel always worked and Run never did.
//
// Measured in docs\goals\win32-to-winui-translation\NATIVE-PRESS-EVIDENCE.md.
constexpr DWORD kActivationSettleMs = 700;

// DWM application cloak: the window is not composited.  A projected window is
// cloaked, and SetForegroundWindow refuses a cloaked window when its own process
// asks, so the press uncloaks it for as long as it holds the foreground.  The
// proxy stays above it, so the user keeps seeing the projected copy.
bool SetApplicationCloak(HWND window, bool cloaked) noexcept {
    if (!window) return false;
    const DWORD value = cloaked ? 1 : 0;
    return SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &value, sizeof(value)));
}

std::wstring DescribeWindow(HWND window) {
    if (!window) return L"none";
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t address[32]{};
    swprintf_s(address, L"0x%llX", static_cast<unsigned long long>(
        reinterpret_cast<uintptr_t>(window)));
    return std::wstring(address) + L" '" + className + L"' pid=" + std::to_wstring(pid);
}

// What the native side sees while the press runs.  A control that gates on the
// user having seen the warning refuses a press it cannot account for, and the
// state it looks at is the one this records.  Only a dialog is recorded: the
// gate is the shell's own dialogs, and the log stays quiet otherwise.
//
// The state is recorded twice: when the press is posted, and when the
// application dispatched the click.  The second record is the one that decides
// whether the control acts on the click.
void LogPressState(HWND root, HWND button, const wchar_t* phase) {
    try {
        wchar_t className[64]{};
        if (!GetClassNameW(root, className, 64) || wcscmp(className, L"#32770") != 0) return;
        DWORD cloaked = 0;
        const bool onScreen = SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked,
            sizeof(cloaked))) && (cloaked & DWM_CLOAKED_APP) == 0;
        RECT buttonRect{};
        GetWindowRect(button, &buttonRect);
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool cursorOverButton = cursor.x >= buttonRect.left && cursor.x < buttonRect.right &&
            cursor.y >= buttonRect.top && cursor.y < buttonRect.bottom;
        const LONG_PTR exStyle = GetWindowLongPtrW(root, GWL_EXSTYLE);
        // The control's own identity, not just its handle.  shdocvw's security
        // warning is dialog template 4416: its Run button is control 4426 and its
        // Cancel is 2, so the id says which button the press really reached, and
        // the style and enabled state say whether it could have acted on it at
        // all (a disabled button swallows BM_CLICK without notifying the dialog).
        wchar_t control[160]{};
        swprintf_s(control, L" id=%d enabled=%s visible=%s style=0x%08lX",
            GetDlgCtrlID(button),
            IsWindowEnabled(button) ? L"yes" : L"no",
            IsWindowVisible(button) ? L"yes" : L"no",
            static_cast<unsigned long>(GetWindowLongPtrW(button, GWL_STYLE)));
        // GetActiveWindow and GetFocus answer with this thread's own bookkeeping,
        // which can say the dialog is active when the system's input state does
        // not.  GUITHREADINFO is the system's own answer for that thread, so both
        // are reported and a disagreement is visible.
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        const bool haveInfo = GetGUIThreadInfo(
            GetWindowThreadProcessId(root, nullptr), &info) != FALSE;
        FluentShell::Log(std::wstring(L"Native press state (") + phase + L"): foreground=" +
            DescribeWindow(GetForegroundWindow()) +
            L" active=" + DescribeWindow(GetActiveWindow()) +
            L" sysActive=" + (haveInfo ? DescribeWindow(info.hwndActive) : L"?") +
            L" sysFocus=" + (haveInfo ? DescribeWindow(info.hwndFocus) : L"?") +
            L" native=" + DescribeWindow(root) +
            L" onScreen=" + (onScreen ? L"yes" : L"no") +
            L" layered=" + ((exStyle & WS_EX_LAYERED) ? L"yes" : L"no") +
            L" focus=" + DescribeWindow(GetFocus()) +
            L" button=" + DescribeWindow(button) + control +
            L" cursor=" + std::to_wstring(cursor.x) + L"," + std::to_wstring(cursor.y) +
            L" overButton=" + (cursorOverButton ? L"yes" : L"no"));
    } catch (...) {}
}

// The foreground the press borrowed goes back to the proxy that covers the
// native window.  The native window owned the foreground, and naming a
// foreground window belongs to the process that owns it: joining the two
// threads' input queues is what lets this process name the proxy.
void HandForegroundBack(HWND proxy) noexcept {
    if (!proxy || !IsWindow(proxy)) return;
    const DWORD proxyThread = GetWindowThreadProcessId(proxy, nullptr);
    const DWORD thread = GetCurrentThreadId();
    if (!proxyThread || proxyThread == thread) {
        SetForegroundWindow(proxy);
        return;
    }
    if (AttachThreadInput(thread, proxyThread, TRUE) != FALSE) {
        SetForegroundWindow(proxy);
        AttachThreadInput(thread, proxyThread, FALSE);
    }
}

// Posts the click once the activation has settled.
//
// The wait cannot happen on the control's own GUI thread: that thread has to be
// free to pump, or the window never finishes coming forward and the click is
// dispatched into the very state the wait is there to let pass.  So a short-lived
// thread does the waiting and posts the click, and the press's own hold keeps the
// foreground in place until the application has acted on it.
bool PostClickAfterActivationSettles(HWND button, DWORD settleMs) noexcept {
    struct Pending final {
        HWND button;
        DWORD settleMs;
    };
    const auto entry = [](void* parameter) -> DWORD {
        const std::unique_ptr<Pending> pending{ static_cast<Pending*>(parameter) };
        Sleep(pending->settleMs);
        if (IsWindow(pending->button)) PostMessageW(pending->button, BM_CLICK, 0, 0);
        return 0;
    };
    auto pending = std::make_unique<Pending>(Pending{ button, settleMs });
    const HANDLE thread = CreateThread(nullptr, 0, entry, pending.get(), 0, nullptr);
    if (!thread) return PostMessageW(button, BM_CLICK, 0, 0) != FALSE;
    pending.release();
    // The thread is not joined: joining would put the wait back on the thread that
    // has to pump.  It owns nothing but the window handle, which it re-validates.
    CloseHandle(thread);
    return true;
}

} // namespace

DWORD NativePressActivationSettleMs() noexcept { return kActivationSettleMs; }

void LogPressDispatchState(HWND root, HWND button, const wchar_t* phase) noexcept {
    LogPressState(root, button, phase ? phase : L"dispatched");
}

NativePressActivation::NativePressActivation(HWND root, HWND proxy) noexcept
    : root_(root), proxy_(proxy) {
    if (!root_) return;
    // The foreground is taken first and the window uncloaked after.  The order is
    // load-bearing in that direction: uncloaking first makes the shell's own
    // activation change land between the uncloak and the take, and the take then
    // fails and leaves the press on a thread with no active window.
    //
    // The renderer owns the foreground and granted this process the right to take
    // it, so the native window can be the foreground window while the press runs.
    // A press that follows one still holding the foreground finds the window
    // already forward, and then nothing has just changed for a prompt to guard
    // against: that press needs no settling time.
    alreadyForeground_ = GetForegroundWindow() == root_;
    if (alreadyForeground_) {
        // The renderer handed the foreground over before it sent the action, or an
        // earlier press is still holding it.  Either way no queue is joined and
        // the active window belongs to this thread alone.
        foregroundHeld_ = true;
    } else if (SetForegroundWindow(root_) != FALSE && GetForegroundWindow() == root_) {
        // The grant was there.  This take stands on its own and outlives the call.
        foregroundHeld_ = true;
    } else {
        // The grant was not there.  Joining the input queues of this thread and of
        // the foreground window's thread is the other way to take the foreground
        // without it.
        //
        // That take lasts exactly as long as the join.  Unjoining the queues puts
        // the foreground and the active window straight back to the proxy, even
        // though GetForegroundWindow still answers with the native window for a
        // moment afterwards -- measured both ways in
        // docs\goals\win32-to-winui-translation\NATIVE-PRESS-EVIDENCE.md.  So the
        // join is held for the whole press and released with it, not handed back
        // after the handover: a press that let go here delivered its click to a
        // thread that was no longer the active one, which is what the shell's Run
        // button refuses while its Cancel, finished inside its own WM_COMMAND,
        // does not notice.
        const HWND foreground = GetForegroundWindow();
        const DWORD foreign = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        const DWORD thread = GetCurrentThreadId();
        // A guest in Explorer joins only the renderer proxy's own thread, never a shell
        // thread (taskbar, folder): the press then fails visibly instead.
        if (foreign && foreign != thread && ProcessHostScope().AllowsInputJoinWith(foreign, proxy_) &&
            AttachThreadInput(thread, foreign, TRUE) != FALSE) {
            joinedThread_ = foreign;
            if (SetForegroundWindow(root_) != FALSE && GetForegroundWindow() == root_) {
                foregroundHeld_ = true;
            }
        }
    }
    // The window is uncloaked for as long as the press holds the foreground: a
    // cloaked window cannot keep the keyboard focus, so the focus the press needs
    // would not land on the control.
    uncloaked_ = SetApplicationCloak(root_, false);
    // Taking the foreground raised the native window above the proxy that covers
    // it.  The proxy goes back on top, so the uncloaked native window is never
    // the one the user sees.
    if (uncloaked_ && proxy_) {
        SetWindowPos(proxy_, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

NativePressActivation::~NativePressActivation() {
    Release();
}

void NativePressActivation::Release() noexcept {
    if (released_) return;
    released_ = true;
    // The foreground the press borrowed goes back to the proxy.  The renderer
    // takes it back on the action result, which arrives before the press runs,
    // so the press hands it back itself.
    if (joinedThread_) {
        // The queues are still joined, which is what made the take hold.  The
        // proxy is named while they are, and only then are they unjoined.
        if (proxy_ && IsWindow(proxy_)) SetForegroundWindow(proxy_);
        AttachThreadInput(GetCurrentThreadId(), joinedThread_, FALSE);
        joinedThread_ = 0;
    } else if (foregroundHeld_ && proxy_) {
        HandForegroundBack(proxy_);
    }
    foregroundHeld_ = false;
    // The native window goes back behind its cloak last, once it no longer owns
    // the foreground: a projected window is never left uncloaked.
    if (uncloaked_ && IsWindow(root_)) SetApplicationCloak(root_, true);
    uncloaked_ = false;
}

bool PressNativeButton(NativePressActivation& press, HWND button,
    PressDelivery delivery, DWORD settleMs) noexcept {
    if (!button || !IsWindow(button)) return false;
    // The native window has to be in the foreground and on screen before the
    // press: a control that gates on the user having seen the warning refuses a
    // press while the window is cloaked, and a cloaked window cannot take the
    // keyboard focus the press needs either.  BM_CLICK focuses the button
    // between its button-down and its button-up, and an activation in between
    // cancels the press half-way -- the click is accepted and BN_CLICKED never
    // comes -- so the focus is taken before the press starts.
    if (GetFocus() != button) {
        // The focus is moved through the dialog manager, not with a bare
        // SetFocus.  WM_NEXTDLGCTL is what a dialog's own tab and click handling
        // uses: it moves the focus *and* updates the bookkeeping that goes with
        // it -- the default-push-button state (DM_SETDEFID) and the
        // WM_SETFOCUS/BN_SETFOCUS the dialog procedure sees.  A bare SetFocus
        // moves only the focus, and leaves a dialog whose records say another
        // control is still the default one.
        //
        // A window that is not a dialog ignores WM_NEXTDLGCTL, so the bare
        // SetFocus stays as the fallback for one.
        SendMessageW(press.Root(), WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(button), TRUE);
        if (!IsWindow(button)) return false;
        if (GetFocus() != button) SetFocus(button);
    }
    if (!IsWindow(button)) return false;
    LogPressState(press.Root(), button, L"posted");
    // The click has to reach the control the way a real click arrives: the
    // application's own message loop dispatches it.  A click that is sent to a
    // control of this thread is delivered re-entrantly, inside the message the
    // application was already handling, and the shell refuses a click that
    // arrives that way: the press reaches the button, every state a native
    // click has is in place, and the application still does not act on it.  So
    // the click of a control of this thread is posted, and the application's own
    // loop dispatches it.
    //
    // A control of another thread is sent instead: the owning thread's own loop
    // dispatches a sent click, so it is not delivered re-entrantly there either,
    // and the caller learns the outcome.
    const bool ownThread = GetWindowThreadProcessId(button, nullptr) == GetCurrentThreadId();
    if (delivery == PressDelivery::Synchronous || !ownThread) {
        SendMessageW(button, BM_CLICK, 0, 0);
        // The click has been handled when the call returns, so the state the
        // press borrowed ends with it.
        press.Release();
    } else if (settleMs == 0
            ? !PostMessageW(button, BM_CLICK, 0, 0)
            : !PostClickAfterActivationSettles(button, settleMs)) {
        FluentShell::Log(L"Native press: the press could not be posted");
        press.Release();
        return false;
    }
    // A posted click is dispatched by the application's own message loop after
    // this call returns, so the state the press borrowed is still in place then.
    return press.ForegroundHeld();
}

} // namespace FluentShell::Bridge::Translation
