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

    // A drop-down list whose items draw images (recdisc's drives) carries the faces the
    // ComboBoxEx paints, named by the text it stores; its style evidence says owner-drawn.
    const HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
    Check(images && ImageList_AddIcon(images, LoadIconW(nullptr, IDI_APPLICATION)) == 0,
        "ComboBoxEx fixture could not build its image list");
    SendMessageW(dropList, CBEM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    const bool painted = capture(snapshot, error);
    if (!painted) std::wcerr << L"ComboBoxEx image capture rejection: " << error << L'\n';
    const auto* pictured = painted ? comboFor(snapshot, 5013) : nullptr;
    Check(pictured && pictured->ownerDrawCombo && pictured->ownerDrawCombo->items.size() == 2 &&
          pictured->ownerDrawCombo->selectionFace && !pictured->editable &&
          pictured->items == std::vector<std::wstring>{ L"alpha.contoso.test", L"beta" } &&
          pictured->selectedIndex == 1 && (pictured->style & CBS_OWNERDRAWFIXED) != 0 &&
          !pictured->ownerDrawCombo->items[0].normal.imageData.empty(),
        "a ComboBoxEx whose items draw images was not carried as its painted faces");
    // With nothing selected the closed field is blank, not a refusal.
    SendMessageW(dropList, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    Check(capture(snapshot, error) && comboFor(snapshot, 5013) && comboFor(snapshot, 5013)->ownerDrawCombo &&
          comboFor(snapshot, 5013)->ownerDrawCombo->selectionFace && comboFor(snapshot, 5013)->selectedIndex == -1,
        "an owner-drawn ComboBoxEx with nothing selected kept its window native");
    // An editable one with images would need an edit box beside the faces: it stays native.
    SendMessageW(editable, CBEM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    Check(!capture(snapshot, error) && error.find(L"editable ComboBoxEx item images") != std::wstring::npos,
        "an editable ComboBoxEx with item images was projected without them");
    SendMessageW(editable, CBEM_SETIMAGELIST, 0, 0);
    SendMessageW(dropList, CBEM_SETIMAGELIST, 0, 0);
    if (images) ImageList_Destroy(images);
}
