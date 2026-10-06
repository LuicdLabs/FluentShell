#include "NativeDocument.h"
#include "AccessibleIsland.h"
#include "DirectUiEngine.h"
#include "GuiThreadDispatch.h"
#include "../../Common/FluentShell.h"

#include <mshtml.h>
#include <oleacc.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace FluentShell::Bridge::Translation {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kMaxElements = 256;
constexpr size_t kMaxRegions = 16;
constexpr size_t kMaxCounters = 256;
constexpr size_t kMaxDocumentText = 65536;
const CLSID kSystemMonitor = {0xc4d2d8e0, 0xd1dd, 0x11ce, {0x94,0x0f,0x00,0x80,0x29,0x00,0x43,0x47}};

struct Text final {
    BSTR value = nullptr;
    Text() = default;
    explicit Text(const wchar_t* text) : value(SysAllocString(text)) {}
    ~Text() { SysFreeString(value); }
    std::wstring String() const { return value ? std::wstring(value, SysStringLen(value)) : L""; }
};
struct Variant final {
    VARIANT value{};
    ~Variant() { VariantClear(&value); }
};
struct ActionBinding final {
    ComPtr<IUnknown> handler;
    std::wstring href;
    std::wstring token;
};
struct RegionBinding final {
    ComPtr<IHTMLElement2> element;
    HWND report = nullptr;
};
struct DocumentState final {
    ComPtr<IUnknown> identity;
    HWND window = nullptr;
    std::unordered_map<std::wstring, ActionBinding> actions;
    std::unordered_map<std::wstring, RegionBinding> regions;
    std::vector<HWND> representedWindows;
};
thread_local std::unordered_map<std::wstring, DocumentState> states;
std::atomic<uint64_t> nextActionToken{1};
std::atomic<uint64_t> nextDocumentToken{1};

