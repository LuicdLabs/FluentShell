#pragma once

// Included in main.cpp's test namespace after TestMenuAccessible and its subclass.
// The provider opens ephemeral HMENUs asynchronously and destroys each immediately
// after the hook returns, just as the MMC toolbar does.
struct MenuIdentityRuntime final {
    HWND root = nullptr;
    HWND otherOwner = nullptr;
    UINT flags = TPM_RETURNCMD;
    UINT nativeId = 77;
    bool changedLabel = false;
    bool disabledAncestor = false;
    bool changedOwner = false;
    bool firstSystem = false;
    bool excessive = false;
    unsigned ownerCommands = 0;
    unsigned rootCommands = 0;
    UINT lastCommand = 0;
    BOOL returnValue = FALSE;
    WPARAM returnedSlot = 0;
    bool guardReleasedAtReturn = false;
};

LRESULT CALLBACK MenuIdentitySubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& state = *reinterpret_cast<MenuIdentityRuntime*>(data);
    if (message == kTestMenuPopup) {
        const HMENU popup = CreatePopupMenu();
        if (state.firstSystem && wParam == 1) {
            AppendMenuW(popup, MF_STRING, SC_CLOSE, L"Close");
        } else {
            AppendMenuW(popup, MF_STRING, state.nativeId,
                state.changedLabel ? L"Changed" : L"Shared");
            const HMENU nested = CreatePopupMenu();
            AppendMenuW(nested, MF_STRING, state.nativeId, L"Nested");
            AppendMenuW(popup, MF_POPUP | (state.disabledAncestor ? MF_GRAYED : 0),
                reinterpret_cast<UINT_PTR>(nested), L"Options");
            MENUITEMINFOW popupIdentity{sizeof(popupIdentity)};
            popupIdentity.fMask = MIIM_ID;
            popupIdentity.wID = 0xF100;
            Check(SetMenuItemInfoW(popup, 1, TRUE, &popupIdentity) != FALSE,
                "test popup's non-command identity was not set");
            Check(!Translation::IsWindowSystemMenu(popup),
                "submenu identity was mistaken for an executable system command");
            if (state.excessive) {
                for (int index = 0; index < 150; ++index)
                    AppendMenuW(popup, MF_STRING, 200 + index, L"Many");
            }
        }
        const HWND owner = state.changedOwner ? state.root :
            wParam == 2 ? state.otherOwner : window;
        state.returnValue = Translation::RecordInterceptedPopup(popup, state.flags, owner);
        state.returnedSlot = wParam;
        if (state.returnValue) {
            state.guardReleasedAtReturn = !Translation::MenuBarReadInProgress() &&
                !Translation::PopupInterceptionArmed() && !Translation::PopupSuppressionActive();
        }
        DestroyMenu(popup);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (window == state.root) ++state.rootCommands;
        else ++state.ownerCommands;
        state.lastCommand = LOWORD(wParam);
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, MenuIdentitySubclass, subclassId);
    return result;
}

