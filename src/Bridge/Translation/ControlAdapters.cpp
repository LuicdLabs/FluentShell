#include "ControlAdapters.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <utility>

namespace FluentShell::Bridge::Translation {
namespace {

constexpr size_t kMaxListViewColumns = 64;
constexpr size_t kMaxStatusBarParts = 64;
constexpr size_t kMaxStructuredTextChars = 256 * 1024;
constexpr size_t kMaxTabLabelChars = 4096;

struct IconBitmaps final {
    ICONINFO info{};
    ~IconBitmaps() {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
};

// Every adapter reports refusals the same way, so the rejection reason and the
// `false` return can never drift apart.
bool Reject(std::wstring& reason, const wchar_t* text) {
    reason = text;
    return false;
}

// ---------------------------------------------------------------------------
// Match(class) + Probe(styles)
//
// One function per native class.  Each receives the class's own style bits and
// either names the projected kind or rejects with a specific reason.
// ---------------------------------------------------------------------------

bool ProbeStatic(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    switch (style & SS_TYPEMASK) {
    case SS_LEFT:
    case SS_CENTER:
    case SS_RIGHT:
    case SS_SIMPLE:
    case SS_LEFTNOWORDWRAP:
        kind = ControlKind::StaticText;
        return true;
    case SS_ETCHEDHORZ:
    case SS_ETCHEDVERT:
        kind = ControlKind::Separator;
        return true;
    case SS_ICON:
        if ((style & (SS_NOTIFY | WS_TABSTOP)) != 0) {
            return Reject(reason, L"interactive Static icon is not supported");
        }
        kind = ControlKind::StaticIcon;
        return true;
    default:
        return Reject(reason, L"unsupported Static draw style");
    }
}

bool ProbeButton(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & BS_TYPEMASK) == BS_OWNERDRAW || (style & (BS_BITMAP | BS_ICON)) != 0) {
        return Reject(reason, L"unsupported Button draw style");
    }
    switch (style & BS_TYPEMASK) {
    case BS_PUSHBUTTON:
    case BS_DEFPUSHBUTTON:
        kind = ControlKind::Button;
        return true;
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
        kind = ControlKind::CheckBox;
        return true;
    case BS_3STATE:
    case BS_AUTO3STATE:
        kind = ControlKind::ThreeState;
        return true;
    case BS_RADIOBUTTON:
    case BS_AUTORADIOBUTTON:
        kind = ControlKind::RadioButton;
        return true;
    case BS_GROUPBOX:
        if ((style & WS_TABSTOP) != 0) {
            return Reject(reason, L"tab-stop GroupBox is not supported");
        }
        kind = ControlKind::GroupBox;
        return true;
    default:
        return Reject(reason, L"unsupported Button type");
    }
}

bool ProbeEdit(HWND, DWORD style, ControlKind& kind, std::wstring&) {
    kind = (style & ES_PASSWORD) != 0 ? ControlKind::Password : ControlKind::Edit;
    return true;
}

bool ProbeComboBox(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0) {
        return Reject(reason, L"owner-draw ComboBox");
    }
    const DWORD type = style & 0x0003u;
    if (type != CBS_DROPDOWNLIST &&
        (type != CBS_DROPDOWN || (style & CBS_HASSTRINGS) == 0)) {
        return Reject(reason,
            L"ComboBox is simple, non-string-backed, or has an unsupported type");
    }
    kind = ControlKind::ComboBox;
    return true;
}

bool ProbeListBox(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    constexpr DWORD unsupported = LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE |
        LBS_NODATA | LBS_MULTIPLESEL | LBS_EXTENDEDSEL | LBS_MULTICOLUMN | LBS_NOSEL;
    if ((style & unsupported) != 0) {
        return Reject(reason,
            L"owner-draw, virtual, multi-select, or multi-column ListBox");
    }
    kind = ControlKind::ListBox;
    return true;
}

bool ProbeBitmapDisplay(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & (WS_TABSTOP | SS_NOTIFY)) != 0)
        return Reject(reason, L"interactive BitmapDisplayClass is not supported");
    kind = ControlKind::StaticIcon;
    return true;
}

bool ProbeBitmapSwitch(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & SS_NOTIFY) != 0)
        return Reject(reason, L"callback BitmapSwitchClass is not supported");
    kind = ControlKind::RadioButton;
    return true;
}

bool ProbeMonitorPalette(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & SS_NOTIFY) != 0)
        return Reject(reason, L"callback MonitorPaletteClass is not supported");
    kind = ControlKind::StaticIcon;
    return true;
}

bool ProbeProgressBar(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & (PBS_VERTICAL | WS_TABSTOP)) != 0) {
        return Reject(reason,
            L"vertical or tab-stop ProgressBar is not supported");
    }
    kind = ControlKind::ProgressBar;
    return true;
}

bool ProbeSysLink(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    constexpr DWORD unsupported = LWS_IGNORERETURN | LWS_USECUSTOMTEXT |
        LWS_RIGHT | LWS_NOPREFIX;
    if ((style & unsupported) != 0) {
        return Reject(reason,
            L"SysLink requires unsupported callback, alignment, prefix, or keyboard semantics");
    }
    kind = ControlKind::SysLink;
    return true;
}

bool ProbeListView(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & LVS_TYPEMASK) != LVS_REPORT ||
        (style & (LVS_OWNERDATA | LVS_OWNERDRAWFIXED | LVS_EDITLABELS)) != 0) {
        return Reject(reason,
            L"ListView is virtual, owner-draw, label-editable, or not in report view");
    }
    if (SendMessageW(hwnd, LVM_ISGROUPVIEWENABLED, 0, 0) != FALSE) {
        return Reject(reason, L"grouped ListView is not supported");
    }
    constexpr DWORD unsupportedExtended = LVS_EX_HEADERDRAGDROP | LVS_EX_TRACKSELECT |
        LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
    const DWORD rejected = extended & unsupportedExtended;
    if (rejected != 0) {
        struct ExtendedFlag final { DWORD value; const wchar_t* name; };
        static constexpr ExtendedFlag flags[] = {
            { LVS_EX_HEADERDRAGDROP, L"LVS_EX_HEADERDRAGDROP" },
            { LVS_EX_TRACKSELECT, L"LVS_EX_TRACKSELECT" },
            { LVS_EX_ONECLICKACTIVATE, L"LVS_EX_ONECLICKACTIVATE" },
            { LVS_EX_TWOCLICKACTIVATE, L"LVS_EX_TWOCLICKACTIVATE" },
        };
        reason = L"ListView has unsupported extended flag(s) ";
        bool first = true;
        for (const auto& flag : flags) {
            if ((rejected & flag.value) == 0) continue;
            if (!first) reason += L"|";
            reason += flag.name;
            first = false;
        }
        wchar_t numeric[64]{};
        swprintf_s(numeric, L" (unsupported=0x%08lX, extended=0x%08lX)", rejected, extended);
        reason += numeric;
        return false;
    }
    kind = ControlKind::ListView;
    return true;
}

