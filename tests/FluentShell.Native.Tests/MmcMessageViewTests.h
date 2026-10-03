#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// MMC's message view (mmcndmgr.dll) is an ATL window whose accessible object is a
// pane with exactly a title, a body, and one graphic; the text travels as each
// static-text child's value, while its name only says which part it is.  The real
// class is registered by mmcndmgr.dll, so this fixture serves the same accessible
// shape from a test window and drives the reader directly.

class TestMessageViewAccessible final : public IAccessible, public IEnumVARIANT {
public:
    struct Part final {
        long role = ROLE_SYSTEM_STATICTEXT;
        std::wstring name;
        std::wstring value;
        std::wstring action;
        RECT rect{};
    };
    HWND view = nullptr;
    long rootRole = ROLE_SYSTEM_PANE;
    std::vector<Part> parts;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IEnumVARIANT) *object = static_cast<IEnumVARIANT*>(this);
        else if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible)
            *object = static_cast<IAccessible*>(this);
        else return E_NOINTERFACE;
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
        *count = static_cast<long>(parts.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        return Text(child, name, [](const Part& part) -> const std::wstring& { return part.name; });
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
        return Text(child, value, [](const Part& part) -> const std::wstring& { return part.value; });
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        role->vt = VT_I4;
        if (child.lVal == CHILDID_SELF) { role->lVal = rootRole; return S_OK; }
        const Part* part = At(child);
        if (!part) return E_INVALIDARG;
        role->lVal = part->role;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT, VARIANT* state) override {
        if (!state) return E_POINTER;
        state->vt = VT_I4;
        state->lVal = STATE_SYSTEM_READONLY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
        return Text(child, action, [](const Part& part) -> const std::wstring& { return part.action; });
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height, VARIANT child) override {
        if (!left || !top || !width || !height) return E_POINTER;
        const Part* part = At(child);
        if (!part) return E_INVALIDARG;
        POINT origin{ part->rect.left, part->rect.top };
        if (!ClientToScreen(view, &origin)) return E_FAIL;
        *left = origin.x;
        *top = origin.y;
        *width = part->rect.right - part->rect.left;
        *height = part->rect.bottom - part->rect.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, VARIANT* values, ULONG* fetched) override {
        if (!values) return E_POINTER;
        ULONG copied = 0;
        while (copied < count && enumIndex_ < parts.size()) {
            VariantInit(&values[copied]);
            values[copied].vt = VT_I4;
            values[copied].lVal = static_cast<LONG>(++enumIndex_);
            ++copied;
        }
        if (fetched) *fetched = copied;
        return copied == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const size_t skipped = std::min<size_t>(count, parts.size() - enumIndex_);
        enumIndex_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { enumIndex_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT**) override { return E_NOTIMPL; }

private:
    const Part* At(const VARIANT& child) const noexcept {
        if (child.vt != VT_I4 || child.lVal < 1 ||
            static_cast<size_t>(child.lVal) > parts.size()) return nullptr;
        return &parts[static_cast<size_t>(child.lVal - 1)];
    }
    template <typename Field>
    HRESULT Text(const VARIANT& child, BSTR* out, Field field) const {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (child.vt == VT_I4 && child.lVal == CHILDID_SELF) return S_FALSE;
        const Part* part = At(child);
        if (!part) return E_INVALIDARG;
        const std::wstring& text = field(*part);
        if (text.empty()) return S_FALSE;
        *out = SysAllocString(text.c_str());
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    std::atomic<ULONG> references_{ 1 };
    size_t enumIndex_ = 0;
};

LRESULT CALLBACK TestMessageViewSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_GETOBJECT && static_cast<LONG>(lParam) == OBJID_CLIENT) {
        return LresultFromObject(IID_IAccessible, wParam,
            static_cast<IAccessible*>(reinterpret_cast<TestMessageViewAccessible*>(data)));
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

// RSoP's "processing" dialog flags its progress bar WS_TABSTOP.  The control takes no
// keyboard input, so it projects and stays out of dialog traversal while the button
// beside it keeps its stop.
void TestTabStopProgressBarLeavesTraversal() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"processing",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 40, 40, 360, 160, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND progress = root ? CreateWindowExW(WS_EX_CLIENTEDGE, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 20, 320, 18, root,
        reinterpret_cast<HMENU>(1001), instance, nullptr) : nullptr;
    const HWND cancel = root ? CreateWindowExW(0, L"Button", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 250, 80, 80, 26, root,
        reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr) : nullptr;
    Check(root && progress && cancel, "tab-stop progress fixture could not create its windows");
    if (!root || !progress || !cancel) return;
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"a1a1a1a1-2b2b-3c3c-4d4d-5e5e5e5e5e5e";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring reason;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"tab-stop progress capture: " << reason << L"\n";
    Check(captured, "a dialog with a tab-stop progress bar was refused");
    const auto find = [&](HWND window) -> const Translation::ControlNode* {
        for (const auto& node : snapshot.nodes) if (node.hwnd == window) return &node;
        return nullptr;
    };
    const auto* bar = find(progress);
    const auto* button = find(cancel);
    Check(bar && bar->kind == Translation::ControlKind::ProgressBar && !bar->tabStop &&
        bar->tabIndex == -1, "a tab-stop progress bar was kept in dialog traversal");
    Check(button && button->tabStop && button->tabIndex >= 0,
        "the button beside a tab-stop progress bar lost its traversal stop");
}