bool Reject(std::wstring& reason, std::wstring_view message) {
    reason.assign(message); return false;
}
std::wstring Compact(std::wstring_view value) {
    std::wstring result;
    for (const auto c : value) if (!iswspace(c)) result.push_back(c);
    return result;
}
bool BoundedText(const std::wstring& value) {
    return value.size() <= Ipc::kMaxStringChars && value.find(L'\0') == std::wstring::npos;
}
template<class Interface, class Getter, size_t Count>
bool NoHandlers(Interface* object, const std::array<Getter, Count>& getters) {
    for (const auto getter : getters) {
        Variant handler;
        if (FAILED((object->*getter)(&handler.value)) ||
            (handler.value.vt != VT_EMPTY && handler.value.vt != VT_NULL)) return false;
    }
    return true;
}
bool RepresentedInput(IHTMLElement* element, bool link) {
    ComPtr<IHTMLElement2> element2;
    ComPtr<IHTMLElement3> element3;
    ComPtr<IHTMLElement4> element4;
    if (FAILED(element->QueryInterface(IID_PPV_ARGS(&element2))) ||
        FAILED(element->QueryInterface(IID_PPV_ARGS(&element3))) ||
        FAILED(element->QueryInterface(IID_PPV_ARGS(&element4)))) return false;
    Variant click;
    if (!link && (FAILED(element->get_onclick(&click.value)) ||
        (click.value.vt != VT_EMPTY && click.value.vt != VT_NULL))) return false;
    Text attribute(L"tabindex");
    ComPtr<IHTMLDOMAttribute> tab;
    VARIANT_BOOL specified = VARIANT_FALSE;
    if (FAILED(element4->getAttributeNode(attribute.value, &tab)) ||
        (tab && FAILED(tab->get_specified(&specified)))) return false;
    if (specified != VARIANT_FALSE) {
        short index = 0;
        if (FAILED(element2->get_tabIndex(&index)) || (link ? index != 0 : index >= 0)) return false;
    }
    return NoHandlers(element, std::array{&IHTMLElement::get_onhelp,
        &IHTMLElement::get_ondblclick, &IHTMLElement::get_onmousedown, &IHTMLElement::get_onmouseup,
        &IHTMLElement::get_onmousemove, &IHTMLElement::get_onmouseover, &IHTMLElement::get_onmouseout,
        &IHTMLElement::get_onkeydown, &IHTMLElement::get_onkeypress, &IHTMLElement::get_onkeyup,
        &IHTMLElement::get_ondragstart}) &&
        NoHandlers(element2.Get(), std::array{&IHTMLElement2::get_oncontextmenu,
        &IHTMLElement2::get_ondrag, &IHTMLElement2::get_ondragend, &IHTMLElement2::get_ondragenter,
        &IHTMLElement2::get_ondragover, &IHTMLElement2::get_ondragleave, &IHTMLElement2::get_ondrop,
        &IHTMLElement2::get_onbeforecut, &IHTMLElement2::get_oncut, &IHTMLElement2::get_onbeforecopy,
        &IHTMLElement2::get_oncopy, &IHTMLElement2::get_onbeforepaste, &IHTMLElement2::get_onpaste,
        &IHTMLElement2::get_onfocus, &IHTMLElement2::get_onblur, &IHTMLElement2::get_onlosecapture,
        &IHTMLElement2::get_onscroll, &IHTMLElement2::get_onbeforeeditfocus}) &&
        NoHandlers(element3.Get(), std::array{&IHTMLElement3::get_onmouseenter,
        &IHTMLElement3::get_onmouseleave, &IHTMLElement3::get_onactivate, &IHTMLElement3::get_ondeactivate,
        &IHTMLElement3::get_onbeforedeactivate, &IHTMLElement3::get_oncontrolselect}) &&
        NoHandlers(element4.Get(), std::array{&IHTMLElement4::get_onmousewheel,
        &IHTMLElement4::get_onbeforeactivate, &IHTMLElement4::get_onfocusin, &IHTMLElement4::get_onfocusout});
}
bool Key(IHTMLElement* element, std::wstring& key) {
    ComPtr<IHTMLUniqueName> unique;
    Text text;
    if (FAILED(element->QueryInterface(IID_PPV_ARGS(&unique))) ||
        FAILED(unique->get_uniqueID(&text.value))) return false;
    key = text.String();
    return !key.empty() && BoundedText(key);
}
bool DocumentKey(IHTMLDocument2* document, std::wstring& key, ComPtr<IUnknown>& identity) {
    if (!document || FAILED(document->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
    // IHTMLDocument3.uniqueID is an ID generator, not the document's identity.
    // Bind our lifecycle token to canonical IUnknown and retain that COM identity
    // until navigation/destruction retires the host's cached state.
    for (const auto& [id, state] : states)
        if (state.identity == identity) { key = id; return true; }
    key = L"document-" + std::to_wstring(nextDocumentToken.fetch_add(1));
    return true;
}
bool Bounds(IHTMLElement2* element, RECT& rect) {
    ComPtr<IHTMLRect> bounds;
    if (FAILED(element->getBoundingClientRect(&bounds)) || !bounds ||
        FAILED(bounds->get_left(&rect.left)) || FAILED(bounds->get_top(&rect.top)) ||
        FAILED(bounds->get_right(&rect.right)) || FAILED(bounds->get_bottom(&rect.bottom))) return false;
    return rect.left >= -Ipc::kMaxCoordinate && rect.top >= -Ipc::kMaxCoordinate &&
        rect.right <= Ipc::kMaxCoordinate && rect.bottom <= Ipc::kMaxCoordinate &&
        rect.right >= rect.left && rect.bottom >= rect.top;
}
bool Inside(const RECT& child, const RECT& parent) {
    return child.left >= parent.left && child.top >= parent.top &&
        child.right <= parent.right && child.bottom <= parent.bottom;
}
bool GetProperty(IDispatch* object, const wchar_t* name, Variant& value,
    VARIANT* argument = nullptr) {
    LPOLESTR property = const_cast<LPOLESTR>(name);
    DISPID id = 0;
    if (!object || FAILED(object->GetIDsOfNames(IID_NULL, &property, 1, LOCALE_USER_DEFAULT, &id))) return false;
    DISPPARAMS args{argument, nullptr, argument ? 1u : 0u, 0};
    return SUCCEEDED(object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
        DISPATCH_PROPERTYGET, &args, &value.value, nullptr, nullptr));
}
bool LongProperty(IDispatch* object, const wchar_t* name, long& result) {
    Variant raw, converted;
    if (!GetProperty(object, name, raw) ||
        FAILED(VariantChangeType(&converted.value, &raw.value, 0, VT_I4))) return false;
    result = converted.value.lVal; return true;
}
bool BoolProperty(IDispatch* object, const wchar_t* name, bool& result) {
    Variant raw;
    if (!GetProperty(object, name, raw) || raw.value.vt != VT_BOOL) return false;
    result = raw.value.boolVal != VARIANT_FALSE; return true;
}
bool DispatchValue(const VARIANT& value, ComPtr<IDispatch>& result) {
    IUnknown* object = value.vt == VT_DISPATCH ? value.pdispVal :
        value.vt == VT_UNKNOWN ? value.punkVal : nullptr;
    return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result))) && result;
}
bool ReadScroll(HWND window, NativeDocumentRegion& region) {
    SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
    SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS};
    const auto axis = [&](int bar, SCROLLINFO& scroll, int& maximum, int& offset) {
        if (!GetScrollInfo(window, bar, &scroll)) {
            maximum = offset = 0;
            return (GetWindowLongPtrW(window, GWL_STYLE) & (bar == SB_HORZ ? WS_HSCROLL : WS_VSCROLL)) == 0;
        }
        const int64_t extent = static_cast<int64_t>(scroll.nMax) - scroll.nMin + 1;
        if (scroll.nMin != 0 || extent < 0 || extent > Ipc::kMaxCoordinate) return false;
        maximum = static_cast<int>((std::max)(int64_t{0}, extent - scroll.nPage));
        offset = scroll.nPos;
        return offset >= 0 && offset <= maximum;
    };
    return axis(SB_HORZ, horizontal, region.maximumX, region.offsetX) &&
        axis(SB_VERT, vertical, region.maximumY, region.offsetY);
}
bool ReadReport(IHTMLObjectElement* element, NativeDocumentItem& item,
    NativeDocumentRegion& region, HWND document, HWND& report,
    std::vector<HWND>& represented, std::wstring& reason) {
    ComPtr<IDispatch> object;
    ComPtr<IPersist> persistence;
    CLSID clsid{};
    if (FAILED(element->get_object(&object)) || !object || FAILED(object.As(&persistence)) ||
        FAILED(persistence->GetClassID(&clsid)) || clsid != kSystemMonitor)
        return Reject(reason, L"native document embedded object has no supported semantic contract");
    bool readOnly = false, toolbar = true, legend = true, valueBar = true;
    long display = 0, valueType = -1;
    if (!BoolProperty(object.Get(), L"ReadOnly", readOnly) || !readOnly ||
        !LongProperty(object.Get(), L"DisplayType", display) || display != 3 ||
        !LongProperty(object.Get(), L"ReportValueType", valueType) || valueType != 0 ||
        !BoolProperty(object.Get(), L"ShowToolbar", toolbar) || toolbar ||
        !BoolProperty(object.Get(), L"ShowLegend", legend) || legend ||
        !BoolProperty(object.Get(), L"ShowValueBar", valueBar) || valueBar)
        return Reject(reason, L"native report requires a read-only current-value report without command chrome");
    ComPtr<IOleWindow> nativeWindow;
    HWND root = nullptr;
    if (FAILED(object.As(&nativeWindow)) || FAILED(nativeWindow->GetWindow(&root)) || !root)
        return Reject(reason, L"native report has no owned window");
    wchar_t rootClass[64]{};
    if (!document || !IsChild(document, root) ||
        !GetClassNameW(root, rootClass, 64) || wcscmp(rootClass, L"SysmonCtrl") != 0 ||
        (GetWindowLongPtrW(root, GWL_STYLE) & WS_TABSTOP) != 0 ||
        !WindowClassRegisteredBySystemModule(root, L"sysmon.ocx"))
        return Reject(reason, L"native report root has no System Monitor window contract");
    struct FindReport { HWND value = nullptr; HWND root = nullptr; int count = 0; bool complete = true; } found{nullptr, root};
    EnumChildWindows(root, [](HWND child, LPARAM raw) -> BOOL {
        auto& result = *reinterpret_cast<FindReport*>(raw);
        if (!IsWindowVisible(child)) return TRUE;
        wchar_t name[64]{};
        if (GetParent(child) == result.root && GetClassNameW(child, name, 64) &&
            FluentShell::EqualsIgnoreCase(name, L"SysmonReport") &&
            (GetWindowLongPtrW(child, GWL_STYLE) & WS_TABSTOP) == 0 &&
            WindowClassRegisteredBySystemModule(child, L"sysmon.ocx")) {
            result.value = child; ++result.count;
        } else result.complete = false;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    if (!found.complete || found.count != 1 || !ReadScroll(found.value, region))
        return Reject(reason, L"native report scroll contract is unavailable or ambiguous");
    report = found.value;
    represented.push_back(root);
    represented.push_back(report);
    for (HWND parent = GetParent(root); parent && parent != document; parent = GetParent(parent)) {
        wchar_t name[64]{};
        if (!IsChild(document, parent) || !GetClassNameW(parent, name, 64) || wcscmp(name, L"Hatchwin") != 0 ||
            (GetWindowLongPtrW(parent, GWL_STYLE) & WS_TABSTOP) != 0 ||
            (!WindowClassRegisteredBySystemModule(parent, L"ole32.dll") &&
             !WindowClassRegisteredBySystemModule(parent, L"combase.dll") &&
             !WindowClassRegisteredBySystemModule(parent, L"mshtml.dll")))
            return Reject(reason, L"native report has an unrepresented native host ancestor");
        represented.push_back(parent);
    }
    Variant countersValue;
    ComPtr<IDispatch> counters;
    long count = 0;
    if (!GetProperty(object.Get(), L"Counters", countersValue) ||
        !DispatchValue(countersValue.value, counters) || !LongProperty(counters.Get(), L"Count", count) ||
        count < 0 || count > kMaxCounters)
        return Reject(reason, L"native report counter collection exceeds its readable bound");
    for (long index = 1; index <= count; ++index) {
        VARIANT at{}; at.vt = VT_I4; at.lVal = index;
        Variant counterValue, path, value, number;
        ComPtr<IDispatch> counter;
        bool visible = false;
        if (!GetProperty(counters.Get(), L"Item", counterValue, &at) ||
            !DispatchValue(counterValue.value, counter) ||
            !BoolProperty(counter.Get(), L"Visible", visible))
            return Reject(reason, L"native report counter identity or visibility is unavailable");
        if (!visible) continue;
        if (!GetProperty(counter.Get(), L"Path", path) || path.value.vt != VT_BSTR)
            return Reject(reason, L"native report counter path is unavailable");
        NativeReportRow row;
        row.path.assign(path.value.bstrVal, SysStringLen(path.value.bstrVal));
        if (row.path.empty() || !BoundedText(row.path))
            return Reject(reason, L"native report counter path exceeds the text bound");
        row.available = GetProperty(counter.Get(), L"Value", value) &&
            SUCCEEDED(VariantChangeType(&number.value, &value.value, 0, VT_R8)) &&
            std::isfinite(number.value.dblVal) && number.value.dblVal >= 0;
        if (row.available) row.value = number.value.dblVal;
        item.reportRows.push_back(std::move(row));
    }
    return true;
}

bool ReadAction(IHTMLElement* element, const std::wstring& key, DocumentState& state,
    const DocumentState* previous, std::wstring& token, std::wstring& reason) {
    Variant onclick;
    ComPtr<IHTMLAnchorElement> anchor;
    Text href;
    if (FAILED(element->get_onclick(&onclick.value)) || FAILED(element->QueryInterface(IID_PPV_ARGS(&anchor))) ||
        FAILED(anchor->get_href(&href.value)))
        return Reject(reason, L"native document link action is unavailable");
    ActionBinding binding;
    binding.href = href.String();
    if (onclick.value.vt == VT_DISPATCH && onclick.value.pdispVal) {
        if (FAILED(onclick.value.pdispVal->QueryInterface(IID_PPV_ARGS(&binding.handler)))) return false;
    } else if (onclick.value.vt != VT_EMPTY && onclick.value.vt != VT_NULL) {
        return Reject(reason, L"native document link handler has an unsupported identity");
    }
    if (!binding.handler && binding.href.empty())
        return Reject(reason, L"native document link has no semantic action");
    if (previous) {
        const auto found = previous->actions.find(key);
        if (found != previous->actions.end() && found->second.handler == binding.handler &&
            found->second.href == binding.href) binding.token = found->second.token;
    }
    if (binding.token.empty()) binding.token = std::to_wstring(nextActionToken.fetch_add(1));
    token = binding.token;
    state.actions.emplace(key, std::move(binding));
    return true;
}

bool OpenDocument(HWND window, ComPtr<IHTMLDocument2>& document) {
    const auto message = RegisterWindowMessageW(L"WM_HTML_GETOBJECT");
    const auto result = SendMessageW(window, message, 0, 0);
    return result && SUCCEEDED(ObjectFromLresult(result, __uuidof(IHTMLDocument2), 0,
        reinterpret_cast<void**>(document.GetAddressOf()))) && document;
}
bool OwnThread(HWND window) {
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId();
}
bool ResourceDocument(IHTMLDocument2* document) {
    Text url;
    if (FAILED(document->get_URL(&url.value))) return false;
    const auto value = url.String();
    if (value.size() < 7 || !FluentShell::EqualsIgnoreCase(value.substr(0, 6), L"res://")) return false;
    const auto end = value.find(L'/', 6);
    if (end == std::wstring::npos) return false;
    const auto moduleName = value.substr(6, end - 6);
    HMODULE module = GetModuleHandleW(moduleName.c_str());
    wchar_t path[MAX_PATH]{}, system[MAX_PATH]{};
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH) || !GetSystemDirectoryW(system, MAX_PATH)) return false;
    const std::wstring canonical(path);
    const auto slash = canonical.find_last_of(L"\\/");
    return slash != std::wstring::npos &&
        FluentShell::EqualsIgnoreCase(canonical.substr(0, slash), system);
}
}