void TestMenuBarCommandIdentityAndNativeRouting() {
    const HWND root = CreateWindowExW(0, L"Static", L"menu-identity",
        WS_OVERLAPPEDWINDOW, 0, 0, 360, 220, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND toolbar = root ? CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 350, 30, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND owner = root ? CreateWindowExW(0, L"Static", L"tracking-owner",
        WS_CHILD | WS_VISIBLE, 0, 40, 50, 30, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && toolbar && owner, "menu identity test windows were not created");
    if (!root || !toolbar || !owner) { if (root) DestroyWindow(root); return; }
    auto* accessible = new TestMenuAccessible();
    accessible->toolbar = toolbar;
    accessible->names = {L"&File", L"&View", L"&Window"};
    SetWindowSubclass(toolbar, TestMenuToolbarSubclass, 0xAA11, reinterpret_cast<DWORD_PTR>(accessible));
    MenuIdentityRuntime runtime;
    runtime.root = root;
    runtime.otherOwner = owner;
    for (const HWND window : {root, toolbar, owner})
        SetWindowSubclass(window, MenuIdentitySubclass, 0xAA12, reinterpret_cast<DWORD_PTR>(&runtime));
    ShowWindow(root, SW_SHOWNOACTIVATE);
    std::vector<Translation::MenuBarButton> buttons;
    std::vector<Translation::MenuItemSnapshot> menu;
    std::vector<Translation::MenuBarCommandBinding> commands;
    std::atomic<bool> cancelled{false};
    std::wstring error;
    const auto capture = [&] {
        return Translation::ReadMenuBarButtons(toolbar, buttons) &&
            Translation::CaptureMenuBarToolbar(root, toolbar, buttons, 30, 2000,
                cancelled, menu, error, &commands);
    };
    const bool captured = capture();
    if (!captured || menu.size() != 3 || commands.size() != 4)
        std::wcerr << L"Menu identity capture: " << error << L" menus=" << menu.size()
            << L" bindings=" << commands.size() << L'\n';
    Check(captured && menu.size() == 3 && commands.size() == 4,
        "duplicate native commands across/within ephemeral popups were not captured");
    if (commands.size() == 4 && menu.size() == 3) {
        std::vector<uint32_t> ids;
        for (const auto& command : commands) ids.push_back(command.commandId);
        std::sort(ids.begin(), ids.end());
        Check(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
            "complete toolbar bar emitted ambiguous projected command IDs");
        Check(commands[0].nativeCommandId == 77 && commands[1].nativeCommandId == 77 &&
            commands[2].nativeCommandId == 77 && commands[3].nativeCommandId == 77 &&
            commands[0].owner == toolbar && commands[2].owner == owner &&
            commands[2].toolbarIndex == 2 && commands[2].itemId == L"1.0",
            "synthetic menu identities lost the native ID, owner, path or toolbar slot");
        Check(menu[0].items[0].commandId == commands[0].commandId &&
            menu[1].items[0].commandId == commands[2].commandId,
            "projected menu IDs disagree with native bindings");
        const auto direct = commands[2];
        const auto nested = commands[3];
        Check(Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error) && runtime.returnValue == 77 &&
                runtime.returnedSlot == 2 && runtime.ownerCommands == 0 && runtime.rootCommands == 0 &&
                runtime.guardReleasedAtReturn,
            "TPM_RETURNCMD did not return the native ID from the correct slot with modal-safe guards");
        runtime.nativeId = 78;
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error) && runtime.returnValue == FALSE,
            "stale native command ID ran through a synthetic menu identity");
        runtime.nativeId = 77;
        runtime.changedLabel = true;
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error), "relabeled menu command ran with a stale binding");
        runtime.changedLabel = false;
        runtime.disabledAncestor = true;
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                nested, cancelled, error), "disabled native submenu ancestor permitted dispatch");
        runtime.disabledAncestor = false;
        runtime.changedOwner = true;
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error), "native menu selection changed tracking owner");
        runtime.changedOwner = false;
        ShowWindow(toolbar, SW_HIDE);
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error), "hidden toolbar dispatched a menu selection");
        ShowWindow(toolbar, SW_SHOWNOACTIVATE);
        EnableWindow(toolbar, FALSE);
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error), "disabled toolbar dispatched a menu selection");
        EnableWindow(toolbar, TRUE);
        runtime.flags = 0;
        Check(!Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                direct, cancelled, error), "native menu selection changed its return/notify contract");
        Check(capture() && commands.size() == 4, "notification-style menus could not be refreshed");
        if (commands.size() == 4) {
            Check(Translation::InvokeMenuBarToolbarCommand(root, toolbar, buttons,
                    commands[2], cancelled, error), "notification-style menu selection failed");
            MSG command{};
            while (PeekMessageW(&command, nullptr, WM_COMMAND, WM_COMMAND, PM_REMOVE))
                DispatchMessageW(&command);
            Check(runtime.ownerCommands == 1 && runtime.rootCommands == 0 && runtime.lastCommand == 77,
                "menu command was delivered to the frame or used its synthetic ID");
        }
        runtime.firstSystem = true;
        accessible->names = {L"System", L"&File", L"&Window"};
        Check(capture() && commands.size() == 2 && commands[0].toolbarIndex == 2 &&
                commands[0].itemId == L"0.0", "skipped system slot shifted the native toolbar binding");
        runtime.firstSystem = false;
        runtime.excessive = true;
        Check(!capture(), "independently bounded popups exceeded the complete bar's protocol cap");
    }
    Translation::WindowSnapshot snapshot;
    snapshot.menuBindingGeneration = 1;
    const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.menuBindingGeneration = 2;
    Check(fingerprint != Translation::SnapshotFingerprint(snapshot),
        "wire-identical menu binding replacement did not invalidate the snapshot revision");
    Check(!Translation::MenuBarReadInProgress() && !Translation::PopupInterceptionArmed() &&
        !Translation::PopupSuppressionActive(), "menu identity test leaked its interception scope");
    DestroyWindow(root);
    accessible->Release();
}
