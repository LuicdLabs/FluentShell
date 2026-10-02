#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
void TestOverlappingSiblingZOrderCapture() {
    using Translation::ControlKind;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    // Creation, capture, reordering, and destruction all remain on this GUI
    // thread. Destroying the root also retires every child on early failure.
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"Static", L"overlapping-sibling-order",
        WS_OVERLAPPEDWINDOW, 20, 20, 420, 290, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    Check(root != nullptr, "sibling z-order root was not created");
    if (!root) return;

    std::array<HWND, 2> panes{};
    std::array<HWND, 4> buttons{};
    for (size_t index = 0; index < panes.size(); ++index) {
        panes[index] = CreateWindowExW(0, L"Static", index == 0 ? L"First pane" : L"Second pane",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            20 + static_cast<int>(index) * 60, 20 + static_cast<int>(index) * 30,
            260, 150, root, reinterpret_cast<HMENU>(510 + index), instance, nullptr);
        Check(panes[index] != nullptr, "overlapping sibling pane was not created");
        if (!panes[index]) return;
        for (size_t item = 0; item < 2; ++item) {
            // The larger button fills its pane; the smaller one overlaps it.
            // The pane therefore has no incidental painted bands to capture.
            buttons[index * 2 + item] = CreateWindowExW(0, L"Button",
                item == 0 ? L"Full pane command" : L"Overlay command",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON,
                item == 0 ? 0 : 20, item == 0 ? 0 : 20,
                item == 0 ? 260 : 180, item == 0 ? 150 : 90,
                panes[index], reinterpret_cast<HMENU>(520 + index * 2 + item), instance, nullptr);
            Check(buttons[index * 2 + item] != nullptr, "overlapping sibling button was not created");
            if (!buttons[index * 2 + item]) return;
        }
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);

    const auto bringToFront = [](HWND window) {
        const bool moved = SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
        Check(moved, "sibling z-order could not be changed without moving its window");
        return moved;
    };
    if (!bringToFront(panes[0]) || !bringToFront(buttons[0]) || !bringToFront(buttons[2])) return;

    Translation::CaptureContext context;
    context.surfaceId = L"91919191-2323-4545-6767-898989898989";
    context.generation = 1;
    context.revision = 1;
    const auto nodeFor = [](const Translation::WindowSnapshot& snapshot, HWND window) {
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [window](const auto& node) { return node.hwnd == window; });
        return found == snapshot.nodes.end() ? nullptr : &*found;
    };
    const auto verifySiblingOrder = [&](const Translation::WindowSnapshot& snapshot,
        HWND parent, HWND front, HWND back) {
        Check(GetWindow(parent, GW_CHILD) == front && GetWindow(front, GW_HWNDNEXT) == back,
            "native sibling order did not put the requested HWND in front");
        RECT frontRect{}, backRect{}, overlap{};
        Check(GetWindowRect(front, &frontRect) && GetWindowRect(back, &backRect) &&
              IntersectRect(&overlap, &frontRect, &backRect),
            "sibling z-order fixture no longer exercises overlapping windows");
        POINT point{ (overlap.left + overlap.right) / 2, (overlap.top + overlap.bottom) / 2 };
        ScreenToClient(parent, &point);
        Check(ChildWindowFromPointEx(parent, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED) == front,
            "native child hit testing disagreed with the front sibling");
        const auto* frontNode = nodeFor(snapshot, front);
        const auto* backNode = nodeFor(snapshot, back);
        Check(frontNode && backNode && frontNode < backNode && frontNode->zIndex < backNode->zIndex,
            "capture did not assign the front native sibling an earlier node and lower z-index");
    };
    const auto capture = [&](Translation::WindowSnapshot& snapshot, size_t frontIndex) {
        std::wstring error;
        const bool captured = Translation::CaptureWindow(root, context, snapshot, error);
        if (!captured) std::wcerr << L"sibling z-order capture rejection: " << error << L'\n';
        Check(captured, "overlapping supported siblings were rejected by capture");
        if (!captured) return false;
        Check(snapshot.surfaceKind == Translation::SurfaceKind::Window && snapshot.nodes.size() == 6,
            "overlapping sibling capture lost a native node");
        verifySiblingOrder(snapshot, root, panes[frontIndex], panes[1 - frontIndex]);
        for (size_t index = 0; index < panes.size(); ++index) {
            verifySiblingOrder(snapshot, panes[index], buttons[index * 2 + frontIndex],
                buttons[index * 2 + 1 - frontIndex]);
            const auto* pane = nodeFor(snapshot, panes[index]);
            Check(pane && pane->kind == ControlKind::PaneContainer && !pane->parentNodeId,
                "overlapping pane lost its root parent or container role");
            for (size_t item = 0; item < 2; ++item) {
                const auto* button = nodeFor(snapshot, buttons[index * 2 + item]);
                Check(pane && button && button->kind == ControlKind::Button &&
                      button->parentNodeId == pane->nodeId && pane < button && pane->zIndex < button->zIndex,
                    "native z-order capture no longer emits a parent before its child");
            }
        }
        return true;
    };

    Translation::WindowSnapshot before;
    if (!capture(before, 0)) return;
    // Change both the outer pane order and each pane's button order. Node IDs
    // must follow their HWNDs when enumeration visits those subtrees differently.
    if (!bringToFront(panes[1]) || !bringToFront(buttons[1]) || !bringToFront(buttons[3])) return;
    ++context.revision;
    Translation::WindowSnapshot after;
    if (!capture(after, 1)) return;
    for (const auto& original : before.nodes) {
        const auto* reordered = nodeFor(after, original.hwnd);
        Check(reordered && reordered->nodeId == original.nodeId &&
              reordered->generation == original.generation && reordered->parentNodeId == original.parentNodeId,
            "reordering native siblings changed a node identity or parent");
        Check(reordered && EqualRect(&reordered->rect, &original.rect),
            "z-order-only mutation changed captured sibling geometry");
    }
    Check(Translation::SnapshotFingerprint(before) != Translation::SnapshotFingerprint(after),
        "sibling z-order change did not invalidate the canonical snapshot fingerprint");
}
