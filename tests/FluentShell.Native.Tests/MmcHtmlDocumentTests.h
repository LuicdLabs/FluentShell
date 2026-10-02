#pragma once

#include "../../src/Bridge/Translation/MmcHtmlDocument.h"

#include <docobj.h>
#include <mshtml.h>
#include <ocidl.h>
#include <oleacc.h>
#include <wrl/client.h>

#include <array>
#include <cwchar>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace FluentShell::Tests {
namespace MmcHtmlDocumentTests {

using Microsoft::WRL::ComPtr;
namespace Translation = FluentShell::Bridge::Translation;

struct Text final {
    BSTR value = nullptr;
    explicit Text(const wchar_t* text = nullptr) : value(text ? SysAllocString(text) : nullptr) {}
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    ~Text() { SysFreeString(value); }
    std::wstring String() const { return value ? std::wstring(value, SysStringLen(value)) : L""; }
};

// A hidden, in-process HTMLDocument view supplies real MSHTML layout and DOM
// semantics. It has no browser navigation, external resources, or MMC automation
// object. Its own STA owns the window, COM objects, message pump, and teardown.
class Fixture final : public IOleClientSite, public IOleInPlaceSite, public IOleInPlaceFrame {
public:
    static constexpr const wchar_t* kClassName = L"FluentShell.Test.MmcHtmlDocument";
    static constexpr RECT kViewport{ 0, 0, 640, 480 };

    Fixture() {
        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.lpszClassName = kClassName;
        registered_ = RegisterClassExW(&windowClass) != 0;
        if (!registered_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        window_ = CreateWindowExW(0, kClassName, L"MMC HTML adapter test", WS_POPUP | WS_CLIPCHILDREN,
            0, 0, kViewport.right, kViewport.bottom, nullptr, nullptr, windowClass.hInstance, this);
    }

    ~Fixture() {
        if (view_) {
            view_->UIActivate(FALSE);
            view_->Show(FALSE);
            view_->CloseView(0);
            view_->SetInPlaceSite(nullptr);
        }
        view_.Reset();
        if (object_) {
            object_->Close(OLECLOSE_NOSAVE);
            object_->SetClientSite(nullptr);
        }
        document_.Reset();
        object_.Reset();
        if (window_) DestroyWindow(window_);
        if (registered_) UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    bool Load(const std::wstring& html) {
        if (!window_) return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE), "CreateWindowEx");
        if (!Require(CoCreateInstance(__uuidof(HTMLDocument), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IHTMLDocument2), reinterpret_cast<void**>(document_.GetAddressOf())),
                "CoCreateInstance HTMLDocument") ||
            !Require(document_.As(&object_), "IOleObject") ||
            !Require(object_->SetClientSite(this), "SetClientSite")) return false;
        ComPtr<IPersistStreamInit> persistence;
        ComPtr<IOleDocument> oleDocument;
        RECT viewport = kViewport;
        if (!Require(document_.As(&persistence), "IPersistStreamInit") ||
            !Require(persistence->InitNew(), "InitNew") ||
            !Require(document_.As(&oleDocument), "IOleDocument") ||
            !Require(oleDocument->CreateView(this, nullptr, 0, &view_), "CreateView") || !view_ ||
            !Require(view_->SetInPlaceSite(this), "SetInPlaceSite") ||
            !Require(view_->SetRect(&viewport), "SetRect") ||
            !Require(view_->UIActivate(TRUE), "UIActivate") ||
            !Require(view_->Show(TRUE), "Show view")) return false;