bool NativeDocumentDetail::ReadContent(IHTMLDocument2* document, HWND window, const RECT& viewport,
    NativeDocumentSnapshot& snapshot, std::wstring& reason) noexcept {
    snapshot = {};
    try {
        if (!document || viewport.left != 0 || viewport.top != 0 ||
            viewport.right <= 0 || viewport.bottom <= 0 ||
            viewport.right > Ipc::kMaxCoordinate || viewport.bottom > Ipc::kMaxCoordinate)
            return Reject(reason, L"native document viewport is unavailable");
        DocumentState next;
        if (!DocumentKey(document, snapshot.documentId, next.identity))
            return Reject(reason, L"native document identity is unavailable");
        next.window = window;
        const auto previousEntry = states.find(snapshot.documentId);
        // MSHTML calls can pump messages, including destruction and a nested
        // capture. Own the old bindings instead of retaining a map iterator.
        const std::optional<DocumentState> previous = previousEntry != states.end() &&
            previousEntry->second.identity == next.identity ?
            std::optional<DocumentState>{previousEntry->second} : std::nullopt;
        Text ready, editing;
        if (FAILED(document->get_readyState(&ready.value)) || ready.String() != L"complete" ||
            FAILED(document->get_designMode(&editing.value)) ||
            (!FluentShell::EqualsIgnoreCase(editing.String(), L"Off") &&
             !FluentShell::EqualsIgnoreCase(editing.String(), L"Inherit")))
            return Reject(reason, L"native document is loading or editable");
        ComPtr<IHTMLDocument3> document3;
        if (FAILED(document->QueryInterface(IID_PPV_ARGS(&document3))) ||
            !NoHandlers(document, std::array{&IHTMLDocument2::get_onhelp, &IHTMLDocument2::get_onclick,
                &IHTMLDocument2::get_ondblclick, &IHTMLDocument2::get_onmousedown, &IHTMLDocument2::get_onmouseup,
                &IHTMLDocument2::get_onmousemove, &IHTMLDocument2::get_onmouseover, &IHTMLDocument2::get_onmouseout,
                &IHTMLDocument2::get_onkeydown, &IHTMLDocument2::get_onkeypress, &IHTMLDocument2::get_onkeyup,
                &IHTMLDocument2::get_ondragstart}) ||
            !NoHandlers(document3.Get(), std::array{&IHTMLDocument3::get_oncontextmenu,
                &IHTMLDocument3::get_onbeforeeditfocus}))
            return Reject(reason, L"native document has an unrepresented document input handler");
        ComPtr<IHTMLElement> body;
        ComPtr<IHTMLElementCollection> all;
        long count = 0;
        if (FAILED(document->get_body(&body)) || !body || FAILED(document->get_all(&all)) ||
            !all || FAILED(all->get_length(&count)) || count < 0 || count > kMaxElements)
            return Reject(reason, L"native document element census exceeds its bound");
        struct Element final {
            ComPtr<IHTMLElement> element;
            ComPtr<IHTMLElement2> geometry;
            std::wstring key, tag, text;
            RECT rect{};
            bool hidden = false;
            bool absorbed = false;
            int region = -1;
        };
        std::vector<Element> elements;
        std::unordered_map<std::wstring, size_t> indexes;
        for (long index = 0; index < count; ++index) {
            VARIANT at{}, unused{}; at.vt = VT_I4; at.lVal = index;
            ComPtr<IDispatch> dispatch;
            Element value;
            Text tag, text;
            if (FAILED(all->item(at, unused, &dispatch)) || !dispatch ||
                FAILED(dispatch.As(&value.element)) || FAILED(dispatch.As(&value.geometry)) ||
                !Key(value.element.Get(), value.key) ||
                FAILED(value.element->get_tagName(&tag.value)) ||
                FAILED(value.element->get_innerText(&text.value)) || !Bounds(value.geometry.Get(), value.rect))
                return Reject(reason, L"native document element facts are unavailable");
            value.tag = tag.String(); value.text = text.String();
            if (!indexes.emplace(value.key, elements.size()).second)
                return Reject(reason, L"native document element identity is duplicated");
            elements.push_back(std::move(value));
        }
        std::wstring represented;
        size_t textSize = 0;
        size_t pixelBytes = 0, reportRows = 0;
        for (auto& value : elements) {
            auto parent = value.element;
            bool underBody = false, underHead = false;
            for (size_t depth = 0; parent && depth <= kMaxElements; ++depth) {
                Text tag;
                parent->get_tagName(&tag.value);
                underBody = underBody || tag.String() == L"BODY";
                underHead = underHead || tag.String() == L"HEAD";
                if (parent.Get() != value.element.Get()) {
                    std::wstring parentKey;
                    if (!Key(parent.Get(), parentKey)) return Reject(reason, L"native document ancestry is unavailable");
                    const auto position = indexes.find(parentKey);
                    if (position != indexes.end()) {
                        const auto& ancestor = elements[position->second];
                        value.hidden = value.hidden || ancestor.hidden;
                        if (ancestor.tag == L"A" || ancestor.tag == L"OBJECT") value.absorbed = true;
                        if (value.region < 0 && ancestor.region >= 0) value.region = ancestor.region;
                    }
                }
                ComPtr<IHTMLElement> nextParent;
                if (FAILED(parent->get_parentElement(&nextParent))) return false;
                parent = std::move(nextParent);
            }
            if (!underBody || underHead) continue;
            ComPtr<IHTMLCurrentStyle> style;
            ComPtr<IHTMLElement3> element3;
            Text display, visibility, overflowX, overflowY, accessKey;
            VARIANT_BOOL editable = VARIANT_FALSE;
            if (FAILED(value.geometry->get_currentStyle(&style)) || !style ||
                FAILED(style->get_display(&display.value)) || FAILED(style->get_visibility(&visibility.value)) ||
                FAILED(style->get_overflowX(&overflowX.value)) || FAILED(style->get_overflowY(&overflowY.value)) ||
                FAILED(value.element.As(&element3)) || FAILED(element3->get_isContentEditable(&editable)) ||
                FAILED(value.geometry->get_accessKey(&accessKey.value)) || editable != VARIANT_FALSE)
                return Reject(reason, L"native document element style or editing semantics are unavailable");
            value.hidden = value.hidden || display.String() == L"none" || visibility.String() == L"hidden";
            if (value.hidden) continue;
            if (!RepresentedInput(value.element.Get(), value.tag == L"A" && !value.absorbed))
                return Reject(reason, L"native document has an unrepresented element input handler or focus target");
            if (value.absorbed) continue;
            if (!accessKey.String().empty()) return Reject(reason, L"native document access keys require an explicit key contract");
            static constexpr std::array tags{L"BODY", L"DIV", L"SPAN", L"TABLE", L"TBODY", L"TR", L"TD", L"TH",
                L"NOBR", L"BR", L"P", L"B", L"I", L"U", L"STRONG", L"EM", L"FONT", L"A", L"IMG",
                L"OBJECT", L"H1", L"H2", L"H3", L"H4", L"H5", L"H6"};
            if (std::none_of(tags.begin(), tags.end(), [&](const auto* tag) { return value.tag == tag; }))
                return Reject(reason, L"native document contains an unrepresented element: " + value.tag);
            long clientWidth = 0, clientHeight = 0, scrollWidth = 0, scrollHeight = 0, x = 0, y = 0;
            if (FAILED(value.geometry->get_clientWidth(&clientWidth)) ||
                FAILED(value.geometry->get_clientHeight(&clientHeight)) ||
                FAILED(value.geometry->get_scrollWidth(&scrollWidth)) ||
                FAILED(value.geometry->get_scrollHeight(&scrollHeight)) ||
                FAILED(value.geometry->get_scrollLeft(&x)) || FAILED(value.geometry->get_scrollTop(&y)))
                return Reject(reason, L"native document scroll facts are unavailable");
            const auto scrolls = [](const std::wstring& overflow) { return overflow == L"auto" || overflow == L"scroll"; };
            const bool scrollport = (scrolls(overflowX.String()) || scrolls(overflowY.String())) &&
                clientWidth > 0 && clientHeight > 0;
            if (scrollport) {
                if (value.region >= 0 || snapshot.regions.size() >= kMaxRegions)
                    return Reject(reason, L"native document nested or excessive scrollports are unsupported");
                NativeDocumentRegion region;
                region.key = value.key; region.kind = L"html"; region.rect = value.rect;
                region.rect.right = region.rect.left + clientWidth;
                region.rect.bottom = region.rect.top + clientHeight;
                region.contentWidth = (std::max)(clientWidth, scrollWidth);
                region.contentHeight = (std::max)(clientHeight, scrollHeight);
                region.maximumX = region.contentWidth - clientWidth;
                region.maximumY = region.contentHeight - clientHeight;
                region.offsetX = x; region.offsetY = y;
                if (!Inside(region.rect, viewport) || region.contentWidth > Ipc::kMaxCoordinate ||
                    region.contentHeight > Ipc::kMaxCoordinate || x < 0 || x > region.maximumX ||
                    y < 0 || y > region.maximumY)
                    return Reject(reason, L"native document scrollport exceeds its bounds");
                value.region = static_cast<int>(snapshot.regions.size());
                snapshot.regions.push_back(std::move(region));
                next.regions.emplace(value.key, RegionBinding{value.geometry, nullptr});
            } else if (x != 0 || y != 0) {
                return Reject(reason, L"native document has scrolling outside a represented scrollport");
            }
            const bool link = value.tag == L"A", graphic = value.tag == L"IMG", object = value.tag == L"OBJECT";
            bool textLeaf = !Compact(value.text).empty();
            if (textLeaf && !link && !object) {
                ComPtr<IDispatch> childrenDispatch;
                ComPtr<IHTMLElementCollection> children;
                long childCount = 0;
                if (FAILED(value.element->get_children(&childrenDispatch)) ||
                    FAILED(childrenDispatch.As(&children)) || FAILED(children->get_length(&childCount))) return false;
                for (long childIndex = 0; childIndex < childCount; ++childIndex) {
                    VARIANT at{}, unused{}; at.vt = VT_I4; at.lVal = childIndex;
                    ComPtr<IDispatch> childDispatch; ComPtr<IHTMLElement> child; Text childText;
                    if (FAILED(children->item(at, unused, &childDispatch)) || !childDispatch ||
                        FAILED(childDispatch.As(&child)) || FAILED(child->get_innerText(&childText.value))) return false;
                    if (!Compact(childText.String()).empty()) { textLeaf = false; break; }
                }
            }
            if (!link && !graphic && !object && !textLeaf) continue;
            NativeDocumentItem item;
            item.key = value.key; item.rect = value.rect; item.regionIndex = value.region;
            item.kind = link ? L"link" : graphic ? L"image" : object ? L"report" : L"text";
            if (!link && value.tag.size() == 2 && value.tag[0] == L'H' &&
                value.tag[1] >= L'1' && value.tag[1] <= L'6') item.kind = L"heading";
            item.name = value.text;
            VARIANT_BOOL disabled = VARIANT_FALSE;
            if (FAILED(element3->get_disabled(&disabled))) return false;
            item.enabled = disabled == VARIANT_FALSE;
            if (item.rect.right <= item.rect.left || item.rect.bottom <= item.rect.top)
                return Reject(reason, L"native document semantic element has no layout box");
            if (link && !ReadAction(value.element.Get(), value.key, next,
                previous ? &*previous : nullptr, item.actionIdentity, reason)) return false;
            if (graphic || (link && Compact(value.text).empty())) {
                DirectUiWindowEvidence pixels;
                if (!window || !Inside(value.rect, viewport) ||
                    !CaptureDirectUiVirtualImagePixels(window, value.rect, pixels, reason))
                    return Reject(reason, L"native document image is outside its current paintable viewport");
                item.image = ImageListEntry{pixels.imageWidth, pixels.imageHeight, pixels.imageFormat, std::move(pixels.imageData)};
                if ((pixelBytes += item.image->imageData.size()) > 1024 * 1024)
                    return Reject(reason, L"native document images exceed their aggregate bound");
                if (graphic) {
                    ComPtr<IHTMLImgElement> image; Text alt;
                    if (SUCCEEDED(value.element.As(&image)) && SUCCEEDED(image->get_alt(&alt.value))) item.name = alt.String();
                }
            }
            if (object) {
                ComPtr<IHTMLObjectElement> embedded;
                NativeDocumentRegion region;
                region.key = value.key; region.kind = L"report"; region.rect = value.rect;
                HWND nativeReport = nullptr;
                if (value.region >= 0 || snapshot.regions.size() >= kMaxRegions || !Inside(value.rect, viewport) ||
                    FAILED(value.element.As(&embedded)) || !ReadReport(embedded.Get(), item, region,
                        window, nativeReport, next.representedWindows, reason)) return false;
                item.reportRegionIndex = static_cast<int>(snapshot.regions.size());
                snapshot.regions.push_back(std::move(region));
                next.regions.emplace(value.key, RegionBinding{nullptr, nativeReport});
                if ((reportRows += item.reportRows.size()) > kMaxCounters)
                    return Reject(reason, L"native document report rows exceed their aggregate bound");
                for (const auto& row : item.reportRows) textSize += row.path.size();
            }
            if (value.region >= 0) {
                const auto& region = snapshot.regions[value.region];
                OffsetRect(&item.rect, region.offsetX - region.rect.left, region.offsetY - region.rect.top);
                if (!Inside(item.rect, RECT{0, 0, region.contentWidth, region.contentHeight}))
                    return Reject(reason, L"native document semantic element escapes its scroll content");
            } else if (!Inside(item.rect, viewport)) {
                return Reject(reason, L"native document semantic element escapes its viewport");
            }
            if (!BoundedText(item.name) || (textSize += item.name.size()) > kMaxDocumentText)
                return Reject(reason, L"native document semantic text exceeds its bound");
            if (!graphic && !object) represented += Compact(value.text);
            snapshot.items.push_back(std::move(item));
        }
        Text bodyText;
        if (FAILED(body->get_innerText(&bodyText.value)) || represented != Compact(bodyText.String()) || snapshot.items.empty())
            return Reject(reason, L"native document has text outside its represented semantics");
        if (window) {
            struct Census { const DocumentState* state; bool complete = true; } census{&next};
            EnumChildWindows(window, [](HWND child, LPARAM raw) -> BOOL {
                auto& value = *reinterpret_cast<Census*>(raw);
                if (!IsWindowVisible(child)) return TRUE;
                if (std::find(value.state->representedWindows.begin(),
                    value.state->representedWindows.end(), child) != value.state->representedWindows.end()) return TRUE;
                value.complete = false; return FALSE;
            }, reinterpret_cast<LPARAM>(&census));
            if (!census.complete) return Reject(reason, L"native document has an unrepresented native child window");
        }
        if (GuiThreadOperationCancelled()) return Reject(reason, L"native document capture was cancelled");
        // A navigation can replace the document while retaining the host HWND.
        // Remove old bindings before releasing their COM objects (Release may pump).
        std::vector<DocumentState> retired;
        for (auto entry = states.begin(); entry != states.end();) {
            if (entry->second.window == window) {
                auto removed = states.extract(entry++);
                retired.push_back(std::move(removed.mapped()));
            } else ++entry;
        }
        states.emplace(snapshot.documentId, std::move(next));
        return true;
    } catch (...) { snapshot = {}; return Reject(reason, L"native document capture raised an exception"); }
}