bool ProbeStatusBar(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_TABSTOP) != 0) {
        return Reject(reason, L"tab-stop StatusBar is not supported");
    }
    kind = ControlKind::StatusBar;
    return true;
}

bool ProbeToolbar(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (exStyle != 0) {
        wchar_t text[112]{};
        swprintf_s(text, L"ToolbarWindow32 requires exStyle 0 (actual=0x%08lX)", exStyle);
        reason = text;
        return false;
    }
    const DWORD toolbarStyle = style & 0xffffu;
    constexpr DWORD accepted = CCS_TOP | TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE |
        TBSTYLE_FLAT | TBSTYLE_TRANSPARENT;
    if ((toolbarStyle & ~accepted) != 0) {
        wchar_t text[128]{};
        swprintf_s(text, L"ToolbarWindow32 has unsupported style bits 0x%04lX (style=0x%08lX)",
            toolbarStyle & ~accepted, style);
        reason = text;
        return false;
    }
    const auto extended = static_cast<DWORD>(SendMessageW(hwnd, TB_GETEXTENDEDSTYLE, 0, 0));
    if (extended != 0) {
        wchar_t text[112]{};
        swprintf_s(text, L"ToolbarWindow32 has unsupported extended style 0x%08lX", extended);
        reason = text;
        return false;
    }
    if (SendMessageW(hwnd, TB_GETHOTIMAGELIST, 0, 0) != 0 ||
        SendMessageW(hwnd, TB_GETDISABLEDIMAGELIST, 0, 0) != 0 ||
        SendMessageW(hwnd, TB_GETPRESSEDIMAGELIST, 0, 0) != 0) {
        return Reject(reason, L"ToolbarWindow32 has unsupported hot, disabled, or pressed image lists");
    }
    kind = ControlKind::Toolbar;
    return true;
}

bool ProbeTabControl(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const DWORD unsupportedExStyle = exStyle & ~WS_EX_NOPARENTNOTIFY;
    if (unsupportedExStyle != 0) {
        wchar_t text[112]{};
        swprintf_s(text,
            L"TabControl has unsupported window exStyle bits 0x%08lX (exStyle=0x%08lX)",
            unsupportedExStyle, exStyle);
        reason = text;
        return false;
    }
    const DWORD tabStyle = style & 0xffffu;
    if ((tabStyle & TCS_OWNERDRAWFIXED) != 0)
        return Reject(reason, L"TabControl TCS_OWNERDRAWFIXED is not supported");
    if ((tabStyle & (TCS_BUTTONS | TCS_FLATBUTTONS)) != 0)
        return Reject(reason, L"TabControl button and flat-button modes are not supported");
    if ((tabStyle & (TCS_VERTICAL | TCS_BOTTOM)) != 0)
        return Reject(reason, L"TabControl vertical, right, and bottom placement is not supported");
    if ((tabStyle & TCS_FIXEDWIDTH) != 0)
        return Reject(reason, L"TabControl TCS_FIXEDWIDTH cannot be captured faithfully");
    if ((tabStyle & TCS_TOOLTIPS) != 0 ||
        SendMessageW(hwnd, TCM_GETTOOLTIPS, 0, 0) != 0)
        return Reject(reason, L"TabControl tooltips are not supported");

    constexpr DWORD accepted = TCS_MULTILINE | TCS_HOTTRACK;
    const DWORD unsupported = tabStyle & ~accepted;
    if (unsupported != 0) {
        wchar_t text[128]{};
        swprintf_s(text,
            L"TabControl has unsupported style bits 0x%04lX (tabStyle=0x%04lX)",
            unsupported, tabStyle);
        reason = text;
        return false;
    }
    const DWORD extended = static_cast<DWORD>(
        SendMessageW(hwnd, TCM_GETEXTENDEDSTYLE, 0, 0));
    if (extended != 0) {
        wchar_t text[96]{};
        swprintf_s(text, L"TabControl has unsupported extended style 0x%08lX", extended);
        reason = text;
        return false;
    }
    if (SendMessageW(hwnd, TCM_GETIMAGELIST, 0, 0) != 0)
        return Reject(reason, L"TabControl image-backed tabs are not supported");
    kind = ControlKind::TabControl;
    return true;
}

bool ProbeDialogContainer(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if ((style & (WS_CHILD | DS_CONTROL)) != (WS_CHILD | DS_CONTROL) ||
        (exStyle & WS_EX_CONTROLPARENT) == 0) {
        return Reject(reason, L"child dialog is not a DS_CONTROL navigation container");
    }
    constexpr DWORD unsupportedStyle = WS_CAPTION | WS_THICKFRAME | WS_BORDER |
        WS_DLGFRAME | WS_HSCROLL | WS_VSCROLL | WS_SYSMENU | DS_MODALFRAME;
    constexpr DWORD unsupportedExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
        WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_LAYERED;
    if ((style & unsupportedStyle) != 0 || (exStyle & unsupportedExStyle) != 0) {
        return Reject(reason, L"DS_CONTROL child dialog has unsupported visible chrome");
    }
    if (const HMENU menu = GetMenu(hwnd); menu && IsMenu(menu)) {
        return Reject(reason, L"DS_CONTROL child dialog has a menu");
    }
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int regionType = region ? GetWindowRgn(hwnd, region) : ERROR;
    if (region) DeleteObject(region);
    if (regionType != ERROR) {
        return Reject(reason, L"DS_CONTROL child dialog has a custom region");
    }
    kind = ControlKind::DialogContainer;
    return true;
}

using ProbeFn = bool (*)(HWND, DWORD, ControlKind&, std::wstring&);

struct ClassAdapter final {
    std::wstring_view className;
    ProbeFn probe;
};

// The registry. Class names select a candidate adapter; only its probe can
// establish support.
constexpr std::array kClassAdapters{
    ClassAdapter{ L"Static", &ProbeStatic },
    ClassAdapter{ L"BitmapDisplayClass", &ProbeBitmapDisplay },
    ClassAdapter{ L"BitmapSwitchClass", &ProbeBitmapSwitch },
    ClassAdapter{ L"MonitorPaletteClass", &ProbeMonitorPalette },
    ClassAdapter{ L"Button", &ProbeButton },
    ClassAdapter{ L"Edit", &ProbeEdit },
    ClassAdapter{ L"ComboBox", &ProbeComboBox },
    ClassAdapter{ L"ListBox", &ProbeListBox },
    ClassAdapter{ PROGRESS_CLASSW, &ProbeProgressBar },
    ClassAdapter{ WC_LINK, &ProbeSysLink },
    ClassAdapter{ WC_LISTVIEWW, &ProbeListView },
    ClassAdapter{ WC_TABCONTROLW, &ProbeTabControl },
    ClassAdapter{ L"#32770", &ProbeDialogContainer },
    ClassAdapter{ STATUSCLASSNAMEW, &ProbeStatusBar },
    ClassAdapter{ TOOLBARCLASSNAMEW, &ProbeToolbar },
};

// ---------------------------------------------------------------------------
// Capture(HWND) -> canonical typed state
//
// One function per projected kind, reading only what that kind adds on top of
// the facets every control shares.
// ---------------------------------------------------------------------------