// MMC's description bar is an owner-draw Static under MMC's own view window.  The
// owner-draw refusal is lifted only for that parent when mmc.exe registered it, so a
// look-alike parent class in another image keeps the Static refused.
void TestMmcDescriptionBarIdentity() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"MMCViewWindow";
    const ATOM registered = RegisterClassW(&wc);
    const HWND view = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        30, 30, 400, 200, nullptr, nullptr, instance, nullptr);
    const HWND bar = view ? CreateWindowExW(WS_EX_STATICEDGE, L"Static", L"Console Root",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOPREFIX | SS_ENDELLIPSIS,
        0, 0, 300, 24, view, nullptr, instance, nullptr) : nullptr;
    Check(view && bar, "description bar fixture could not create its windows");
    if (bar) {
        Translation::ControlKind kind{};
        std::wstring reason;
        Check(!Translation::ClassifyControl(bar, kind, reason) &&
            reason.find(L"Static draw style") != std::wstring::npos,
            "an owner-draw Static under a look-alike MMC view was admitted");
        Check(!Translation::WindowClassRegisteredBySystemModule(view, L"mmc.exe"),
            "a test-registered class was attributed to System32 mmc.exe");
    }
    if (view) DestroyWindow(view);
    if (registered) UnregisterClassW(wc.lpszClassName, instance);
}

