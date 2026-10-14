#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// A multi-select list box (LBS_MULTIPLESEL / LBS_EXTENDEDSEL: Performance Monitor's Add Counters
// lists) is admitted and carries its selection as an index list that setSelection can change; the
// styles that change what an item is stay refused.

void TestMultiSelectListBox() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"lists", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        40, 40, 360, 300, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const auto makeList = [&](DWORD style, int top, int id) {
        const HWND list = root ? CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | style,
            10, top, 200, 90, root, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr) : nullptr;
        if (list)
            for (const wchar_t* text : { L"alpha", L"beta", L"gamma", L"delta", L"epsilon" })
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        return list;
    };
    const HWND extended = makeList(LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, 10, 41);
    const HWND toggling = makeList(LBS_MULTIPLESEL | LBS_NOINTEGRALHEIGHT, 110, 42);
    const HWND single = makeList(LBS_NOINTEGRALHEIGHT, 210, 43);
    Check(root && extended && toggling && single, "ListBox fixture could not create its windows");
    if (!root || !extended || !toggling || !single) return;
    ShowWindow(root, SW_SHOWNOACTIVATE);
    SendMessageW(extended, LB_SETSEL, TRUE, 1);
    SendMessageW(extended, LB_SETSEL, TRUE, 3);
    SendMessageW(single, LB_SETCURSEL, 2, 0);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(extended, kind, reason) && kind == Translation::ControlKind::ListBox,
        "an extended-selection ListBox was refused");
    Check(Translation::ClassifyControl(toggling, kind, reason) && kind == Translation::ControlKind::ListBox,
        "a multiple-selection ListBox was refused");

    Translation::CaptureContext context;
    context.surfaceId = L"a1a1a1a1-2b2b-3c3c-4d4d-5e5e5e5e5e5e";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"ListBox capture: " << reason << L"\n";
    const auto find = [&](HWND list) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [list](const auto& candidate) { return candidate.hwnd == list; });
    };
    const auto many = find(extended);
    const auto one = find(single);
    Check(captured && many != snapshot.nodes.end() && one != snapshot.nodes.end(),
        "ListBox fixtures were not captured");
    if (!captured || many == snapshot.nodes.end() || one == snapshot.nodes.end()) return;
    Check(many->multiSelect && many->selectedIndices == std::vector<int>({ 1, 3 }) && many->selectedIndex == 1 &&
          many->items.size() == 5,
        "a multi-select ListBox did not carry its selection as an index list");
    Check(!one->multiSelect && one->selectedIndices.empty() && one->selectedIndex == 2,
        "a single-selection ListBox changed how it carries its selection");

    // The selection travels on the wire and a change of it is a change of the snapshot.
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"selectedIndices\":[1,3]") != std::string::npos &&
          json.find("\"multiSelect\":true") != std::string::npos,
        "a multi-select ListBox's selection was not serialized");
    const auto before = Translation::SnapshotFingerprint(snapshot);
    Check(Translation::SetListBoxSelection(extended, { 0, 2, 4 }),
        "a multi-select ListBox did not take an index list as its selection");
    context.revision = 2;
    Translation::WindowSnapshot after;
    Check(Translation::CaptureWindow(root, context, after, reason) && find(extended) != snapshot.nodes.end(),
        "the ListBox was not recaptured");
    const auto changed = std::find_if(after.nodes.begin(), after.nodes.end(),
        [extended](const auto& candidate) { return candidate.hwnd == extended; });
    Check(changed != after.nodes.end() && changed->selectedIndices == std::vector<int>({ 0, 2, 4 }) &&
          changed->selectedIndex == 0 && Translation::SnapshotFingerprint(after) != before,
        "the new selection was not captured as a change");

    // What the selection action may name: the list's own rows, ascending, on a multi-select box only.
    Translation::ActionRequest action;
    action.action = L"setSelection";
    action.nodeId = many->nodeId;
    action.integerValues = { 0, 4 };
    Check(Translation::ValidateActionForSnapshot(action, snapshot, reason),
        "a multi-select ListBox refused a selection of its own rows");
    action.integerValues = { 0, 5 };
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, reason),
        "a multi-select ListBox accepted a row it does not have");
    action.nodeId = one->nodeId;
    action.integerValues = { 1 };
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, reason),
        "a single-selection ListBox accepted an index-list selection");

    // The control refuses what is not a selection of its own: out-of-range, unordered, or a single list.
    Check(!Translation::SetListBoxSelection(extended, { 5 }) && !Translation::SetListBoxSelection(extended, { 3, 1 }) &&
          !Translation::SetListBoxSelection(single, { 0 }),
        "a list box accepted an unordered, out-of-range or single-selection request");

    // A style that changes what an item is still keeps the window native; an owner-draw list is
    // admitted by style and judged at capture by whether its owner paints its rows.
    const HWND columns = makeList(LBS_MULTICOLUMN | LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL, 10, 44);
    const HWND ownerDraw = makeList(LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL, 10, 45);
    if (columns && ownerDraw) {
        Check(!Translation::ClassifyControl(columns, kind, reason),
            "a multi-column ListBox was admitted");
        Check(Translation::ClassifyControl(ownerDraw, kind, reason) && kind == Translation::ControlKind::ListBox,
            "an owner-draw ListBox was not admitted by style");
    }
}