bool ReadMaskBitmap(
    HBITMAP bitmap,
    int width,
    int height,
    std::vector<uint8_t>& bits,
    size_t& stride,
    std::wstring& reason) {
    stride = (static_cast<size_t>(width) + 31u) / 32u * 4u;
    if (height <= 0 || stride > Ipc::kMaxImageBytes ||
        static_cast<size_t>(height) > Ipc::kMaxImageBytes / stride) {
        return Reject(reason, L"Static icon mask dimensions are invalid");
    }
    bits.resize(stride * static_cast<size_t>(height));
    struct MonoBitmapInfo final {
        BITMAPINFOHEADER header{};
        RGBQUAD colors[2]{};
    } info;
    info.header.biSize = sizeof(BITMAPINFOHEADER);
    info.header.biWidth = width;
    info.header.biHeight = height;
    info.header.biPlanes = 1;
    info.header.biBitCount = 1;
    info.header.biCompression = BI_RGB;
    info.colors[1] = RGBQUAD{ 0xff, 0xff, 0xff, 0 };
    const HDC dc = GetDC(nullptr);
    const int rows = dc ? GetDIBits(dc, bitmap, 0, static_cast<UINT>(height),
        bits.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS) : 0;
    if (dc) ReleaseDC(nullptr, dc);
    if (rows != height) return Reject(reason, L"Static icon mask pixels are unavailable");
    return true;
}

bool MaskBit(
    const std::vector<uint8_t>& bits,
    size_t stride,
    int bitmapHeight,
    int x,
    int logicalY) noexcept {
    const size_t row = static_cast<size_t>(bitmapHeight - 1 - logicalY) * stride;
    return (bits[row + static_cast<size_t>(x) / 8] &
        (0x80u >> (static_cast<unsigned>(x) & 7u))) != 0;
}

bool CaptureIconPixels(
    HICON icon,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason) {
    IconBitmaps bitmaps;
    if (!GetIconInfo(icon, &bitmaps.info) || !bitmaps.info.hbmMask) {
        return Reject(reason, L"Static icon bitmap copies are unavailable");
    }
    BITMAP maskBitmap{};
    BITMAP colorBitmap{};
    if (GetObjectW(bitmaps.info.hbmMask, sizeof(maskBitmap), &maskBitmap) != sizeof(maskBitmap) ||
        (bitmaps.info.hbmColor &&
            GetObjectW(bitmaps.info.hbmColor, sizeof(colorBitmap), &colorBitmap) != sizeof(colorBitmap))) {
        return Reject(reason, L"Static icon bitmap metadata is unavailable");
    }
    const int width = bitmaps.info.hbmColor ? colorBitmap.bmWidth : maskBitmap.bmWidth;
    const int height = bitmaps.info.hbmColor ? colorBitmap.bmHeight : maskBitmap.bmHeight / 2;
    if (width <= 0 || height <= 0 || width > static_cast<int>(Ipc::kMaxImageDimension) ||
        height > static_cast<int>(Ipc::kMaxImageDimension) ||
        maskBitmap.bmWidth < width ||
        maskBitmap.bmHeight != height * (bitmaps.info.hbmColor ? 1 : 2)) {
        return Reject(reason, L"Static icon dimensions exceed the bounded payload");
    }
    const size_t byteCount = static_cast<size_t>(width) * height * 4u;
    if (byteCount == 0 || byteCount > Ipc::kMaxImageBytes) {
        return Reject(reason, L"Static icon decoded payload exceeds the byte cap");
    }

    std::vector<uint8_t> mask;
    size_t maskStride = 0;
    if (!ReadMaskBitmap(bitmaps.info.hbmMask, maskBitmap.bmWidth,
            maskBitmap.bmHeight, mask, maskStride, reason)) return false;

    imageData.assign(byteCount, 0);
    if (bitmaps.info.hbmColor) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const HDC dc = GetDC(nullptr);
        const int rows = dc ? GetDIBits(dc, bitmaps.info.hbmColor, 0,
            static_cast<UINT>(height), imageData.data(), &info, DIB_RGB_COLORS) : 0;
        if (dc) ReleaseDC(nullptr, dc);
        if (rows != height) return Reject(reason, L"Static icon color pixels are unavailable");

        bool anyAlpha = false;
        for (size_t offset = 3; offset < imageData.size(); offset += 4) {
            if (imageData[offset] != 0) { anyAlpha = true; break; }
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
                uint8_t& alpha = imageData[offset + 3];
                if (!anyAlpha) alpha = MaskBit(mask, maskStride, maskBitmap.bmHeight, x, y) ? 0 : 255;
                if (alpha == 0) {
                    imageData[offset] = imageData[offset + 1] =
                        imageData[offset + 2] = 0;
                } else if (anyAlpha && alpha != 255) {
                    for (size_t channel = 0; channel < 3; ++channel) {
                        imageData[offset + channel] = static_cast<uint8_t>(
                            (static_cast<unsigned>(imageData[offset + channel]) * alpha + 127u) / 255u);
                    }
                }
            }
        }
    } else {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool transparent = MaskBit(mask, maskStride, maskBitmap.bmHeight, x, y);
                const bool white = MaskBit(mask, maskStride, maskBitmap.bmHeight, x, y + height);
                if (transparent && white) {
                    return Reject(reason, L"Static monochrome icon contains unsupported invert pixels");
                }
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
                imageData[offset] = imageData[offset + 1] =
                    imageData[offset + 2] = white ? 255 : 0;
                imageData[offset + 3] = transparent ? 0 : 255;
                if (transparent) imageData[offset] = imageData[offset + 1] =
                    imageData[offset + 2] = 0;
            }
        }
    }
    imageWidth = static_cast<uint32_t>(width);
    imageHeight = static_cast<uint32_t>(height);
    imageFormat = L"bgra8-premultiplied";
    return true;
}

bool CaptureToggleState(HWND hwnd, ControlNode& node, std::wstring&);

bool CaptureWindowPixels(
    HWND hwnd,
    uint32_t maxDimension,
    size_t maxBytes,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason) {
    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return Reject(reason, L"bitmap display client bounds are unavailable");
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0 ||
        width > static_cast<int>(maxDimension) ||
        height > static_cast<int>(maxDimension)) {
        return Reject(reason, L"bitmap display dimensions are outside the protocol cap");
    }
    const size_t byteCount = static_cast<size_t>(width) * height * 4;
    if (byteCount == 0 || byteCount > maxBytes)
        return Reject(reason, L"bitmap display pixels exceed the protocol cap");

    HDC source = GetDC(hwnd);
    HDC memory = source ? CreateCompatibleDC(source) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(
        memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool copied = source && memory && bitmap && bits && previous &&
        (PrintWindow(hwnd, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != FALSE ||
         BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY | CAPTUREBLT) != FALSE);
    if (copied) {
        imageWidth = static_cast<uint32_t>(width);
        imageHeight = static_cast<uint32_t>(height);
        imageFormat = L"bgra8-premultiplied";
        const auto* first = static_cast<const uint8_t*>(bits);
        imageData.assign(first, first + byteCount);
        for (size_t offset = 3; offset < imageData.size(); offset += 4)
            imageData[offset] = 255;
    }
    if (previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (source) ReleaseDC(hwnd, source);
    return copied || Reject(reason, L"bitmap display pixels could not be copied");
}

bool CaptureBitmapSwitchState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    node.checked = (style & WS_TABSTOP) != 0 ? 1 : 0;
    node.groupStart = (style & WS_GROUP) != 0;
    return CaptureWindowPixels(hwnd, Ipc::kMaxDirectUiBitmapDimension,
        Ipc::kMaxDirectUiBitmapBytes, node.imageWidth, node.imageHeight,
        node.imageFormat, node.imageData, reason);
}

