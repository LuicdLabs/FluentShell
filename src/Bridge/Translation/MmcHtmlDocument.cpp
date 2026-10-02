#include "MmcHtmlDocument.h"
#include "../../Common/FluentShell.h"
#include "../Ipc/Protocol.h"

#include <oleacc.h>
#include <mshtml.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cwctype>

namespace FluentShell::Bridge::Translation {
namespace {
using Microsoft::WRL::ComPtr;

struct Text final {
    BSTR value = nullptr;
    Text() = default;
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    ~Text() { SysFreeString(value); }
    std::wstring String() const { return value ? std::wstring(value, SysStringLen(value)) : L""; }
};

std::wstring WithoutWhitespace(const std::wstring& text) {
    std::wstring result;
    for (const auto character : text) if (!iswspace(character)) result.push_back(character);
    return result;
}

bool OpenDocument(HWND window, ComPtr<IHTMLDocument2>& document) {
    const UINT message = RegisterWindowMessageW(L"WM_HTML_GETOBJECT");
    const LRESULT result = SendMessageW(window, message, 0, 0);
    return result && SUCCEEDED(ObjectFromLresult(result, __uuidof(IHTMLDocument2), 0,
        reinterpret_cast<void**>(document.GetAddressOf()))) && document;
}

template<typename Interface, typename Getter, size_t Count>
bool HaveNoHandlers(Interface* target, const std::array<Getter, Count>& getters) {
    for (const auto getter : getters) {
        VARIANT handler{};
        const HRESULT read = (target->*getter)(&handler);
        const bool absent = handler.vt == VT_EMPTY || handler.vt == VT_NULL;
        VariantClear(&handler);
        if (FAILED(read) || !absent) return false;
    }
    return true;
}

bool ReadSpecifiedAttribute(IHTMLElement4* element, const wchar_t* name, bool& specified) {
    Text key;
    key.value = SysAllocString(name);
    if (!key.value) return false;
    ComPtr<IHTMLDOMAttribute> attribute;
    if (FAILED(element->getAttributeNode(key.value, &attribute))) return false;
    VARIANT_BOOL present = VARIANT_FALSE;
    if (attribute && FAILED(attribute->get_specified(&present))) return false;
    specified = present != VARIANT_FALSE;
    return true;
}

bool AxisOverflows(long clientExtent, long scrollExtent) noexcept {
    // Subtract only after ordering the nonnegative extents, avoiding overflow at
    // LONG_MAX while permitting MSHTML's one-pixel layout rounding.
    return scrollExtent > clientExtent && scrollExtent - clientExtent > 1;
}

std::wstring DescribeScrollGeometry(IHTMLElement* element, long clientWidth, long clientHeight,
    long scrollWidth, long scrollHeight, long scrollLeft, long scrollTop) {
    Text tag, id;
    element->get_tagName(&tag.value);
    element->get_id(&id.value);
    return L" [tag=" + tag.String().substr(0, 32) + L" id=" + id.String().substr(0, 64) +
        L" client=" + std::to_wstring(clientWidth) + L"x" + std::to_wstring(clientHeight) +
        L" scroll=" + std::to_wstring(scrollWidth) + L"x" + std::to_wstring(scrollHeight) +
        L" offset=" + std::to_wstring(scrollLeft) + L"," + std::to_wstring(scrollTop) + L"]";
}
}

bool MmcHtmlDocumentDetail::ReadContent(IHTMLDocument2* source, const RECT& client,
    std::vector<AccessibleIslandItem>& items, std::wstring& reason) noexcept {
    items.clear();
    std::wstring contentSlot;
    auto reject = [&](std::wstring_view value) {
        items.clear();
        reason = value;
        if (!contentSlot.empty()) reason += L" [slot=" + contentSlot + L"]";
        return false;
    };
    try {
        ComPtr<IHTMLDocument2> document = source;
        if (!document) return reject(L"MMC HTML document is unavailable");
        if (client.left != 0 || client.top != 0 || client.right <= 0 || client.bottom <= 0)
            return reject(L"MMC HTML viewport is unavailable");
        Text ready, designMode;
        if (FAILED(document->get_readyState(&ready.value)))
            return reject(L"MMC HTML document readiness is unavailable");
        if (ready.String() != L"complete") return reject(L"MMC HTML document is still loading");
        if (FAILED(document->get_designMode(&designMode.value)) ||
            (!FluentShell::EqualsIgnoreCase(designMode.String(), L"Off") &&
             !FluentShell::EqualsIgnoreCase(designMode.String(), L"Inherit")))
            return reject(L"MMC HTML document is editable or its editing state is unavailable");
        ComPtr<IHTMLElement> body;
        ComPtr<IUnknown> bodyIdentity;
        ComPtr<IHTMLDocument3> document3;
        if (FAILED(document->get_body(&body)) || !body || FAILED(body.As(&bodyIdentity)) ||
            !bodyIdentity || FAILED(document.As(&document3)))
            return reject(L"MMC HTML body is unavailable");
        // views.js intentionally suppresses selection through document.onselectstart,
        // and BODY's OnLoad populates the resource. Neither is a projected action.
        if (!HaveNoHandlers(document.Get(), std::array{
                &IHTMLDocument2::get_onhelp, &IHTMLDocument2::get_onclick,
                &IHTMLDocument2::get_ondblclick, &IHTMLDocument2::get_onmousedown,
                &IHTMLDocument2::get_onmouseup, &IHTMLDocument2::get_onmousemove,
                &IHTMLDocument2::get_onmouseover, &IHTMLDocument2::get_onmouseout,
                &IHTMLDocument2::get_onkeydown, &IHTMLDocument2::get_onkeypress,
                &IHTMLDocument2::get_onkeyup, &IHTMLDocument2::get_ondragstart }) ||
            !HaveNoHandlers(document3.Get(), std::array{
                &IHTMLDocument3::get_oncontextmenu, &IHTMLDocument3::get_onbeforeeditfocus }))
            return reject(L"MMC HTML document contains an unrepresented input handler");
        ComPtr<IHTMLElementCollection> all;
        long count = 0;
        if (FAILED(document->get_all(&all)) || !all || FAILED(all->get_length(&count)) ||
            count < 0 || count > 256) return reject(L"MMC HTML element count is outside the bound");
        // These two native COM objects implement the decorative notch and the MMC
        // event sink; neither is a user action. No other embedded object is admitted.
        for (long index = 0; index < count; ++index) {
            VARIANT at{}, unused{};
            at.vt = VT_I4; at.lVal = index;
            ComPtr<IDispatch> dispatch;
            ComPtr<IHTMLElement> element;
            if (FAILED(all->item(at, unused, &dispatch)) || !dispatch || FAILED(dispatch.As(&element)))
                return reject(L"MMC HTML element is unavailable");
            Text tag;
            if (FAILED(element->get_tagName(&tag.value))) return reject(L"MMC HTML element tag is unavailable");
            const auto tagName = tag.String();
            // "!" is the tag MSHTML reports for the DOCTYPE declaration. A
            // standards-mode document carries one, and it is a parsing directive
            // with no content, layout, or behavior, so it is inert markup.
            static constexpr std::array inertTags{ L"HTML", L"HEAD", L"TITLE", L"META", L"LINK", L"STYLE",
                L"SCRIPT", L"BODY", L"DIV", L"SPAN", L"TABLE", L"TBODY", L"TR", L"TD", L"TH",
                L"NOBR", L"BR", L"P", L"B", L"I", L"U", L"STRONG", L"EM", L"FONT", L"PARAM", L"!" };
            bool decoration = false;
            if (tagName == L"OBJECT") {
                ComPtr<IHTMLObjectElement> object;
                Text id, classId;
                if (FAILED(element->get_id(&id.value)) ||
                    FAILED(element.As(&object)) || FAILED(object->get_classid(&classId.value)))
                    return reject(L"MMC embedded object has no class identity");
                decoration = id.String() == L"FolderIcon" && FluentShell::EqualsIgnoreCase(
                    classId.String(), L"clsid:B0395DA5-6A15-4E44-9F36-9A9DC7A2F341");
                const bool events = id.String() == L"MMCEvents" && FluentShell::EqualsIgnoreCase(
                    classId.String(), L"clsid:ADE6444B-C91F-4e37-92A4-5BB430A33340");
                if (!decoration && !events) return reject(L"MMC HTML contains an unsupported embedded object");
            } else if (std::none_of(inertTags.begin(), inertTags.end(), [&](const auto* allowed) {
                    return tagName == allowed;
                })) return reject(std::wstring(L"MMC HTML contains interactive or unsupported content: ") + tagName);
            // The resource keeps executable and document-level markup in HEAD.
            // In particular, CCF_HTML_DETAILS must not add it to a content slot.
            if (tagName == L"SCRIPT" || tagName == L"STYLE" || tagName == L"LINK" ||
                tagName == L"META" || tagName == L"TITLE") {
                ComPtr<IHTMLElement> parent;
                Text parentTag;
                if (FAILED(element->get_parentElement(&parent)) || !parent ||
                    FAILED(parent->get_tagName(&parentTag.value)) || parentTag.String() != L"HEAD")
                    return reject(L"MMC HTML contains document markup outside its resource head");
            }
            ComPtr<IHTMLElement2> element2;
            ComPtr<IHTMLElement3> element3;
            ComPtr<IHTMLElement4> element4;
            VARIANT_BOOL editable = VARIANT_FALSE;
            Text accessKey;
            bool explicitTabIndex = false;
            if (FAILED(element.As(&element2)) || FAILED(element.As(&element3)) ||
                FAILED(element.As(&element4)) ||
                FAILED(element3->get_isContentEditable(&editable)) ||
                FAILED(element2->get_accessKey(&accessKey.value)) ||
                !ReadSpecifiedAttribute(element4.Get(), L"tabindex", explicitTabIndex))
                return reject(L"MMC HTML element input semantics are unavailable");
            if (editable != VARIANT_FALSE || !accessKey.String().empty())
                return reject(L"MMC HTML contains editable or access-key content");
            if (explicitTabIndex) {
                short tabIndex = 0;
                if (FAILED(element2->get_tabIndex(&tabIndex)) || !decoration || tabIndex != -1)
                    return reject(L"MMC HTML contains an unrepresented focus target");
            }
            if (!HaveNoHandlers(element.Get(), std::array{
                    &IHTMLElement::get_onhelp, &IHTMLElement::get_onclick,
                    &IHTMLElement::get_ondblclick, &IHTMLElement::get_onmousedown,
                    &IHTMLElement::get_onmouseup, &IHTMLElement::get_onmousemove,
                    &IHTMLElement::get_onmouseover, &IHTMLElement::get_onmouseout,
                    &IHTMLElement::get_onkeydown, &IHTMLElement::get_onkeypress,
                    &IHTMLElement::get_onkeyup, &IHTMLElement::get_onselectstart,
                    &IHTMLElement::get_ondragstart }) ||
                !HaveNoHandlers(element2.Get(), std::array{
                    &IHTMLElement2::get_oncontextmenu, &IHTMLElement2::get_ondrag,
                    &IHTMLElement2::get_ondragend, &IHTMLElement2::get_ondragenter,
                    &IHTMLElement2::get_ondragover, &IHTMLElement2::get_ondragleave,
                    &IHTMLElement2::get_ondrop, &IHTMLElement2::get_onbeforecut,
                    &IHTMLElement2::get_oncut, &IHTMLElement2::get_onbeforecopy,
                    &IHTMLElement2::get_oncopy, &IHTMLElement2::get_onbeforepaste,
                    &IHTMLElement2::get_onpaste, &IHTMLElement2::get_onfocus,
                    &IHTMLElement2::get_onblur, &IHTMLElement2::get_onlosecapture,
                    &IHTMLElement2::get_onscroll, &IHTMLElement2::get_onbeforeeditfocus }) ||
                !HaveNoHandlers(element3.Get(), std::array{
                    &IHTMLElement3::get_onmouseenter, &IHTMLElement3::get_onmouseleave,
                    &IHTMLElement3::get_onactivate, &IHTMLElement3::get_ondeactivate,
                    &IHTMLElement3::get_onbeforedeactivate, &IHTMLElement3::get_oncontrolselect }) ||
                !HaveNoHandlers(element4.Get(), std::array{
                    &IHTMLElement4::get_onmousewheel, &IHTMLElement4::get_onbeforeactivate,
                    &IHTMLElement4::get_onfocusin, &IHTMLElement4::get_onfocusout }))
                return reject(L"MMC HTML contains an unrepresented input handler");
        }
        std::wstring representedText;
        for (const auto* id : { L"TaskpadName", L"DisplayNameElem", L"DetailsElem", L"DescriptionElem" }) {
            contentSlot = id;
            Text key; key.value = SysAllocString(id);
            ComPtr<IHTMLElement> element;
            if (!key.value || FAILED(document3->getElementById(key.value, &element)) || !element)
                return reject(L"MMC HTML template is missing a content slot");
            Text text;
            if (FAILED(element->get_innerText(&text.value)))
                return reject(L"MMC HTML content text is unavailable");
            const auto content = text.String();
            if (WithoutWhitespace(content).empty()) continue;
            if (content.size() > Ipc::kMaxStringChars)
                return reject(L"MMC HTML content exceeds the string bound");
            ComPtr<IHTMLElement2> geometry;
            ComPtr<IUnknown> elementIdentity;
            ComPtr<IHTMLRect> rect;
            if (FAILED(element.As(&elementIdentity)) || !elementIdentity ||
                FAILED(element.As(&geometry)) || FAILED(geometry->getBoundingClientRect(&rect)) || !rect)
                return reject(L"MMC HTML content bounds are unavailable");
            AccessibleIslandItem item;
            if (FAILED(rect->get_left(&item.rect.left)) || FAILED(rect->get_top(&item.rect.top)) ||
                FAILED(rect->get_right(&item.rect.right)) || FAILED(rect->get_bottom(&item.rect.bottom)))
                return reject(L"MMC HTML content bounds are unavailable");
            if (item.rect.right <= item.rect.left || item.rect.bottom <= item.rect.top)
                return reject(L"MMC HTML content has no visible bounds");
            if (item.rect.left < client.left || item.rect.top < client.top ||
                item.rect.right > client.right || item.rect.bottom > client.bottom)
                return reject(L"MMC HTML content falls outside its viewport");
            // An overflowing details pane needs an explicit scroll contract. Do not
            // publish its full text into a clipped, unscrollable canvas.
            ComPtr<IHTMLElement> ancestor = element;
            for (unsigned depth = 0; ancestor && depth < 32; ++depth) {
                ComPtr<IHTMLElement> parent;
                if (FAILED(ancestor->get_parentElement(&parent)))
                    return reject(L"MMC HTML content ancestry is unavailable");
                ComPtr<IHTMLElement2> scroll;
                ComPtr<IUnknown> ancestorIdentity;
                long clientHeight = 0, scrollHeight = 0, scrollTop = 0;
                long clientWidth = 0, scrollWidth = 0, scrollLeft = 0;
                const auto rejectGeometry = [&](std::wstring_view cause) {
                    return reject(std::wstring(cause) + DescribeScrollGeometry(ancestor.Get(),
                        clientWidth, clientHeight, scrollWidth, scrollHeight, scrollLeft, scrollTop));
                };
                if (FAILED(ancestor.As(&ancestorIdentity)) || !ancestorIdentity ||
                    FAILED(ancestor.As(&scroll)) ||
                    FAILED(scroll->get_clientHeight(&clientHeight)) ||
                    FAILED(scroll->get_scrollHeight(&scrollHeight)) ||
                    FAILED(scroll->get_scrollTop(&scrollTop)) ||
                    FAILED(scroll->get_clientWidth(&clientWidth)) ||
                    FAILED(scroll->get_scrollWidth(&scrollWidth)) ||
                    FAILED(scroll->get_scrollLeft(&scrollLeft)) ||
                    clientHeight < 0 || scrollHeight < 0 || clientWidth < 0 || scrollWidth < 0)
                    return rejectGeometry(L"MMC HTML content scroll geometry is unavailable");
                if (scrollTop != 0 || scrollLeft != 0)
                    return rejectGeometry(L"MMC HTML content requires scrolling");
                if (!parent) {
                    Text ancestorTag;
                    if (FAILED(ancestor->get_tagName(&ancestorTag.value)) || ancestorTag.String() != L"HTML")
                        return reject(L"MMC HTML content ancestry does not reach its document root");
                    // In legacy quirks mode HTML can report a zero client extent;
                    // the host's real viewport remains a measurable bound.
                    if (clientHeight == 0) clientHeight = client.bottom;
                    if (clientWidth == 0) clientWidth = client.right;
                }
                ComPtr<IHTMLCurrentStyle> style;
                ComPtr<IHTMLCurrentStyle2> style2;
                Text display, overflowX, overflowY;
                VARIANT_BOOL hasLayout = VARIANT_TRUE;
                if (FAILED(scroll->get_currentStyle(&style)) || !style ||
                    FAILED(style.As(&style2)) || FAILED(style2->get_hasLayout(&hasLayout)) ||
                    FAILED(style->get_display(&display.value)) ||
                    FAILED(style->get_overflowX(&overflowX.value)) ||
                    FAILED(style->get_overflowY(&overflowY.value)))
                    return rejectGeometry(L"MMC HTML content overflow style is unavailable");
                const bool verticalOverflow = AxisOverflows(clientHeight, scrollHeight);
                const bool horizontalOverflow = AxisOverflows(clientWidth, scrollWidth);
                // views.htm clips its document BODY. Its 100%-height table can
                // extend an empty band below that viewport (observed: +16 px).
                // That is not missing text or a user scroll affordance: require
                // zero offsets above and contain every text slot in the BODY's
                // clip below, rather than rejecting its blank scroll extent.
                // MSHTML can return distinct IHTMLElement tear-offs for the same
                // DOM object. COM identity is established only by IUnknown.
                const bool clippedDocumentBody = ancestorIdentity.Get() == bodyIdentity.Get() &&
                    FluentShell::EqualsIgnoreCase(overflowX.String(), L"hidden") &&
                    FluentShell::EqualsIgnoreCase(overflowY.String(), L"hidden");
                if ((verticalOverflow || horizontalOverflow) && !clippedDocumentBody) {
                    // Legacy MSHTML reports zero client dimensions for elements
                    // without an independent layout box, including TBODY/TR and
                    // some ordinary flow DIVs. Their scroll dimensions describe
                    // descendants, not a scrollport. Only exempt that zero-box
                    // case when computed overflow remains visible on both axes;
                    // actual clipping/scroll containers and the final text bounds
                    // are still checked.
                    const auto displayName = display.String();
                    const bool structuralBox = displayName == L"inline" || displayName == L"table-row" ||
                        displayName == L"table-row-group" || displayName == L"table-header-group" ||
                        displayName == L"table-footer-group" || displayName == L"table-column" ||
                        displayName == L"table-column-group";
                    if ((verticalOverflow && clientHeight != 0) ||
                        (horizontalOverflow && clientWidth != 0) ||
                        (hasLayout != VARIANT_FALSE && !structuralBox) ||
                        !FluentShell::EqualsIgnoreCase(overflowX.String(), L"visible") ||
                        !FluentShell::EqualsIgnoreCase(overflowY.String(), L"visible"))
                        return rejectGeometry(L"MMC HTML content requires scrolling [display=" + displayName +
                            L" overflow=" + overflowX.String() + L"," + overflowY.String() +
                            L" hasLayout=" + std::to_wstring(hasLayout != VARIANT_FALSE) + L"]");
                }
                // A clipped descendant can still report its full bounding box
                // while a zero-sized legacy ancestor reports no scroll extent.
                // Check containment directly whenever an actual ancestor layout
                // box clips an axis. A slot's own border box is not its text box,
                // so its own overflow is handled by the metrics above instead.
                const auto clips = [](const std::wstring& value) {
                    return FluentShell::EqualsIgnoreCase(value, L"hidden") ||
                        FluentShell::EqualsIgnoreCase(value, L"auto") ||
                        FluentShell::EqualsIgnoreCase(value, L"scroll") ||
                        FluentShell::EqualsIgnoreCase(value, L"clip");
                };
                const bool clipsX = clips(overflowX.String());
                const bool clipsY = clips(overflowY.String());
                // A zero-sized element with hidden overflow is not a harmless
                // MSHTML layout wrapper: it clips every descendant on that axis,
                // even when scrollHeight is reported as zero by legacy layout.
                // Reject it before the normal containment calculation can lose
                // the clip to a zero/zero client rectangle.
                if (ancestorIdentity.Get() != elementIdentity.Get() &&
                    hasLayout != VARIANT_FALSE &&
                    ((clipsY && clientHeight == 0) || (clipsX && clientWidth == 0)))
                    return rejectGeometry(L"MMC HTML content is clipped by an ancestor");
                if (ancestorIdentity.Get() != elementIdentity.Get() &&
                    hasLayout != VARIANT_FALSE && (clipsX || clipsY)) {
                    ComPtr<IHTMLRect> ancestorBounds;
                    long left = 0, top = 0, clientLeft = 0, clientTop = 0;
                    if (FAILED(scroll->getBoundingClientRect(&ancestorBounds)) || !ancestorBounds ||
                        FAILED(ancestorBounds->get_left(&left)) || FAILED(ancestorBounds->get_top(&top)) ||
                        FAILED(scroll->get_clientLeft(&clientLeft)) || FAILED(scroll->get_clientTop(&clientTop)) ||
                        clientLeft < 0 || clientTop < 0)
                        return rejectGeometry(L"MMC HTML ancestor clipping bounds are unavailable");
                    const long long clipLeft = static_cast<long long>(left) + clientLeft;
                    const long long clipTop = static_cast<long long>(top) + clientTop;
                    if ((clipsX && (item.rect.left < clipLeft || item.rect.right > clipLeft + clientWidth)) ||
                        (clipsY && (item.rect.top < clipTop || item.rect.bottom > clipTop + clientHeight)))
                        return rejectGeometry(L"MMC HTML content is clipped by an ancestor");
                }
                ancestor = std::move(parent);
            }
            if (ancestor) return reject(L"MMC HTML content ancestry exceeds the depth bound");
            item.name = content;
            item.kind = AccessibleItemKind::Text;
            representedText += WithoutWhitespace(content);
            items.push_back(std::move(item));
        }
        contentSlot.clear();
        Text bodyText;
        if (FAILED(body->get_innerText(&bodyText.value)) || items.empty() ||
            representedText != WithoutWhitespace(bodyText.String()))
            return reject(L"MMC HTML has content outside the represented slots");
        return true;
    } catch (...) { return reject(L"MMC HTML capture raised an exception"); }
}

bool ReadMmcHtmlDocument(HWND window, std::vector<AccessibleIslandItem>& items,
    std::wstring& reason) noexcept {
    items.clear();
    auto reject = [&](std::wstring_view value) { items.clear(); reason = value; return false; };
    try {
        ComPtr<IHTMLDocument2> document;
        if (!OpenDocument(window, document)) return reject(L"MMC HTML document is unavailable");
        Text url;
        if (FAILED(document->get_URL(&url.value)))
            return reject(L"MMC HTML document identity is unavailable");
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (systemLength == 0 || systemLength >= MAX_PATH)
            return reject(L"MMC resource directory is unavailable");
        const std::wstring expected = L"res://" + std::wstring(systemDirectory) + L"\\mmcndmgr.dll/views.htm";
        if (!FluentShell::EqualsIgnoreCase(url.String(), expected))
            return reject(L"HTML document is not the MMC extended-view resource");
        RECT client{};
        if (!GetClientRect(window, &client)) return reject(L"MMC HTML viewport is unavailable");
        return MmcHtmlDocumentDetail::ReadContent(document.Get(), client, items, reason);
    } catch (...) { return reject(L"MMC HTML capture raised an exception"); }
}

std::wstring DescribeMmcHtmlDocument(HWND window) noexcept {
    try {
        const UINT message = RegisterWindowMessageW(L"WM_HTML_GETOBJECT");
        const LRESULT result = SendMessageW(window, message, 0, 0);
        Microsoft::WRL::ComPtr<IHTMLDocument2> document;
        if (!result || FAILED(ObjectFromLresult(result, __uuidof(IHTMLDocument2), 0,
                reinterpret_cast<void**>(document.GetAddressOf()))))
            return L"native HTML document is unavailable";
        auto read = [](BSTR value, size_t cap) {
            std::wstring text;
            if (value) {
                text.assign(value, (std::min)(static_cast<size_t>(SysStringLen(value)), cap));
                SysFreeString(value);
                for (auto& character : text)
                    if (character < L' ') character = L' ';
            }
            return text;
        };
        BSTR value = nullptr;
        document->get_URL(&value);
        std::wstring text = L" URL=" + read(value, 512);
        value = nullptr;
        document->get_readyState(&value);
        text += L" ready=" + read(value, 64);
        Microsoft::WRL::ComPtr<IHTMLElement> body;
        if (SUCCEEDED(document->get_body(&body)) && body) {
            value = nullptr;
            body->get_outerHTML(&value);
            text += L" body=" + read(value, 12000);
        }
        return text;
    } catch (...) {
        return L"native HTML diagnostic failed";
    }
}

}
