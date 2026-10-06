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
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override { if (count) *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** child) override { if (child) *child = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT, BSTR* name) override { if (name) *name = nullptr; return S_FALSE; }
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
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT, BSTR* action) override { if (action) *action = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long*, long*, long*, long*, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT) override { return E_NOTIMPL; }
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
    int y, long role, long state) {
    const HWND window = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
        10, y, 200, 20, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return nullptr;
    auto* accessible = new StubAccessible();
    accessible->role = role;
    accessible->state = state;
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
    Check(!classify(link, kind), "owner-draw WinForms LinkLabel was admitted as plain text");
    Check(!classify(plainOwnerDraw, kind), "a non-WinForms owner-draw Static lost its refusal");

    Check(Translation::ReadButtonCheckState(check) == BST_UNCHECKED &&
        Translation::ReadButtonCheckState(radio) == BST_CHECKED,
        "owner-draw WinForms toggle state was not read from MSAA");
    SendMessageW(check, BM_CLICK, 0, 0);
    Check(Translation::ReadButtonCheckState(check) == BST_CHECKED,
        "BM_CLICK did not reach the owner-draw WinForms toggle's click handler");
    Check(Translation::ReadButtonCheckState(push) == -1,
        "an owner-draw push button reported a check state");

    // A leaf WinForms surface (Panel/PictureBox): inert painted pixels when it is no
    // tab stop and offers no action (WinForms reports every enabled control as
    // focusable); a tab-stop one, like a collapsible section header, keeps its refusal.
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
        ROLE_SYSTEM_CLIENT, STATE_SYSTEM_FOCUSABLE);
    Check(strip && classify(strip, kind) && kind == Translation::ControlKind::StaticBitmap,
        "an inert leaf WinForms surface was not admitted as its painted pixels");
    Check(focusable && !classify(focusable, kind),
        "a tab-stop WinForms surface was admitted as inert pixels");
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