bool CaptureRadioState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    wchar_t className[kMaxClassNameChars]{};
    if (FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), L"BitmapSwitchClass"))
        return CaptureBitmapSwitchState(hwnd, node, reason);
    return CaptureToggleState(hwnd, node, reason);
}

bool CaptureStaticIconState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    wchar_t className[kMaxClassNameChars]{};
    if (FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), L"BitmapDisplayClass") ||
        FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), L"MonitorPaletteClass")) {
        return CaptureWindowPixels(hwnd, Ipc::kMaxDirectUiBitmapDimension,
            Ipc::kMaxDirectUiBitmapBytes, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData, reason);
    }
    HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, STM_GETICON, 0, 0));
    if (!icon) icon = reinterpret_cast<HICON>(SendMessageW(hwnd, STM_GETIMAGE, IMAGE_ICON, 0));
    if (!icon) return Reject(reason, L"Static icon has no current HICON");
    return CaptureIconPixels(icon, node.imageWidth, node.imageHeight,
        node.imageFormat, node.imageData, reason);
}

bool ReadStringItems(
    HWND hwnd,
    bool combo,
    std::vector<std::wstring>& items,
    std::wstring& reason) {
    const UINT countMessage = combo ? CB_GETCOUNT : LB_GETCOUNT;
    const UINT lengthMessage = combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
    const UINT textMessage = combo ? CB_GETLBTEXT : LB_GETTEXT;
    const LRESULT rawCount = SendMessageW(hwnd, countMessage, 0, 0);
    if (rawCount < 0 || rawCount > static_cast<LRESULT>(Ipc::kMaxListItems)) {
        return Reject(reason, L"invalid or excessive string item count");
    }
    items.reserve(static_cast<size_t>(rawCount));
    for (LRESULT index = 0; index < rawCount; ++index) {
        const LRESULT length = SendMessageW(hwnd, lengthMessage, index, 0);
        if (length < 0 || length > static_cast<LRESULT>(Ipc::kMaxStringChars)) {
            return Reject(reason, L"invalid string item length");
        }
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        const LRESULT copied = SendMessageW(
            hwnd, textMessage, index, reinterpret_cast<LPARAM>(text.data()));
        if (copied < 0) return Reject(reason, L"failed to read string item");
        text.resize(static_cast<size_t>(copied));
        items.push_back(std::move(text));
    }
    return true;
}

// Reads text through a message that reports the copied length, growing the
// buffer until the result provably fits.  Shared by the ListView cell and
// column readers, which differ only in the message they send.
template <typename Read>
bool ReadGrowingText(std::wstring& text, std::wstring& reason,
                     const wchar_t* overflowReason, Read&& read) {
    for (size_t capacity = 256; capacity <= Ipc::kMaxStringChars + 1;
         capacity = std::min(Ipc::kMaxStringChars + 1, capacity * 2)) {
        std::wstring buffer(capacity, L'\0');
        size_t length = 0;
        if (!read(buffer, length, reason)) return false;
        if (length + 1 < capacity) {
            buffer.resize(length);
            text = std::move(buffer);
            return true;
        }
        if (capacity == Ipc::kMaxStringChars + 1) break;
    }
    return Reject(reason, overflowReason);
}

bool ReadListViewCell(
    HWND hwnd,
    int row,
    int column,
    std::wstring& text,
    std::wstring& reason) {
    return ReadGrowingText(text, reason,
        L"ListView item text exceeds the protocol string limit",
        [&](std::wstring& buffer, size_t& length, std::wstring& error) {
            LVITEMW item{};
            item.iSubItem = column;
            item.pszText = buffer.data();
            item.cchTextMax = static_cast<int>(buffer.size());
            const int copied = static_cast<int>(SendMessageW(
                hwnd, LVM_GETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item)));
            if (copied < 0) return Reject(error, L"ListView item text read failed");
            length = static_cast<size_t>(copied);
            return true;
        });
}

bool ReadListViewColumn(
    HWND hwnd,
    int index,
    std::wstring& text,
    int& width,
    std::wstring& reason) {
    int captured = 0;
    const bool read = ReadGrowingText(text, reason,
        L"ListView column text exceeds the protocol string limit",
        [&](std::wstring& buffer, size_t& length, std::wstring& error) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = buffer.data();
            column.cchTextMax = static_cast<int>(buffer.size());
            if (!SendMessageW(hwnd, LVM_GETCOLUMNW, index,
                    reinterpret_cast<LPARAM>(&column))) {
                return Reject(error, L"ListView column read failed");
            }
            // LVM_GETCOLUMN reports success, not a copied length.
            length = static_cast<size_t>(
                std::find(buffer.begin(), buffer.end(), L'\0') - buffer.begin());
            captured = std::max(0, column.cx);
            return true;
        });
    if (read) width = captured;
    return read;
}

bool ValidateListViewColumns(HWND hwnd, HWND header, int columnCount, std::wstring& reason) {
    std::vector<int> columnOrder(static_cast<size_t>(columnCount), -1);
    if (!SendMessageW(hwnd, LVM_GETCOLUMNORDERARRAY, columnCount,
            reinterpret_cast<LPARAM>(columnOrder.data()))) {
        return Reject(reason, L"ListView column order read failed");
    }
    for (int index = 0; index < columnCount; ++index) {
        if (columnOrder[static_cast<size_t>(index)] != index) {
            return Reject(reason,
                L"reordered ListView columns are outside the bounded adapter");
        }
        HDITEMW headerItem{};
        headerItem.mask = HDI_FORMAT;
        constexpr int unsupportedFormat = HDF_OWNERDRAW | HDF_BITMAP |
            HDF_BITMAP_ON_RIGHT | HDF_IMAGE;
        if (!Header_GetItem(header, index, &headerItem) ||
            (headerItem.fmt & unsupportedFormat) != 0) {
            return Reject(reason,
                L"ListView header requires owner-draw, bitmap, or image semantics");
        }
    }
    return true;
}

