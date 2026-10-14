#pragma once

// Include after main.cpp's Check and InsertTreeItem helpers and ListViewActivationTests.h,
// inside its test namespace.

// A TVS_CHECKBOXES tree (Performance Options' DEP list, a Windows Forms TreeView with CheckBoxes)
// projects its boxes as checkedIndices, and the projection toggles one by the Space key the
// native tree answers: TVN_KEYDOWN, then TVN_ITEMCHANGING/TVN_ITEMCHANGED for the new state.
UINT g_treeCheckKeys = 0;
UINT g_treeCheckChanges = 0;
bool g_vetoTreeCheck = false;

LRESULT CALLBACK TreeCheckOwnerSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_NOTIFY && lParam) {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->code == TVN_KEYDOWN &&
            reinterpret_cast<const NMTVKEYDOWN*>(lParam)->wVKey == VK_SPACE) ++g_treeCheckKeys;
        if (header->code == TVN_ITEMCHANGING) {
            const auto* change = reinterpret_cast<const NMTVITEMCHANGE*>(lParam);
            if ((change->uStateNew ^ change->uStateOld) & TVIS_STATEIMAGEMASK)
                return g_vetoTreeCheck ? TRUE : FALSE;
        }
        if (header->code == TVN_ITEMCHANGED) {
            const auto* change = reinterpret_cast<const NMTVITEMCHANGE*>(lParam);
            if ((change->uStateNew ^ change->uStateOld) & TVIS_STATEIMAGEMASK) ++g_treeCheckChanges;
        }
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TreeCheckOwnerSubclass, subclassId);
    return result;
}

void SetTreeStateImage(HWND tree, HTREEITEM item, UINT image) {
    TVITEMW state{};
    state.mask = TVIF_HANDLE | TVIF_STATE;
    state.hItem = item;
    state.stateMask = TVIS_STATEIMAGEMASK;
    state.state = INDEXTOSTATEIMAGEMASK(image);
    SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&state));
}