void TestMmcMessageViewAdapter() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"ATL:00000000DEADBEEF";
    const ATOM registered = RegisterClassW(&wc);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"Static", L"message-view-host", WS_OVERLAPPEDWINDOW,
        30, 30, 520, 260, nullptr, nullptr, instance, nullptr) };
    const HWND view = owner.window ? CreateWindowExW(0, wc.lpszClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 480, 200, owner.window, nullptr, instance, nullptr) : nullptr;
    Check(owner.window && view, "message view fixture could not create its windows");
    if (!owner.window || !view) {
        if (registered) UnregisterClassW(wc.lpszClassName, instance);
        return;
    }
    auto* provider = new TestMessageViewAccessible();
    provider->view = view;
    provider->parts = {
        { ROLE_SYSTEM_STATICTEXT, L"Title", L"No authorization store selected", L"", { 56, 12, 460, 32 } },
        { ROLE_SYSTEM_STATICTEXT, L"Body", L"Open an existing store from the Action menu.", L"", { 56, 40, 460, 120 } },
        { ROLE_SYSTEM_GRAPHIC, L"Icon", L"", L"", { 12, 12, 44, 44 } },
    };
    SetWindowSubclass(view, TestMessageViewSubclass, 0xAA31, reinterpret_cast<DWORD_PTR>(provider));
    ShowWindow(owner.window, SW_SHOWNOACTIVATE);

    // Identity belongs to the module that registered the class, not to its name: a
    // test window with an ATL-shaped class name is not MMC's view.
    Check(!Translation::IsMmcMessageView(view) && !Translation::IsMmcMessageView(owner.window),
        "a window not registered by mmcndmgr.dll was identified as MMC's message view");

    std::vector<Translation::AccessibleIslandItem> items;
    std::wstring reason;
    Check(Translation::ReadMmcMessageView(view, items, reason) && items.size() == 3,
        "the message view's title, body, and graphic were not read");
    if (items.size() == 3) {
        Check(items[0].kind == Translation::AccessibleItemKind::Heading &&
            items[0].name == L"No authorization store selected" && items[0].description == L"Title" &&
            items[0].rect.left == 56 && items[0].rect.top == 12 && items[0].rect.right == 460,
            "the message view title did not travel as a heading with its value and bounds");
        Check(items[1].kind == Translation::AccessibleItemKind::Text &&
            items[1].name == L"Open an existing store from the Action menu." && items[1].actionName.empty(),
            "the message view body did not travel as inert text carrying its value");
        Check(items[2].kind == Translation::AccessibleItemKind::Image && items[2].name == L"Icon" &&
            items[2].rect.right - items[2].rect.left == 32,
            "the message view graphic did not travel as a named image region");
    }

    // An empty body draws nothing, so it is omitted rather than refused.
    provider->parts[1].value.clear();
    Check(Translation::ReadMmcMessageView(view, items, reason) && items.size() == 2 &&
        items[1].kind == Translation::AccessibleItemKind::Image,
        "an empty message view body was not omitted");
    provider->parts[1].value = L"Body text";

    const auto refuses = [&](const char* message) {
        items.clear();
        reason.clear();
        Check(!Translation::ReadMmcMessageView(view, items, reason) && items.empty() &&
            !reason.empty(), message);
    };
    provider->parts[0].value.clear();
    refuses("a message view without a title was admitted");
    provider->parts[0].value = L"Title text";
    provider->parts[1].action = L"Press";
    refuses("an actionable message view part was admitted as inert text");
    provider->parts[1].action.clear();
    provider->parts[2].role = ROLE_SYSTEM_PUSHBUTTON;
    refuses("a message view part outside text and graphic was admitted");
    provider->parts[2].role = ROLE_SYSTEM_GRAPHIC;
    provider->parts.push_back({ ROLE_SYSTEM_STATICTEXT, L"Extra", L"Third text", L"", { 56, 130, 460, 150 } });
    refuses("a message view with a third text part was admitted");
    provider->parts.pop_back();
    provider->parts[2].rect = { 12, 12, 600, 44 };
    refuses("a message view part outside the view was admitted");
    provider->parts[2].rect = { 12, 12, 44, 44 };
    provider->rootRole = ROLE_SYSTEM_CLIENT;
    refuses("a message view whose root is not a pane was admitted");
    provider->rootRole = ROLE_SYSTEM_PANE;

    // An image item carries its pixels through the fingerprint and the wire format.
    Translation::WindowSnapshot snapshot;
    Translation::ControlNode node;
    node.kind = Translation::ControlKind::AccessibleIsland;
    Translation::AccessibleIslandItemSnapshot image;
    image.kind = L"image";
    image.rect = { 12, 12, 13, 13 };
    image.name = L"Icon";
    image.imageWidth = 1;
    image.imageHeight = 1;
    image.imageFormat = L"bgra8-premultiplied";
    image.imageData = { 0x10, 0x20, 0x30, 0xff };
    node.islandItems.push_back(image);
    snapshot.nodes.push_back(node);
    const auto before = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[0].islandItems[0].imageData[0] = 0x11;
    Check(Translation::SnapshotFingerprint(snapshot) != before,
        "a repainted message view graphic did not change the snapshot fingerprint");

    RemoveWindowSubclass(view, TestMessageViewSubclass, 0xAA31);
    provider->Release();
    DestroyWindow(owner.window);
    owner.window = nullptr;
    if (registered) UnregisterClassW(wc.lpszClassName, instance);
}
