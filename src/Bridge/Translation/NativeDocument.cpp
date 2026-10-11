#include "NativeDocument.h"
#include "AccessibleIsland.h"
#include "DirectUiEngine.h"
#include "GuiThreadDispatch.h"
#include "../../Common/FluentShell.h"

#include <mshtml.h>
#include <oleacc.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// WIC's factory and its premultiplied BGRA format, spelled out so the Bridge needs no
// import of the codec library's GUID definitions.
constexpr CLSID kWicImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
constexpr GUID kWicPixelFormat32bppPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x10}};
constexpr size_t kMaxResourceImageBytes = 1024 * 1024;

// The bytes of a resource of a System32 module, named by a res:// source the way MSHTML
// resolves one: res://<module>/[<type>/]<name>, the type defaulting to RT_HTML and either
// part written "#<number>" for an integer identifier.  Anything else -- another
// directory, an escape, a query -- is not read.
bool ReadSystemResource(std::wstring_view source, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (source.size() < 8 || !FluentShell::EqualsIgnoreCase(source.substr(0, 6), L"res://") ||
        source.find_first_of(L"%?") != std::wstring_view::npos) return false;
    const std::wstring_view rest = source.substr(6);
    const size_t moduleEnd = rest.find(L'/');
    if (moduleEnd == std::wstring_view::npos || moduleEnd == 0) return false;
    const std::wstring module(rest.substr(0, moduleEnd));
    std::vector<std::wstring> parts;
    for (size_t start = moduleEnd + 1; start <= rest.size();) {
        const size_t end = (std::min)(rest.find(L'/', start), rest.size());
        parts.emplace_back(rest.substr(start, end - start));
        if (parts.back().empty() || parts.size() > 2) return false;
        start = end + 1;
    }
    wchar_t system[MAX_PATH]{};
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    if (parts.empty() || systemLength == 0 || systemLength >= MAX_PATH) return false;
    std::wstring path;
    if (module.find_first_of(L"\\/:") != std::wstring::npos) {
        const size_t slash = module.find_last_of(L"\\/");
        if (slash == std::wstring::npos || !FluentShell::EqualsIgnoreCase(module.substr(0, slash), system))
            return false;
        path = module;
    } else {
        path = std::wstring(system) + L"\\" + module;
    }
    const auto identifier = [](const std::wstring& part, bool decimalNumber, LPCWSTR& value) {
        const bool hashed = part[0] == L'#';
        const std::wstring_view digits = hashed ? std::wstring_view(part).substr(1) : std::wstring_view(part);
        const bool numeric = !digits.empty() && digits.size() <= 5 &&
            std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        if (hashed || (decimalNumber && numeric)) {
            if (!numeric) return false;
            const unsigned long number = std::wcstoul(std::wstring(digits).c_str(), nullptr, 10);
            if (number == 0 || number > 0xffff) return false;
            value = MAKEINTRESOURCEW(number);
            return true;
        }
        value = part.c_str();
        return true;
    };
    LPCWSTR type = MAKEINTRESOURCEW(23);
    LPCWSTR name = nullptr;
    if ((parts.size() == 2 && !identifier(parts[0], true, type)) || !identifier(parts.back(), false, name))
        return false;
    const HMODULE handle = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!handle) return false;
    struct Library final { HMODULE handle; ~Library() { FreeLibrary(handle); } } library{handle};
    const HRSRC info = FindResourceW(handle, name, type);
    const DWORD size = info ? SizeofResource(handle, info) : 0;
    const HGLOBAL loaded = info ? LoadResource(handle, info) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0 || size > kMaxResourceImageBytes) return false;
    bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    return true;
}