// Enumerates selected rows through the native LVM_GETNEXTITEM walk, rejecting
// any enumeration that is not strictly increasing and inside `rowCount`.
bool ReadListViewSelection(
    HWND hwnd,
    size_t rowCount,
    std::vector<int>& selected,
    std::wstring& reason) {
    int previous = -1;
    for (;;) {
        const int index = static_cast<int>(
            SendMessageW(hwnd, LVM_GETNEXTITEM, previous, LVNI_SELECTED));
        if (index < 0) return true;
        if (index <= previous || static_cast<size_t>(index) >= rowCount ||
            selected.size() >= rowCount) {
            return Reject(reason, L"ListView selected index enumeration is invalid");
        }
        selected.push_back(index);
        previous = index;
    }
}

bool ReadListViewChecks(
    HWND hwnd,
    size_t itemCount,
    bool checkBoxes,
    std::vector<int>& checkedIndices,
    std::wstring& reason) {
    checkedIndices.clear();
    if (!checkBoxes) return true;
    checkedIndices.reserve(itemCount);
    for (size_t index = 0; index < itemCount; ++index) {
        const UINT stateImage = ListView_GetItemState(
            hwnd, static_cast<int>(index), LVIS_STATEIMAGEMASK) >> 12;
        if (stateImage == 2) checkedIndices.push_back(static_cast<int>(index));
        else if (stateImage != 1) {
            return Reject(reason, L"ListView checkbox state image is not canonical");
        }
    }
    return true;
}

bool CaptureListViewState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const LRESULT count = SendMessageW(hwnd, LVM_GETITEMCOUNT, 0, 0);
    if (count < 0 || count > static_cast<LRESULT>(Ipc::kMaxListItems)) {
        return Reject(reason, L"invalid or excessive ListView item count");
    }
    const HWND header = reinterpret_cast<HWND>(SendMessageW(hwnd, LVM_GETHEADER, 0, 0));
    const int columnCount = header && IsWindow(header) ? Header_GetItemCount(header) : 0;
    if (columnCount <= 0) {
        return Reject(reason,
            L"ListView has no native report columns for faithful bounded projection");
    }
    if (static_cast<size_t>(columnCount) > kMaxListViewColumns) {
        return Reject(reason, L"ListView has excessive report columns");
    }
    if (!ValidateListViewColumns(hwnd, header, columnCount, reason)) return false;
    node.columnHeadersVisible =
        (static_cast<DWORD>(node.style) & LVS_NOCOLUMNHEADER) == 0;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
    node.checkBoxes = (extended & LVS_EX_CHECKBOXES) != 0;

    size_t totalText = 0;
    const auto withinTextBudget = [&](size_t added) {
        totalText += added;
        if (totalText <= kMaxStructuredTextChars) return true;
        return Reject(reason, L"ListView text exceeds the bounded adapter payload");
    };

    node.columns.reserve(static_cast<size_t>(columnCount));
    node.columnWidths.reserve(static_cast<size_t>(columnCount));
    for (int column = 0; column < columnCount; ++column) {
        std::wstring label;
        int width = 0;
        if (!ReadListViewColumn(hwnd, column, label, width, reason)) return false;
        if (!withinTextBudget(label.size())) return false;
        node.columns.push_back(std::move(label));
        node.columnWidths.push_back(width);
    }

    node.rows.reserve(static_cast<size_t>(count));
    node.items.reserve(static_cast<size_t>(count));
    for (int row = 0; row < count; ++row) {
        std::vector<std::wstring> cells;
        cells.reserve(static_cast<size_t>(columnCount));
        for (int column = 0; column < columnCount; ++column) {
            std::wstring text;
            if (!ReadListViewCell(hwnd, row, column, text, reason)) return false;
            if (!withinTextBudget(text.size())) return false;
            cells.push_back(std::move(text));
        }
        node.items.push_back(cells.empty() ? std::wstring() : cells.front());
        node.rows.push_back(std::move(cells));
    }

    if (!ReadListViewSelection(hwnd, node.rows.size(), node.selectedIndices, reason)) {
        return false;
    }
    if (!ReadListViewChecks(
            hwnd, node.rows.size(), node.checkBoxes, node.checkedIndices, reason)) {
        return false;
    }
    node.focusedIndex = static_cast<int>(SendMessageW(
        hwnd, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
    if (node.focusedIndex < -1 ||
        (node.focusedIndex >= 0 &&
            static_cast<size_t>(node.focusedIndex) >= node.rows.size())) {
        return Reject(reason, L"ListView focused index is outside the item range");
    }
    node.multiSelect = (static_cast<DWORD>(node.style) & LVS_SINGLESEL) == 0;
    node.selectedIndex = node.selectedIndices.empty() ? -1 : node.selectedIndices.front();
    return true;
}

// One StatusBar part: its text plus the client-coordinate right edge that
// bounds it.  `previousRight` carries the running left edge across parts.
bool ReadStatusBarPart(
    HWND hwnd,
    WPARAM part,
    bool simple,
    int64_t clientWidth,
    int rawRight,
    bool lastPart,
    int64_t& previousRight,
    ControlNode& node,
    size_t& totalText,
    std::wstring& reason) {
    const LRESULT lengthAndType = SendMessageW(hwnd, SB_GETTEXTLENGTHW, part, 0);
    const size_t length = LOWORD(lengthAndType);
    if ((HIWORD(lengthAndType) & SBT_OWNERDRAW) != 0 || length > Ipc::kMaxStringChars) {
        return Reject(reason, L"StatusBar part is owner-draw or exceeds the text limit");
    }
    std::wstring text(length + 1, L'\0');
    const LRESULT copiedAndType = SendMessageW(
        hwnd, SB_GETTEXTW, part, reinterpret_cast<LPARAM>(text.data()));
    const size_t copied = LOWORD(copiedAndType);
    if (copied > length || (HIWORD(copiedAndType) & SBT_OWNERDRAW) != 0) {
        return Reject(reason, L"StatusBar part text read failed");
    }
    text.resize(copied);
    totalText += text.size();
    if (totalText > kMaxStructuredTextChars) {
        return Reject(reason, L"StatusBar text exceeds the bounded adapter payload");
    }
    node.items.push_back(std::move(text));

    int64_t right = simple ? clientWidth : rawRight;
    if (right == -1) {
        // -1 is the native "stretch to the client edge" marker and is only
        // meaningful on the final part.
        if (!lastPart) {
            return Reject(reason, L"StatusBar stretch part is not the final part");
        }
        right = clientWidth;
    }
    if (right < previousRight || right > clientWidth || right < 0) {
        return Reject(reason,
            L"StatusBar part edges are not monotonic client coordinates");
    }
    const int64_t width = right - previousRight;
    if (width > std::numeric_limits<int>::max()) {
        return Reject(reason, L"StatusBar part width exceeds the protocol range");
    }
    node.columnWidths.push_back(static_cast<int>(width));
    previousRight = right;
    return true;
}

bool CaptureStatusBarState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    // A StatusBar projects as its parts, so the concatenated HWND text would
    // only duplicate them in the UIA name.
    node.text.clear();
    node.automationName.clear();
    const bool simple = SendMessageW(hwnd, SB_ISSIMPLE, 0, 0) != FALSE;
    const int count = simple ? 1 : static_cast<int>(SendMessageW(hwnd, SB_GETPARTS, 0, 0));
    if (count <= 0 || static_cast<size_t>(count) > kMaxStatusBarParts) {
        return Reject(reason, L"StatusBar has no bounded part collection");
    }
    std::vector<int> rightEdges(static_cast<size_t>(count), -1);
    if (!simple && SendMessageW(hwnd, SB_GETPARTS, count,
            reinterpret_cast<LPARAM>(rightEdges.data())) != count) {
        return Reject(reason, L"StatusBar part geometry read failed");
    }
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        return Reject(reason, L"StatusBar client geometry read failed");
    }
    const int64_t clientWidth = static_cast<int64_t>(client.right) - client.left;
    if (clientWidth < 0 || clientWidth > std::numeric_limits<int>::max()) {
        return Reject(reason, L"StatusBar client width is outside the bounded adapter");
    }

    size_t totalText = 0;
    int64_t previousRight = 0;
    node.items.reserve(static_cast<size_t>(count));
    node.columnWidths.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (!ReadStatusBarPart(hwnd,
                simple ? SB_SIMPLEID : static_cast<WPARAM>(index), simple,
                clientWidth, rightEdges[static_cast<size_t>(index)],
                index + 1 == count, previousRight, node, totalText, reason)) {
            return false;
        }
    }
    return true;
}

