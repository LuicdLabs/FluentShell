#pragma once

// Include after main.cpp's Check helper, inside its test namespace.

// MMC's Actions pane as its DirectUI provider describes it: section headers that are
// drop-down buttons reporting open or folded, and rows the provider keeps in the tree
// after their section folds -- offscreen, stacked under the header.  Drawing those rows
// put a second button over the header that took its clicks.
class TestSectionPaneAccessible final : public IAccessible {
public:
    struct Element final {
        long role = ROLE_SYSTEM_PUSHBUTTON;
        long state = 0;
        std::wstring name;
        // Client-relative.
        RECT rect{};
    };
    HWND host = nullptr;
    std::vector<Element> elements;
    long lastInvoked = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IAccessible) return E_NOINTERFACE;
        *object = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --references_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { if (count) *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<long>(elements.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        if (!name) return E_POINTER;
        const Element* element = At(child);
        *name = element ? SysAllocString(element->name.c_str()) : SysAllocString(L"Actions");
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        const Element* element = At(child);
        role->vt = VT_I4;
        role->lVal = element ? element->role : ROLE_SYSTEM_PANE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
        if (!state) return E_POINTER;
        const Element* element = At(child);
        state->vt = VT_I4;
        state->lVal = element ? element->state : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
        if (!action) return E_POINTER;
        const Element* element = At(child);
        *action = element ? SysAllocString(element->name.c_str()) : nullptr;
        return element ? (*action ? S_OK : E_OUTOFMEMORY) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height, VARIANT child) override {
        if (!left || !top || !width || !height) return E_POINTER;
        const Element* element = At(child);
        RECT rect{};
        if (element) rect = element->rect;
        else GetClientRect(host, &rect);
        POINT origin{ rect.left, rect.top };
        if (!ClientToScreen(host, &origin)) return E_FAIL;
        *left = origin.x;
        *top = origin.y;
        *width = rect.right - rect.left;
        *height = rect.bottom - rect.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        if (!At(child)) return E_INVALIDARG;
        lastInvoked = child.lVal;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    const Element* At(const VARIANT& child) const {
        if (child.vt != VT_I4 || child.lVal < 1 || static_cast<size_t>(child.lVal) > elements.size())
            return nullptr;
        return &elements[static_cast<size_t>(child.lVal - 1)];
    }
    std::atomic<ULONG> references_{ 1 };
};

LRESULT CALLBACK TestSectionPaneSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_GETOBJECT && static_cast<LONG>(lParam) == OBJID_CLIENT)
        return LresultFromObject(IID_IAccessible, wParam,
            static_cast<IAccessible*>(reinterpret_cast<TestSectionPaneAccessible*>(data)));
    return DefSubclassProc(window, message, wParam, lParam);
}

void TestActionsPaneSections() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"Static", L"actions-pane-host", WS_OVERLAPPEDWINDOW,
        40, 40, 260, 300, nullptr, nullptr, instance, nullptr) };
    const HWND pane = owner.window ? CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE,
        0, 0, 186, 240, owner.window, reinterpret_cast<HMENU>(730), instance, nullptr) : nullptr;
    Check(pane != nullptr, "Actions pane fixture could not create its windows");
    if (!pane) return;
    auto* provider = new TestSectionPaneAccessible();
    provider->host = pane;
    using Element = TestSectionPaneAccessible::Element;
    const Element header{ ROLE_SYSTEM_BUTTONDROPDOWN, STATE_SYSTEM_EXPANDED, L"Computer Management (Local)", { 0, 0, 186, 25 } };
    const Element more{ ROLE_SYSTEM_MENUPOPUP, 0, L"More Actions", { 0, 25, 186, 49 } };
    provider->elements = { header, more };
    SetWindowSubclass(pane, TestSectionPaneSubclass, 0xAA30, reinterpret_cast<DWORD_PTR>(provider));
    ShowWindow(owner.window, SW_SHOWNOACTIVATE);

    std::vector<Translation::AccessibleIslandItem> items;
    std::wstring reason;
    Check(Translation::ReadAccessibleIslandItems(pane, items, reason) && items.size() == 2 &&
        items[0].kind == Translation::AccessibleItemKind::Button && !items[0].dropDown &&
        items[0].expanded == std::optional<bool>(true) &&
        items[1].dropDown && !items[1].expanded.has_value(),
        "an open section header was not a fold toggle, or its menu row lost its menu");

    // Folded: the provider keeps the row, offscreen, stacked under the header.
    provider->elements[0].state = STATE_SYSTEM_COLLAPSED;
    provider->elements[1].state = STATE_SYSTEM_OFFSCREEN;
    provider->elements[1].rect = { 0, 2, 186, 26 };
    Check(Translation::ReadAccessibleIslandItems(pane, items, reason) && items.size() == 1 &&
        items[0].name == header.name && items[0].expanded == std::optional<bool>(false),
        "a row folded under its section header was still projected over the header");
    Check(Translation::InvokeAccessibleIslandItem(pane, 0, header.name, header.name, reason) &&
        provider->lastInvoked == 1,
        "the folded pane's item index did not address the section header");

    // Scrolled out of view: offscreen too, but outside the pane, so the projection keeps it
    // and scrolls to it itself.
    provider->elements[1].rect = { 0, 260, 186, 284 };
    Check(Translation::ReadAccessibleIslandItems(pane, items, reason) && items.size() == 2 &&
        items[1].rect.top == 260,
        "a row scrolled out of the pane was dropped instead of scrolled to");

    Translation::WindowSnapshot snapshot;
    Translation::ControlNode node;
    node.nodeId = 1;
    node.generation = 1;
    node.kind = Translation::ControlKind::AccessibleIsland;
    Translation::AccessibleIslandItemSnapshot published;
    published.kind = L"button";
    published.name = header.name;
    published.actionName = header.name;
    published.rect = header.rect;
    published.expanded = false;
    node.islandItems.push_back(published);
    snapshot.nodes.push_back(node);
    const auto folded = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[0].islandItems[0].expanded = true;
    Check(folded != Translation::SnapshotFingerprint(snapshot),
        "folding a section did not change the capture fingerprint");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"expanded\":true") != std::string::npos,
        "a section header's fold state was not serialized");

    RemoveWindowSubclass(pane, TestSectionPaneSubclass, 0xAA30);
    provider->Release();
}

