#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// Windows gates some of its own controls on the window being the foreground one.
// Explorer's "Open File - Security Warning" runs the file only while its dialog is
// in the foreground: the press reaches the button, and the dialog refuses it.  A
// projected surface keeps the WinUI proxy in the foreground instead, so the press
// has to be delivered with the native window in the foreground.
//
// The fixture stands for both sides of that arrangement, in two processes, the way
// the product runs it.  The renderer process holds the foreground (its proxy) and
// grants the right to take it, the way the renderer does before it sends an
// action.  The bridge process owns the native dialog and presses its button, and
// the dialog refuses a press that arrives while the proxy is in the foreground.

struct ForegroundGatedPressState final {
    int presses = 0;
    int accepted = 0;
};

LRESULT CALLBACK ForegroundGatedPressSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<ForegroundGatedPressState*>(data);
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 4426) {
        ++state.presses;
        // The gate a shell dialog applies: the press counts only while the dialog
        // is the foreground window.
        if (GetForegroundWindow() == window) ++state.accepted;
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

// The renderer side of the arrangement: a window that holds the foreground, the
// right to take it, and the hand-back of the foreground afterwards.
int RunForegroundGrantOwner(DWORD parentProcessId, const wchar_t* readyName, const wchar_t* pressedName) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"FlsProxyStandIn";
    RegisterClassW(&wc);
    HWND proxy = CreateWindowExW(0, L"FlsProxyStandIn", L"proxy stand-in",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 60, 60, 320, 160, nullptr, nullptr, instance, nullptr);
    if (!proxy) return 2;
    ShowWindow(proxy, SW_SHOW);
    SetForegroundWindow(proxy);
    // The renderer grants the right to take the foreground to the process it
    // projects, the way it does before it sends an action.
    AllowSetForegroundWindow(parentProcessId);
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName)) SetEvent(ready);
    if (HANDLE pressed = OpenEventW(SYNCHRONIZE, FALSE, pressedName)) {
        WaitForSingleObject(pressed, 10000);
    }
    SetForegroundWindow(proxy);
    Sleep(300);
    DestroyWindow(proxy);
    return 0;
}

void TestNativePressActivation() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const DWORD pid = GetCurrentProcessId();
    const std::wstring readyName = L"Local\\FlsGrantReady." + std::to_wstring(pid);
    const std::wstring pressedName = L"Local\\FlsGrantPressed." + std::to_wstring(pid);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE pressed = CreateEventW(nullptr, TRUE, FALSE, pressedName.c_str());
    Check(ready && pressed, "the foreground handover fixture could not create its events");
    if (!ready || !pressed) return;

    // The renderer process: it holds the foreground and grants the right.
    wchar_t self[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::wstring command = L"\"";
    command += self;
    command += L"\" --foreground-grant ";
    command += std::to_wstring(pid);
    command += L" \"";
    command += readyName;
    command += L"\" \"";
    command += pressedName;
    command += L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{ sizeof(startup) };
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        nullptr, &startup, &process) != FALSE,
        "the renderer stand-in could not be started");
    if (process.hProcess == nullptr) { CloseHandle(ready); CloseHandle(pressed); return; }
    Check(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0,
        "the renderer stand-in did not take the foreground");

    // The bridge side: the native dialog and the button that refuses a press
    // that arrives while the proxy is in the foreground.
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"foreground gated press",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 60, 60, 320, 160, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND run = root ? CreateWindowExW(0, L"Button", L"&Run", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        20, 40, 90, 26, root, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4426)), instance, nullptr) : nullptr;
    Check(root && run, "the foreground-gated press fixture could not create its windows");
    if (!root || !run) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        CloseHandle(ready);
        CloseHandle(pressed);
        return;
    }
    ForegroundGatedPressState state;
    SetWindowSubclass(root, ForegroundGatedPressSubclass, 0xAA71, reinterpret_cast<DWORD_PTR>(&state));
    ShowWindow(root, SW_SHOWNOACTIVATE);

    // A press that arrives while the proxy is in the foreground reaches the
    // dialog and is refused: the shell's security warning refuses its Run button
    // the same way.
    SendMessageW(root, WM_COMMAND, MAKEWPARAM(4426, BN_CLICKED), reinterpret_cast<LPARAM>(run));
    Check(state.presses == 1 && state.accepted == 0,
        "a press without the native window in the foreground was not refused");

    // The press the projection uses borrows the foreground for its duration, and
    // the dialog that gates on it accepts the press.  The handover says it held
    // the foreground, so the acceptance is the handover's doing.
    const bool held = Translation::PressNativeButton(root, run);
    Check(held, "the native window could not borrow the foreground for the press");
    Check(state.presses == 2 && state.accepted == 1,
        "a native press was not delivered with the native window in the foreground");

    // The foreground goes back to the proxy that held it.
    wchar_t proxyClass[64]{};
    GetClassNameW(GetForegroundWindow(), proxyClass, 64);
    Check(std::wstring(proxyClass) == L"FlsProxyStandIn",
        "the foreground was not handed back to the proxy after the native press");
    SetEvent(pressed);

    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 2000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(ready);
    CloseHandle(pressed);
}