// The bounded SysLink adapter accepts exactly one hyperlink with no nested
// markup, so its label has an unambiguous position in the flattened text.
bool CaptureSysLinkState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    LITEM item{};
    item.mask = LIF_ITEMINDEX | LIF_STATE | LIF_ITEMID | LIF_URL;
    item.iLink = 0;
    item.stateMask = LIS_ENABLED;
    if (!SendMessageW(hwnd, LM_GETITEM, 0, reinterpret_cast<LPARAM>(&item))) {
        return Reject(reason, L"SysLink has no interrogable link item");
    }
    LITEM second{};
    second.mask = LIF_ITEMINDEX | LIF_STATE;
    second.iLink = 1;
    second.stateMask = LIS_ENABLED;
    if (SendMessageW(hwnd, LM_GETITEM, 0, reinterpret_cast<LPARAM>(&second))) {
        return Reject(reason, L"multi-link SysLink is outside the bounded adapter");
    }

    const std::wstring& markup = node.text;
    std::wstring lowered = markup;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    const size_t open = lowered.find(L"<a");
    const size_t openEnd = open == std::wstring::npos
        ? std::wstring::npos : lowered.find(L'>', open + 2);
    const size_t close = openEnd == std::wstring::npos
        ? std::wstring::npos : lowered.find(L"</a>", openEnd + 1);
    if (open == std::wstring::npos || openEnd == std::wstring::npos ||
        close == std::wstring::npos || close == openEnd + 1 ||
        lowered.find(L"<a", close + 4) != std::wstring::npos) {
        return Reject(reason, L"SysLink markup is not a single bounded hyperlink");
    }
    const std::wstring prefix = markup.substr(0, open);
    const std::wstring label = markup.substr(openEnd + 1, close - openEnd - 1);
    std::wstring suffix = markup.substr(close + 4);
    while (!suffix.empty() && suffix.back() <= L' ') suffix.pop_back();
    const auto hasMarkupOrMnemonic = [](std::wstring_view value) {
        return value.find_first_of(L"<>&") != std::wstring_view::npos;
    };
    if (hasMarkupOrMnemonic(prefix) || hasMarkupOrMnemonic(label) ||
        hasMarkupOrMnemonic(suffix)) {
        return Reject(reason,
            L"SysLink contains unsupported nested markup or mnemonic text");
    }
    const std::wstring flattened = prefix + label + suffix;
    const size_t labelAt = flattened.find(label);
    if (labelAt != prefix.size() ||
        flattened.find(label, labelAt + label.size()) != std::wstring::npos) {
        return Reject(reason, L"SysLink label is ambiguous in its flattened text");
    }
    node.text = flattened;
    node.automationName = node.text;
    node.items.push_back(label);
    node.enabled = node.enabled && (item.state & LIS_ENABLED) != 0;
    return true;
}

bool CaptureButtonState(HWND, ControlNode& node, std::wstring&) {
    node.isDefault = (static_cast<DWORD>(node.style) & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
    return true;
}

bool CaptureToggleState(HWND hwnd, ControlNode& node, std::wstring&) {
    node.checked = static_cast<int>(SendMessageW(hwnd, BM_GETCHECK, 0, 0));
    return true;
}

bool CaptureTextEntryState(HWND hwnd, ControlNode& node, std::wstring&) {
    const auto style = static_cast<DWORD>(node.style);
    node.readOnly = (style & ES_READONLY) != 0;
    node.multiline = (style & ES_MULTILINE) != 0;
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(hwnd, EM_GETSEL,
        reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    node.selectionStart = static_cast<int>(start);
    node.selectionLength = static_cast<int>(end >= start ? end - start : 0);
    return true;
}

bool CaptureStringListState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const bool combo = node.kind == ControlKind::ComboBox;
    node.editable = combo && (static_cast<DWORD>(node.style) & 0x0003u) == CBS_DROPDOWN;
    if (!ReadStringItems(hwnd, combo, node.items, reason)) return false;
    node.selectedIndex = static_cast<int>(
        SendMessageW(hwnd, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0));
    return true;
}

bool CaptureProgressState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    PBRANGE range{};
    SendMessageW(hwnd, PBM_GETRANGE, FALSE, reinterpret_cast<LPARAM>(&range));
    node.minimum = range.iLow;
    node.maximum = range.iHigh;
    node.position = static_cast<int>(SendMessageW(hwnd, PBM_GETPOS, 0, 0));
    node.indeterminate = (static_cast<DWORD>(node.style) & PBS_MARQUEE) != 0;
    if (node.maximum <= node.minimum || node.position < node.minimum ||
        node.position > node.maximum) {
        return Reject(reason, L"ProgressBar has an invalid native range or position");
    }
    return true;
}