bool ReadNativeDocument(HWND window, NativeDocumentSnapshot& snapshot, std::wstring& reason) noexcept {
    snapshot = {};
    if (!OwnThread(window)) return Reject(reason, L"native document capture requires its owning GUI thread");
    if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_TABSTOP) != 0)
        return Reject(reason, L"native document host keyboard focus requires an explicit focus contract");
    ComPtr<IHTMLDocument2> document;
    RECT viewport{};
    if (!OpenDocument(window, document) || !ResourceDocument(document.Get()) || !GetClientRect(window, &viewport))
        return Reject(reason, L"native document is not an available System32 resource document");
    if (!NativeDocumentDetail::ReadContent(document.Get(), window, viewport, snapshot, reason)) {
        snapshot = {}; return false;
    }
    return true;
}

bool NativeDocumentDetail::InvokeItem(IHTMLDocument2* document, const NativeDocumentSnapshot& expected,
    int index, std::wstring& reason) noexcept {
    try {
        if (index < 0 || static_cast<size_t>(index) >= expected.items.size()) return Reject(reason, L"native document link index is invalid");
        const auto& item = expected.items[index];
        if (item.kind != L"link" || !item.enabled || item.actionIdentity.empty()) return Reject(reason, L"native document item offers no link action");
        std::wstring id; DocumentState current;
        if (!DocumentKey(document, id, current.identity) || id != expected.documentId) return Reject(reason, L"native document identity changed before invocation");
        const auto old = states.find(id);
        if (old == states.end() || old->second.identity != current.identity) return Reject(reason, L"native document action binding expired");
        const DocumentState previous = old->second;
        ComPtr<IHTMLElementCollection> all; long count = 0;
        if (FAILED(document->get_all(&all)) || !all || FAILED(all->get_length(&count)) || count > kMaxElements) return false;
        for (long atIndex = 0; atIndex < count; ++atIndex) {
            VARIANT at{}, unused{}; at.vt = VT_I4; at.lVal = atIndex;
            ComPtr<IDispatch> dispatch; ComPtr<IHTMLElement> element; std::wstring key;
            if (FAILED(all->item(at, unused, &dispatch)) || !dispatch || FAILED(dispatch.As(&element)) || !Key(element.Get(), key)) return false;
            if (key != item.key) continue;
            Text text; std::wstring token;
            if (FAILED(element->get_innerText(&text.value)) || text.String() != item.name ||
                !ReadAction(element.Get(), key, current, &previous, token, reason) || token != item.actionIdentity)
                return Reject(reason, L"native document link identity changed before invocation");
            if (GuiThreadOperationCancelled()) return Reject(reason, L"native document invocation was cancelled");
            const auto live = states.find(id);
            if (live == states.end() || live->second.identity != current.identity ||
                !live->second.actions.contains(key) || live->second.actions.at(key).token != token)
                return Reject(reason, L"native document action binding expired during invocation");
            return SUCCEEDED(element->click()) || Reject(reason, L"native document declined its link action");
        }
        return Reject(reason, L"native document link was removed before invocation");
    } catch (...) { return Reject(reason, L"native document invocation raised an exception"); }
}

