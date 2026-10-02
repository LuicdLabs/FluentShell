#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
namespace MdiCaptureRegression {

LRESULT CALLBACK ChildProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefMDIChildProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK FrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefFrameProcW(window, FindWindowExW(window, nullptr, L"MDIClient", nullptr),
        message, wParam, lParam);
}

} // namespace MdiCaptureRegression

void TestMdiCapturePreservesNavigationAcrossModalDisable() {
    using Translation::ControlKind;
    static_assert(WS_MAXIMIZEBOX == WS_TABSTOP);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW frameClass{ sizeof(frameClass) };
    frameClass.lpfnWndProc = MdiCaptureRegression::FrameProc;
    frameClass.hInstance = instance;
    frameClass.lpszClassName = L"FluentShell.Test.MdiModalFrame";
    RegisterClassExW(&frameClass);
    WNDCLASSEXW childClass{ sizeof(childClass) };
    childClass.lpfnWndProc = MdiCaptureRegression::ChildProc;
    childClass.hInstance = instance;
    childClass.lpszClassName = L"FluentShell.Test.MdiModalChild";
    RegisterClassExW(&childClass);

    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, frameClass.lpszClassName,
        L"mdi-modal-navigation", WS_OVERLAPPEDWINDOW, 20, 20, 620, 360,
        nullptr, nullptr, instance, nullptr) };
    const HWND frame = owner.window;
    Check(frame != nullptr, "MDI modal regression frame was not created");
    if (!frame) return;

    CLIENTCREATESTRUCT clientCreate{};
    clientCreate.idFirstChild = 45000;
    // Expose the subtree to the outer dialog walk as well: each edit must still
    // receive its order from its own MDI child, and neither frame consumes a slot.
    const HWND client = CreateWindowExW(WS_EX_CONTROLPARENT, L"MDIClient", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
        0, 35, 600, 270, frame, reinterpret_cast<HMENU>(410), instance, &clientCreate);
    const HWND rootEdit = CreateWindowExW(0, L"Edit", L"frame control",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, 5, 180, 24,
        frame, reinterpret_cast<HMENU>(411), instance, nullptr);
    Check(client && rootEdit, "MDI modal regression client or root edit was not created");
    if (!client || !rootEdit) return;

    std::array<HWND, 2> children{};
    std::array<HWND, 4> edits{};
    for (size_t childIndex = 0; childIndex < children.size(); ++childIndex) {
        MDICREATESTRUCTW create{};
        create.szClass = childClass.lpszClassName;
        create.szTitle = childIndex == 0 ? L"Document 1" : L"Document 2";
        create.hOwner = instance;
        create.x = 10 + static_cast<int>(childIndex) * 275;
        create.y = 10;
        create.cx = 260;
        create.cy = 220;
        const HWND child = reinterpret_cast<HWND>(SendMessageW(
            client, WM_MDICREATE, 0, reinterpret_cast<LPARAM>(&create)));
        children[childIndex] = child;
        Check(child != nullptr, "MDI modal regression child was not created");
        if (!child) return;
        SetWindowLongPtrW(child, GWL_EXSTYLE,
            GetWindowLongPtrW(child, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
        Check((GetWindowLongPtrW(child, GWL_STYLE) & WS_MAXIMIZEBOX) != 0,
            "MDI regression child does not exercise the maximize/tab-stop alias");
        for (size_t editIndex = 0; editIndex < 2; ++editIndex) {
            const size_t index = childIndex * 2 + editIndex;
            edits[index] = CreateWindowExW(0, L"Edit", L"document control",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, 10 + static_cast<int>(editIndex) * 35,
                180, 24, child, reinterpret_cast<HMENU>(420 + index), instance, nullptr);
            Check(edits[index] != nullptr, "MDI modal regression edit was not created");
            if (!edits[index]) return;
        }
    }
    ShowWindow(frame, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-999999999999";
    context.generation = 1;
    const auto verify = [&](bool enabled) {
        ++context.revision;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        const bool captured = Translation::CaptureWindow(frame, context, snapshot, error);
        if (!captured) std::wcerr << L"MDI modal capture rejection: " << error << L'\n';
        Check(captured, "MDI modal enable transition was rejected");
        if (!captured) return;
        Check(snapshot.enabled == enabled && snapshot.nodes.size() == 8,
            "MDI modal capture lost root enabledness or descendant nodes");
        const auto nodeFor = [&](HWND window) -> const Translation::ControlNode* {
            const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                [window](const Translation::ControlNode& node) { return node.hwnd == window; });
            return found == snapshot.nodes.end() ? nullptr : &*found;
        };
        for (const HWND window : { client, children[0], children[1] }) {
            const auto* node = nodeFor(window);
            Check(node && node->kind == (window == client ? ControlKind::MdiClient : ControlKind::MdiChild) &&
                  !node->tabStop && node->tabIndex == -1 && node->enabled == enabled,
                "structural MDI node became a dialog tab stop across modal disable");
            Check(node && node->style == static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_STYLE)) &&
                  node->exStyle == static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
                "MDI navigation normalization changed native style evidence");
        }
        std::vector<int> order;
        for (const HWND edit : { rootEdit, edits[0], edits[1], edits[2], edits[3] }) {
            const auto* node = nodeFor(edit);
            const auto* parent = GetParent(edit) == frame ? nullptr : nodeFor(GetParent(edit));
            Check(node && node->kind == ControlKind::Edit && node->enabled == enabled && node->tabStop &&
                  (enabled ? node->tabIndex >= 0 : node->tabIndex == -1),
                "ordinary MDI edit lost native tab-stop semantics across modal disable");
            Check(node && (edit == rootEdit ? !node->parentNodeId :
                  parent && node->parentNodeId == parent->nodeId),
                "MDI edit lost its native parent scope");
            if (node && enabled) order.push_back(node->tabIndex);
        }
        if (!enabled) return;
        std::sort(order.begin(), order.end());
        Check(order == std::vector<int>{ 0, 1, 2, 3, 4 },
            "MDI containers consumed or duplicated a dialog tab-order slot");
        const auto* rootNode = nodeFor(rootEdit);
        Check(rootNode && rootNode->tabIndex == 0,
            "MDI descendant received its order from the root traversal");
        for (const HWND child : children) {
            const HWND seed = GetWindow(child, GW_CHILD);
            const HWND first = GetNextDlgTabItem(child, seed, FALSE);
            const HWND second = first ? GetNextDlgTabItem(child, first, FALSE) : nullptr;
            const auto* firstNode = nodeFor(first);
            const auto* secondNode = nodeFor(second);
            Check(firstNode && secondNode && first != second &&
                  firstNode->tabIndex + 1 == secondNode->tabIndex,
                "MDI child controls did not retain their own dialog-manager order");
        }
    };

    verify(true);
    // A modal Open dialog disables its native owner; this was the transition
    // that turned WS_MAXIMIZEBOX into TabStop=true and faulted MMC's projection.
    EnableWindow(frame, FALSE);
    verify(false);
    EnableWindow(frame, TRUE);
    verify(true);
}