        struct ArrayOwner final {
            SAFEARRAY* value = SafeArrayCreateVector(VT_VARIANT, 0, 1);
            ~ArrayOwner() { if (value) SafeArrayDestroy(value); }
        } source;
        if (!source.value) return Fail(E_OUTOFMEMORY, "SafeArrayCreateVector");
        VARIANT* entry = nullptr;
        if (!Require(SafeArrayAccessData(source.value, reinterpret_cast<void**>(&entry)),
                "SafeArrayAccessData")) return false;
        entry->vt = VT_BSTR;
        entry->bstrVal = SysAllocStringLen(html.data(), static_cast<UINT>(html.size()));
        const bool allocated = entry->bstrVal != nullptr;
        SafeArrayUnaccessData(source.value);
        if (!allocated) return Fail(E_OUTOFMEMORY, "SysAllocStringLen");
        if (!Require(document_->write(source.value), "document.write") ||
            !Require(document_->close(), "document.close")) return false;

        const ULONGLONG deadline = GetTickCount64() + 2000;
        do {
            PumpMessages();
            Text state;
            if (SUCCEEDED(document_->get_readyState(&state.value)) && state.String() == L"complete") {
                // Force layout using the same in-place viewport as the adapter.
                return Require(view_->SetRect(&viewport), "SetRect after load");
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < deadline);
        return Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "document readyState");
    }

    IHTMLDocument2* Document() const noexcept { return document_.Get(); }
    HWND Window() const noexcept { return window_; }

    bool Capture(std::vector<Translation::AccessibleIslandItem>& items, std::wstring& reason,
        const RECT& viewport = kViewport) {
        PumpMessages();
        return Translation::MmcHtmlDocumentDetail::ReadContent(document_.Get(), viewport, items, reason);
    }

    ComPtr<IHTMLElement> Element(const wchar_t* id) {
        ComPtr<IHTMLDocument3> document3;
        ComPtr<IHTMLElement> element;
        Text key(id);
        if (key.value && SUCCEEDED(document_.As(&document3)))
            document3->getElementById(key.value, &element);
        return element;
    }

    bool AppendInertScript(const wchar_t* id) {
        // Legacy innerHTML can discard a leading SCRIPT before it reaches the
        // DOM. Append an explicitly inert real node so this tests admission of
        // unsupported document markup rather than the parser's normalization.
        auto element = Element(id);
        ComPtr<IHTMLElement> script;
        ComPtr<IHTMLScriptElement> scriptElement;
        ComPtr<IHTMLDOMNode> parentNode, scriptNode, appended;
        Text tag(L"SCRIPT"), scriptId(L"InjectedScript"), type(L"text/plain");
        if (!element || !tag.value || !scriptId.value || !type.value ||
            FAILED(document_->createElement(tag.value, &script)) || !script ||
            FAILED(script->put_id(scriptId.value)) || FAILED(script.As(&scriptElement)) ||
            FAILED(scriptElement->put_type(type.value)) || FAILED(element.As(&parentNode)) ||
            FAILED(script.As(&scriptNode)) || FAILED(parentNode->appendChild(scriptNode.Get(), &appended)) ||
            !appended) return false;
        ComPtr<IHTMLElement> actualParent;
        Text parentId;
        return SUCCEEDED(script->get_parentElement(&actualParent)) && actualParent &&
            SUCCEEDED(actualParent->get_id(&parentId.value)) && parentId.String() == id;
    }

    bool Execute(const wchar_t* script) {
        ComPtr<IHTMLWindow2> window;
        Text source(script), language(L"JScript");
        if (!source.value || !language.value || FAILED(document_->get_parentWindow(&window)) || !window)
            return false;
        VARIANT result{};
        const HRESULT executed = window->execScript(source.value, language.value, &result);
        VariantClear(&result);
        return SUCCEEDED(executed);
    }