bool NativeDocumentDetail::Scroll(IHTMLDocument2* document, const NativeDocumentSnapshot& expected,
    int index, int x, int y, std::wstring& reason) noexcept {
    try {
        std::wstring id; ComPtr<IUnknown> identity;
        if (!DocumentKey(document, id, identity) || id != expected.documentId || index < 0 ||
            static_cast<size_t>(index) >= expected.regions.size()) return Reject(reason, L"native document scroll identity is invalid");
        const auto found = states.find(id);
        const auto& region = expected.regions[index];
        if (found == states.end() || found->second.identity != identity ||
            x < 0 || x > region.maximumX || y < 0 || y > region.maximumY)
            return Reject(reason, L"native document scroll range changed or binding expired");
        const auto bindingEntry = found->second.regions.find(region.key);
        if (bindingEntry == found->second.regions.end()) return Reject(reason, L"native document scrollport was removed");
        const RegionBinding binding = bindingEntry->second;
        const auto ready = [&] {
            if (GuiThreadOperationCancelled()) return false;
            const auto live = states.find(id);
            if (live == states.end() || live->second.identity != identity) return false;
            const auto target = live->second.regions.find(region.key);
            return target != live->second.regions.end() && target->second.element == binding.element &&
                target->second.report == binding.report;
        };
        if (!ready()) return Reject(reason, L"native document scrolling was cancelled or its binding expired");
        if (binding.element) {
            return (SUCCEEDED(binding.element->put_scrollLeft(x)) && ready() &&
                SUCCEEDED(binding.element->put_scrollTop(y))) || Reject(reason, L"native document declined its scroll action");
        }
        const HWND report = binding.report;
        if (!OwnThread(report) || x > 65535 || y > 65535) return Reject(reason, L"native report scroll action exceeds the standard message contract");
        SendMessageW(report, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, x), 0);
        if (!ready() || !OwnThread(report)) return Reject(reason, L"native report scrolling was cancelled");
        SendMessageW(report, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, y), 0);
        if (!ready() || !OwnThread(report)) return Reject(reason, L"native report scrolling was cancelled");
        SendMessageW(report, WM_HSCROLL, SB_ENDSCROLL, 0);
        if (!ready() || !OwnThread(report)) return Reject(reason, L"native report scrolling was cancelled");
        SendMessageW(report, WM_VSCROLL, SB_ENDSCROLL, 0);
        return true;
    } catch (...) { return Reject(reason, L"native document scrolling raised an exception"); }
}

