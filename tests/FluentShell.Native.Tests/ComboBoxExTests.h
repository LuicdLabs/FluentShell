#pragma once

// Include after main.cpp's Check helper, inside its test namespace.

// A ComboBoxEx (mstsc's computer box) stores and draws its items itself: its inner ComboBox is
// owner-drawn without strings and the typed text is in an edit box of its own.  It projects as
// one comboBox node read from what the ComboBoxEx answers; its inner parts are absorbed.
void TestComboBoxExCapture() {
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_USEREX_CLASSES };
    Check(InitCommonControlsEx(&controls) != FALSE, "ComboBoxEx classes could not be registered");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } host{ CreateWindowExW(0, L"Static", L"combo-box-ex", WS_OVERLAPPEDWINDOW,
        40, 40, 420, 260, nullptr, nullptr, instance, nullptr) };
    const HWND editable = host.window ? CreateWindowExW(0, WC_COMBOBOXEXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN, 10, 10, 260, 200, host.window,
        reinterpret_cast<HMENU>(5012), instance, nullptr) : nullptr;
    const HWND dropList = host.window ? CreateWindowExW(0, WC_COMBOBOXEXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 10, 50, 260, 200, host.window,
        reinterpret_cast<HMENU>(5013), instance, nullptr) : nullptr;
    Check(editable && dropList, "ComboBoxEx fixture could not create its controls");
    if (!editable || !dropList) return;
    for (const HWND combo : { editable, dropList }) {
        for (const wchar_t* text : { L"alpha.contoso.test", L"beta" }) {
            COMBOBOXEXITEMW item{};
            item.mask = CBEIF_TEXT;
            item.iItem = -1;
            item.pszText = const_cast<LPWSTR>(text);
            SendMessageW(combo, CBEM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        }
    }
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(editable, CBEM_GETEDITCONTROL, 0, 0));
    Check(edit && SetWindowTextW(edit, L"typed-host"), "the ComboBoxEx edit box was not reachable");
    SendMessageW(dropList, CB_SETCURSEL, 1, 0);
    ShowWindow(host.window, SW_SHOWNOACTIVATE);

    const auto capture = [&](Translation::WindowSnapshot& snapshot, std::wstring& error) {
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-dddddddddddd";
        context.generation = 1;
        context.revision = 1;
        return Translation::CaptureWindow(host.window, context, snapshot, error);
    };
    const auto comboFor = [](const Translation::WindowSnapshot& snapshot, int controlId) {
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& node) { return node.controlId == controlId; });
        return found == snapshot.nodes.end() ? nullptr : &*found;
    };
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = capture(snapshot, error);
    if (!captured) std::wcerr << L"ComboBoxEx capture rejection: " << error << L'\n';
    Check(captured, "a text-only ComboBoxEx kept its window native");
    if (!captured) return;
    Check(snapshot.nodes.size() == 2 &&
          std::all_of(snapshot.nodes.begin(), snapshot.nodes.end(), [](const Translation::ControlNode& node) {
              return node.kind == Translation::ControlKind::ComboBox;
          }),
        "a ComboBoxEx's inner combo or edit box was captured as a control of its own");
    const auto* typed = comboFor(snapshot, 5012);
    Check(typed && typed->editable && typed->selectedIndex == -1 && typed->text == L"typed-host" &&
          typed->items == std::vector<std::wstring>{ L"alpha.contoso.test", L"beta" },
        "the editable ComboBoxEx lost its items, its typed text, or its empty selection");
    const auto* listed = comboFor(snapshot, 5013);
    Check(listed && !listed->editable && listed->selectedIndex == 1 && listed->text == L"beta" &&
          !listed->ownerDrawCombo,
        "the drop-down list ComboBoxEx lost its selection or was read as an owner-draw combo");

    // Choosing an item puts it in the edit box, as the dropdown does.
    SendMessageW(editable, CB_SETCURSEL, 0, 0);
    Check(capture(snapshot, error) && comboFor(snapshot, 5012) &&
          comboFor(snapshot, 5012)->selectedIndex == 0 && comboFor(snapshot, 5012)->text == L"alpha.contoso.test",
        "the ComboBoxEx selection did not reach the captured text");

    // Item images are not projected: a ComboBoxEx with an image list stays native.
    const HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
    SendMessageW(dropList, CBEM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    Check(!capture(snapshot, error) && error.find(L"ComboBoxEx item images") != std::wstring::npos,
        "a ComboBoxEx with item images was projected without them");
    SendMessageW(dropList, CBEM_SETIMAGELIST, 0, 0);
    if (images) ImageList_Destroy(images);
}
