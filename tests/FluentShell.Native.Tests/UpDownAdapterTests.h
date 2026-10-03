#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// A real msctls_updown32 with a UDS_SETBUDDYINT buddy Edit.  The parent records the
// UDN_DELTAPOS and scroll notifications a native arrow click produces, and can veto
// the step or rewrite its delta the way an application handler may.

struct UpDownParentState final {
    int deltaNotifications = 0;
    int lastPos = 0;
    int lastDelta = 0;
    int thumbPositions = 0;
    int endScrolls = 0;
    bool veto = false;
    int rewriteDelta = 0;
};

LRESULT CALLBACK UpDownParentSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<UpDownParentState*>(data);
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && header->code == UDN_DELTAPOS) {
            auto* change = reinterpret_cast<NMUPDOWN*>(lParam);
            ++state.deltaNotifications;
            state.lastPos = change->iPos;
            state.lastDelta = change->iDelta;
            if (state.rewriteDelta != 0) change->iDelta = state.rewriteDelta;
            return state.veto ? 1 : 0;
        }
    }
    if (message == WM_VSCROLL || message == WM_HSCROLL) {
        if (LOWORD(wParam) == SB_THUMBPOSITION) ++state.thumbPositions;
        if (LOWORD(wParam) == SB_ENDSCROLL) ++state.endScrolls;
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void TestUpDownAdapter() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"spin", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        40, 40, 320, 160, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND buddy = root ? CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"", WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | ES_NUMBER, 12, 12, 80, 22, root, reinterpret_cast<HMENU>(31), instance, nullptr) : nullptr;
    const HWND spin = root ? CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE |
        UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0, root,
        reinterpret_cast<HMENU>(32), instance, nullptr) : nullptr;
    Check(root && buddy && spin, "UpDown fixture could not create its windows");
    if (!root || !buddy || !spin) return;
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(buddy), 0);
    SendMessageW(spin, UDM_SETRANGE32, 0, 50);
    SendMessageW(spin, UDM_SETPOS32, 0, 10);
    UpDownParentState state;
    SetWindowSubclass(root, UpDownParentSubclass, 0xAA41, reinterpret_cast<DWORD_PTR>(&state));
    ShowWindow(root, SW_SHOWNOACTIVATE);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(spin, kind, reason) && kind == Translation::ControlKind::UpDown,
        "a standard UpDown was not classified as upDown");
    Translation::CaptureContext context;
    context.surfaceId = L"e1e1e1e1-2f2f-3030-4141-525252525252";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"UpDown capture: " << reason << L"\n";
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [spin](const auto& candidate) { return candidate.hwnd == spin; });
    Check(captured && node != snapshot.nodes.end() && node->kind == Translation::ControlKind::UpDown &&
        node->minimum == 0 && node->maximum == 50 && node->position == 10 && !node->reversed &&
        node->vertical && node->smallChange >= 1,
        "UpDown range, position, orientation or step was not captured");

    // One arrow step: the parent sees UDN_DELTAPOS for +1 from 10, the position and
    // the buddy follow, and the scroll pair arrives.
    bool refused = false;
    Check(Translation::SetUpDownPosition(root, spin, 11, refused) && !refused &&
        state.deltaNotifications == 1 && state.lastPos == 10 && state.lastDelta == 1 &&
        state.thumbPositions == 1 && state.endScrolls == 1,
        "an UpDown step did not run the native notification sequence");
    wchar_t text[16]{};
    GetWindowTextW(buddy, text, 16);
    Check(std::wstring(text) == L"11", "the UDS_SETBUDDYINT buddy did not follow the step");

    // The application vetoes: nothing changes and the step is reported refused.
    state.veto = true;
    Check(!Translation::SetUpDownPosition(root, spin, 12, refused) && refused &&
        SendMessageW(spin, UDM_GETPOS32, 0, 0) == 11 && state.thumbPositions == 1,
        "a vetoed UpDown step changed native state or was not reported as refused");
    state.veto = false;

    // The application rewrites the delta: the control honours the rewritten step.
    state.rewriteDelta = 5;
    Check(Translation::SetUpDownPosition(root, spin, 12, refused) && !refused &&
        SendMessageW(spin, UDM_GETPOS32, 0, 0) == 16,
        "an UpDown step ignored the delta the application rewrote");
    state.rewriteDelta = 0;

    // A backwards range is carried ordered with the direction flagged.
    SendMessageW(spin, UDM_SETRANGE32, 50, 0);
    context.revision = 2;
    Translation::WindowSnapshot reversed;
    const bool reversedCaptured = Translation::CaptureWindow(root, context, reversed, reason);
    const auto reversedNode = std::find_if(reversed.nodes.begin(), reversed.nodes.end(),
        [spin](const auto& candidate) { return candidate.hwnd == spin; });
    Check(reversedCaptured && reversedNode != reversed.nodes.end() && reversedNode->reversed &&
        reversedNode->minimum == 0 && reversedNode->maximum == 50,
        "a backwards UpDown range was not carried ordered and flagged");
    RemoveWindowSubclass(root, UpDownParentSubclass, 0xAA41);
}
