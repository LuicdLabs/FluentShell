#pragma once

// A container that scrolls its own child windows (the WinForms AutoScroll shape):
// it reports a standard vertical scroll bar and moves its children for line and
// page steps only. Like WinForms, it ignores SB_THUMBPOSITION it did not track.

namespace ScrollingContainer {

constexpr int kLine = 7;
constexpr int kContentHeight = 600;

inline void ScrollTo(HWND window, int position) {
    SCROLLINFO info{ sizeof(info), SIF_POS | SIF_PAGE | SIF_RANGE };
    GetScrollInfo(window, SB_VERT, &info);
    const int last = info.nMax - static_cast<int>(info.nPage) + 1;
    const int next = std::clamp(position, info.nMin, std::max(info.nMin, last));
    const int delta = info.nPos - next;
    if (delta == 0) return;
    info.fMask = SIF_POS;
    info.nPos = next;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    ScrollWindowEx(window, 0, delta, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
}

inline LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_VSCROLL) {
        SCROLLINFO info{ sizeof(info), SIF_POS | SIF_PAGE };
        GetScrollInfo(window, SB_VERT, &info);
        switch (LOWORD(wParam)) {
        case SB_LINEDOWN: ScrollTo(window, info.nPos + kLine); break;
        case SB_LINEUP: ScrollTo(window, info.nPos - kLine); break;
        case SB_PAGEDOWN: ScrollTo(window, info.nPos + static_cast<int>(info.nPage)); break;
        case SB_PAGEUP: ScrollTo(window, info.nPos - static_cast<int>(info.nPage)); break;
        case SB_TOP: ScrollTo(window, 0); break;
        case SB_BOTTOM: ScrollTo(window, kContentHeight); break;
        default: break;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace ScrollingContainer

// MMC's Actions pane in a small window: the DirectUI host owns a CtrlNotifySink that
// hosts a real ScrollBar. That pair is the island's chrome, never separate nodes; a
// sink that hosts anything else keeps the ordinary contract.
void TestDirectUiScrollChrome() {
    const auto registerClass = [](const wchar_t* name) {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = name;
        return RegisterClassW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    };
    if (!registerClass(L"DirectUIHWND") || !registerClass(L"CtrlNotifySink")) {
        Check(false, "DirectUI scroll chrome fixture classes were not registered");
        return;
    }
    const HWND root = CreateWindowExW(0, L"Static", L"directui-scroll-chrome", WS_OVERLAPPEDWINDOW,
        0, 0, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND host = root ? CreateWindowExW(0, L"DirectUIHWND", L"", WS_CHILD | WS_VISIBLE,
        0, 0, 280, 260, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND sink = host ? CreateWindowExW(0, L"CtrlNotifySink", L"", WS_CHILD | WS_VISIBLE,
        263, 0, 17, 260, host, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND bar = sink ? CreateWindowExW(0, L"ScrollBar", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
        0, 0, 17, 260, sink, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND otherSink = host ? CreateWindowExW(0, L"CtrlNotifySink", L"", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 30, host, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND hostedButton = otherSink ? CreateWindowExW(0, L"Button", L"Hosted", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 30, otherSink, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (!bar || !hostedButton) {
        Check(false, "DirectUI scroll chrome fixture windows were not created");
        if (root) DestroyWindow(root);
        return;
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Check(Translation::IsDirectUiScrollChrome(sink) && Translation::IsDirectUiScrollChrome(bar),
        "a DirectUI scroll viewer's hosted ScrollBar was not recognized as chrome");
    Check(Translation::IsCompositeImplementationChild(sink) && Translation::IsCompositeImplementationChild(bar),
        "DirectUI scroll chrome would be captured as separate nodes");
    Check(!Translation::IsDirectUiScrollChrome(otherSink) &&
        !Translation::IsCompositeImplementationChild(otherSink),
        "a CtrlNotifySink hosting a real control was absorbed as scroll chrome");
    DestroyWindow(root);
    UnregisterClassW(L"CtrlNotifySink", GetModuleHandleW(nullptr));
    UnregisterClassW(L"DirectUIHWND", GetModuleHandleW(nullptr));
}

// Common-control codes share numbers across classes: TBM_SETLINESIZE (a trackbar
// mutator) is TB_GETBUTTON (a toolbar read). Judging them by number alone made a
// capture's own toolbar reads look like mutations, so a surface spanning GUI
// threads never passed its capture barrier.
void TestMutationMessageClasses() {
    const HWND root = CreateWindowExW(0, L"Static", L"mutation-classes", WS_OVERLAPPEDWINDOW,
        0, 0, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto child = [&](const wchar_t* type) {
        return root ? CreateWindowExW(0, type, L"", WS_CHILD, 0, 0, 50, 20, root, nullptr,
            GetModuleHandleW(nullptr), nullptr) : nullptr;
    };
    const HWND toolbar = child(TOOLBARCLASSNAMEW);
    const HWND trackbar = child(TRACKBAR_CLASSW);
    const HWND progress = child(PROGRESS_CLASSW);
    const HWND label = child(L"Static");
    if (!toolbar || !trackbar || !progress || !label) {
        Check(false, "mutation-class fixture windows were not created");
        if (root) DestroyWindow(root);
        return;
    }
    static_assert(TBM_SETLINESIZE == TB_GETBUTTON, "the fixture depends on this alias");
    Check(!Translation::IsNativeMutationMessage(toolbar, TB_GETBUTTON) &&
        Translation::IsNativeMutationMessage(trackbar, TBM_SETLINESIZE),
        "a toolbar read was treated as the trackbar mutator that shares its number");
    Check(Translation::IsNativeMutationMessage(toolbar, TB_SETSTATE) &&
        Translation::IsNativeMutationMessage(progress, PBM_SETPOS) &&
        !Translation::IsNativeMutationMessage(label, PBM_SETPOS),
        "control mutators were not judged by the receiving window's class");
    Check(Translation::IsNativeMutationMessage(label, WM_SETTEXT) &&
        !Translation::IsNativeMutationMessage(label, WM_GETTEXT),
        "class-independent mutators changed meaning");
    // A repaint's custom draw and a callback read's display-info query notify the
    // parent without changing anything; a selection change still counts.
    NMHDR paint{ toolbar, 1, static_cast<UINT>(NM_CUSTOMDRAW) };
    NMHDR query{ toolbar, 1, static_cast<UINT>(TVN_GETDISPINFOW) };
    NMHDR change{ toolbar, 1, static_cast<UINT>(TVN_SELCHANGEDW) };
    Check(!Translation::IsNativeMutationMessage(root, WM_NOTIFY, reinterpret_cast<LPARAM>(&paint)) &&
        !Translation::IsNativeMutationMessage(root, WM_NOTIFY, reinterpret_cast<LPARAM>(&query)) &&
        Translation::IsNativeMutationMessage(root, WM_NOTIFY, reinterpret_cast<LPARAM>(&change)),
        "WM_NOTIFY was not judged by its notification code");
    // An edit's redraw, focus and scroll notifications travel as WM_COMMAND too; only
    // a real change (or a non-edit command such as a click) counts.
    const auto source = reinterpret_cast<LPARAM>(label);
    Check(!Translation::IsNativeMutationMessage(root, WM_COMMAND, source, MAKEWPARAM(7, EN_UPDATE)) &&
        !Translation::IsNativeMutationMessage(root, WM_COMMAND, source, MAKEWPARAM(7, EN_KILLFOCUS)) &&
        Translation::IsNativeMutationMessage(root, WM_COMMAND, source, MAKEWPARAM(7, EN_CHANGE)) &&
        Translation::IsNativeMutationMessage(root, WM_COMMAND, source, MAKEWPARAM(7, BN_CLICKED)) &&
        Translation::IsNativeMutationMessage(root, WM_COMMAND, 0, MAKEWPARAM(40001, 0)),
        "WM_COMMAND was not judged by its edit notification code");
    DestroyWindow(root);
}

void TestScrollingContainer() {
    TestDirectUiScrollChrome();
    TestMutationMessageClasses();
    WNDCLASSW type{};
    type.lpfnWndProc = ScrollingContainer::Procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"FluentShell.Test.ScrollingPanel";
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    const bool registered = RegisterClassW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    const HWND root = registered ? CreateWindowExW(0, L"Static", L"scrolling-container",
        WS_OVERLAPPEDWINDOW, 40, 40, 320, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND panel = root ? CreateWindowExW(0, type.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 220, 200, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    // One tall content child, as a WinForms AutoScroll panel arranges it.
    const HWND content = panel ? CreateWindowExW(0, L"Button", L"Run with highest privileges",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 180, ScrollingContainer::kContentHeight,
        panel, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (!root || !panel || !content) {
        Check(false, "scrolling container fixture was not created");
        if (root) DestroyWindow(root);
        return;
    }
    RECT client{};
    GetClientRect(panel, &client);
    SCROLLINFO info{ sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };
    info.nMin = 0;
    info.nMax = ScrollingContainer::kContentHeight - 1;
    info.nPage = static_cast<UINT>(client.bottom);
    info.nPos = 0;
    SetScrollInfo(panel, SB_VERT, &info, TRUE);
    ShowWindow(root, SW_SHOWNOACTIVATE);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(panel, kind, reason) && kind == Translation::ControlKind::PaneContainer,
        "a container that scrolls its child windows was not admitted");
    Translation::ControlNode node;
    node.kind = Translation::ControlKind::PaneContainer;
    node.hwnd = panel;
    node.style = static_cast<uint64_t>(GetWindowLongPtrW(panel, GWL_STYLE));
    const bool captured = Translation::CaptureControlDetail(panel, node, reason);
    if (!captured) std::wcerr << L"scrolling container capture: " << reason << L'\n';
    const bool barShape = captured && node.scrollBars.size() == 1 && node.scrollBars[0].vertical &&
        node.scrollBars[0].enabled && node.scrollBars[0].minimum == 0 &&
        node.scrollBars[0].maximum == ScrollingContainer::kContentHeight - 1 &&
        node.scrollBars[0].page == client.bottom && node.scrollBars[0].position == 0 &&
        node.scrollBars[0].rect.left >= node.clientRect.right &&
        node.clientRect.right - node.clientRect.left == client.right &&
        node.clientRect.bottom - node.clientRect.top == client.bottom;
    Check(barShape, "scroll bar range, page, position, or viewport was not captured");

    const auto childTop = [&] {
        RECT rect{};
        GetWindowRect(content, &rect);
        MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&rect), 2);
        return rect.top;
    };
    const auto position = [&] {
        SCROLLINFO current{ sizeof(current), SIF_POS };
        GetScrollInfo(panel, SB_VERT, &current);
        return current.nPos;
    };
    // A target far away is reached with page steps, then line steps; a line step
    // that cannot land exactly stops where it crosses the target.
    Check(Translation::ScrollPaneContainer(panel, true, false, 300) &&
        std::abs(position() - 300) < ScrollingContainer::kLine && childTop() == -position(),
        "scrolling to a position did not move the application's children there");
    const int before = position();
    Check(Translation::ScrollPaneContainer(panel, true, true, -2) && position() == before - 2 * ScrollingContainer::kLine,
        "line steps were not delivered through the application's handler");
    Translation::ScrollPaneContainer(panel, true, false, ScrollingContainer::kContentHeight);
    Check(!Translation::ScrollPaneContainer(panel, true, true, 1),
        "a container already at its end reported that it scrolled");

    Translation::ActionRequest action;
    action.action = L"setScroll";
    action.booleanValue = true;
    action.integerValue = 40;
    Translation::WindowSnapshot snapshot;
    node.nodeId = 9;
    snapshot.nodes.push_back(node);
    action.nodeId = 9;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, reason),
        "setScroll was refused for a captured scroll bar");
    action.booleanValue = false;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, reason),
        "setScroll was accepted for an axis the container does not scroll");
    action.booleanValue = true;
    action.integerValue = ScrollingContainer::kContentHeight + 10;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, reason),
        "setScroll accepted a position outside the scroll range");
    DestroyWindow(root);
}