void TestTreeViewCheckBoxes() {
    ListActivationCommonControls controls(ICC_TREEVIEW_CLASSES);
    Check(static_cast<bool>(controls), "check box tree v6 activation failed");
    if (!controls) return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } host{ CreateWindowExW(0, WC_DIALOG, L"check-tree", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        60, 60, 380, 300, nullptr, nullptr, instance, nullptr) };
    const HWND tree = host.window ? CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_CHECKBOXES | TVS_SHOWSELALWAYS |
            TVS_DISABLEDRAGDROP | TVS_LINESATROOT,
        12, 12, 340, 230, host.window, reinterpret_cast<HMENU>(14005), instance, nullptr) : nullptr;
    Check(tree != nullptr, "check box tree fixture could not create its control");
    if (!tree) return;
    SetWindowSubclass(host.window, TreeCheckOwnerSubclass, 12, 0);
    const HTREEITEM root = InsertTreeItem(tree, TVI_ROOT, L"Programs");
    const HTREEITEM alpha = InsertTreeItem(tree, root, L"Alpha.exe");
    const HTREEITEM beta = InsertTreeItem(tree, root, L"Beta.exe");
    Check(root && alpha && beta, "check box tree items were not inserted");
    if (!root || !alpha || !beta) return;
    SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(root));
    SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(root));
    ShowWindow(host.window, SW_SHOWNOACTIVATE);
    // A painted tree holds the control's own unchecked/checked state images (a check set before
    // they exist is lost when the control builds them).
    UpdateWindow(host.window);
    const auto stateList = reinterpret_cast<HIMAGELIST>(SendMessageW(tree, TVM_GETIMAGELIST, TVSIL_STATE, 0));
    Check(stateList && ImageList_GetImageCount(stateList) == 3,
        "a painted check box tree did not hold the blank, unchecked and checked state images");
    SetTreeStateImage(tree, beta, 2);

    const auto capture = [&](Translation::WindowSnapshot& snapshot, std::wstring& error) {
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-bcbcbcbcbcbc";
        context.generation = 1;
        context.revision = 1;
        return Translation::CaptureWindow(host.window, context, snapshot, error);
    };
    const auto treeNode = [](const Translation::WindowSnapshot& snapshot) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [](const Translation::ControlNode& candidate) { return candidate.controlId == 14005; });
    };
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = capture(snapshot, error);
    if (!captured) std::wcerr << L"check box tree capture rejection: " << error << L'\n';
    const auto node = treeNode(snapshot);
    if (captured && node != snapshot.nodes.end() && node->checkedIndices != std::vector<int>{ 2 }) {
        std::wcerr << L"check box tree: checkBoxes=" << node->checkBoxes << L" checked=";
        for (const int index : node->checkedIndices) std::wcerr << index << L' ';
        for (const HTREEITEM item : { root, alpha, beta })
            std::wcerr << L" state=" << ((static_cast<UINT>(SendMessageW(tree, TVM_GETITEMSTATE,
                reinterpret_cast<WPARAM>(item), TVIS_STATEIMAGEMASK)) & TVIS_STATEIMAGEMASK) >> 12);
        std::wcerr << L'\n';
    }
    Check(captured && node != snapshot.nodes.end() && node->kind == Translation::ControlKind::TreeView &&
          node->checkBoxes && node->checkedIndices == std::vector<int>{ 2 },
        "a check box tree was not captured with its checked items");
    if (!captured || node == snapshot.nodes.end()) return;
    Check(node->stateImageList.empty() && node->itemStateImages == std::vector<int>{ -1, -1, -1 },
        "a check box tree also published its box glyphs as state images");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"checkBoxes\":true") != std::string::npos &&
          json.find("\"checkedIndices\":[2]") != std::string::npos,
        "a check box tree's boxes were not serialized");

    Translation::ActionRequest action;
    const auto request = [&](int index, bool checked) {
        return "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
            "\"surfaceId\":\"77777777-7777-7777-7777-bcbcbcbcbcbc\",\"nodeId\":\"" +
            std::to_string(node->nodeId) + "\",\"eventId\":\"7\",\"expectedRevision\":\"1\","
            "\"action\":\"setItemCheck\",\"value\":{\"index\":" + std::to_string(index) +
            ",\"checked\":" + (checked ? "true" : "false") + "}}";
    };
    Check(Translation::ParseActionInvoke(request(1, true), L"00112233445566778899aabbccddeeff", action, error) &&
          action.itemIndex == 1 && action.booleanValue &&
          Translation::ValidateActionForSnapshot(action, snapshot, error),
        "a check box tree's toggle was not admitted");
    Check(Translation::ParseActionInvoke(request(3, true), L"00112233445566778899aabbccddeeff", action, error) &&
          !Translation::ValidateActionForSnapshot(action, snapshot, error),
        "a toggle outside the tree's items was admitted");

    // The toggle is the Space key on the item, which becomes the caret first.
    g_treeCheckKeys = g_treeCheckChanges = 0;
    Check(Translation::SetTreeViewItemCheck(tree, 1, true) &&
          Translation::IsTreeViewItemChecked(tree, alpha) &&
          reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0)) == alpha,
        "the Space key did not check the tree item it was sent to");
    Check(g_treeCheckKeys == 1 && g_treeCheckChanges == 1,
        "the owner did not see the key and the state change a user's toggle raises");
    // The space typed with the key is the tree's, not an incremental search.
    Check(reinterpret_cast<HTREEITEM>(SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0)) == alpha,
        "the toggle's character moved the tree's selection");
    Check(Translation::SetTreeViewItemCheck(tree, 2, true) && g_treeCheckKeys == 1,
        "a toggle to the state an item already has pressed a key");
    g_vetoTreeCheck = true;
    Check(!Translation::SetTreeViewItemCheck(tree, 1, false) &&
          Translation::IsTreeViewItemChecked(tree, alpha),
        "a toggle the owner refused was reported as applied");
    g_vetoTreeCheck = false;
    Check(Translation::SetTreeViewItemCheck(tree, 1, false) &&
          !Translation::IsTreeViewItemChecked(tree, alpha),
        "the Space key did not clear the tree item's box");
    Translation::WindowSnapshot after;
    Check(capture(after, error) && treeNode(after) != after.nodes.end() &&
          treeNode(after)->checkedIndices == std::vector<int>{ 2 } && treeNode(after)->selectedIndex == 1,
        "the recapture after a toggle did not carry the new state and the caret");

    // State image 0 hides an item's box: a list of check boxes cannot say that.
    SetTreeStateImage(tree, beta, 0);
    Translation::WindowSnapshot hidden;
    Check(!capture(hidden, error) && error.find(L"hides its check box") != std::wstring::npos,
        "a tree item without its check box was captured");
}