bool InvokeNativeDocumentItem(HWND window, const NativeDocumentSnapshot& expected,
    int index, std::wstring& reason) noexcept {
    ComPtr<IHTMLDocument2> document;
    NativeDocumentSnapshot current;
    if (!ReadNativeDocument(window, current, reason) || current.documentId != expected.documentId ||
        index < 0 || static_cast<size_t>(index) >= current.items.size() ||
        static_cast<size_t>(index) >= expected.items.size() || !current.items[index].enabled ||
        current.items[index].key != expected.items[index].key ||
        current.items[index].actionIdentity != expected.items[index].actionIdentity)
        return Reject(reason, L"native document link changed before its queued action");
    return (OpenDocument(window, document) &&
        NativeDocumentDetail::InvokeItem(document.Get(), expected, index, reason));
}
bool ScrollNativeDocument(HWND window, const NativeDocumentSnapshot& expected,
    int index, int x, int y, std::wstring& reason) noexcept {
    ComPtr<IHTMLDocument2> document;
    NativeDocumentSnapshot current;
    if (!ReadNativeDocument(window, current, reason) || current.documentId != expected.documentId ||
        index < 0 || static_cast<size_t>(index) >= current.regions.size() ||
        static_cast<size_t>(index) >= expected.regions.size() ||
        current.regions[index].key != expected.regions[index].key ||
        current.regions[index].kind != expected.regions[index].kind)
        return Reject(reason, L"native document scrollport changed before its queued action");
    return (OpenDocument(window, document) &&
        NativeDocumentDetail::Scroll(document.Get(), current, index, x, y, reason));
}
bool NativeDocumentOwnsDescendant(HWND document, HWND descendant) noexcept {
    if (!OwnThread(document) || !IsChild(document, descendant)) return false;
    for (const auto& [id, state] : states) if (state.window == document) {
        if (std::find(state.representedWindows.begin(), state.representedWindows.end(), descendant) !=
            state.representedWindows.end()) return true;
    }
    return false;
}
void ForgetNativeDocument(HWND window) noexcept {
    try {
        std::vector<DocumentState> retired;
        for (auto entry = states.begin(); entry != states.end();) {
            if (entry->second.window == window) {
                auto removed = states.extract(entry++);
                retired.push_back(std::move(removed.mapped()));
            } else ++entry;
        }
    } catch (...) {}
}
}
