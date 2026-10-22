#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// Windows gates some of its own controls on the window being the foreground one.
// Explorer's "Open File - Security Warning" runs the file only while its dialog is
// the foreground window *and* a window that is on screen: a press on the dialog
// while the window is application-cloaked (as a projection cloaks it) or layered
// reaches the button and the dialog refuses it.  A projected surface keeps the
// WinUI proxy in the foreground instead.
//
// The delivery path matters as much as the state does.  The shell refuses a
// click that arrives re-entrantly -- inside the message the application was
// already handling -- even when the foreground, the on-screen state, the focus
// and the activation are all in place, and it acts on a click its own message
// loop dispatched.
//
// The state has to be in place when the application dispatches the click, not
// only when the click is posted: a posted click is dispatched after the press
// that posted it returned, so a press that gives the state back before that
// reaches a control that gates on it.
//
// The fixture is the two sides of that arrangement: a proxy window holds the
// foreground, as the WinUI proxy of a projected surface does, and the dialog
// refuses a press that arrives while the proxy is in the foreground or the
// dialog is cloaked.

// The other half of what the shell's prompt does, and the half that kept its Run
// button from working: it guards that button against a click that arrives too
// soon after its window came forward -- click-through protection -- and guards
// only that button, which is why its Cancel always worked.  Measured against the
// real dialog in docs\goals\win32-to-winui-translation\NATIVE-PRESS-EVIDENCE.md:
// a click ~150 ms after the activation is declined, the same click at ~700 ms
// runs the file, with every other piece of state identical.
struct SettleGatedPressState final {
    // How long after coming forward the window refuses its button.  Shorter than
    // the press's own settling time, so the production press clears it and an
    // immediate click does not.
    static constexpr ULONGLONG kGuardMs = 300;
    ULONGLONG activatedAt = 0;
    int presses = 0;
    int accepted = 0;
    ULONGLONG lastDelayMs = 0;
};

// Records when the window came forward, and accepts its button only once the
// guard has passed.
LRESULT CALLBACK SettleGatedPressSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<SettleGatedPressState*>(data);
    if (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) {
        state.activatedAt = GetTickCount64();
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 4426) {
        ++state.presses;
        state.lastDelayMs = state.activatedAt == 0 ? 0 : GetTickCount64() - state.activatedAt;
        if (state.lastDelayMs >= SettleGatedPressState::kGuardMs) ++state.accepted;
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

struct ForegroundGatedPressState final {
    int presses = 0;
    int accepted = 0;
    // Set while a click is delivered re-entrantly.  The application's own
    // message loop never sees such a click, so the application does not act on
    // it, however complete the rest of the state is.
    bool reentrant = false;
    // The state the application saw when the click was dispatched.
    bool foreground = false;
    bool onScreen = false;
    bool focused = false;
    bool active = false;
};

// The gate a shell dialog applies: the press counts only while the click arrived
// through the application's own message loop and the window had the state a
// native click has.  A cloaked window takes no focus, so a press that arrives
// while the window is cloaked cannot be accounted for either, and the state the
// application saw is recorded with it.
LRESULT CALLBACK ForegroundGatedPressSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<ForegroundGatedPressState*>(data);
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 4426) {
        ++state.presses;
        DWORD cloaked = 0;
        state.onScreen = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
            sizeof(cloaked))) && (cloaked & DWM_CLOAKED_APP) == 0;
        const HWND focus = GetFocus();
        state.focused = focus != nullptr && GetParent(focus) == window;
        state.active = GetActiveWindow() == window;
        state.foreground = GetForegroundWindow() == window;
        if (!state.reentrant && state.foreground && state.onScreen && state.focused &&
            state.active) {
            ++state.accepted;
        }
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