bool CaptureTabControlState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const LRESULT rawCount = SendMessageW(hwnd, TCM_GETITEMCOUNT, 0, 0);
    if (rawCount <= 0 || rawCount > static_cast<LRESULT>(Ipc::kMaxTabItems))
        return Reject(reason, L"TabControl item count is empty, invalid, or excessive");
    const size_t count = static_cast<size_t>(rawCount);
    const int selected = static_cast<int>(SendMessageW(hwnd, TCM_GETCURSEL, 0, 0));
    if (selected < 0 || static_cast<size_t>(selected) >= count)
        return Reject(reason, L"TabControl selected index is outside its items");

    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0)
        return Reject(reason, L"TabControl client geometry is unavailable");
    node.items.clear();
    node.itemRects.clear();
    node.items.reserve(count);
    node.itemRects.reserve(count);
    size_t totalText = 0;
    std::vector<wchar_t> buffer(kMaxTabLabelChars + 1);
    for (size_t index = 0; index < count; ++index) {
        std::fill(buffer.begin(), buffer.end(), L'\0');
        TCITEMW item{};
        item.mask = TCIF_TEXT | TCIF_IMAGE | TCIF_PARAM | TCIF_STATE;
        item.dwStateMask = 0xffffffffu;
        item.pszText = buffer.data();
        item.cchTextMax = static_cast<int>(buffer.size());
        item.iImage = -1;
        if (!SendMessageW(hwnd, TCM_GETITEMW, index, reinterpret_cast<LPARAM>(&item)))
            return Reject(reason, L"TabControl item text or metadata is unavailable");
        if (item.pszText == LPSTR_TEXTCALLBACKW)
            return Reject(reason, L"TabControl callback text is not supported");
        if (item.iImage != -1)
            return Reject(reason, L"TabControl image-backed item is not supported");
        // lParam is opaque application identity. It remains owned by the native
        // item and is deliberately neither interpreted nor copied to ControlNode.
        constexpr DWORD acceptedState = TCIS_BUTTONPRESSED | TCIS_HIGHLIGHTED;
        if ((item.dwState & ~acceptedState) != 0)
            return Reject(reason, L"TabControl custom item state is not supported");
        const size_t length = wcsnlen_s(buffer.data(), buffer.size());
        if (length == 0)
            return Reject(reason, L"TabControl requires nonempty textual labels");
        if (length >= kMaxTabLabelChars)
            return Reject(reason, L"TabControl item label exceeds the text cap");
        if (totalText > kMaxStructuredTextChars - length)
            return Reject(reason, L"TabControl labels exceed the aggregate text cap");
        totalText += length;

        RECT rect{};
        if (!SendMessageW(hwnd, TCM_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect)) ||
            rect.left < 0 || rect.top < 0 || rect.right <= rect.left ||
            rect.bottom <= rect.top || rect.right > client.right || rect.bottom > client.bottom)
            return Reject(reason, L"TabControl item rectangle is malformed or outside client bounds");
        for (const RECT& previous : node.itemRects) {
            if (rect.left < previous.right && rect.right > previous.left &&
                rect.top < previous.bottom && rect.bottom > previous.top)
                return Reject(reason, L"TabControl item rectangles overlap");
        }
        node.items.emplace_back(buffer.data(), length);
        node.itemRects.push_back(rect);
    }
    node.selectedIndex = selected;
    return true;
}

bool CaptureToolbarState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const LRESULT rawCount = SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0);
    if (rawCount <= 0 || rawCount > static_cast<LRESULT>(Ipc::kMaxToolbarItems))
        return Reject(reason, L"ToolbarWindow32 button count is empty, invalid, or exceeds 64");
    if (SendMessageW(hwnd, TB_GETROWS, 0, 0) != 1)
        return Reject(reason, L"ToolbarWindow32 TB_GETROWS reports multiple rows");

    const HIMAGELIST images = reinterpret_cast<HIMAGELIST>(
        SendMessageW(hwnd, TB_GETIMAGELIST, 0, 0));
    int imageWidth = 0;
    int imageHeight = 0;
    const int imageCount = images ? ImageList_GetImageCount(images) : 0;
    if (!images || imageCount <= 0 || !ImageList_GetIconSize(images, &imageWidth, &imageHeight) ||
        imageWidth <= 0 || imageHeight <= 0 ||
        imageWidth > static_cast<int>(Ipc::kMaxImageDimension) ||
        imageHeight > static_cast<int>(Ipc::kMaxImageDimension)) {
        return Reject(reason, L"ToolbarWindow32 normal image list is missing, invalid, or outside the image cap");
    }
    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= client.left || client.bottom <= client.top)
        return Reject(reason, L"ToolbarWindow32 client geometry is unavailable");

    std::vector<uint32_t> commandIds;
    node.text.clear();
    node.automationName = L"Toolbar";
    node.toolbarItems.clear();
    node.toolbarItems.reserve(static_cast<size_t>(rawCount));
    bool haveVisibleBand = false;
    LONG bandTop = 0;
    LONG bandBottom = 0;
    LONG previousRight = client.left;
    size_t totalText = 0;
    for (int index = 0; index < rawCount; ++index) {
        TBBUTTON button{};
        if (!SendMessageW(hwnd, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&button)))
            return Reject(reason, L"ToolbarWindow32 TB_GETBUTTON failed");
        if (button.dwData != 0)
            return Reject(reason, L"ToolbarWindow32 button has nonzero opaque dwData semantics");
        if ((button.fsState & ~(TBSTATE_ENABLED | TBSTATE_HIDDEN)) != 0) {
            if ((button.fsState & TBSTATE_WRAP) != 0)
                return Reject(reason, L"ToolbarWindow32 button has TBSTATE_WRAP multi-row semantics");
            return Reject(reason, L"ToolbarWindow32 button has unsupported checked, pressed, marked, or indeterminate state");
        }

        ToolbarItemSnapshot item;
        item.enabled = (button.fsState & TBSTATE_ENABLED) != 0;
        item.hidden = (button.fsState & TBSTATE_HIDDEN) != 0;
        const BYTE type = button.fsStyle & (BTNS_SEP | BTNS_CHECK | BTNS_GROUP | BTNS_DROPDOWN);
        if (type == BTNS_SEP) {
            if ((button.fsStyle & ~BTNS_SEP) != 0)
                return Reject(reason, L"ToolbarWindow32 separator has unsupported style semantics");
            item.kind = ToolbarItemKind::Separator;
        } else {
            constexpr BYTE acceptedButtonStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT;
            if (type != BTNS_BUTTON || (button.fsStyle & ~acceptedButtonStyle) != 0)
                return Reject(reason, L"ToolbarWindow32 dropdown, check, group, or custom button style is not supported");
            if (button.idCommand <= 0 || button.idCommand > 0xffff)
                return Reject(reason, L"ToolbarWindow32 push button command ID is outside the 16-bit WM_COMMAND range");
            item.commandId = static_cast<uint32_t>(button.idCommand);
            if (std::find(commandIds.begin(), commandIds.end(), item.commandId) != commandIds.end())
                return Reject(reason, L"ToolbarWindow32 push button command IDs are not unique");
            commandIds.push_back(item.commandId);
            if (button.iString == reinterpret_cast<INT_PTR>(LPSTR_TEXTCALLBACKW))
                return Reject(reason, L"ToolbarWindow32 push button label is callback-only (LPSTR_TEXTCALLBACK)");
            std::vector<wchar_t> label(kMaxTabLabelChars + 1, L'\0');
            TBBUTTONINFOW info{};
            info.cbSize = sizeof(info);
            info.dwMask = TBIF_TEXT;
            info.pszText = label.data();
            info.cchText = static_cast<int>(label.size());
            if (SendMessageW(hwnd, TB_GETBUTTONINFOW, item.commandId,
                    reinterpret_cast<LPARAM>(&info)) < 0)
                return Reject(reason, L"ToolbarWindow32 direct push button label read failed");
            const size_t length = wcsnlen_s(label.data(), label.size());
            if (length == 0)
                return Reject(reason, L"ToolbarWindow32 push button requires a nonempty direct label");
            if (length >= kMaxTabLabelChars)
                return Reject(reason, L"ToolbarWindow32 push button label exceeds the bounded text cap");
            if (totalText > kMaxStructuredTextChars - length)
                return Reject(reason, L"ToolbarWindow32 labels exceed the aggregate text cap");
            totalText += length;
            item.text.assign(label.data(), length);

            if (button.iBitmap == I_IMAGECALLBACK)
                return Reject(reason, L"ToolbarWindow32 push button image is callback-only (I_IMAGECALLBACK)");
            if (button.iBitmap < 0 || button.iBitmap >= imageCount)
                return Reject(reason, L"ToolbarWindow32 push button has no valid normal image-list icon");
            HICON icon = ImageList_GetIcon(images, button.iBitmap, ILD_NORMAL);
            if (!icon)
                return Reject(reason, L"ToolbarWindow32 normal image-list icon copy failed");
            const bool copied = CaptureIconPixels(icon, item.imageWidth, item.imageHeight,
                item.imageFormat, item.imageData, reason);
            DestroyIcon(icon);
            if (!copied) {
                reason = L"ToolbarWindow32 normal image-list icon: " + reason;
                return false;
            }
        }

        RECT rect{};
        if (!item.hidden) {
            if (!SendMessageW(hwnd, TB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect)) ||
                rect.left < client.left || rect.top < client.top || rect.right <= rect.left ||
                rect.bottom <= rect.top || rect.right > client.right || rect.bottom > client.bottom)
                return Reject(reason, L"ToolbarWindow32 visible item rectangle is malformed or outside client bounds");
            if (!haveVisibleBand) {
                bandTop = rect.top;
                bandBottom = rect.bottom;
                haveVisibleBand = true;
            } else if (rect.top != bandTop || rect.bottom != bandBottom) {
                return Reject(reason, L"ToolbarWindow32 actual item geometry proves multiple rows or wrapping");
            }
            if (rect.left < previousRight)
                return Reject(reason, L"ToolbarWindow32 visible item rectangles overlap or are not horizontally ordered");
            previousRight = rect.right;
        }
        item.rect = rect;
        node.toolbarItems.push_back(std::move(item));
    }
    if (!haveVisibleBand)
        return Reject(reason, L"ToolbarWindow32 has no visible one-row item geometry");
    return true;
}