// Decodes an encoded image (GIF, PNG, BMP, JPEG, ICO) to premultiplied BGRA at exactly
// `width` x `height`, scaling as the layout box did.
bool DecodeImageBytes(const std::vector<uint8_t>& bytes, UINT width, UINT height, std::vector<uint8_t>& pixels) {
    pixels.clear();
    if (bytes.empty() || width == 0 || height == 0) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(CoCreateInstance(kWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), kWicPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&sourceWidth, &sourceHeight))) return false;
    ComPtr<IWICBitmapSource> source = converter;
    if (sourceWidth != width || sourceHeight != height) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(converter.Get(), width, height, WICBitmapInterpolationModeFant))) return false;
        source = scaler;
    }
    pixels.resize(static_cast<size_t>(width) * height * 4);
    if (FAILED(source->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
        pixels.clear();
        return false;
    }
    return true;
}

// An image the document takes from a resource of a trusted System32 module (Performance
// Monitor's res://wdc.dll/perfmon.gif) is decoded from that resource rather than read
// back from the host's paint, so it is exact whatever the scroll position, the cloak or
// the host's print path.  `element` is the <img> itself or a link whose only content is
// one; the image is placed at its own layout box inside the item's box.
bool DecodeDocumentImage(IHTMLElement* element, const std::wstring& tag, const RECT& itemRect,
    ImageListEntry& image) {
    ComPtr<IHTMLElement> imageElement;
    if (tag == L"IMG") {
        imageElement = element;
    } else {
        ComPtr<IDispatch> childrenDispatch;
        ComPtr<IHTMLElementCollection> children;
        long count = 0;
        if (FAILED(element->get_children(&childrenDispatch)) || FAILED(childrenDispatch.As(&children)) ||
            FAILED(children->get_length(&count)) || count != 1) return false;
        VARIANT at{}, unused{}; at.vt = VT_I4; at.lVal = 0;
        ComPtr<IDispatch> childDispatch;
        Text childTag;
        if (FAILED(children->item(at, unused, &childDispatch)) || !childDispatch ||
            FAILED(childDispatch.As(&imageElement)) || FAILED(imageElement->get_tagName(&childTag.value)) ||
            childTag.String() != L"IMG") return false;
    }
    ComPtr<IHTMLImgElement> img;
    ComPtr<IHTMLElement2> geometry;
    Text source;
    RECT imageRect{};
    if (FAILED(imageElement.As(&img)) || FAILED(img->get_src(&source.value)) ||
        FAILED(imageElement.As(&geometry)) || !Bounds(geometry.Get(), imageRect)) return false;
    const int itemWidth = itemRect.right - itemRect.left;
    const int itemHeight = itemRect.bottom - itemRect.top;
    const int width = imageRect.right - imageRect.left;
    const int height = imageRect.bottom - imageRect.top;
    if (itemWidth <= 0 || itemHeight <= 0 || width <= 0 || height <= 0 ||
        itemWidth > static_cast<int>(Ipc::kMaxImageDimension) ||
        itemHeight > static_cast<int>(Ipc::kMaxImageDimension) || !Inside(imageRect, itemRect)) return false;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> decoded;
    if (!ReadSystemResource(source.String(), bytes) ||
        !DecodeImageBytes(bytes, static_cast<UINT>(width), static_cast<UINT>(height), decoded)) return false;
    image.imageWidth = static_cast<uint32_t>(itemWidth);
    image.imageHeight = static_cast<uint32_t>(itemHeight);
    image.imageFormat = L"bgra8-premultiplied";
    image.imageData.assign(static_cast<size_t>(itemWidth) * itemHeight * 4, 0);
    for (int row = 0; row < height; ++row) {
        const size_t to = (static_cast<size_t>(imageRect.top - itemRect.top + row) * itemWidth +
            static_cast<size_t>(imageRect.left - itemRect.left)) * 4;
        std::memcpy(image.imageData.data() + to, decoded.data() + static_cast<size_t>(row) * width * 4,
            static_cast<size_t>(width) * 4);
    }
    return true;
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
    // A VT_DISPATCH value already is the object's IDispatch. The System Monitor's counter
    // items refuse IUnknown::QueryInterface(IID_IDispatch) on it, so it is used as delivered.
    if (value.vt == VT_DISPATCH) {
        result = value.pdispVal;
        return result != nullptr;
    }
    return value.vt == VT_UNKNOWN && value.punkVal &&
        SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&result))) && result;
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
// A report's Counters collection normally answers Item(index) for 1..Count. When an index
// answers with no object, the collection's _NewEnum enumeration is the second source: it
// walks the same counters in order. `detail` says what each source returned when neither
// yields Count counters.
bool CollectCounters(IDispatch* counters, long count, std::vector<ComPtr<IDispatch>>& result,
    std::wstring& detail) {
    result.clear();
    std::wstring byIndex;
    for (long index = 1; index <= count; ++index) {
        VARIANT at{}; at.vt = VT_I4; at.lVal = index;
        Variant value;
        ComPtr<IDispatch> counter;
        if (!GetProperty(counters, L"Item", value, &at)) {
            byIndex = L"Item(" + std::to_wstring(index) + L") could not be fetched";
            break;
        }
        if (!DispatchValue(value.value, counter)) {
            byIndex = L"Item(" + std::to_wstring(index) + L") returned variant type " +
                std::to_wstring(value.value.vt) +
                (value.value.vt == VT_DISPATCH && !value.value.pdispVal ? L" holding no object" : L"");
            break;
        }
        result.push_back(std::move(counter));
    }
    if (byIndex.empty()) return true;
    result.clear();
    DISPPARAMS none{};
    Variant enumerator;
    ComPtr<IUnknown> unknown;
    ComPtr<IEnumVARIANT> walk;
    if (FAILED(counters->Invoke(DISPID_NEWENUM, IID_NULL, LOCALE_USER_DEFAULT,
            DISPATCH_METHOD | DISPATCH_PROPERTYGET, &none, &enumerator.value, nullptr, nullptr)) ||
        (enumerator.value.vt != VT_UNKNOWN && enumerator.value.vt != VT_DISPATCH)) {
        detail = byIndex + L"; the _NewEnum enumeration is unavailable";
        return false;
    }
    unknown = enumerator.value.vt == VT_UNKNOWN ? enumerator.value.punkVal : enumerator.value.pdispVal;
    if (!unknown || FAILED(unknown.As(&walk))) {
        detail = byIndex + L"; the _NewEnum enumeration is not an IEnumVARIANT";
        return false;
    }
    walk->Reset();
    while (result.size() <= static_cast<size_t>(count)) {
        Variant element;
        ULONG fetched = 0;
        if (walk->Next(1, &element.value, &fetched) != S_OK || fetched != 1) break;
        ComPtr<IDispatch> counter;
        if (!DispatchValue(element.value, counter)) {
            detail = byIndex + L"; _NewEnum element " + std::to_wstring(result.size() + 1) +
                L" is variant type " + std::to_wstring(element.value.vt) +
                (element.value.vt == VT_DISPATCH && !element.value.pdispVal ? L" holding no object" : L"");
            result.clear();
            return false;
        }
        result.push_back(std::move(counter));
    }
    if (result.size() == static_cast<size_t>(count)) return true;
    detail = byIndex + L"; _NewEnum yielded " + std::to_wstring(result.size()) + L" counters";
    result.clear();
    return false;
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
    // The object's IOleWindow can answer with its OLE site window (class Hatchwin) rather
    // than the control's own: when that site frames exactly one visible SysmonCtrl, the
    // control is the report root and the site becomes its first host ancestor.
    int siteChildren = -1;
    if (root && GetClassNameW(root, rootClass, 64) && wcscmp(rootClass, L"Hatchwin") == 0) {
        HWND control = nullptr;
        int visibleChildren = 0;
        for (HWND child = GetWindow(root, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            ++visibleChildren;
            wchar_t childClass[64]{};
            if (GetClassNameW(child, childClass, 64) && wcscmp(childClass, L"SysmonCtrl") == 0) control = child;
        }
        siteChildren = visibleChildren;
        if (visibleChildren == 1 && control) root = control;
    }
    if (!document || !IsChild(document, root))
        return Reject(reason, L"native report root is not inside the document window");
    if (!GetClassNameW(root, rootClass, 64) || wcscmp(rootClass, L"SysmonCtrl") != 0)
        return Reject(reason, std::wstring(L"native report root is a '") + rootClass +
            L"' window, not SysmonCtrl" + (siteChildren >= 0
                ? L" (its site frames " + std::to_wstring(siteChildren) + L" visible window(s), not one SysmonCtrl)"
                : std::wstring()));
    if ((GetWindowLongPtrW(root, GWL_STYLE) & WS_TABSTOP) != 0)
        return Reject(reason, L"native report root is a keyboard tab stop");
    if (!WindowClassRegisteredBySystemModule(root, L"sysmon.ocx")) {
        wchar_t module[MAX_PATH]{};
        const auto handle = reinterpret_cast<HMODULE>(GetClassLongPtrW(root, GCLP_HMODULE));
        if (!handle || !GetModuleFileNameW(handle, module, MAX_PATH)) wcscpy_s(module, L"(unknown module)");
        return Reject(reason, std::wstring(L"native report root's class is registered by ") + module +
            L", not System32 sysmon.ocx");
    }
    struct FindReport {
        HWND value = nullptr; HWND root = nullptr; int count = 0; bool complete = true;
        wchar_t other[64]{};
    } found{nullptr, root};
    EnumChildWindows(root, [](HWND child, LPARAM raw) -> BOOL {
        auto& result = *reinterpret_cast<FindReport*>(raw);
        if (!IsWindowVisible(child)) return TRUE;
        wchar_t name[64]{};
        if (GetParent(child) == result.root && GetClassNameW(child, name, 64) &&
            FluentShell::EqualsIgnoreCase(name, L"SysmonReport") &&
            (GetWindowLongPtrW(child, GWL_STYLE) & WS_TABSTOP) == 0 &&
            WindowClassRegisteredBySystemModule(child, L"sysmon.ocx")) {
            result.value = child; ++result.count;
        } else {
            if (result.complete) {
                GetClassNameW(child, result.other, 64);
                if (!result.other[0]) wcscpy_s(result.other, L"(unnamed)");
            }
            result.complete = false;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    if (!found.complete)
        return Reject(reason, std::wstring(L"native report hosts a visible '") + found.other +
            L"' window besides its SysmonReport");
    if (found.count != 1)
        return Reject(reason, L"native report hosts " + std::to_wstring(found.count) +
            L" SysmonReport windows instead of one");
    if (!ReadScroll(found.value, region))
        return Reject(reason, L"native report scroll range is unavailable");
    report = found.value;
    represented.push_back(root);
    represented.push_back(report);
    for (HWND parent = GetParent(root); parent && parent != document; parent = GetParent(parent)) {
        wchar_t name[64]{};
        GetClassNameW(parent, name, 64);
        const bool named = IsChild(document, parent) && wcscmp(name, L"Hatchwin") == 0;
        // sysmon.ocx registers its own hatch-window class for its in-place UI, so the
        // control's component is as trusted as the report root it hosts.
        const bool trusted = WindowClassRegisteredBySystemModule(parent, L"ole32.dll") ||
            WindowClassRegisteredBySystemModule(parent, L"combase.dll") ||
            WindowClassRegisteredBySystemModule(parent, L"mshtml.dll") ||
            WindowClassRegisteredBySystemModule(parent, L"sysmon.ocx");
        if (!named || (GetWindowLongPtrW(parent, GWL_STYLE) & WS_TABSTOP) != 0 || !trusted) {
            wchar_t module[MAX_PATH]{};
            const auto handle = reinterpret_cast<HMODULE>(GetClassLongPtrW(parent, GCLP_HMODULE));
            if (!handle || !GetModuleFileNameW(handle, module, MAX_PATH)) wcscpy_s(module, L"(unknown module)");
            return Reject(reason, std::wstring(L"native report has an unrepresented native host ancestor: '") +
                (name[0] ? name : L"?") + L"' registered by " + module);
        }
        represented.push_back(parent);
    }
    Variant countersValue;
    ComPtr<IDispatch> counters;
    long count = 0;
    if (!GetProperty(object.Get(), L"Counters", countersValue) ||
        !DispatchValue(countersValue.value, counters) || !LongProperty(counters.Get(), L"Count", count) ||
        count < 0 || count > kMaxCounters)
        return Reject(reason, L"native report counter collection exceeds its readable bound");
    std::vector<ComPtr<IDispatch>> collected;
    std::wstring collectDetail;
    if (!CollectCounters(counters.Get(), count, collected, collectDetail))
        return Reject(reason, L"native report counters are not automation objects (" +
            std::to_wstring(count) + L" counted; " + collectDetail + L")");
    for (long index = 1; index <= count; ++index) {
        Variant path, value, number;
        const auto& counter = collected[static_cast<size_t>(index - 1)];
        bool visible = false;
        const std::wstring which = L"native report counter " + std::to_wstring(index) + L" of " + std::to_wstring(count);
        Variant visibleValue;
        if (!GetProperty(counter.Get(), L"Visible", visibleValue))
            return Reject(reason, which + L" has no readable Visible property");
        if (visibleValue.value.vt != VT_BOOL)
            return Reject(reason, which + L" reports Visible as variant type " +
                std::to_wstring(visibleValue.value.vt) + L", not a boolean");
        visible = visibleValue.value.boolVal != VARIANT_FALSE;
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

// MMC's own page chrome in views.htm: the notch icon beside the title (a decorative
// control) and the event sink that forwards selection changes. Neither carries content
// or offers an action, so the document does not have to account for them.
bool InertMmcObject(IHTMLElement* element) {
    ComPtr<IHTMLObjectElement> object;
    Text id, classId;
    if (FAILED(element->QueryInterface(IID_PPV_ARGS(&object))) || FAILED(element->get_id(&id.value)) ||
        FAILED(object->get_classid(&classId.value))) return false;
    return (id.String() == L"FolderIcon" &&
            FluentShell::EqualsIgnoreCase(classId.String(), L"clsid:B0395DA5-6A15-4E44-9F36-9A9DC7A2F341")) ||
        (id.String() == L"MMCEvents" &&
            FluentShell::EqualsIgnoreCase(classId.String(), L"clsid:ADE6444B-C91F-4e37-92A4-5BB430A33340"));
}

struct TextRun final {
    std::wstring text;
    RECT rect{};
    size_t ordinal = 0;
};

// The text an element holds directly beside inline children that carry text of their own
// (views.htm's "<a>Start</a> the service": the verb is a link, the rest is loose text).
// Each run is measured with a text range bounded by its neighbouring elements, so its box
// is where the page laid that text out.
bool ReadLooseText(IHTMLDocument2* document, IHTMLElement* parent, std::vector<TextRun>& runs,
    std::wstring& reason) {
    runs.clear();
    ComPtr<IHTMLDOMNode> node;
    ComPtr<IDispatch> childDispatch;
    ComPtr<IHTMLDOMChildrenCollection> children;
    long count = 0;
    if (FAILED(parent->QueryInterface(IID_PPV_ARGS(&node))) || FAILED(node->get_childNodes(&childDispatch)) ||
        FAILED(childDispatch.As(&children)) || FAILED(children->get_length(&count)) || count < 0 || count > 64)
        return Reject(reason, L"native document inline content is unavailable or too long");
    struct Child final {
        ComPtr<IHTMLElement> element;
        std::wstring text;
        bool isText = false;
    };
    std::vector<Child> kids;
    for (long index = 0; index < count; ++index) {
        ComPtr<IDispatch> dispatch;
        ComPtr<IHTMLDOMNode> child;
        long type = 0;
        if (FAILED(children->item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&child)) ||
            FAILED(child->get_nodeType(&type)))
            return Reject(reason, L"native document inline child is unavailable");
        Child entry;
        if (type == 3) {
            Variant value;
            if (FAILED(child->get_nodeValue(&value.value)) || value.value.vt != VT_BSTR)
                return Reject(reason, L"native document text node has no value");
            entry.isText = true;
            if (value.value.bstrVal) entry.text.assign(value.value.bstrVal, SysStringLen(value.value.bstrVal));
        } else if (type == 1) {
            if (FAILED(dispatch.As(&entry.element))) return Reject(reason, L"native document inline element is unavailable");
        } else {
            continue;
        }
        kids.push_back(std::move(entry));
    }
    // Most containers hold no text of their own; they need no text range.
    if (std::none_of(kids.begin(), kids.end(), [](const Child& child) {
            return child.isText && !Compact(child.text).empty();
        })) return true;
    ComPtr<IHTMLElement> bodyElement;
    ComPtr<IHTMLBodyElement> body;
    if (FAILED(document->get_body(&bodyElement)) || FAILED(bodyElement.As(&body)))
        return Reject(reason, L"native document text ranges are unavailable");
    const auto rangeOf = [&](IHTMLElement* element, ComPtr<IHTMLTxtRange>& range) {
        range.Reset();
        return SUCCEEDED(body->createTextRange(&range)) && range && SUCCEEDED(range->moveToElementText(element));
    };
    ComPtr<IHTMLTxtRange> whole;
    if (!rangeOf(parent, whole)) return Reject(reason, L"native document inline content has no text range");
    ComPtr<IHTMLElement> previous;
    size_t index = 0;
    while (index < kids.size()) {
        if (!kids[index].isText) {
            previous = kids[index].element;
            ++index;
            continue;
        }
        std::wstring text;
        while (index < kids.size() && kids[index].isText) text += kids[index++].text;
        const ComPtr<IHTMLElement> next = index < kids.size() ? kids[index].element : nullptr;
        if (Compact(text).empty()) continue;
        ComPtr<IHTMLTxtRange> range, boundary;
        if (FAILED(whole->duplicate(&range)) || !range)
            return Reject(reason, L"native document text run has no text range");
        if (previous && (!rangeOf(previous.Get(), boundary) ||
                FAILED(range->setEndPoint(Text(L"StartToEnd").value, boundary.Get()))))
            return Reject(reason, L"native document text run has no start boundary");
        if (next && (!rangeOf(next.Get(), boundary) ||
                FAILED(range->setEndPoint(Text(L"EndToStart").value, boundary.Get()))))
            return Reject(reason, L"native document text run has no end boundary");
        ComPtr<IHTMLTextRangeMetrics2> metrics;
        ComPtr<IHTMLRect> bounds;
        TextRun run;
        if (FAILED(range.As(&metrics)) || FAILED(metrics->getBoundingClientRect(&bounds)) || !bounds ||
            FAILED(bounds->get_left(&run.rect.left)) || FAILED(bounds->get_top(&run.rect.top)) ||
            FAILED(bounds->get_right(&run.rect.right)) || FAILED(bounds->get_bottom(&run.rect.bottom)) ||
            run.rect.right <= run.rect.left || run.rect.bottom <= run.rect.top)
            return Reject(reason, L"native document text run has no layout box");
        // Whitespace around a run is layout, not content: the box already excludes it.
        const size_t first = text.find_first_not_of(L" \t\r\n");
        const size_t last = text.find_last_not_of(L" \t\r\n");
        run.text = text.substr(first, last - first + 1);
        run.ordinal = runs.size();
        runs.push_back(std::move(run));
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
        if (FAILED(onclick.value.pdispVal->QueryInterface(IID_PPV_ARGS(&binding.handler))))
            return Reject(reason, L"native document link handler identity is unavailable");
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
        size_t pixelBytes = 0, reportRows = 0, looseRuns = 0;
        // The common end of every semantic item: its box is placed in its scroll region or
        // the viewport, its text is bounded, and the text it stands for is accounted.
        const auto commit = [&](NativeDocumentItem item, const std::wstring& representedText) {
            if (item.regionIndex >= 0) {
                const auto& region = snapshot.regions[item.regionIndex];
                OffsetRect(&item.rect, region.offsetX - region.rect.left, region.offsetY - region.rect.top);
                if (!Inside(item.rect, RECT{0, 0, region.contentWidth, region.contentHeight}))
                    return Reject(reason, L"native document semantic element escapes its scroll content");
            } else if (!Inside(item.rect, viewport)) {
                return Reject(reason, L"native document semantic element escapes its viewport");
            }
            if (!BoundedText(item.name) || (textSize += item.name.size()) > kMaxDocumentText)
                return Reject(reason, L"native document semantic text exceeds its bound");
            represented += representedText;
            snapshot.items.push_back(std::move(item));
            return true;
        };
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
                if (FAILED(parent->get_parentElement(&nextParent)))
                    return Reject(reason, L"native document ancestry is unavailable");
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
            if (value.tag == L"OBJECT" && InertMmcObject(value.element.Get())) continue;
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
            // A box styled to scroll whose content fits and is not scrolled -- the BODY of
            // a quirks-mode page such as Performance Monitor's overview -- moves nothing,
            // so it is not a region; its descendants keep viewport coordinates.
            const bool overflowing = scrollWidth > clientWidth || scrollHeight > clientHeight || x != 0 || y != 0;
            const bool scrollport = (scrolls(overflowX.String()) || scrolls(overflowY.String())) &&
                clientWidth > 0 && clientHeight > 0 && overflowing;
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
                    FAILED(childrenDispatch.As(&children)) || FAILED(children->get_length(&childCount)))
                    return Reject(reason, L"native document text element children are unavailable");
                for (long childIndex = 0; childIndex < childCount; ++childIndex) {
                    VARIANT at{}, unused{}; at.vt = VT_I4; at.lVal = childIndex;
                    ComPtr<IDispatch> childDispatch; ComPtr<IHTMLElement> child; Text childText;
                    if (FAILED(children->item(at, unused, &childDispatch)) || !childDispatch ||
                        FAILED(childDispatch.As(&child)) || FAILED(child->get_innerText(&childText.value)))
                        return Reject(reason, L"native document text element child text is unavailable");
                    if (!Compact(childText.String()).empty()) { textLeaf = false; break; }
                }
            }
            if (!link && !graphic && !object && !textLeaf) {
                if (Compact(value.text).empty()) continue;
                // Mixed content: an inline child carries text, and the element may hold more
                // loose text beside it. Each run of that text is its own item.
                std::vector<TextRun> runs;
                if (!ReadLooseText(document, value.element.Get(), runs, reason)) return false;
                for (const auto& run : runs) {
                    NativeDocumentItem loose;
                    loose.key = value.key + L":" + std::to_wstring(run.ordinal);
                    loose.kind = L"text";
                    loose.rect = run.rect;
                    loose.regionIndex = value.region;
                    loose.name = run.text;
                    if (!commit(std::move(loose), Compact(run.text))) return false;
                    ++looseRuns;
                }
                continue;
            }
            NativeDocumentItem item;
            item.key = value.key; item.rect = value.rect; item.regionIndex = value.region;
            item.kind = link ? L"link" : graphic ? L"image" : object ? L"report" : L"text";
            if (!link && value.tag.size() == 2 && value.tag[0] == L'H' &&
                value.tag[1] >= L'1' && value.tag[1] <= L'6') item.kind = L"heading";
            item.name = value.text;
            VARIANT_BOOL disabled = VARIANT_FALSE;
            if (FAILED(element3->get_disabled(&disabled)))
                return Reject(reason, L"native document element enabled state is unavailable");
            item.enabled = disabled == VARIANT_FALSE;
            if (item.rect.right <= item.rect.left || item.rect.bottom <= item.rect.top)
                return Reject(reason, L"native document semantic element has no layout box");
            if (link && !ReadAction(value.element.Get(), value.key, next,
                previous ? &*previous : nullptr, item.actionIdentity, reason)) return false;
            if (graphic || (link && Compact(value.text).empty())) {
                ImageListEntry decoded;
                if (DecodeDocumentImage(value.element.Get(), value.tag, value.rect, decoded)) {
                    item.image = std::move(decoded);
                } else {
                    DirectUiWindowEvidence pixels;
                    if (!window || !Inside(value.rect, viewport) ||
                        !CaptureDirectUiVirtualImagePixels(window, value.rect, pixels, reason)) {
                        const std::wstring evidence =
                            L"native document image is outside its current paintable viewport (" + value.tag +
                            L" at " + std::to_wstring(value.rect.left) + L"," + std::to_wstring(value.rect.top) +
                            L"-" + std::to_wstring(value.rect.right) + L"," + std::to_wstring(value.rect.bottom) +
                            L"; viewport " + std::to_wstring(viewport.right) + L"x" +
                            std::to_wstring(viewport.bottom) + L"): " + reason;
                        return Reject(reason, evidence);
                    }
                    item.image = ImageListEntry{pixels.imageWidth, pixels.imageHeight, pixels.imageFormat,
                        std::move(pixels.imageData)};
                }
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
                if (value.region >= 0)
                    return Reject(reason, L"native document report inside a scrollport is unsupported");
                if (snapshot.regions.size() >= kMaxRegions)
                    return Reject(reason, L"native document has more regions than the adapter carries");
                if (!Inside(value.rect, viewport))
                    return Reject(reason, L"native document report is outside its viewport (" +
                        std::to_wstring(value.rect.left) + L"," + std::to_wstring(value.rect.top) + L"-" +
                        std::to_wstring(value.rect.right) + L"," + std::to_wstring(value.rect.bottom) +
                        L"; viewport " + std::to_wstring(viewport.right) + L"x" + std::to_wstring(viewport.bottom) + L")");
                if (FAILED(value.element.As(&embedded)))
                    return Reject(reason, L"native document embedded object is unavailable");
                if (!ReadReport(embedded.Get(), item, region, window, nativeReport,
                        next.representedWindows, reason)) return false;
                item.reportRegionIndex = static_cast<int>(snapshot.regions.size());
                snapshot.regions.push_back(std::move(region));
                next.regions.emplace(value.key, RegionBinding{nullptr, nativeReport});
                if ((reportRows += item.reportRows.size()) > kMaxCounters)
                    return Reject(reason, L"native document report rows exceed their aggregate bound");
                for (const auto& row : item.reportRows) textSize += row.path.size();
            }
            if (!commit(std::move(item), graphic || object ? std::wstring() : Compact(value.text))) return false;
        }
        Text bodyText;
        if (FAILED(body->get_innerText(&bodyText.value)))
            return Reject(reason, L"native document body text is unavailable");
        std::wstring expected = Compact(bodyText.String());
        // Loose runs are gathered with the element that holds them, ahead of the inline
        // children that come before them in reading order, so with runs only the
        // multiset of characters can be compared; without runs the order must match too.
        if (looseRuns != 0) {
            std::sort(represented.begin(), represented.end());
            std::sort(expected.begin(), expected.end());
        }
        if (represented != expected || snapshot.items.empty())
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