    void DescribeFailure(const char* scenario) const {
        std::cerr << "MMC HTML fixture '" << scenario << "' failed at " << stage_
                  << " with HRESULT " << error_ << '\n';
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IOleClientSite))
            *value = static_cast<IOleClientSite*>(this);
        else if (iid == __uuidof(IOleWindow) || iid == __uuidof(IOleInPlaceSite))
            *value = static_cast<IOleInPlaceSite*>(this);
        else if (iid == __uuidof(IOleInPlaceUIWindow) || iid == __uuidof(IOleInPlaceFrame))
            *value = static_cast<IOleInPlaceFrame*>(this);
        if (!*value) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&references_)); }

    HRESULT STDMETHODCALLTYPE SaveObject() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMoniker(DWORD, DWORD, IMoniker** value) override {
        if (value) *value = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetContainer(IOleContainer** value) override {
        if (value) *value = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ShowObject() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShowWindow(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestNewObjectLayout() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* value) override {
        if (!value) return E_POINTER;
        *value = window_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CanInPlaceActivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnInPlaceActivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUIActivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWindowContext(IOleInPlaceFrame** frame, IOleInPlaceUIWindow** document,
        LPRECT position, LPRECT clip, LPOLEINPLACEFRAMEINFO info) override {
        if (!frame || !document || !position || !clip || !info) return E_POINTER;
        *frame = static_cast<IOleInPlaceFrame*>(this);
        AddRef();
        *document = nullptr;
        *position = kViewport;
        *clip = kViewport;
        *info = { sizeof(OLEINPLACEFRAMEINFO), FALSE, window_, nullptr, 0 };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Scroll(SIZE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnUIDeactivate(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnInPlaceDeactivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DiscardUndoState() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeactivateAndUndo() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnPosRectChange(LPCRECT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBorder(LPRECT border) override {
        if (!border) return E_POINTER;
        *border = kViewport;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestBorderSpace(LPCBORDERWIDTHS) override { return INPLACE_E_NOTOOLSPACE; }
    HRESULT STDMETHODCALLTYPE SetBorderSpace(LPCBORDERWIDTHS) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetActiveObject(IOleInPlaceActiveObject*, LPCOLESTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMenu(HMENU, HOLEMENU, HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoveMenus(HMENU) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetStatusText(LPCOLESTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(LPMSG, WORD) override { return E_NOTIMPL; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        static const UINT getObject = RegisterWindowMessageW(L"WM_HTML_GETOBJECT");
        if (message == getObject && fixture && fixture->document_)
            return LresultFromObject(__uuidof(IHTMLDocument2), wParam, fixture->document_.Get());
        return DefWindowProcW(window, message, wParam, lParam);
    }
    static void PumpMessages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    bool Require(HRESULT result, const char* stage) {
        return SUCCEEDED(result) || Fail(result, stage);
    }
    bool Fail(HRESULT result, const char* stage) {
        error_ = result;
        stage_ = stage;
        return false;
    }
    LONG references_ = 1;
    bool registered_ = false;
    HWND window_ = nullptr;
    HRESULT error_ = E_FAIL;
    const char* stage_ = "initialization";
    ComPtr<IHTMLDocument2> document_;
    ComPtr<IOleObject> object_;
    ComPtr<IOleDocumentView> view_;
};

inline std::wstring Page(const wchar_t* details = L"Details", const wchar_t* attributes = L"",
    const wchar_t* extraBody = L"", const wchar_t* wrappersBefore = L"", const wchar_t* wrappersAfter = L"") {
    // These fixed CSS boxes leave ample spare space, so negative semantic cases
    // cannot pass merely because unrelated text happened to overflow first.
    return std::wstring(L"<html><head><meta charset='utf-8'><style>"
        L"html,body{margin:0;padding:0;width:100%;height:100%;font:12px Arial;}"
        L"#TaskpadName{position:absolute;left:8px;top:8px;width:300px;height:24px;}"
        L"#DisplayNameElem{position:absolute;left:8px;top:40px;width:300px;height:24px;}"
        L"#DetailsHost{position:absolute;left:8px;top:72px;width:300px;height:120px;overflow:auto;}"
        L"#DescriptionElem{position:absolute;left:8px;top:220px;width:300px;height:40px;}"
        L"</style><script>function OnLoad(){};document.onselectstart=function(){return false;};"
        L"</script></head><body onload='OnLoad()'>"
        L"<div id='TaskpadName'>Scope</div><div id='DisplayNameElem'>Selected item</div>"
        L"<div id='DetailsHost'>") + wrappersBefore + L"<span id='DetailsElem' " + attributes + L">" +
        details + L"</span>" + wrappersAfter + L"</div><span id='DescriptionElem'>Description</span>" +
        extraBody + L"</body></html>";
}

inline std::wstring LegacyMmcPage() {
    // The installed views.htm has no doctype, an absolutely positioned caption
    // table, and a second 100%-height table. Keep those legacy layout conditions:
    // ordinary DIV-only fixtures do not expose the same client/scroll metrics.
    return L"<html><head><style>body{margin:0;font:icon;}span{font:icon;}"
        L"#TaskpadName{font:caption;padding-left:3px;padding-top:5px;padding-bottom:7px;}"
        L"#DisplayNameElem{padding-left:5px;padding-top:5px;padding-bottom:3px;padding-right:5px;}"
        L"</style></head><body scroll='no'>"
        L"<div style='position:absolute;left:0;top:0'><table width='100%' cellspacing='0' cellpadding='0'>"
        L"<tr><td><span style='width:32px'></span></td><td id='TaskpadName'><nobr>Local Computer Policy</nobr>"
        L"</td></tr></table></div>"
        L"<table width='100%' height='100%' cellspacing='0' cellpadding='0'><tr>"
        L"<td id='LeftPanel' rowspan='2' width='212'><span style='display:block;height:25px'></span></td>"
        L"<td id='RightPanel' valign='top' width='100%' style='height:15px'></td></tr>"
        L"<tr><td height='10'></td></tr><tr><td><div id='DisplayNameElem'>"
        L"Select an item to view its description.<br></div></td><td id='ViewPanel' height='100%' rowspan='2'></td></tr>"
        L"<tr><td height='100%'><div style='overflow:auto;height:100%;padding-left:5px;padding-right:5px;word-wrap:break-word'>"
        L"<span id='DetailsElem'></span><span id='DescriptionElem'></span></div></td></tr></table></body></html>";
}

inline bool Load(Fixture& fixture, const std::wstring& html, const char* scenario,
    void (*check)(bool, const char*)) {
    const bool loaded = fixture.Load(html);
    if (!loaded) fixture.DescribeFailure(scenario);
    check(loaded, "in-process MMC HTML fixture could not initialize and complete layout");
    return loaded;
}

inline void ExpectRejected(Fixture& fixture, const wchar_t* reasonFragment, const char* scenario,
    void (*check)(bool, const char*)) {
    std::vector<Translation::AccessibleIslandItem> items(1);
    items[0].name = L"stale previously captured text";
    std::wstring reason;
    const bool admitted = fixture.Capture(items, reason);
    if (admitted || reason.find(reasonFragment) == std::wstring::npos)
        std::wcerr << L"MMC HTML unexpected admission/reason: " << reason << L'\n';
    check(!admitted && items.empty() && reason.find(reasonFragment) != std::wstring::npos, scenario);
}

inline void Run(void (*check)(bool, const char*)) {
    {
        Fixture fixture;
        if (!Load(fixture, Page(L"<b>Read-only</b><br><i>details</i>"), "read-only rich text", check)) return;
        std::vector<Translation::AccessibleIslandItem> items;
        std::wstring reason;
        const bool admitted = fixture.Capture(items, reason);
        if (!admitted) std::wcerr << L"MMC HTML positive fixture rejected: " << reason << L'\n';
        check(admitted && items.size() == 4, "read-only MMC slots with inline formatted details were rejected");
        if (!admitted || items.size() != 4) return;
        check(items[0].name == L"Scope" && items[1].name == L"Selected item" &&
              items[2].name.find(L"Read-only") != std::wstring::npos && items[3].name == L"Description",
            "MMC HTML slot capture lost native content or its order");
        for (const auto& item : items)
            check(item.kind == Translation::AccessibleItemKind::Text && item.actionName.empty() &&
                  item.rect.left >= 0 && item.rect.top >= 0 && item.rect.right <= Fixture::kViewport.right &&
                  item.rect.bottom <= Fixture::kViewport.bottom && item.rect.right > item.rect.left &&
                  item.rect.bottom > item.rect.top,
                "MMC HTML text acquired an action or lost its client-relative visible bounds");

        // The same actual document must fail the public HWND entry point: it is
        // about:blank, not the installed MMC resource. Tests never override trust.
        items.assign(1, {});
        items[0].name = L"stale";
        check(!Translation::ReadMmcHtmlDocument(fixture.Window(), items, reason) && items.empty() &&
              reason.find(L"not the MMC extended-view resource") != std::wstring::npos,
            "MMC HTML public entry admitted a test document without the trusted resource URL");
        const RECT emptyViewport{};
        items.assign(1, {});
        check(!fixture.Capture(items, reason, emptyViewport) && items.empty() &&
              reason.find(L"viewport is unavailable") != std::wstring::npos,
            "MMC HTML admitted content without a measurable viewport");
    }
    {
        Fixture fixture;
        if (!Load(fixture, LegacyMmcPage(), "legacy MMC startup table layout", check)) return;
        std::vector<Translation::AccessibleIslandItem> items;
        std::wstring reason;
        const bool admitted = fixture.Capture(items, reason);
        if (!admitted) std::wcerr << L"MMC HTML legacy table fixture rejected: " << reason << L'\n';
        check(admitted && items.size() == 2 && items[0].name == L"Local Computer Policy" &&
              items[1].name.find(L"Select an item") != std::wstring::npos,
            "MMC HTML rejected the legacy startup layout despite its two visible text slots fitting");
    }
    {
        Fixture fixture;
        if (!Load(fixture, Page(L"Details", L"",
                L"<div style='position:absolute;left:0;top:500px;width:1px;height:16px'></div>"),
                "clipped BODY with blank overflow", check)) return;
        check(fixture.Execute(L"document.body.scroll='no';document.body.style.overflow='hidden';"),
            "MMC HTML fixture could not establish document clipping");
        ComPtr<IHTMLElement> body;
        ComPtr<IHTMLElement2> geometry;
        ComPtr<IHTMLCurrentStyle> style;
        long clientHeight = 0, scrollHeight = 0;
        Text overflowX, overflowY;
        const bool blankOverflow = SUCCEEDED(fixture.Document()->get_body(&body)) && body &&
            SUCCEEDED(body.As(&geometry)) && SUCCEEDED(geometry->get_clientHeight(&clientHeight)) &&
            SUCCEEDED(geometry->get_scrollHeight(&scrollHeight)) && scrollHeight > clientHeight + 1 &&
            SUCCEEDED(geometry->get_currentStyle(&style)) && style &&
            SUCCEEDED(style->get_overflowX(&overflowX.value)) && SUCCEEDED(style->get_overflowY(&overflowY.value)) &&
            overflowX.String() == L"hidden" && overflowY.String() == L"hidden";
        if (!blankOverflow)
            std::wcerr << L"MMC HTML blank BODY fixture state: clientHeight=" << clientHeight <<
                L" scrollHeight=" << scrollHeight << L" overflow=" << overflowX.String() <<
                L"," << overflowY.String() << L'\n';
        check(blankOverflow, "MMC HTML fixture did not establish harmless blank BODY overflow");
        std::vector<Translation::AccessibleIslandItem> items;
        std::wstring reason;
        const bool admitted = fixture.Capture(items, reason);
        if (!admitted) std::wcerr << L"MMC HTML blank BODY overflow rejected: " << reason << L'\n';
        check(admitted && items.size() == 4,
            "MMC HTML rejected blank clipped BODY overflow despite every text slot fitting");
    }

    struct Rejection final {
        const char* scenario;
        const wchar_t* details;
        const wchar_t* attributes;
        const wchar_t* reason;
    };
    const std::array cases{
        Rejection{ "MMC HTML admitted editable details", L"<span contenteditable='true'>Editable</span>", L"", L"editable" },
        Rejection{ "MMC HTML admitted inherited editable details", L"Details", L"contenteditable='true'", L"editable" },
        Rejection{ "MMC HTML admitted a tab stop", L"Details", L"tabindex='0'", L"focus target" },
        Rejection{ "MMC HTML admitted an unrepresented programmatic focus target", L"Details", L"tabindex='-1'", L"focus target" },
        Rejection{ "MMC HTML admitted an access key", L"Details", L"accesskey='d'", L"access-key" },
        Rejection{ "MMC HTML admitted a details link", L"<a href='about:blank'>Open</a>", L"", L"interactive or unsupported" },
        Rejection{ "MMC HTML admitted a details input", L"<input value='Editable'>", L"", L"interactive or unsupported" },
        Rejection{ "MMC HTML admitted a context-menu handler", L"Details", L"oncontextmenu='return false'", L"input handler" },
        Rejection{ "MMC HTML admitted a hover handler", L"Details", L"onmouseover='return false'", L"input handler" },
        Rejection{ "MMC HTML admitted a drop handler", L"Details", L"ondrop='return false'", L"input handler" },
        Rejection{ "MMC HTML admitted a paste handler", L"Details", L"onpaste='return false'", L"input handler" },
        Rejection{ "MMC HTML admitted a mouse-wheel handler", L"Details", L"onmousewheel='return false'", L"input handler" },
        Rejection{ "MMC HTML admitted a focus handler", L"Details", L"onfocusin='return false'", L"input handler" },
        Rejection{ "MMC HTML admitted horizontal detail overflow", L"<span style='display:inline-block;width:400px'>Wide details</span>", L"", L"requires scrolling" },
        Rejection{ "MMC HTML admitted vertical detail overflow", L"<div style='height:300px'>Tall details</div>", L"", L"requires scrolling" },
    };
    for (const auto& test : cases) {
        Fixture fixture;
        if (!Load(fixture, Page(test.details, test.attributes), test.scenario, check)) return;
        ExpectRejected(fixture, test.reason, test.scenario, check);
    }

    {
        Fixture fixture;
        if (!Load(fixture, Page(), "document editing mode", check)) return;
        Text mode(L"On");
        check(mode.value && SUCCEEDED(fixture.Document()->put_designMode(mode.value)),
            "MMC HTML fixture could not enable document design mode");
        Text actualMode;
        check(SUCCEEDED(fixture.Document()->get_designMode(&actualMode.value)) &&
              _wcsicmp(actualMode.String().c_str(), L"On") == 0,
            "MMC HTML fixture did not enter document design mode");
        std::vector<Translation::AccessibleIslandItem> items(1);
        std::wstring reason;
        // Enabling designMode can itself restart loading. Both states must stay
        // native; asserting only the editing reason incorrectly rejects that
        // legitimate earlier readiness guard.
        check(!fixture.Capture(items, reason) && items.empty() &&
              (reason.find(L"editable") != std::wstring::npos || reason.find(L"still loading") != std::wstring::npos),
            "MMC HTML admitted document design mode or lost its fail-closed readiness gate");
    }
    {
        Fixture fixture;
        if (!Load(fixture, Page(), "document context-menu handler", check)) return;
        check(fixture.Execute(L"document.oncontextmenu=function(){return false;};"),
            "MMC HTML fixture could not set a document input handler");
        ExpectRejected(fixture, L"input handler", "MMC HTML admitted a document context-menu handler", check);
    }
    {
        Fixture fixture;
        if (!Load(fixture, Page(), "dynamic details markup", check)) return;
        check(fixture.AppendInertScript(L"DetailsElem"),
            "MMC HTML fixture could not retain an inert SCRIPT node under the details slot");
        ExpectRejected(fixture, L"document markup outside", "MMC HTML admitted script markup in dynamic details", check);
    }
    {
        Fixture fixture;
        if (!Load(fixture, Page(L"Details", L"", L"<div>Unrepresented content</div>"), "extra body text", check)) return;
        ExpectRejected(fixture, L"outside the represented slots", "MMC HTML silently dropped body text outside its slots", check);
    }
    {
        Fixture fixture;
        std::wstring before, after;
        for (unsigned depth = 0; depth < 35; ++depth) {
            before += L"<div style='width:300px;height:24px;overflow:visible'>";
            after += L"</div>";
        }
        if (!Load(fixture, Page(L"Details", L"", L"", before.c_str(), after.c_str()), "deep details ancestry", check)) return;
        ExpectRejected(fixture, L"ancestry exceeds", "MMC HTML accepted unexamined ancestors beyond its depth bound", check);
    }
    {
        Fixture fixture;
        // Quirks-mode height can act as a minimum and expand to the text. Use
        // standards mode here and verify the resulting zero-height clip box.
        // First admit this same document without clipping so a later rejection
        // cannot pass merely because another guard already rejected the fixture.
        // The separate legacy TABLE fixture continues to cover MMC's mode.
        if (!Load(fixture, L"<!doctype html>" + Page(L"Clipped details", L"", L"",
                L"<div id='ClipBox' style='width:300px;height:24px;max-height:24px;overflow:visible'>", L"</div>"),
                "zero-height clipping ancestor", check)) return;
        std::vector<Translation::AccessibleIslandItem> baselineItems;
        std::wstring baselineReason;
        const bool baselineAdmitted = fixture.Capture(baselineItems, baselineReason);
        if (!baselineAdmitted)
            std::wcerr << L"MMC HTML zero-height baseline rejected: " << baselineReason << L'\n';
        check(baselineAdmitted && baselineItems.size() == 4,
            "MMC HTML zero-height fixture was not admitted before applying the clip");
        if (!baselineAdmitted || baselineItems.size() != 4) return;
        check(fixture.Execute(L"var clip=document.getElementById('ClipBox');"
                L"clip.style.height='0px';clip.style.maxHeight='0px';clip.style.overflow='hidden';"),
            "MMC HTML fixture could not apply the zero-height ancestor clip");
        auto clip = fixture.Element(L"ClipBox");
        auto details = fixture.Element(L"DetailsElem");
        ComPtr<IHTMLElement> detailParent;
        ComPtr<IHTMLElement2> clipGeometry, detailGeometry;
        ComPtr<IHTMLCurrentStyle> clipStyle;
        ComPtr<IHTMLCurrentStyle2> clipStyle2;
        ComPtr<IHTMLRect> detailBounds;
        long clientHeight = -1, scrollHeight = -1, top = 0, bottom = 0;
        VARIANT_BOOL hasLayout = VARIANT_FALSE;
        Text overflow, text, parentId;
        const bool retained = clip && details && SUCCEEDED(clip.As(&clipGeometry)) &&
            SUCCEEDED(details.As(&detailGeometry)) && SUCCEEDED(clipGeometry->get_clientHeight(&clientHeight)) &&
            SUCCEEDED(clipGeometry->get_scrollHeight(&scrollHeight)) &&
            SUCCEEDED(clipGeometry->get_currentStyle(&clipStyle)) && clipStyle &&
            SUCCEEDED(clipStyle.As(&clipStyle2)) && SUCCEEDED(clipStyle2->get_hasLayout(&hasLayout)) &&
            SUCCEEDED(clipStyle->get_overflowY(&overflow.value)) &&
            SUCCEEDED(details->get_parentElement(&detailParent)) && detailParent &&
            SUCCEEDED(detailParent->get_id(&parentId.value)) &&
            SUCCEEDED(details->get_innerText(&text.value)) &&
            SUCCEEDED(detailGeometry->getBoundingClientRect(&detailBounds)) && detailBounds &&
            SUCCEEDED(detailBounds->get_top(&top)) && SUCCEEDED(detailBounds->get_bottom(&bottom)) &&
            clientHeight == 0 && hasLayout != VARIANT_FALSE && overflow.String() == L"hidden" &&
            parentId.String() == L"ClipBox" &&
            text.String() == L"Clipped details" && bottom > top;
        if (!retained)
            std::wcerr << L"MMC HTML zero-height fixture state: clientHeight=" << clientHeight <<
                L" scrollHeight=" << scrollHeight << L" overflow=" << overflow.String() <<
                L" hasLayout=" << (hasLayout != VARIANT_FALSE) << L" parent=" << parentId.String() <<
                L" details='" << text.String() << L"' bounds=" << top << L"," << bottom << L'\n';
        check(retained, "MMC HTML fixture did not retain text within a real zero-height hidden-overflow ancestor");
        std::vector<Translation::AccessibleIslandItem> items(1);
        std::wstring reason;
        const bool admitted = fixture.Capture(items, reason);
        // MSHTML can report the clipped text either as empty bounds or as full
        // bounds outside the ancestor's scrollport. Both must remain native;
        // the accepted baseline above isolates clipping as the changed input.
        const bool rejectedForClipping = !admitted && items.empty();
        if (!rejectedForClipping)
            std::wcerr << L"MMC HTML zero-height capture: admitted=" << admitted <<
                L" items=" << items.size() << L" scrollHeight=" << scrollHeight <<
                L" reason=" << reason << L'\n';
        check(rejectedForClipping,
            "MMC HTML treated a zero-height clipping ancestor as an inert layout wrapper");
    }
    {
        Fixture fixture;
        if (!Load(fixture, Page(), "partially clipped slot", check)) return;
        check(fixture.Execute(L"document.getElementById('TaskpadName').style.left='-12px';"),
            "MMC HTML fixture could not move its title outside the viewport");
        ExpectRejected(fixture, L"outside its viewport", "MMC HTML admitted a slot partly outside its viewport", check);
    }
    {
        Fixture fixture;
        if (!Load(fixture, Page(L"<div style='height:300px'>Tall details</div>"), "scrolled details", check)) return;
        auto host = fixture.Element(L"DetailsHost");
        ComPtr<IHTMLElement2> geometry;
        long offset = 0;
        const bool scrolled = host && SUCCEEDED(host.As(&geometry)) &&
            SUCCEEDED(geometry->put_scrollTop(12)) && SUCCEEDED(geometry->get_scrollTop(&offset)) && offset != 0;
        check(scrolled, "MMC HTML fixture did not establish a real nonzero scroll offset");
        ExpectRejected(fixture, L"requires scrolling", "MMC HTML admitted scrolled details", check);
    }
    {
        Fixture fixture;
        const wchar_t* decoration = L"<object id='FolderIcon' tabindex='-1' style='display:none' "
            L"classid='clsid:B0395DA5-6A15-4E44-9F36-9A9DC7A2F341'></object>";
        if (!Load(fixture, Page(L"Details", L"", decoration), "known decoration", check)) return;
        std::vector<Translation::AccessibleIslandItem> items;
        std::wstring reason;
        const bool admitted = fixture.Capture(items, reason);
        if (!admitted) std::wcerr << L"MMC HTML known decoration rejected: " << reason << L'\n';
        check(admitted && items.size() == 4, "MMC HTML rejected the known decorative object with tabindex=-1");
    }
}

} // namespace MmcHtmlDocumentTests

inline void TestMmcHtmlDocumentAdmission(void (*check)(bool, const char*)) {
    // The protocol test process uses its own COM apartment. Keep MSHTML entirely
    // inside a dedicated STA rather than changing the process's COM setup.
    std::thread source([check] {
        const HRESULT initialized = OleInitialize(nullptr);
        check(SUCCEEDED(initialized), "MMC HTML test STA could not initialize OLE");
        if (FAILED(initialized)) return;
        try { MmcHtmlDocumentTests::Run(check); }
        catch (...) { check(false, "MMC HTML test fixture raised an exception"); }
        OleUninitialize();
    });
    source.join();
}

} // namespace FluentShell::Tests
