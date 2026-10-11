#pragma once

// Windows Forms Labels and Buttons (FlatStyle.Standard) are SS_OWNERDRAW and
// BS_OWNERDRAW at the Win32 level; their kind and check state come from the MSAA
// object WinForms publishes for the window. The fixture superclasses STATIC and
// BUTTON under WinForms-shaped class names and answers WM_GETOBJECT the same way.

namespace WinFormsOwnerDraw {

class StubAccessible final : public IAccessible {
public:
    long role = ROLE_SYSTEM_STATICTEXT;
    long state = 0;
    std::wstring name;
    std::wstring defaultAction;
    int invocations = 0;
    // Owned references, published as full child objects like WinForms' LinkLabel links.
    std::vector<StubAccessible*> children;

    ~StubAccessible() {
        for (StubAccessible* child : children) child->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IAccessible) {
            *object = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override { if (parent) *parent = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (count) *count = static_cast<long>(children.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT id, IDispatch** child) override {
        if (!child) return E_POINTER;
        *child = nullptr;
        if (id.vt != VT_I4 || id.lVal < 1 || static_cast<size_t>(id.lVal) > children.size())
            return E_INVALIDARG;
        StubAccessible* entry = children[static_cast<size_t>(id.lVal - 1)];
        entry->AddRef();
        *child = static_cast<IAccessible*>(entry);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT, BSTR* value) override {
        if (!value) return E_POINTER;
        *value = name.empty() ? nullptr : SysAllocString(name.c_str());
        return name.empty() ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR* value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* text) override { if (text) *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* value) override {
        if (!value || child.vt != VT_I4 || child.lVal != CHILDID_SELF) return E_INVALIDARG;
        value->vt = VT_I4;
        value->lVal = role;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* value) override {
        if (!value || child.vt != VT_I4 || child.lVal != CHILDID_SELF) return E_INVALIDARG;
        value->vt = VT_I4;
        value->lVal = state;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* help) override { if (help) *help = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* key) override { if (key) *key = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override { if (focus) focus->vt = VT_EMPTY; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override { if (selection) selection->vt = VT_EMPTY; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT, BSTR* action) override {
        if (!action) return E_POINTER;
        *action = defaultAction.empty() ? nullptr : SysAllocString(defaultAction.c_str());
        return defaultAction.empty() ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long*, long*, long*, long*, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT id) override {
        if (id.vt != VT_I4 || id.lVal != CHILDID_SELF || defaultAction.empty()) return DISP_E_MEMBERNOTFOUND;
        ++invocations;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> references_{ 1 };
};

// Answers WM_GETOBJECT with the stub and, like WinForms' CheckBox/RadioButton
// OnClick, moves the check state when a click completes.
inline LRESULT CALLBACK Subclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR data) {
    auto* accessible = reinterpret_cast<StubAccessible*>(data);
    if (message == WM_GETOBJECT && static_cast<long>(lParam) == OBJID_CLIENT)
        return LresultFromObject(IID_IAccessible, wParam, accessible);
    if (message == WM_LBUTTONUP) {
        if (accessible->role == ROLE_SYSTEM_CHECKBUTTON) accessible->state ^= STATE_SYSTEM_CHECKED;
        if (accessible->role == ROLE_SYSTEM_RADIOBUTTON) accessible->state |= STATE_SYSTEM_CHECKED;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, Subclass, 0xF0F0);
        accessible->Release();
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

inline bool Superclass(const wchar_t* base, const wchar_t* name) {
    WNDCLASSEXW info{ sizeof(info) };
    if (!GetClassInfoExW(nullptr, base, &info)) return false;
    info.hInstance = GetModuleHandleW(nullptr);
    info.lpszClassName = name;
    return RegisterClassExW(&info) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

inline HWND Create(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style,
    int y, long role, long state, int height = 20, StubAccessible** created = nullptr, int width = 200) {
    const HWND window = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
        10, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return nullptr;
    auto* accessible = new StubAccessible();
    accessible->role = role;
    accessible->state = state;
    if (created) *created = accessible;
    SetWindowSubclass(window, Subclass, 0xF0F0, reinterpret_cast<DWORD_PTR>(accessible));
    return window;
}

} // namespace WinFormsOwnerDraw

void TestWinFormsOwnerDrawControls() {
    constexpr auto staticClass = L"WindowsForms10.STATIC.app.0.fluentshell_test";
    constexpr auto buttonClass = L"WindowsForms10.BUTTON.app.0.fluentshell_test";
    const bool registered = WinFormsOwnerDraw::Superclass(L"STATIC", staticClass) &&
        WinFormsOwnerDraw::Superclass(L"BUTTON", buttonClass);
    Check(registered, "WinForms-shaped fixture classes were not registered");
    if (!registered) return;
    const HWND root = CreateWindowExW(0, L"Static", L"winforms-owner-draw", WS_OVERLAPPEDWINDOW,
        30, 30, 260, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND label = WinFormsOwnerDraw::Create(root, staticClass, L"Run whether user is logged on",
        SS_OWNERDRAW, 10, ROLE_SYSTEM_STATICTEXT, 0);
    const HWND check = WinFormsOwnerDraw::Create(root, buttonClass, L"", BS_OWNERDRAW | WS_TABSTOP,
        40, ROLE_SYSTEM_CHECKBUTTON, 0);
    const HWND radio = WinFormsOwnerDraw::Create(root, buttonClass, L"", BS_OWNERDRAW | WS_TABSTOP,
        70, ROLE_SYSTEM_RADIOBUTTON, STATE_SYSTEM_CHECKED);
    const HWND push = WinFormsOwnerDraw::Create(root, buttonClass, L"Refresh", BS_OWNERDRAW | WS_TABSTOP,
        100, ROLE_SYSTEM_PUSHBUTTON, STATE_SYSTEM_DEFAULT);
    const HWND link = WinFormsOwnerDraw::Create(root, staticClass, L"Open", SS_OWNERDRAW,
        130, ROLE_SYSTEM_LINK, 0);
    const HWND plainOwnerDraw = CreateWindowExW(0, L"Static", L"Painted", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        10, 160, 200, 20, root, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!root || !label || !check || !radio || !push || !link || !plainOwnerDraw) {
        Check(false, "WinForms-shaped fixture controls were not created");
        if (root) DestroyWindow(root);
        return;
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);
    const auto classify = [](HWND hwnd, Translation::ControlKind& kind) {
        std::wstring reason;
        return Translation::ClassifyControl(hwnd, kind, reason);
    };
    Translation::ControlKind kind{};
    Check(classify(label, kind) && kind == Translation::ControlKind::StaticText,
        "owner-draw WinForms Label was not admitted as its text");
    Check(classify(check, kind) && kind == Translation::ControlKind::CheckBox,
        "owner-draw WinForms CheckBox was not classified by its accessible role");
    Check(classify(radio, kind) && kind == Translation::ControlKind::RadioButton,
        "owner-draw WinForms RadioButton was not classified by its accessible role");
    Check(classify(push, kind) && kind == Translation::ControlKind::Button,
        "owner-draw WinForms Button was not classified by its accessible role");
    Check(!classify(link, kind), "an owner-draw WinForms static with a link role was admitted as plain text");
    Check(!classify(plainOwnerDraw, kind), "a non-WinForms owner-draw Static lost its refusal");

    // A real LinkLabel answers with the static-text role and publishes its link as an
    // accessible child; one link is the bounded SysLink contract, activated through the
    // link's own default action.
    WinFormsOwnerDraw::StubAccessible* linkLabelObject = nullptr;
    const HWND linkLabel = WinFormsOwnerDraw::Create(root, staticClass, L"Open the firewall properties",
        SS_OWNERDRAW | WS_TABSTOP, 150, ROLE_SYSTEM_STATICTEXT, STATE_SYSTEM_READONLY, 20, &linkLabelObject);
    WinFormsOwnerDraw::StubAccessible* linkObject = nullptr;
    if (linkLabelObject) {
        linkObject = new WinFormsOwnerDraw::StubAccessible();
        linkObject->role = ROLE_SYSTEM_LINK;
        linkObject->state = STATE_SYSTEM_FOCUSABLE;
        linkObject->name = L"firewall properties";
        linkObject->defaultAction = L"Click";
        linkLabelObject->children.push_back(linkObject);
    }
    Check(linkLabel && linkObject && classify(linkLabel, kind) && kind == Translation::ControlKind::SysLink,
        "a WinForms LinkLabel with one accessible link was not admitted as a SysLink");
    if (linkLabel && linkObject) {
        Translation::ControlNode linkNode;
        linkNode.kind = Translation::ControlKind::SysLink;
        linkNode.hwnd = linkLabel;
        linkNode.text = L"Open the firewall properties";
        linkNode.enabled = true;
        std::wstring linkError;
        Check(Translation::CaptureControlDetail(linkLabel, linkNode, linkError) &&
            linkNode.items.size() == 1 && linkNode.items[0] == L"firewall properties" &&
            linkNode.text == L"Open the firewall properties" && linkNode.enabled,
            "a WinForms LinkLabel did not capture its one link run");
        Check(Translation::IsWindowsFormsLinkLabelWindow(linkLabel) &&
            !Translation::InvokeWindowsFormsLink(linkLabel, L"another link") && linkObject->invocations == 0 &&
            Translation::InvokeWindowsFormsLink(linkLabel, L"firewall properties") && linkObject->invocations == 1,
            "a WinForms link was not activated exactly through its own accessible default action");
        // A second link leaves the bounded one-link contract.
        auto* second = new WinFormsOwnerDraw::StubAccessible();
        second->role = ROLE_SYSTEM_LINK;
        second->name = L"Open";
        second->defaultAction = L"Click";
        linkLabelObject->children.push_back(second);
        Check(!classify(linkLabel, kind) && !Translation::InvokeWindowsFormsLink(linkLabel, L"firewall properties"),
            "a WinForms LinkLabel with two links was admitted as a one-link SysLink");
        DestroyWindow(linkLabel);
    }

    Check(Translation::ReadButtonCheckState(check) == BST_UNCHECKED &&
        Translation::ReadButtonCheckState(radio) == BST_CHECKED,
        "owner-draw WinForms toggle state was not read from MSAA");
    SendMessageW(check, BM_CLICK, 0, 0);
    Check(Translation::ReadButtonCheckState(check) == BST_CHECKED,
        "BM_CLICK did not reach the owner-draw WinForms toggle's click handler");
    Check(Translation::ReadButtonCheckState(push) == -1,
        "an owner-draw push button reported a check state");

    // A leaf WinForms surface (Panel/PictureBox): inert painted pixels when it offers no
    // action (WinForms reports every enabled control as focusable).  A tab stop admits
    // only a header strip that publishes no action; a taller focusable surface or one
    // with an action keeps its refusal.
    constexpr auto panelClass = L"WindowsForms10.Window.8.app.0.fluentshell_test";
    {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = panelClass;
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_HIGHLIGHT + 1);
        RegisterClassW(&type);
    }
    const HWND strip = WinFormsOwnerDraw::Create(root, panelClass, L"", 0, 190, ROLE_SYSTEM_CLIENT,
        STATE_SYSTEM_FOCUSABLE);
    const HWND focusable = WinFormsOwnerDraw::Create(root, panelClass, L"", WS_TABSTOP, 210,
        ROLE_SYSTEM_CLIENT, STATE_SYSTEM_FOCUSABLE, 40);
    const HWND headerStrip = WinFormsOwnerDraw::Create(root, panelClass, L"", WS_TABSTOP, 150,
        ROLE_SYSTEM_CLIENT, STATE_SYSTEM_FOCUSABLE, 24);
    WinFormsOwnerDraw::StubAccessible* toggleObject = nullptr;
    const HWND toggleStrip = WinFormsOwnerDraw::Create(root, panelClass, L"", WS_TABSTOP, 120,
        ROLE_SYSTEM_CLIENT, STATE_SYSTEM_FOCUSABLE, 24, &toggleObject);
    if (toggleObject) toggleObject->defaultAction = L"Collapse";
    Check(strip && classify(strip, kind) && kind == Translation::ControlKind::StaticBitmap,
        "an inert leaf WinForms surface was not admitted as its painted pixels");
    // A Panel answers as a grouping: the 1-pixel rules a wizard draws between its header, page and
    // buttons are inert painted surfaces too.
    const HWND rule = WinFormsOwnerDraw::Create(root, panelClass, L"", 0, 236, ROLE_SYSTEM_GROUPING,
        STATE_SYSTEM_FOCUSABLE, 1, nullptr, 120);
    Check(rule && classify(rule, kind) && kind == Translation::ControlKind::StaticBitmap,
        "a 1-pixel WinForms Panel rule was not admitted as its painted pixels");
    if (rule) DestroyWindow(rule);
    // A rule runs the whole height of a page, taller than the surface cap allows a box.
    const HWND divider = WinFormsOwnerDraw::Create(root, panelClass, L"", 0, 4, ROLE_SYSTEM_GROUPING,
        STATE_SYSTEM_FOCUSABLE, 406, nullptr, 1);
    Check(divider && classify(divider, kind) && kind == Translation::ControlKind::StaticBitmap,
        "a full-height 1-pixel WinForms Panel rule was refused by the surface cap");
    if (divider) DestroyWindow(divider);
    Check(focusable && !classify(focusable, kind),
        "a tall tab-stop WinForms surface was admitted as inert pixels");
    Check(headerStrip && classify(headerStrip, kind) && kind == Translation::ControlKind::StaticBitmap,
        "an actionless focusable WinForms header strip was not admitted as its painted pixels");
    Check(toggleStrip && !classify(toggleStrip, kind),
        "a focusable WinForms strip with an accessible action was admitted as inert pixels");
    if (headerStrip) DestroyWindow(headerStrip);
    if (toggleStrip) DestroyWindow(toggleStrip);

    // Symbol-font text is glyphs, not letters: a Marlett label projects as its pixels,
    // and a picture button (no text, or a Marlett glyph) carries its painted face.
    const HFONT marlett = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SYMBOL_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Marlett");
    const HWND chevron = WinFormsOwnerDraw::Create(root, staticClass, L"5", SS_OWNERDRAW,
        150, ROLE_SYSTEM_STATICTEXT, 0, 14);
    const HWND close = WinFormsOwnerDraw::Create(root, buttonClass, L"r", BS_OWNERDRAW | WS_TABSTOP,
        120, ROLE_SYSTEM_PUSHBUTTON, 0, 19, nullptr, 19);
    const HWND collapse = WinFormsOwnerDraw::Create(root, buttonClass, L"", BS_OWNERDRAW | WS_TABSTOP,
        150, ROLE_SYSTEM_PUSHBUTTON, 0, 22, nullptr, 19);
    if (marlett && chevron && close) {
        SendMessageW(chevron, WM_SETFONT, reinterpret_cast<WPARAM>(marlett), FALSE);
        SendMessageW(close, WM_SETFONT, reinterpret_cast<WPARAM>(marlett), FALSE);
    }
    Check(marlett && chevron && classify(chevron, kind) && kind == Translation::ControlKind::StaticBitmap,
        "a Marlett WinForms label was projected as letters instead of its glyphs");
    const auto captureAs = [](HWND hwnd, Translation::ControlKind captureKind, const wchar_t* text,
            Translation::ControlNode& node) {
        node = {};
        node.kind = captureKind;
        node.hwnd = hwnd;
        node.text = text;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        std::wstring captureError;
        return Translation::CaptureControlDetail(hwnd, node, captureError);
    };
    Translation::ControlNode face;
    Check(chevron && captureAs(chevron, Translation::ControlKind::StaticBitmap, L"5", face) &&
        face.imageWidth == 200 && face.imageHeight == 14 && face.imageFormat == L"bgra8-premultiplied",
        "a Marlett WinForms label did not capture its painted glyphs");
    Check(close && captureAs(close, Translation::ControlKind::Button, L"r", face) &&
        face.imageWidth == 19 && face.imageHeight == 19 && !face.imageData.empty(),
        "a Marlett WinForms button did not carry its painted face as its picture");
    Check(collapse && captureAs(collapse, Translation::ControlKind::Button, L"", face) &&
        face.imageHeight == 22 && !face.imageData.empty(),
        "a textless WinForms picture button did not carry its painted face");
    Translation::ControlNode plainButton;
    Check(captureAs(push, Translation::ControlKind::Button, L"Refresh", plainButton) &&
        plainButton.imageData.empty(),
        "a WinForms text button was captured as a picture");
    if (chevron) DestroyWindow(chevron);
    if (close) DestroyWindow(close);
    if (collapse) DestroyWindow(collapse);
    if (marlett) DeleteObject(marlett);
    if (strip) {
        Translation::ControlNode pixels;
        pixels.kind = Translation::ControlKind::StaticBitmap;
        pixels.hwnd = strip;
        std::wstring pixelError;
        Check(Translation::CaptureControlDetail(strip, pixels, pixelError) &&
            pixels.imageWidth == 200 && pixels.imageHeight == 20 && !pixels.imageData.empty(),
            "an inert WinForms surface did not capture its own client pixels");
    }
    if (focusable) DestroyWindow(focusable);

    Translation::CaptureContext context;
    context.surfaceId = L"51515151-6262-7373-8484-959595959595";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    DestroyWindow(link);
    DestroyWindow(plainOwnerDraw);
    const bool captured = Translation::CaptureWindow(root, context, snapshot, error);
    if (!captured) std::wcerr << L"WinForms owner-draw capture: " << error << L'\n';
    const auto find = [&](HWND hwnd) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& node) { return node.hwnd == hwnd; });
    };
    Check(captured && find(label) != snapshot.nodes.end() &&
        find(label)->text == L"Run whether user is logged on" &&
        find(check) != snapshot.nodes.end() && find(check)->checked == BST_CHECKED &&
        find(radio) != snapshot.nodes.end() && find(radio)->checked == BST_CHECKED &&
        find(push) != snapshot.nodes.end() && find(push)->isDefault,
        "owner-draw WinForms controls lost their text, check state, or default role in capture");
    DestroyWindow(root);
}

namespace WinFormsSpin {

struct Buddy final { HWND edit = nullptr; };

// Steps like UpDownBase.UpDownButtons: the press of the upper half adds one to the number in
// the buddy edit, the lower half takes one away.
inline LRESULT CALLBACK Subclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR data) {
    auto* buddy = reinterpret_cast<Buddy*>(data);
    if (message == WM_LBUTTONDOWN) {
        RECT client{};
        GetClientRect(window, &client);
        wchar_t text[32]{};
        GetWindowTextW(buddy->edit, text, 32);
        const int value = _wtoi(text) + (static_cast<short>(HIWORD(lParam)) < client.bottom / 2 ? 1 : -1);
        SetWindowTextW(buddy->edit, std::to_wstring(value).c_str());
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Subclass, 0xF0F1);
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace WinFormsSpin

// A Windows Forms NumericUpDown: a container of an edit that holds the number and a small leaf
// that paints two arrows. The leaf is an accessible spin box with an up and a down push button
// and steps on a press of its upper or lower half; the projection carries it as an UpDown whose
// position is the number in the edit.
void TestWindowsFormsSpinButtons() {
    constexpr auto paneClass = L"WindowsForms10.Window.8.app.0.fluentshell_test";
    constexpr auto editClass = L"WindowsForms10.EDIT.app.0.fluentshell_test";
    {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = paneClass;
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&type);
    }
    Check(WinFormsOwnerDraw::Superclass(L"EDIT", editClass), "spin fixture edit class was not registered");
    const HWND root = CreateWindowExW(0, L"Static", L"winforms-spin", WS_OVERLAPPEDWINDOW,
        30, 30, 260, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND numeric = root ? CreateWindowExW(0, paneClass, L"", WS_CHILD | WS_VISIBLE,
        10, 10, 145, 23, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND edit = numeric ? CreateWindowExW(0, editClass, L"20480", WS_CHILD | WS_VISIBLE,
        2, 2, 125, 19, numeric, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND buttons = numeric ? CreateWindowExW(0, paneClass, L"", WS_CHILD | WS_VISIBLE,
        128, 1, 16, 21, numeric, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && numeric && edit && buttons, "spin fixture windows were not created");
    if (!root || !numeric || !edit || !buttons) {
        if (root) DestroyWindow(root);
        return;
    }
    auto* accessible = new WinFormsOwnerDraw::StubAccessible();
    accessible->role = ROLE_SYSTEM_SPINBUTTON;
    accessible->name = L"CustomNumericUpDown";
    for (const wchar_t* name : { L"Up", L"Down" }) {
        auto* button = new WinFormsOwnerDraw::StubAccessible();
        button->role = ROLE_SYSTEM_PUSHBUTTON;
        button->name = name;
        accessible->children.push_back(button);
    }
    SetWindowSubclass(buttons, WinFormsOwnerDraw::Subclass, 0xF0F0, reinterpret_cast<DWORD_PTR>(accessible));
    WinFormsSpin::Buddy buddy{ edit };
    SetWindowSubclass(buttons, WinFormsSpin::Subclass, 0xF0F1, reinterpret_cast<DWORD_PTR>(&buddy));
    ShowWindow(root, SW_SHOWNOACTIVATE);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(buttons, kind, reason) && kind == Translation::ControlKind::UpDown,
        "the arrows of a Windows Forms NumericUpDown were not admitted as an UpDown");
    if (kind != Translation::ControlKind::UpDown) std::wcerr << L"spin buttons: " << reason << L'\n';

    // The number beside the arrows is the position; both ends are open.
    Translation::ControlNode node;
    node.kind = Translation::ControlKind::UpDown;
    node.hwnd = buttons;
    node.style = static_cast<uint64_t>(GetWindowLongPtrW(buttons, GWL_STYLE));
    reason.clear();
    Check(Translation::CaptureControlDetail(buttons, node, reason) && node.position == 20480 &&
        node.minimum == (std::numeric_limits<int>::min)() && node.maximum == (std::numeric_limits<int>::max)() &&
        node.smallChange == 1 && node.vertical &&
        !node.reversed, "a spin pair did not capture the number beside it as its position");

    // A press steps the control by its own increment, whatever that is.
    Check(Translation::StepWindowsFormsSpin(buttons, true), "a spin step up was refused");
    wchar_t text[32]{};
    GetWindowTextW(edit, text, 32);
    Check(std::wstring(text) == L"20481", "the upper half did not step the number up");
    Check(Translation::StepWindowsFormsSpin(buttons, false) && Translation::StepWindowsFormsSpin(buttons, false),
        "a spin step down was refused");
    GetWindowTextW(edit, text, 32);
    Check(std::wstring(text) == L"20479", "the lower half did not step the number down");

    // A grouped number is still an integer; anything else is not one the arrows can state.
    SetWindowTextW(edit, L"1,024");
    Translation::WindowsFormsSpin spin;
    Check(Translation::ReadWindowsFormsSpin(buttons, spin, reason) && spin.value == 1024 && spin.edit == edit,
        "a number with a thousands separator was not read");
    SetWindowTextW(edit, L"-12");
    Check(Translation::ReadWindowsFormsSpin(buttons, spin, reason) && spin.value == -12, "a negative number was not read");
    SetWindowTextW(edit, L"2.5");
    reason.clear();
    Check(!Translation::ReadWindowsFormsSpin(buttons, spin, reason) && reason.find(L"integer") != std::wstring::npos &&
        !Translation::ClassifyControl(buttons, kind, reason), "a decimal number was taken for an integer position");
    SetWindowTextW(edit, L"99999999999");
    Check(!Translation::ReadWindowsFormsSpin(buttons, spin, reason), "a number beyond 32 bits was taken for a position");
    SetWindowTextW(edit, L"20480");

    // A second candidate edit leaves the buddy ambiguous.
    const HWND other = CreateWindowExW(0, editClass, L"7", WS_CHILD | WS_VISIBLE, 2, 60, 20, 19, numeric,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(other && !Translation::ReadWindowsFormsSpin(buttons, spin, reason),
        "spin buttons with two candidate edits were admitted");
    if (other) DestroyWindow(other);

    // Not a spin pair: no push-button children.
    for (auto* child : accessible->children) child->Release();
    accessible->children.clear();
    Check(!Translation::ReadWindowsFormsSpin(buttons, spin, reason),
        "spin buttons without an up and a down button were admitted");
    DestroyWindow(root);
}