// The application's own message loop: it dispatches what it retrieves from its
// queue, the way a real application does.  A click the projection posted is
// dispatched here; a click that was sent is delivered inside the message the
// application was already handling and never reaches this loop.
void PumpSettleGatedMessages(int expectedPresses, SettleGatedPressState& state, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (state.presses < expectedPresses && GetTickCount64() < deadline) {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
}

void PumpApplicationMessages(int expectedPresses, ForegroundGatedPressState& state, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (state.presses < expectedPresses && GetTickCount64() < deadline) {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
}

// Windows only lets a process take the foreground when it is entitled to, and
// the suite may start while another window holds it.  A click on the test's own
// window makes the test the process that received the last input, which is
// enough; the cursor is put back where it was.
void ClickOwnWindow(HWND window) {
    // The click has to reach the test's own window, so it is on top for it.
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RECT rect{};
    GetWindowRect(window, &rect);
    POINT cursor{};
    GetCursorPos(&cursor);
    const int x = (rect.left + rect.right) / 2;
    const int y = (rect.top + rect.bottom) / 2;
    SetCursorPos(x, y);
    Sleep(80);
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
    Sleep(150);
    SetCursorPos(cursor.x, cursor.y);
}

// Puts the dialog in the state a native click has: the foreground window, on
// screen, with the focus on the button.  A press that arrives with this state
// and is delivered re-entrantly is still refused, which is what the shell's
// dialog does.
bool GiveDialogTheClickState(HWND root, HWND button) {
    DWORD cloak = 0;
    DwmSetWindowAttribute(root, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(root, SW_SHOW);
    if (SetForegroundWindow(root) == FALSE || GetForegroundWindow() != root) return false;
    SetFocus(button);
    return GetFocus() == button && GetActiveWindow() == root;
}

// A window that refuses its button for a moment after coming forward -- what the
// shell's security warning does to its Run button -- is pressed the way the
// projection presses it.  A click delivered the instant the window came forward
// is refused; the press that waits out its settling time is accepted.  This is
// the regression test for the projected Run button doing nothing.
void TestNativePressSettlesBeforeTheClick() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    };
    WindowOwner proxy{ CreateWindowExW(0, L"Static", L"settle proxy stand-in",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 70, 70, 320, 160, nullptr, nullptr, instance, nullptr) };
    WindowOwner owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"settle gated press",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 70, 70, 320, 160, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND run = root ? CreateWindowExW(0, L"Button", L"&Run", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        20, 40, 90, 26, root, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4426)), instance, nullptr) : nullptr;
    Check(proxy.window && root && run, "the settle-gated press fixture could not create its windows");
    if (!proxy.window || !root || !run) return;
    SettleGatedPressState state;
    SetWindowSubclass(root, SettleGatedPressSubclass, 0xAA72, reinterpret_cast<DWORD_PTR>(&state));
    ShowWindow(root, SW_SHOWNOACTIVATE);
    const DWORD cloak = 1;
    DwmSetWindowAttribute(root, DWMWA_CLOAK, &cloak, sizeof(cloak));

    const auto proxyTakesForeground = [&] {
        ShowWindow(proxy.window, SW_SHOW);
        for (int attempt = 0; attempt < 20 && GetForegroundWindow() != proxy.window; ++attempt) {
            SetForegroundWindow(proxy.window);
            if (GetForegroundWindow() != proxy.window) Sleep(50);
        }
        if (GetForegroundWindow() != proxy.window) {
            ClickOwnWindow(proxy.window);
            AllowSetForegroundWindow(ASFW_ANY);
            SetForegroundWindow(proxy.window);
        }
        return GetForegroundWindow() == proxy.window;
    };
    Check(proxyTakesForeground(), "the settle proxy stand-in could not take the foreground");
    if (GetForegroundWindow() != proxy.window) return;

    // The click delivered the instant the window came forward: the window has
    // every other piece of state a native click has, and refuses it anyway.
    {
        Translation::NativePressActivation press(root, proxy.window);
        Check(Translation::PressNativeButton(press, run,
                  Translation::PressDelivery::ThroughTheApplicationLoop, 0),
            "the immediate native press was not delivered");
        PumpSettleGatedMessages(1, state, 2000);
        Check(state.presses == 1, "the immediate press never reached the window");
        Check(state.accepted == 0,
            "a press delivered before the window had settled was not refused by the fixture");
    }

    // The press the projection uses waits out its settling time, so the same
    // window acts on the same click.
    Check(proxyTakesForeground(), "the settle proxy stand-in could not retake the foreground");
    {
        Translation::NativePressActivation press(root, proxy.window);
        Check(Translation::PressNativeButton(press, run,
                  Translation::PressDelivery::ThroughTheApplicationLoop,
                  Translation::NativePressActivationSettleMs()),
            "the settling native press was not delivered");
        PumpSettleGatedMessages(2, state,
            Translation::NativePressActivationSettleMs() + 4000);
        Check(state.presses == 2, "the settling press never reached the window");
        Check(state.accepted == 1,
            "a press that waited out the window's settling time was still refused");
        Check(state.lastDelayMs >= SettleGatedPressState::kGuardMs,
            "the settling press did not actually wait out the window's guard");
    }

    Check(Translation::NativePressActivationSettleMs() >= SettleGatedPressState::kGuardMs,
        "the press's settling time no longer clears a click-through guard");
}

