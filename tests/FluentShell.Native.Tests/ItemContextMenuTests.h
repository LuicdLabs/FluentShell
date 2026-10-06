#pragma once

// TreeView and ListView context menus: a projected right-click selects the native
// item through the control's own messages, sends the keyboard form of
// WM_CONTEXTMENU, and the application's TrackPopupMenu call is held for a WinUI
// answer. The fixture's owner builds a menu naming the item it was asked about,
// which is exactly how MMC derives its context menu from the current selection.

namespace ItemContextMenu {

struct Runtime final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE tracking = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> root{ nullptr };
    std::atomic<HWND> tree{ nullptr };
    std::atomic<HWND> list{ nullptr };
    // MMC tracks its context menus with an owner outside the frame's hierarchy.
    std::atomic<HWND> detachedOwner{ nullptr };
    std::atomic<bool> useDetachedOwner{ false };
    std::atomic<DWORD> thread{ 0 };
    std::atomic<BOOL> result{ FALSE };
    std::atomic<bool> handled{ false };
    std::atomic<bool> keyboardForm{ false };
    std::wstring subject;
    ~Runtime() {
        if (ready) CloseHandle(ready);
        if (tracking) CloseHandle(tracking);
        if (returned) CloseHandle(returned);
    }
};

inline std::wstring SelectedLabel(HWND control) {
    wchar_t buffer[128]{};
    wchar_t className[32]{};
    GetClassNameW(control, className, 32);
    if (wcscmp(className, WC_TREEVIEWW) == 0) {
        TVITEMW item{};
        item.mask = TVIF_HANDLE | TVIF_TEXT;
        item.hItem = reinterpret_cast<HTREEITEM>(SendMessageW(control, TVM_GETNEXTITEM, TVGN_CARET, 0));
        item.pszText = buffer;
        item.cchTextMax = 128;
        if (!item.hItem || !SendMessageW(control, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))) return {};
        return buffer;
    }
    const int index = static_cast<int>(SendMessageW(control, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
    if (index < 0) return {};
    LVITEMW item{};
    item.pszText = buffer;
    item.cchTextMax = 128;
    SendMessageW(control, LVM_GETITEMTEXTW, index, reinterpret_cast<LPARAM>(&item));
    return buffer;
}

// The owner answers WM_CONTEXTMENU the way MMC does for the keyboard form: the menu
// belongs to the control's current selection.
inline LRESULT CALLBACK OwnerSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR data) {
    auto& runtime = *reinterpret_cast<Runtime*>(data);
    if (message == WM_CONTEXTMENU) {
        const HWND source = reinterpret_cast<HWND>(wParam);
        runtime.keyboardForm.store(LOWORD(lParam) == 0xFFFF && HIWORD(lParam) == 0xFFFF);
        runtime.subject = SelectedLabel(source);
        const HMENU popup = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING, 700, (L"Open " + runtime.subject).c_str());
        AppendMenuW(popup, MF_STRING, 701, L"Properties");
        SetEvent(runtime.tracking);
        BOOL result = FALSE;
        const HWND owner = runtime.useDetachedOwner.load() ? runtime.detachedOwner.load() : window;
        runtime.handled.store(Translation::TryTrackIslandPopup(popup, TPM_RETURNCMD, owner, result));
        runtime.result.store(result);
        DestroyMenu(popup);
        SetEvent(runtime.returned);
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

inline HTREEITEM Insert(HWND tree, HTREEITEM parent, const wchar_t* text) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = const_cast<LPWSTR>(text);
    return reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
}

} // namespace ItemContextMenu

