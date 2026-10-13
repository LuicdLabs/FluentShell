#pragma once

// Include after main.cpp's Check helper and ListViewNotificationSubclass, inside its test namespace.

// A label session the application opened itself (regedit's Rename command, from its menu or a
// context menu) is answered through the projected editor: a new label goes into the control's
// own edit box, the unchanged one cancels the session.  regedit vetoes every session it did not
// open, so opening a second one over it is not an answer.
void TestApplicationLabelSessions() {
    HWND window = CreateWindowExW(0, L"Static", L"label-sessions", WS_OVERLAPPEDWINDOW,
        20, 20, 520, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND list = window ? CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_EDITLABELS, 10, 10, 240, 260, window,
        reinterpret_cast<HMENU>(470), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND tree = window ? CreateWindowExW(0, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_EDITLABELS,
        260, 10, 240, 260, window, reinterpret_cast<HMENU>(471), GetModuleHandleW(nullptr), nullptr)
        : nullptr;
    Check(window && list && tree, "label session test controls were not created");
    if (!window || !list || !tree) {
        if (window) DestroyWindow(window);
        return;
    }
    SetWindowSubclass(window, ListViewNotificationSubclass, 7, 0);
    ShowWindow(window, SW_SHOWNOACTIVATE);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 200;
    column.pszText = const_cast<LPWSTR>(L"Name");
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    for (const wchar_t* text : { L"ValueA", L"BinaryA", L"Third" }) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
        item.pszText = const_cast<LPWSTR>(text);
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }
    HTREEITEM keys[3]{};
    for (int index = 0; index < 3; ++index) {
        static constexpr const wchar_t* kNames[] = { L"Root", L"KeyA", L"KeyB" };
        TVINSERTSTRUCTW insert{};
        insert.hParent = index == 0 ? TVI_ROOT : keys[0];
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT;
        insert.item.pszText = const_cast<LPWSTR>(kNames[index]);
        keys[index] = reinterpret_cast<HTREEITEM>(
            SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
    }
    SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(keys[0]));

    std::wstring label;
    Check(!Translation::LabelSessionOpenOn(list, false, 0),
        "a list without an edit box reported an open label session");
    Check(Translation::ReadItemLabel(list, false, 1, label) && label == L"BinaryA",
        "the live list label was not read");

    // The application's own session, as its Rename command opens it: focus in the list, the
    // focused row edited.
    g_acceptLabelEdit = true;
    g_labelEditsBegun = g_labelEditsEnded = 0;
    SetFocus(list);
    Translation::SetListViewFocusedIndex(list, 0);
    Check(SendMessageW(list, LVM_EDITLABELW, 0, 0) != 0,
        "the list did not open its label session");
    Check(Translation::LabelSessionOpenOn(list, false, 0) &&
          !Translation::LabelSessionOpenOn(list, false, 1),
        "the open list session was not placed on its focused row");
    Check(Translation::CommitOpenLabelEdit(list, false, L"ValueC") &&
          !SendMessageW(list, LVM_GETEDITCONTROL, 0, 0) &&
          Translation::ReadItemLabel(list, false, 0, label) && label == L"ValueC" &&
          g_labelEditsEnded == 1,
        "the projected label did not commit through the list's own edit box");

    // The unchanged label ends the session without a rename.
    Check(SendMessageW(list, LVM_EDITLABELW, 0, 0) != 0 &&
          Translation::LabelSessionOpenOn(list, false, 0),
        "the list did not reopen its label session");
    Translation::CancelOpenLabelEdit(list, false);
    Check(!SendMessageW(list, LVM_GETEDITCONTROL, 0, 0) &&
          Translation::ReadItemLabel(list, false, 0, label) && label == L"ValueC",
        "cancelling the list session renamed the row or left the edit box open");

    // The same for a tree: the session is on the caret item.
    Check(Translation::SelectTreeViewItem(tree, 1), "the tree did not select its key");
    SetFocus(tree);
    Check(SendMessageW(tree, TVM_EDITLABELW, 0, reinterpret_cast<LPARAM>(keys[1])) != 0,
        "the tree did not open its label session");
    Check(Translation::LabelSessionOpenOn(tree, true, 1) &&
          !Translation::LabelSessionOpenOn(tree, true, 2),
        "the open tree session was not placed on its caret item");
    Translation::CancelOpenLabelEdit(tree, true);
    Check(!SendMessageW(tree, TVM_GETEDITCONTROL, 0, 0) &&
          Translation::ReadItemLabel(tree, true, 1, label) && label == L"KeyA",
        "cancelling the tree session renamed the item or left the edit box open");
    Check(SendMessageW(tree, TVM_EDITLABELW, 0, reinterpret_cast<LPARAM>(keys[1])) != 0 &&
          Translation::CommitOpenLabelEdit(tree, true, L"Renamed") &&
          Translation::ReadItemLabel(tree, true, 1, label) && label == L"Renamed",
        "the projected label did not commit through the tree's own edit box");

    DestroyWindow(window);
}

// The user putting the focus in a projected list or tree moves the thread's focus there, as
// the click does natively: menu commands act on the pane that holds it.
void TestFocusAction() {
    constexpr auto nonce = L"00112233445566778899aabbccddeeff";
    std::wstring error;
    Translation::ActionRequest action;
    Check(Translation::ParseActionInvoke(ReadFixture(L"action.invoke.focus.json"), nonce, action, error) &&
          action.action == L"focus" && action.nodeId == std::optional<uint64_t>(2) &&
          action.eventId == 31 && action.expectedRevision == 9 &&
          Translation::IsRequestSemanticAction(L"focus"),
        "the focus fixture lost its node, identity, or request semantics");

    Translation::WindowSnapshot snapshot;
    Translation::ControlNode list;
    list.nodeId = 2;
    list.kind = Translation::ControlKind::ListView;
    snapshot.nodes.push_back(list);
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "focus was refused for a ListView node");
    snapshot.nodes[0].kind = Translation::ControlKind::TreeView;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "focus was refused for a TreeView node");
    snapshot.nodes[0].kind = Translation::ControlKind::Button;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "focus was admitted for a node that is neither a list nor a tree");

    const auto withValue = std::string(ReadFixture(L"action.invoke.focus.json"));
    auto valued = withValue;
    const auto at = valued.find("\"value\": null");
    Check(at != std::string::npos, "the focus fixture no longer carries a null value");
    if (at != std::string::npos) {
        valued.replace(at, 13, "\"value\": 1");
        Translation::ActionRequest refused;
        Check(!Translation::ParseActionInvoke(valued, nonce, refused, error),
            "focus was parsed with a value");
    }
}
