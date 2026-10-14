#pragma once

// Include after main.cpp's Check helper and ListViewActivationTests.h, inside its test namespace.

// A split button (the Sound applet's Set Default) is a push button whose arrow asks its owner
// for a menu: BCN_DROPDOWN, which the projection sends as the arrow's click does.
UINT g_splitDropDowns = 0;
HWND g_splitDropDownFrom = nullptr;

LRESULT CALLBACK SplitButtonOwnerSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_NOTIFY && lParam &&
        reinterpret_cast<const NMHDR*>(lParam)->code == BCN_DROPDOWN) {
        ++g_splitDropDowns;
        g_splitDropDownFrom = reinterpret_cast<const NMHDR*>(lParam)->hwndFrom;
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SplitButtonOwnerSubclass, subclassId);
    return result;
}

void TestSplitButtons() {
    ListActivationCommonControls controls(ICC_STANDARD_CLASSES);
    Check(static_cast<bool>(controls), "split button v6 activation failed");
    if (!controls) return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } host{ CreateWindowExW(0, WC_DIALOG, L"split-button", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        60, 60, 360, 160, nullptr, nullptr, instance, nullptr) };
    const HWND split = host.window ? CreateWindowExW(0, L"Button", L"Set &Default",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_SPLITBUTTON, 20, 20, 140, 28, host.window,
        reinterpret_cast<HMENU>(1012), instance, nullptr) : nullptr;
    Check(split != nullptr, "split button fixture could not create its control");
    if (!split) return;
    SetWindowSubclass(host.window, SplitButtonOwnerSubclass, 11, 0);
    ShowWindow(host.window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-afafafafafaf";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(host.window, context, snapshot, error);
    if (!captured) std::wcerr << L"split button capture rejection: " << error << L'\n';
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) { return candidate.controlId == 1012; });
    Check(captured && node != snapshot.nodes.end() && node->kind == Translation::ControlKind::Button &&
          node->presentationVariant == L"splitButton" && !node->isDefault,
        "a split button was not captured as a button with its split variant");
    if (!captured || node == snapshot.nodes.end()) return;

    Translation::ActionRequest action;
    const std::string request = "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"77777777-7777-7777-7777-afafafafafaf\",\"nodeId\":\"" + std::to_string(node->nodeId) + "\","
        "\"eventId\":\"5\",\"expectedRevision\":\"1\",\"action\":\"splitDropDown\",\"value\":null}";
    Check(Translation::ParseActionInvoke(request, L"00112233445566778899aabbccddeeff", action, error) &&
          action.action == L"splitDropDown" && Translation::IsRequestSemanticAction(L"splitDropDown") &&
          Translation::ValidateActionForSnapshot(action, snapshot, error),
        "the split button's arrow request was not admitted");
    Translation::WindowSnapshot plain = snapshot;
    for (auto& candidate : plain.nodes) if (candidate.controlId == 1012) candidate.presentationVariant.clear();
    Check(!Translation::ValidateActionForSnapshot(action, plain, error),
        "an arrow request was admitted for a button without an arrow");

    g_splitDropDowns = 0;
    g_splitDropDownFrom = nullptr;
    Check(Translation::RequestSplitButtonDropDown(split, error) && g_splitDropDowns == 1 &&
          g_splitDropDownFrom == split,
        "the arrow request did not reach the owner as BCN_DROPDOWN from the button");
    EnableWindow(split, FALSE);
    Check(!Translation::RequestSplitButtonDropDown(split, error) && g_splitDropDowns == 1,
        "a disabled split button's arrow still asked its owner for a menu");
}