void TestItemContextMenuProtocol() {
    constexpr auto nonce = L"00112233445566778899aabbccddeeff";
    std::wstring error;
    Translation::ActionRequest action;
    Check(Translation::ParseActionInvoke(ReadFixture(L"action.invoke.context-menu.json"), nonce, action, error) &&
        action.action == L"contextMenu" && action.nodeId == std::optional<uint64_t>(3) && action.itemIndex == 2 &&
        action.text == L"Leaf" && Translation::IsRequestSemanticAction(L"contextMenu"),
        "contextMenu fixture lost its node, item witness, or revision policy");

    Translation::WindowSnapshot snapshot;
    Translation::ControlNode tree;
    tree.nodeId = 3;
    tree.kind = Translation::ControlKind::TreeView;
    tree.items = { L"Root", L"Child", L"Leaf" };
    snapshot.nodes.push_back(tree);
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "contextMenu was refused for an existing TreeView item");
    action.itemIndex = 3;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "contextMenu accepted an index past the TreeView items");
    action.itemIndex = 1;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "contextMenu accepted an index whose item no longer carries the requested label");
    action.text = L"Child";
    snapshot.nodes[0].kind = Translation::ControlKind::ListBox;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "contextMenu accepted a control without an item context-menu contract");
    snapshot.nodes[0].kind = Translation::ControlKind::ListView;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "contextMenu was refused for an existing ListView item");
    snapshot.nodes[0].adapterId = L"microsoft.windows.directui.semantic.v1";
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "contextMenu accepted a DirectUI-backed list without a comctl32 HWND");
    snapshot.nodes[0].adapterId.clear();
    snapshot.popupMenu = Translation::PopupMenuSnapshot{ 9, 3, 1, {} };
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "a second context menu was accepted while a native popup awaits selection");
}