// The title-bar close button: a window closes from it unless it has no system menu, its
// class says CS_NOCLOSE, or the application greyed SC_CLOSE (which DefWindowProc honours
// for Alt+F4 too).  Escape is a different contract (canCancel).
void TestTitleBarCloseCapture() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto capture = [&](HWND window, Translation::WindowSnapshot& snapshot) {
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-cccccccccccc";
        context.generation = 1;
        context.revision = 1;
        std::wstring error;
        const bool captured = Translation::CaptureWindow(window, context, snapshot, error);
        if (!captured) std::wcerr << L"close capture rejection: " << error << L'\n';
        return captured;
    };
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } closable{ CreateWindowExW(0, L"Static", L"closable", WS_OVERLAPPEDWINDOW,
        30, 30, 240, 160, nullptr, nullptr, instance, nullptr) },
      noMenu{ CreateWindowExW(0, L"Static", L"no-system-menu", WS_POPUP | WS_CAPTION,
        60, 60, 240, 160, nullptr, nullptr, instance, nullptr) };
    Check(closable.window && noMenu.window, "title-bar close fixture could not create its windows");
    if (!closable.window || !noMenu.window) return;
    ShowWindow(closable.window, SW_SHOWNOACTIVATE);
    ShowWindow(noMenu.window, SW_SHOWNOACTIVATE);
    Translation::WindowSnapshot snapshot;
    Check(capture(closable.window, snapshot) && snapshot.canClose && !snapshot.canCancel,
        "an ordinary window lost its close button or gained Escape-to-close");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"canClose\":true") != std::string::npos, "canClose was not serialized");
    const auto open = Translation::SnapshotFingerprint(snapshot);
    EnableMenuItem(GetSystemMenu(closable.window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    Check(capture(closable.window, snapshot) && !snapshot.canClose &&
        Translation::SnapshotFingerprint(snapshot) != open,
        "a greyed SC_CLOSE still offered the close button");
    Check(capture(noMenu.window, snapshot) && !snapshot.canClose,
        "a window without a system menu offered a close button");
}

// A command link (BS_COMMANDLINK) is a push button with a note under its label.  The
// common-controls v6 button draws it; the adapter carries the label, the note and which
// one is the default.
void TestCommandLinkButtons() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"Static", L"command-links", WS_OVERLAPPEDWINDOW,
        30, 30, 380, 260, nullptr, nullptr, instance, nullptr) };
    if (!owner.window) {
        Check(false, "command link fixture root was not created");
        return;
    }
    const HWND restart = CreateWindowExW(0, L"Button", L"Restart now",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFCOMMANDLINK, 10, 10, 340, 58,
        owner.window, reinterpret_cast<HMENU>(740), instance, nullptr);
    const HWND later = CreateWindowExW(0, L"Button", L"Check next time",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_COMMANDLINK, 10, 80, 340, 41,
        owner.window, reinterpret_cast<HMENU>(741), instance, nullptr);
    Check(restart && later, "command link buttons were not created");
    if (!restart || !later) return;
    SendMessageW(restart, BCM_SETNOTE, 0, reinterpret_cast<LPARAM>(L"Save your work first."));
    ShowWindow(owner.window, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-dddddddddddd";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(owner.window, context, snapshot, error);
    if (!captured) std::wcerr << L"command link capture rejection: " << error << L'\n';
    Check(captured && snapshot.nodes.size() == 2, "a dialog with command links was refused");
    if (!captured || snapshot.nodes.size() != 2) return;
    const auto& first = snapshot.nodes[0].hwnd == restart ? snapshot.nodes[0] : snapshot.nodes[1];
    const auto& second = snapshot.nodes[0].hwnd == restart ? snapshot.nodes[1] : snapshot.nodes[0];
    Check(first.kind == Translation::ControlKind::Button && first.presentationVariant == L"commandLink" &&
        first.isDefault && first.text == L"Restart now" && first.helpText == L"Save your work first." &&
        second.presentationVariant == L"commandLink" && !second.isDefault && second.helpText.empty(),
        "a command link lost its label, note or default state");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"presentationVariant\":\"commandLink\"") != std::string::npos &&
        json.find("Save your work first.") != std::string::npos,
        "a command link was serialized without its presentation or note");
}