void TestNativePressActivation() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    };
    // The proxy: the window that holds the foreground, as the WinUI proxy of a
    // projected surface does.
    WindowOwner proxy{ CreateWindowExW(0, L"Static", L"proxy stand-in",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 60, 60, 320, 160, nullptr, nullptr, instance, nullptr) };
    // The native dialog: a window that refuses its own button unless it is the
    // foreground window and on screen.
    WindowOwner owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"foreground gated press",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 60, 60, 320, 160, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND run = root ? CreateWindowExW(0, L"Button", L"&Run", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        20, 40, 90, 26, root, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4426)), instance, nullptr) : nullptr;
    Check(proxy.window && root && run, "the foreground-gated press fixture could not create its windows");
    if (!proxy.window || !root || !run) return;
    ForegroundGatedPressState state;
    SetWindowSubclass(root, ForegroundGatedPressSubclass, 0xAA71, reinterpret_cast<DWORD_PTR>(&state));
    ShowWindow(root, SW_SHOWNOACTIVATE);
    // The projection cloaks the native window; the proxy covers it.
    const DWORD cloak = 1;
    DwmSetWindowAttribute(root, DWMWA_CLOAK, &cloak, sizeof(cloak));

    // The proxy takes the foreground, as it does when a surface is committed.
    ShowWindow(proxy.window, SW_SHOW);
    for (int attempt = 0; attempt < 20 && GetForegroundWindow() != proxy.window; ++attempt) {
        SetForegroundWindow(proxy.window);
        if (GetForegroundWindow() != proxy.window) Sleep(50);
    }
    if (GetForegroundWindow() != proxy.window) {
        // The suite may start while another application holds the foreground.  A
        // click on the test's own window makes the test the process that received
        // the last input, and the grant lets it take the foreground from there.
        ClickOwnWindow(proxy.window);
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(proxy.window);
    }
    Check(GetForegroundWindow() == proxy.window, "the proxy stand-in could not take the foreground");
    if (GetForegroundWindow() != proxy.window) return;

    // A click that is delivered re-entrantly -- inside the message the
    // application was already handling -- reaches the dialog and is refused: the
    // shell's security warning refuses its Run button the same way.  The dialog
    // has the state a native click has while the click is refused, so the
    // re-entrancy alone is what refuses it.
    Check(GiveDialogTheClickState(root, run),
        "the foreground-gated press fixture could not give its dialog the state a native click has");
    state.reentrant = true;
    SendMessageW(run, BM_CLICK, 0, 0);
    state.reentrant = false;
    Check(state.presses == 1 && state.accepted == 0,
        "a re-entrant press was not refused");

    // The press the projection uses takes the foreground for the press, puts the
    // native window on screen, takes the focus, and posts the click, so the
    // application's own message loop dispatches it the way a real click arrives.
    // The state the press borrowed stays in place until the application has
    // dispatched the click: the dialog that refuses a re-entrant click acts on a
    // click that arrives with the state a native click has.
    SetForegroundWindow(proxy.window);
    Translation::NativePressActivation press(root, proxy.window);
    const bool delivered = Translation::PressNativeButton(press, run);
    Check(delivered, "the native press was not delivered with the native window in the foreground and on screen");
    Check(GetForegroundWindow() == root,
        "the native window did not hold the foreground for the press");
    PumpApplicationMessages(2, state, 2000);
    Check(state.presses == 2 && state.accepted == 1,
        "a posted native press was not acted on by the application's own message loop");
    Check(state.foreground && state.onScreen && state.focused && state.active,
        "the native press did not keep the state a native click has until the application dispatched the click");

    // The native window is cloaked again, and the foreground is the proxy that
    // covers it: the press hands the foreground back when it ends.
    press.Release();
    DWORD cloaked = 0;
    const bool stillCloaked = SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked,
        sizeof(cloaked))) && (cloaked & DWM_CLOAKED_APP) != 0;
    Check(stillCloaked, "the native window was left on screen after the native press");
    Check(GetForegroundWindow() == proxy.window,
        "the foreground was not handed back to the proxy after the native press");
}