void TestItemContextMenuTracking() {
    ItemContextMenu::Runtime runtime;
    if (!runtime.ready || !runtime.tracking || !runtime.returned) {
        Check(false, "item context-menu events were not created");
        return;
    }
    std::thread gui([&] {
        runtime.thread.store(GetCurrentThreadId());
        HWND root = CreateWindowExW(0, L"Static", L"item-context-menu",
            WS_OVERLAPPEDWINDOW, 60, 60, 520, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND tree = root ? CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 200, 260, root, reinterpret_cast<HMENU>(810), GetModuleHandleW(nullptr), nullptr) : nullptr;
        HWND list = root ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_REPORT | LVS_SHOWSELALWAYS, 204, 0, 300, 260, root, reinterpret_cast<HMENU>(811),
            GetModuleHandleW(nullptr), nullptr) : nullptr;
        HWND detached = CreateWindowExW(0, L"Static", L"detached-menu-owner", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        runtime.detachedOwner.store(detached);
        if (root && tree && list) {
            const HTREEITEM top = ItemContextMenu::Insert(tree, TVI_ROOT, L"Console Root");
            ItemContextMenu::Insert(tree, top, L"Services");
            ItemContextMenu::Insert(tree, top, L"Event Viewer");
            SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(top));
            SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(top));
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<LPWSTR>(L"Name");
            column.cx = 200;
            SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
            const wchar_t* rows[] = { L"Row A", L"Row B", L"Row C" };
            for (int index = 0; index < 3; ++index) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = index;
                item.pszText = const_cast<LPWSTR>(rows[index]);
                SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            }
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            SetWindowSubclass(root, ItemContextMenu::OwnerSubclass, 0xC7C7, reinterpret_cast<DWORD_PTR>(&runtime));
            ShowWindow(root, SW_SHOWNOACTIVATE);
            runtime.tree.store(tree);
            runtime.list.store(list);
            runtime.root.store(root);
        }
        SetEvent(runtime.ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (root) DestroyWindow(root);
        if (detached) DestroyWindow(detached);
    });

    std::shared_ptr<Translation::SourceThreadAgent> agent;
    const auto cleanup = [&] {
        if (agent) {
            std::wstring ignored;
            agent->Restore(ignored);
            agent->Shutdown();
        }
        if (runtime.thread.load()) PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
        gui.join();
        agent.reset();
    };
    Check(WaitForSingleObject(runtime.ready, 5000) == WAIT_OBJECT_0 && runtime.root.load(),
        "item context-menu GUI thread did not create its controls");
    if (!runtime.root.load()) { cleanup(); return; }
    agent = Translation::SourceThreadAgent::Attach(runtime.root.load(), GetModuleHandleW(nullptr));
    Check(agent != nullptr, "item context-menu agent could not attach");
    if (!agent) { cleanup(); return; }
    std::wstring error;
    Check(agent->SetCloaked(true, error), "item context-menu test could not cloak its native root");
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"31313131-4242-5353-6464-757575757575";
    snapshot.revision = 1;
    if (!agent->Capture(snapshot, error)) {
        std::wcerr << L"item context-menu capture: " << error << L'\n';
        Check(false, "item context-menu fixture was not captured");
        cleanup();
        return;
    }

    // Opens the context menu for one item, then answers it. Returns the label the
    // application built its menu for, or empty on any failure.
    const auto openAndAnswer = [&](Translation::ControlKind kind, int index,
        std::optional<std::wstring> choice, BOOL expectedResult) -> std::wstring {
        ResetEvent(runtime.tracking);
        ResetEvent(runtime.returned);
        const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& candidate) { return candidate.kind == kind; });
        if (node == snapshot.nodes.end()) { std::wcerr << L"context-menu node missing\n"; return {}; }
        const uint64_t nodeId = node->nodeId;
        Translation::ActionRequest action;
        action.surfaceId = snapshot.surfaceId;
        action.expectedRevision = snapshot.revision;
        action.eventId = 40 + index;
        action.nodeId = nodeId;
        action.action = L"contextMenu";
        action.itemIndex = index;
        action.text = static_cast<size_t>(index) < node->items.size() ? node->items[index] : L"";
        Translation::ActionOutcome outcome;
        if (!agent->Invoke(action, outcome) || !outcome.accepted) {
            std::wcerr << L"context-menu invoke: " << outcome.error << L'\n';
            return {};
        }
        snapshot = std::move(outcome.snapshot);
        if (WaitForSingleObject(runtime.tracking, 2000) != WAIT_OBJECT_0) {
            std::wcerr << L"context-menu owner never tracked a popup\n";
            return {};
        }
        if (!agent->Capture(snapshot, error) || !snapshot.popupMenu ||
            snapshot.popupMenu->nodeId != nodeId || snapshot.popupMenu->itemIndex != index ||
            snapshot.popupMenu->items.size() != 2) {
            std::wcerr << L"context-menu popup snapshot: " << error << L'\n';
            return {};
        }
        Translation::ActionRequest answer;
        answer.surfaceId = snapshot.surfaceId;
        answer.expectedRevision = snapshot.revision;
        answer.eventId = 60 + index;
        answer.action = L"popupCommand";
        answer.popupId = snapshot.popupMenu->popupId;
        answer.popupItemId = std::move(choice);
        Translation::ActionOutcome answered;
        if (!agent->Invoke(answer, answered) || !answered.accepted ||
            WaitForSingleObject(runtime.returned, 2000) != WAIT_OBJECT_0 ||
            !runtime.handled.load() || runtime.result.load() != expectedResult ||
            !runtime.keyboardForm.load()) {
            std::wcerr << L"context-menu answer: " << answered.error << L" result=" << runtime.result.load() << L'\n';
            return {};
        }
        snapshot = std::move(answered.snapshot);
        if (!agent->Capture(snapshot, error) || snapshot.popupMenu) return {};
        return runtime.subject;
    };

    // Flattened tree: Console Root, Services, Event Viewer.
    Check(openAndAnswer(Translation::ControlKind::TreeView, 2, std::wstring(L"0"), 700) == L"Event Viewer" &&
        ItemContextMenu::SelectedLabel(runtime.tree.load()) == L"Event Viewer",
        "TreeView context menu did not target the requested item or lost the native command");
    runtime.useDetachedOwner.store(true);
    Check(openAndAnswer(Translation::ControlKind::ListView, 2, std::nullopt, FALSE) == L"Row C" &&
        ListView_GetSelectedCount(runtime.list.load()) == 1 &&
        (ListView_GetItemState(runtime.list.load(), 2, LVIS_SELECTED) & LVIS_SELECTED) != 0,
        "ListView context menu did not select only its row or a dismissal returned a command");
    cleanup();
}
