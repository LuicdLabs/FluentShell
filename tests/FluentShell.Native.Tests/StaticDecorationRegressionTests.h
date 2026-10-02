#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
void TestStaticDecorationCaptureAndRejection() {
    using Translation::ControlKind;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"Static", L"static-decoration-regression",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 340, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    Check(root != nullptr, "Static decoration regression root was not created");
    if (!root) return;

    const std::array<DWORD, 7> drawStyles{
        SS_BLACKFRAME | SS_SUNKEN, SS_GRAYFRAME, SS_WHITEFRAME, SS_ETCHEDFRAME,
        SS_BLACKRECT, SS_GRAYRECT, SS_WHITERECT,
    };
    std::array<HWND, 7> decorations{};
    for (size_t index = 0; index < drawStyles.size(); ++index) {
        // The first item reproduces Add/Remove Columns: 0x50001007 / 0x00020004.
        decorations[index] = CreateWindowExW(index == 0 ? WS_EX_STATICEDGE | WS_EX_NOPARENTNOTIFY : 0,
            L"Static", L"ignored by the native draw style", WS_CHILD | WS_VISIBLE | drawStyles[index],
            10, 10 + static_cast<int>(index) * 35, 280, 24, root,
            reinterpret_cast<HMENU>(500 + index), instance, nullptr);
        Check(decorations[index] != nullptr, "built-in Static decoration was not created");
        if (!decorations[index]) return;
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-aaaaaaaaaaaa";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, error);
    if (!captured) std::wcerr << L"Static decoration capture rejection: " << error << L'\n';
    Check(captured, "built-in Static frames or fills were rejected");
    Check(snapshot.nodes.size() == drawStyles.size(), "Static decorations were dropped from the snapshot");
    for (const auto& node : snapshot.nodes) {
        Check(node.kind == ControlKind::StaticDecoration && !node.tabStop && node.tabIndex == -1 &&
              node.text.empty() && node.automationName.empty() && node.supportedActions.empty(),
            "Static decoration acquired an interactive role or a label");
        Check(node.style == static_cast<uint64_t>(GetWindowLongPtrW(node.hwnd, GWL_STYLE)) &&
              node.exStyle == static_cast<uint64_t>(GetWindowLongPtrW(node.hwnd, GWL_EXSTYLE)),
            "Static decoration lost native frame/fill or edge style evidence");
        for (const wchar_t* verb : { L"invoke", L"setText", L"setCheck", L"select", L"setValue" }) {
            Translation::ActionRequest action;
            action.nodeId = node.nodeId;
            action.action = verb;
            Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
                "Static decoration accepted an interactive control action");
        }
    }
    if (captured) {
        const auto json = Translation::SerializeWindowOpen(
            L"00112233445566778899aabbccddeeff", snapshot);
        Check(json.find("\"kind\":\"staticDecoration\"") != std::string::npos,
            "Static decoration kind was not serialized");
    }

    for (const DWORD rejected : { SS_NOTIFY, WS_TABSTOP }) {
        const HWND interactive = CreateWindowExW(0, L"Static", nullptr,
            WS_CHILD | WS_VISIBLE | SS_BLACKFRAME | rejected, 0, 0, 20, 20,
            root, nullptr, instance, nullptr);
        ControlKind kind{};
        Check(interactive && !Translation::ClassifyControl(interactive, kind, error),
            "interactive Static frame was admitted as a decoration");
        if (interactive) DestroyWindow(interactive);
    }
    const HWND composed = CreateWindowExW(WS_EX_TRANSPARENT, L"Static", nullptr,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME, 0, 0, 20, 20, root, nullptr, instance, nullptr);
    ControlKind kind{};
    Check(composed && !Translation::ClassifyControl(composed, kind, error),
        "Static frame with unsupported composition was admitted");
    if (composed) DestroyWindow(composed);
}