using CaptureFn = bool (*)(HWND, ControlNode&, std::wstring&);

// The explicit sentinel prevents table sizing from silently becoming stale when
// another kind is appended before it.
constexpr size_t kControlKindCount = static_cast<size_t>(ControlKind::Count);

// Kind -> typed state reader.  A null entry means the kind adds nothing to the
// common facets, so lookup stays a single indexed load either way.
constexpr std::array<CaptureFn, kControlKindCount> MakeCaptureTable() {
    std::array<CaptureFn, kControlKindCount> table{};
    const auto at = [&table](ControlKind kind) -> CaptureFn& {
        return table[static_cast<size_t>(kind)];
    };
    at(ControlKind::Button) = &CaptureButtonState;
    at(ControlKind::CheckBox) = &CaptureToggleState;
    at(ControlKind::ThreeState) = &CaptureToggleState;
    at(ControlKind::RadioButton) = &CaptureRadioState;
    at(ControlKind::Edit) = &CaptureTextEntryState;
    at(ControlKind::Password) = &CaptureTextEntryState;
    at(ControlKind::ComboBox) = &CaptureStringListState;
    at(ControlKind::ListBox) = &CaptureStringListState;
    at(ControlKind::ProgressBar) = &CaptureProgressState;
    at(ControlKind::SysLink) = &CaptureSysLinkState;
    at(ControlKind::ListView) = &CaptureListViewState;
    at(ControlKind::StaticIcon) = &CaptureStaticIconState;
    at(ControlKind::TabControl) = &CaptureTabControlState;
    at(ControlKind::StatusBar) = &CaptureStatusBarState;
    at(ControlKind::Toolbar) = &CaptureToolbarState;
    // StaticText, Separator, GroupBox, and DialogContainer are fully described by the common
    // facets.  TreeView and Slider have no adapter yet, so no
    // probe can produce them.
    return table;
}

constexpr auto kCaptureTable = MakeCaptureTable();

} // namespace

bool CaptureOwnedIconPixels(
    HICON icon,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason) {
    return CaptureIconPixels(icon, imageWidth, imageHeight, imageFormat, imageData, reason);
}

bool CaptureOwnedWindowPixels(
    HWND hwnd,
    uint32_t maxDimension,
    size_t maxBytes,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason) {
    return CaptureWindowPixels(hwnd, maxDimension, maxBytes, imageWidth, imageHeight,
        imageFormat, imageData, reason);
}

std::wstring_view ClassNameOf(HWND hwnd, wchar_t (&buffer)[kMaxClassNameChars]) noexcept {
    const int length = GetClassNameW(hwnd, buffer, static_cast<int>(kMaxClassNameChars));
    return length > 0
        ? std::wstring_view(buffer, static_cast<size_t>(length))
        : std::wstring_view();
}

bool ClassifyControl(HWND hwnd, ControlKind& kind, std::wstring& reason) {
    wchar_t buffer[kMaxClassNameChars]{};
    const auto className = ClassNameOf(hwnd, buffer);
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    for (const auto& adapter : kClassAdapters) {
        if (FluentShell::EqualsIgnoreCase(className, adapter.className)) {
            return adapter.probe(hwnd, style, kind, reason);
        }
    }
    reason = L"unsupported visible control class: ";
    reason.append(className);
    return false;
}

bool CaptureControlDetail(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto index = static_cast<size_t>(node.kind);
    if (index >= kCaptureTable.size()) {
        return Reject(reason, L"control kind has no registered adapter");
    }
    const CaptureFn capture = kCaptureTable[index];
    return capture == nullptr || capture(hwnd, node, reason);
}

bool IsCompositeImplementationChild(HWND hwnd) noexcept {
    const HWND parent = GetParent(hwnd);
    if (!parent) return false;
    wchar_t parentBuffer[kMaxClassNameChars]{};
    const auto parentClass = ClassNameOf(parent, parentBuffer);
    if (FluentShell::EqualsIgnoreCase(parentClass, L"ComboBox")) {
        COMBOBOXINFO info{sizeof(info)};
        return GetComboBoxInfo(parent, &info) &&
            (hwnd == info.hwndItem || hwnd == info.hwndList);
    }
    wchar_t childBuffer[kMaxClassNameChars]{};
    if (FluentShell::EqualsIgnoreCase(parentClass, L"MonitorPaletteClass")) {
        return FluentShell::EqualsIgnoreCase(
            ClassNameOf(hwnd, childBuffer), L"MonitorPaletteEntryClass");
    }
    return FluentShell::EqualsIgnoreCase(parentClass, WC_LISTVIEWW) &&
        FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, childBuffer), WC_HEADERW);
}

} // namespace FluentShell::Bridge::Translation
