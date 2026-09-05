#include "ControlAdapters.h"

#include "AccessibleIsland.h"

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
constexpr size_t kMaxTreeLabelChars = 4096;

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

// Renders the named bits of a rejected style mask so the log says which bit
// refused the control instead of only that some bit did.
struct NamedFlag final {
    DWORD value;
    const wchar_t* name;
};

bool RejectFlags(
    std::wstring& reason,
    const wchar_t* prefix,
    DWORD rejected,
    DWORD actual,
    const NamedFlag* flags,
    size_t flagCount) {
    reason = prefix;
    bool first = true;
    for (size_t index = 0; index < flagCount; ++index) {
        if ((rejected & flags[index].value) == 0) continue;
        if (!first) reason += L"|";
        reason += flags[index].name;
        first = false;
    }
    wchar_t numeric[80]{};
    swprintf_s(numeric, L" (unsupported=0x%08lX, actual=0x%08lX)", rejected, actual);
    reason += numeric;
    return false;
}

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
        (style & (LVS_OWNERDATA | LVS_OWNERDRAWFIXED)) != 0) {
        return Reject(reason,
            L"ListView is virtual, owner-draw, or not in report view");
    }
    if (SendMessageW(hwnd, LVM_ISGROUPVIEWENABLED, 0, 0) != FALSE) {
        return Reject(reason, L"grouped ListView is not supported");
    }
    // LVS_EX_HEADERDRAGDROP only says the user may reorder the columns.  The
    // projection captures the display order and offers the same reordering, so the
    // flag is evidence to carry rather than a reason to refuse.
    constexpr DWORD unsupportedExtended = LVS_EX_TRACKSELECT |
        LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
    if (const DWORD rejected = extended & unsupportedExtended; rejected != 0) {
        static constexpr NamedFlag flags[] = {
            { LVS_EX_TRACKSELECT, L"LVS_EX_TRACKSELECT" },
            { LVS_EX_ONECLICKACTIVATE, L"LVS_EX_ONECLICKACTIVATE" },
            { LVS_EX_TWOCLICKACTIVATE, L"LVS_EX_TWOCLICKACTIVATE" },
        };
        return RejectFlags(reason, L"ListView has unsupported extended flag(s) ",
            rejected, extended, flags, std::size(flags));
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
    // WS_EX_TOOLWINDOW has no meaning on a child window and WS_EX_NOPARENTNOTIFY
    // only suppresses WM_PARENTNOTIFY, so neither changes what the toolbar paints or
    // how its buttons behave.
    const DWORD unsupportedExStyle = exStyle & ~(WS_EX_TOOLWINDOW | WS_EX_NOPARENTNOTIFY);
    if (unsupportedExStyle != 0) {
        wchar_t text[144]{};
        swprintf_s(text,
            L"ToolbarWindow32 has unsupported exStyle 0x%08lX (actual=0x%08lX)",
            unsupportedExStyle, exStyle);
        reason = text;
        return false;
    }
    const DWORD toolbarStyle = style & 0xffffu;
    // CCS_NODIVIDER and CCS_NORESIZE only tell the control not to draw its top
    // divider and not to resize itself with its parent; the projection lays the
    // toolbar out from its native rectangle either way.  TBSTYLE_LIST puts the
    // label beside the icon, which the projected button already does.
    constexpr DWORD accepted = CCS_TOP | CCS_NODIVIDER | CCS_NORESIZE |
        TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE | TBSTYLE_FLAT | TBSTYLE_TRANSPARENT |
        TBSTYLE_LIST;
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

// The bounded TreeView subset: a textual, iconless, single-selection tree whose
// hierarchy, expansion, and selection are all readable from the control itself.
// Label editing, checkbox/state images, hover selection, auto-collapse, and
// callback tooltips each own behavior the projection cannot reproduce, so each
// keeps the whole window native by name.
bool ProbeTreeView(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    constexpr DWORD unsupportedStyle = TVS_CHECKBOXES |
        TVS_TRACKSELECT | TVS_SINGLEEXPAND | TVS_INFOTIP | TVS_RTLREADING;
    if (const DWORD rejected = style & unsupportedStyle; rejected != 0) {
        static constexpr NamedFlag flags[] = {
            { TVS_CHECKBOXES, L"TVS_CHECKBOXES" },
            { TVS_TRACKSELECT, L"TVS_TRACKSELECT" },
            { TVS_SINGLEEXPAND, L"TVS_SINGLEEXPAND" },
            { TVS_INFOTIP, L"TVS_INFOTIP" },
            { TVS_RTLREADING, L"TVS_RTLREADING" },
        };
        return RejectFlags(reason, L"TreeView has unsupported style bit(s) ",
            rejected, style, flags, std::size(flags));
    }
    // The accepted extended bits are presentation only: double buffering, expando
    // animation, and horizontal auto-scroll change no state the adapter reads.
    constexpr DWORD acceptedExtended = TVS_EX_DOUBLEBUFFER |
        TVS_EX_FADEINOUTEXPANDOS | TVS_EX_AUTOHSCROLL;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, TVM_GETEXTENDEDSTYLE, 0, 0));
    if (const DWORD rejected = extended & ~acceptedExtended; rejected != 0) {
        static constexpr NamedFlag flags[] = {
            { TVS_EX_NOSINGLECOLLAPSE, L"TVS_EX_NOSINGLECOLLAPSE" },
            { TVS_EX_MULTISELECT, L"TVS_EX_MULTISELECT" },
            { TVS_EX_NOINDENTSTATE, L"TVS_EX_NOINDENTSTATE" },
            { TVS_EX_RICHTOOLTIP, L"TVS_EX_RICHTOOLTIP" },
            { TVS_EX_PARTIALCHECKBOXES, L"TVS_EX_PARTIALCHECKBOXES" },
            { TVS_EX_EXCLUSIONCHECKBOXES, L"TVS_EX_EXCLUSIONCHECKBOXES" },
            { TVS_EX_DIMMEDCHECKBOXES, L"TVS_EX_DIMMEDCHECKBOXES" },
            { TVS_EX_DRAWIMAGEASYNC, L"TVS_EX_DRAWIMAGEASYNC" },
        };
        return RejectFlags(reason, L"TreeView has unsupported extended style bit(s) ",
            rejected, extended, flags, std::size(flags));
    }
    // The normal image list is projected as bounded owned pixels the items index
    // into.  A state image list is a different contract -- checkbox and overlay
    // imagery the projection does not reproduce -- so it still keeps the tree
    // native.
    if (SendMessageW(hwnd, TVM_GETIMAGELIST, TVSIL_STATE, 0) != 0)
        return Reject(reason, L"TreeView state image list is not supported");
    // The control's built-in tooltip only re-renders a truncated item label the
    // adapter already captures, so its presence is not interrogated.  Application
    // tooltip *content* arrives through TVS_INFOTIP and TVS_EX_RICHTOOLTIP, and
    // both are refused above.
    kind = ControlKind::TreeView;
    return true;
}

// The bounded Trackbar subset: a plain range control whose value the projection
// can both read and drive through the control's own notification.  A selection
// range, a thumbless bar, control-owned tooltips, and pre-move veto snapping are
// each contracts the projection would have to invent, so they stay native.
bool ProbeTrackbar(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    constexpr DWORD acceptedStyle = TBS_AUTOTICKS | TBS_VERT | TBS_TOP | TBS_BOTH |
        TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_REVERSED | TBS_DOWNISLEFT |
        TBS_TRANSPARENTBKGND;
    if (const DWORD rejected = style & 0xffffu & ~acceptedStyle; rejected != 0) {
        static constexpr NamedFlag flags[] = {
            { TBS_ENABLESELRANGE, L"TBS_ENABLESELRANGE" },
            { TBS_NOTHUMB, L"TBS_NOTHUMB" },
            { TBS_TOOLTIPS, L"TBS_TOOLTIPS" },
            { TBS_NOTIFYBEFOREMOVE, L"TBS_NOTIFYBEFOREMOVE" },
        };
        return RejectFlags(reason, L"Trackbar has unsupported style bit(s) ",
            rejected, style, flags, std::size(flags));
    }
    if (SendMessageW(hwnd, TBM_GETTOOLTIPS, 0, 0) != 0)
        return Reject(reason, L"Trackbar tooltips are not supported");
    kind = ControlKind::Slider;
    return true;
}

// The MDI client area: a structural container whose only projected job is to own
// the frame's child windows.  It draws nothing the projection has to reproduce,
// so the probe only proves it is the plain client Win32 creates for an MDI frame.
bool ProbeMdiClient(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    constexpr DWORD unsupportedExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_COMPOSITED | WS_EX_LAYOUTRTL;
    if ((style & WS_CHILD) == 0)
        return Reject(reason, L"MDIClient is not a child window");
    if ((exStyle & unsupportedExStyle) != 0)
        return Reject(reason, L"MDIClient has layered, transparent, or RTL composition");
    if (const HMENU menu = GetMenu(hwnd); menu && IsMenu(menu))
        return Reject(reason, L"MDIClient carries its own menu");
    kind = ControlKind::MdiClient;
    return true;
}

// One MDI child frame.  Its caption, state, and system commands are the whole
// contract; the frame's own drawing is chrome the projection replaces rather than
// reproduces, which is why an owner-draw or regioned child is refused.
bool ProbeMdiChild(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if ((exStyle & WS_EX_MDICHILD) == 0 || (style & WS_CHILD) == 0)
        return Reject(reason, L"window is not an MDI child frame");
    constexpr DWORD unsupportedExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_COMPOSITED | WS_EX_LAYOUTRTL;
    if ((exStyle & unsupportedExStyle) != 0)
        return Reject(reason, L"MDI child has layered, transparent, or RTL composition");
    if ((style & (WS_HSCROLL | WS_VSCROLL)) != 0)
        return Reject(reason, L"scrolling MDI child client area is not supported");
    // The window region of an MDI child belongs to the MDI client, which uses it
    // to clip a child against the client area and against its siblings, so it is
    // not evidence of an application-defined shape.  The projection clips each
    // child to the same client band, so nothing here needs the region.
    kind = ControlKind::MdiChild;
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

// A container's own painting is what a projection would lose, so the boundary is
// geometric rather than a list of class names: when a window's visible children
// tile its client area and leave only thin strips between them, everything it draws
// is background plus separators, both of which the projection re-renders in Fluent.
// A strip that fully spans the container and has a pane on each side is a real
// splitter, which the projection offers as a draggable one.
constexpr int kMaxPaneGap = 14;
constexpr int kMinPaneExtent = 24;

RECT ChildRectInParent(HWND parent, HWND child) noexcept {
    RECT rect{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

void CollectVisibleChildRects(HWND container, std::vector<RECT>& rects) {
    for (HWND child = GetWindow(container, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        // A child the projection absorbs is not a pane: the pane layout has to agree
        // with the node tree the renderer is given, or the container offers a splitter
        // between two things the projection never draws side by side.  MMC's relocated
        // MDI caption cluster in the rebar band is exactly that case.
        if (IsCompositeImplementationChild(child)) continue;
        rects.push_back(ChildRectInParent(container, child));
    }
}

struct PaneGap final {
    RECT rect{};
    bool vertical = false;
};

// The uncovered part of the client area, as the region left after subtracting every
// visible child.  A thin strip is a gap between panes; anything thicker is a band
// the container paints itself, which is captured as bounded pixels rather than
// dropped.  A container with more chrome than the caps allow is refused with the
// offending rectangle as evidence.
struct PaneSurface final {
    std::vector<PaneGap> gaps;
    std::vector<RECT> chrome;
};

bool CollectPaneSurface(
    HWND container,
    const RECT& client,
    PaneSurface& surface,
    std::wstring& reason) {
    struct RegionGuard final {
        HRGN handle = nullptr;
        ~RegionGuard() { if (handle) DeleteObject(handle); }
    };
    RegionGuard remaining{ CreateRectRgn(client.left, client.top, client.right, client.bottom) };
    if (!remaining.handle) return Reject(reason, L"container region allocation failed");
    std::vector<RECT> children;
    CollectVisibleChildRects(container, children);
    for (const RECT& child : children) {
        RegionGuard covered{ CreateRectRgn(child.left, child.top, child.right, child.bottom) };
        if (!covered.handle) return Reject(reason, L"container region allocation failed");
        CombineRgn(remaining.handle, remaining.handle, covered.handle, RGN_DIFF);
    }
    const DWORD size = GetRegionData(remaining.handle, 0, nullptr);
    if (size == 0) return Reject(reason, L"container region could not be measured");
    std::vector<uint8_t> buffer(size);
    auto* data = reinterpret_cast<RGNDATA*>(buffer.data());
    if (GetRegionData(remaining.handle, size, data) == 0) {
        return Reject(reason, L"container region could not be measured");
    }
    const auto* rects = reinterpret_cast<const RECT*>(data->Buffer);
    size_t chromeBytes = 0;
    for (DWORD index = 0; index < data->rdh.nCount; ++index) {
        const RECT& gap = rects[index];
        const int width = gap.right - gap.left;
        const int height = gap.bottom - gap.top;
        if (width <= 0 || height <= 0) continue;
        if (width <= kMaxPaneGap || height <= kMaxPaneGap) {
            surface.gaps.push_back(PaneGap{ gap, height > width });
            continue;
        }
        if (width > static_cast<int>(Ipc::kMaxChromeRegionDimension) ||
            height > static_cast<int>(Ipc::kMaxChromeRegionDimension) ||
            surface.chrome.size() >= Ipc::kMaxChromeRegions) {
            wchar_t evidence[176]{};
            swprintf_s(evidence,
                L"container paints more than the projection can reproduce: %ldx%ld at %ld,%ld",
                width, height, gap.left, gap.top);
            return Reject(reason, evidence);
        }
        chromeBytes += static_cast<size_t>(width) * height * 4;
        if (chromeBytes > Ipc::kMaxChromeRegionBytes) {
            return Reject(reason, L"container chrome exceeds the protocol pixel cap");
        }
        surface.chrome.push_back(gap);
    }
    return true;
}

// The panes a split resizes: the child whose trailing edge touches the gap and the
// child whose leading edge touches it, in the split's direction.  Both neighbours
// must cover the same extent across the other axis as the gap itself, which is what
// makes "drag this strip" mean "resize exactly these two panes".  The seam between
// two toolbar bands of different widths fails that test, so it is never offered as a
// splitter; a splitter sitting under a caption band the container paints still
// passes, because the band is chrome rather than a pane.
struct PaneNeighbours final {
    HWND before = nullptr;
    HWND after = nullptr;
    RECT beforeRect{};
    RECT afterRect{};
};

PaneNeighbours ResolvePaneNeighbours(HWND container, const RECT& client, const PaneGap& gap) {
    PaneNeighbours neighbours;
    const int leading = gap.vertical ? gap.rect.left : gap.rect.top;
    const int trailing = gap.vertical ? gap.rect.right : gap.rect.bottom;
    const int crossFrom = gap.vertical ? gap.rect.top : gap.rect.left;
    const int crossTo = gap.vertical ? gap.rect.bottom : gap.rect.right;
    if (crossTo - crossFrom <= kMinPaneExtent) return neighbours;
    for (HWND child = GetWindow(container, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        const RECT rect = ChildRectInParent(container, child);
        const int childCrossFrom = gap.vertical ? rect.top : rect.left;
        const int childCrossTo = gap.vertical ? rect.bottom : rect.right;
        if (std::abs(childCrossFrom - crossFrom) > kMaxPaneGap ||
            std::abs(childCrossTo - crossTo) > kMaxPaneGap) {
            continue;
        }
        const int childTrailing = gap.vertical ? rect.right : rect.bottom;
        const int childLeading = gap.vertical ? rect.left : rect.top;
        if (!neighbours.before && childTrailing >= leading - 1 && childTrailing <= leading + 1) {
            neighbours.before = child;
            neighbours.beforeRect = rect;
        }
        if (!neighbours.after && childLeading >= trailing - 1 && childLeading <= trailing + 1) {
            neighbours.after = child;
            neighbours.afterRect = rect;
        }
    }
    return neighbours;
}

// Renders a window's client area once and hands back a cropper.  Both a container's
// painted bands and a toolbar's custom-drawn button faces need the same thing: the
// pixels the control itself drew, cropped to a rectangle, without one render per
// rectangle.
class PaintedClientSurface final {
public:
    explicit PaintedClientSurface(HWND window) noexcept {
        if (!window || !GetClientRect(window, &client_)) return;
        width_ = client_.right - client_.left;
        height_ = client_.bottom - client_.top;
        if (width_ <= 0 || height_ <= 0) return;
        screen_ = GetDC(nullptr);
        memory_ = screen_ ? CreateCompatibleDC(screen_) : nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = memory_
            ? CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0)
            : nullptr;
        previous_ = bitmap_ ? SelectObject(memory_, bitmap_) : nullptr;
        if (!screen_ || !memory_ || !bitmap_ || !bits_ || !previous_) return;
        // A window paints only what it chooses to paint, and GDI carries no alpha, so a
        // single render cannot say whether a pixel is black because the window painted
        // it black or because the window painted nothing there.  Rendering the same
        // paint over white and over black answers that exactly: a pixel that came out
        // the same in both is opaque, and the difference between the two is how much
        // background showed through.  The black render is already the premultiplied
        // colour, which is the format the projection publishes.
        //
        // PW_RENDERFULLCONTENT is what makes this work while the native window is
        // cloaked, which is exactly when a reconcile capture runs.
        rendered_ = RenderOver(window, RGB(0, 0, 0), black_) &&
            RenderOver(window, RGB(255, 255, 255), white_);
    }

    PaintedClientSurface(const PaintedClientSurface&) = delete;
    PaintedClientSurface& operator=(const PaintedClientSurface&) = delete;

    ~PaintedClientSurface() {
        if (previous_) SelectObject(memory_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (memory_) DeleteDC(memory_);
        if (screen_) ReleaseDC(nullptr, screen_);
    }

    bool Rendered() const noexcept { return rendered_; }
    const RECT& Client() const noexcept { return client_; }

    // Copies one client-relative rectangle out as premultiplied BGRA, with the alpha
    // the two renders recovered: a region the window never painted travels transparent
    // rather than as an opaque black rectangle over the projection.
    bool Crop(
        const RECT& area,
        uint32_t& imageWidth,
        uint32_t& imageHeight,
        std::wstring& imageFormat,
        std::vector<uint8_t>& imageData) const {
        if (!rendered_) return false;
        const int width = area.right - area.left;
        const int height = area.bottom - area.top;
        if (width <= 0 || height <= 0 ||
            area.left < client_.left || area.top < client_.top ||
            area.right > client_.right || area.bottom > client_.bottom) {
            return false;
        }
        const size_t stride = static_cast<size_t>(width_) * 4;
        imageWidth = static_cast<uint32_t>(width);
        imageHeight = static_cast<uint32_t>(height);
        imageFormat = L"bgra8-premultiplied";
        imageData.assign(static_cast<size_t>(width) * height * 4, 0);
        for (int row = 0; row < height; ++row) {
            const size_t sourceOffset =
                static_cast<size_t>(area.top - client_.top + row) * stride +
                static_cast<size_t>(area.left - client_.left) * 4;
            for (int column = 0; column < width; ++column) {
                const size_t source = sourceOffset + static_cast<size_t>(column) * 4;
                const size_t target =
                    (static_cast<size_t>(row) * width + column) * 4;
                int showThrough = 0;
                for (size_t channel = 0; channel < 3; ++channel) {
                    const int over = static_cast<int>(white_[source + channel]);
                    const int under = static_cast<int>(black_[source + channel]);
                    imageData[target + channel] = black_[source + channel];
                    showThrough = std::max(showThrough, over - under);
                }
                imageData[target + 3] = static_cast<uint8_t>(
                    std::clamp(255 - showThrough, 0, 255));
            }
        }
        return true;
    }

private:
    // One render of the window's own paint over a known background.
    bool RenderOver(HWND window, COLORREF background, std::vector<uint8_t>& into) noexcept {
        RECT full{ 0, 0, width_, height_ };
        HBRUSH brush = CreateSolidBrush(background);
        if (!brush) return false;
        FillRect(memory_, &full, brush);
        DeleteObject(brush);
        if (!PrintWindow(window, memory_, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) return false;
        GdiFlush();
        const size_t bytes = static_cast<size_t>(width_) * height_ * 4;
        try {
            into.resize(bytes);
        } catch (...) {
            return false;
        }
        std::memcpy(into.data(), bits_, bytes);
        return true;
    }

    RECT client_{};
    int width_ = 0;
    int height_ = 0;
    HDC screen_ = nullptr;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
    std::vector<uint8_t> black_;
    std::vector<uint8_t> white_;
    bool rendered_ = false;
};

// Renders the container's client area once and crops each chrome band out of it.
// One render rather than one per band: the bands belong to the same paint pass, so
// cropping keeps them consistent with each other and with the panes around them.
bool CaptureContainerChrome(
    HWND container,
    const RECT& client,
    const std::vector<RECT>& chrome,
    std::vector<ChromeRegionSnapshot>& regions,
    std::wstring& reason) {
    regions.clear();
    if (chrome.empty()) return true;
    const PaintedClientSurface surface(container);
    if (!surface.Rendered()) {
        return Reject(reason, L"container chrome could not be rendered");
    }
    for (const RECT& band : chrome) {
        ChromeRegionSnapshot region;
        region.rect = band;
        if (!surface.Crop(band, region.imageWidth, region.imageHeight,
                region.imageFormat, region.imageData)) {
            regions.clear();
            return Reject(reason, L"container chrome band is outside the rendered client area");
        }
        regions.push_back(std::move(region));
    }
    return true;
}

// True when the window owns at least one visible child window.  A control that
// frames other windows is not drawing its own content in that space.
bool HasVisibleChildWindow(HWND hwnd) noexcept {
    bool found = false;
    EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
        if (!IsWindowVisible(child)) return TRUE;
        *reinterpret_cast<bool*>(param) = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool ProbePaneContainer(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) {
        return Reject(reason, L"private container is not a child window");
    }
    if ((style & (WS_HSCROLL | WS_VSCROLL)) != 0) {
        return Reject(reason, L"private container scrolls content beyond its client area");
    }
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    constexpr DWORD unsupportedExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_COMPOSITED;
    if ((exStyle & unsupportedExStyle) != 0) {
        return Reject(reason, L"private container composites its own surface");
    }
    if (const HMENU menu = GetMenu(hwnd); menu && IsMenu(menu)) {
        return Reject(reason, L"private container owns a menu");
    }
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int regionType = region ? GetWindowRgn(hwnd, region) : ERROR;
    if (region) DeleteObject(region);
    if (regionType != ERROR) {
        return Reject(reason, L"private container has a custom region");
    }
    RECT client{};
    if (!GetClientRect(hwnd, &client) ||
        client.right - client.left <= 0 || client.bottom - client.top <= 0) {
        return Reject(reason, L"private container has no client area");
    }
    std::vector<RECT> children;
    CollectVisibleChildRects(hwnd, children);
    if (children.empty()) {
        return Reject(reason, L"private container has no visible children to frame");
    }
    PaneSurface surface;
    if (!CollectPaneSurface(hwnd, client, surface, reason)) return false;
    kind = ControlKind::PaneContainer;
    return true;
}

bool CapturePaneContainerState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    node.splits.clear();
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        return Reject(reason, L"private container has no client area");
    }
    PaneSurface surface;
    if (!CollectPaneSurface(hwnd, client, surface, reason)) return false;
    if (!CaptureContainerChrome(hwnd, client, surface.chrome, node.chromeRegions, reason)) {
        return false;
    }
    for (const PaneGap& gap : surface.gaps) {
        const PaneNeighbours neighbours = ResolvePaneNeighbours(hwnd, client, gap);
        if (!neighbours.before || !neighbours.after) continue;
        PaneSplitSnapshot split;
        split.vertical = gap.vertical;
        split.position = gap.vertical ? gap.rect.left : gap.rect.top;
        split.thickness = gap.vertical
            ? gap.rect.right - gap.rect.left
            : gap.rect.bottom - gap.rect.top;
        // The range is bounded by the two panes' far edges: moving the split cannot
        // collapse either of them, and nothing outside the pair ever moves.
        split.minimum = (gap.vertical ? neighbours.beforeRect.left : neighbours.beforeRect.top) +
            kMinPaneExtent;
        split.maximum = (gap.vertical ? neighbours.afterRect.right : neighbours.afterRect.bottom) -
            kMinPaneExtent - split.thickness;
        if (split.maximum < split.minimum) continue;
        node.splits.push_back(split);
    }
    return true;
}

// An accessible island hosts content that owns no HWND, so the geometric container
// rule cannot see it: the window looks empty.  It is admitted by host class because
// those classes belong to Windows rather than to the application, and the elements
// themselves are then read through the accessibility contract the window answers.
bool ProbeAccessibleIsland(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) {
        return Reject(reason, L"accessible island is not a child window");
    }
    if ((style & (WS_HSCROLL | WS_VSCROLL)) != 0) {
        return Reject(reason, L"accessible island scrolls content beyond its client area");
    }
    // A host that also owns real child windows is a hybrid the projection has no
    // contract for: its HWND children would be projected while its accessible elements
    // were read from the same surface, with no rule for how they interleave.
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsWindowVisible(child)) {
            return Reject(reason, L"accessible island also owns visible child windows");
        }
    }
    std::vector<AccessibleIslandItem> items;
    if (!ReadAccessibleIslandItems(hwnd, items, reason)) return false;
    kind = ControlKind::AccessibleIsland;
    return true;
}

bool CaptureAccessibleIslandState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    std::vector<AccessibleIslandItem> items;
    if (!ReadAccessibleIslandItems(hwnd, items, reason)) return false;
    node.islandItems.clear();
    node.islandItems.reserve(items.size());
    for (const AccessibleIslandItem& item : items) {
        AccessibleIslandItemSnapshot published;
        published.kind = item.kind == AccessibleItemKind::Text ? L"text"
            : item.kind == AccessibleItemKind::Link ? L"link" : L"button";
        published.rect = item.rect;
        published.name = item.name;
        published.description = item.description;
        published.actionName = item.actionName;
        published.enabled = item.enabled;
        published.dropDown = item.dropDown;
        node.islandItems.push_back(std::move(published));
    }
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
    ClassAdapter{ WC_TREEVIEWW, &ProbeTreeView },
    ClassAdapter{ WC_TABCONTROLW, &ProbeTabControl },
    ClassAdapter{ TRACKBAR_CLASSW, &ProbeTrackbar },
    ClassAdapter{ L"#32770", &ProbeDialogContainer },
    ClassAdapter{ L"MDIClient", &ProbeMdiClient },
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

// Copies a control's own image list into bounded owned pixels.  Items then carry
// only an index, so the same icon is never repeated in the payload.
bool CaptureImageList(
    HIMAGELIST images,
    std::vector<ImageListEntry>& imageList,
    std::wstring& reason) {
    imageList.clear();
    if (!images) return true;
    int width = 0;
    int height = 0;
    const int count = ImageList_GetImageCount(images);
    if (count < 0 || !ImageList_GetIconSize(images, &width, &height))
        return Reject(reason, L"image list metadata is unavailable");
    if (count == 0) return true;
    if (static_cast<size_t>(count) > Ipc::kMaxImageListImages) {
        wchar_t text[128]{};
        swprintf_s(text, L"image list has %d icons, above the %zu icon cap",
            count, Ipc::kMaxImageListImages);
        reason = text;
        return false;
    }
    if (width <= 0 || height <= 0 ||
        width > static_cast<int>(Ipc::kMaxImageListDimension) ||
        height > static_cast<int>(Ipc::kMaxImageListDimension)) {
        wchar_t text[128]{};
        swprintf_s(text, L"image list icons are %dx%d, outside the %ux%u cap",
            width, height, Ipc::kMaxImageListDimension, Ipc::kMaxImageListDimension);
        reason = text;
        return false;
    }
    imageList.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        HICON icon = ImageList_GetIcon(images, index, ILD_NORMAL);
        if (!icon) return Reject(reason, L"image list icon copy failed");
        ImageListEntry entry;
        const bool copied = CaptureIconPixels(icon, entry.imageWidth, entry.imageHeight,
            entry.imageFormat, entry.imageData, reason);
        DestroyIcon(icon);
        if (!copied) {
            reason = L"image list icon: " + reason;
            return false;
        }
        imageList.push_back(std::move(entry));
    }
    return true;
}

// One item's index into the captured image list, or -1.  An index outside the
// list is refused rather than silently drawn as nothing.
bool ResolveItemImage(
    int nativeIndex,
    size_t imageCount,
    int& resolved,
    std::wstring& reason) {
    if (nativeIndex == I_IMAGECALLBACK)
        return Reject(reason, L"callback item image is not supported");
    if (nativeIndex == I_IMAGENONE || nativeIndex < 0 || imageCount == 0) {
        resolved = -1;
        return true;
    }
    if (static_cast<size_t>(nativeIndex) >= imageCount)
        return Reject(reason, L"item image index is outside the captured image list");
    resolved = nativeIndex;
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
    for (int index = 0; index < columnCount; ++index) {
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

// The display order of the columns, as the header shows them.  Columns themselves
// travel in the application's own logical order, so the projection permutes for
// presentation and every index on the wire keeps meaning what the application means
// by it.
bool ReadListViewColumnOrder(
    HWND hwnd,
    int columnCount,
    std::vector<int>& order,
    std::wstring& reason) {
    order.assign(static_cast<size_t>(columnCount), -1);
    if (columnCount <= 0) return true;
    if (!SendMessageW(hwnd, LVM_GETCOLUMNORDERARRAY, columnCount,
            reinterpret_cast<LPARAM>(order.data()))) {
        return Reject(reason, L"ListView column order read failed");
    }
    std::vector<bool> seen(static_cast<size_t>(columnCount), false);
    for (const int logical : order) {
        if (logical < 0 || logical >= columnCount ||
            seen[static_cast<size_t>(logical)]) {
            return Reject(reason, L"ListView column order is not a permutation");
        }
        seen[static_cast<size_t>(logical)] = true;
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
    node.editableLabels = (static_cast<DWORD>(node.style) & LVS_EDITLABELS) != 0;
    // Report view draws the small image list, so that is the one the projection
    // carries.
    if (!CaptureImageList(reinterpret_cast<HIMAGELIST>(
            SendMessageW(hwnd, LVM_GETIMAGELIST, LVSIL_SMALL, 0)),
            node.imageList, reason)) return false;
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
    if (!ReadListViewColumnOrder(hwnd, columnCount, node.columnOrder, reason)) return false;
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
    node.itemImages.clear();
    node.itemImages.reserve(static_cast<size_t>(count));
    for (int row = 0; row < count; ++row) {
        std::vector<std::wstring> cells;
        cells.reserve(static_cast<size_t>(columnCount));
        for (int column = 0; column < columnCount; ++column) {
            std::wstring text;
            if (!ReadListViewCell(hwnd, row, column, text, reason)) return false;
            if (!withinTextBudget(text.size())) return false;
            cells.push_back(std::move(text));
        }
        LVITEMW image{};
        image.mask = LVIF_IMAGE;
        image.iItem = row;
        int resolved = -1;
        if (!node.imageList.empty()) {
            if (!SendMessageW(hwnd, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&image)))
                return Reject(reason, L"ListView item image read failed");
            if (!ResolveItemImage(image.iImage, node.imageList.size(), resolved, reason))
                return false;
        }
        node.itemImages.push_back(resolved);
        node.items.push_back(cells.empty() ? std::wstring() : cells.front());
        node.rows.push_back(std::move(cells));
    }
    const HWND editControl = reinterpret_cast<HWND>(
        SendMessageW(hwnd, LVM_GETEDITCONTROL, 0, 0));
    node.editingIndex = editControl && IsWindow(editControl)
        ? static_cast<int>(SendMessageW(
              hwnd, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED))
        : -1;
    if (node.editingIndex >= 0 &&
        static_cast<size_t>(node.editingIndex) >= node.rows.size())
        return Reject(reason, L"ListView edit session names a row outside its items");

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

// The accessible name of one toolbar button, addressed by its one-based accessible
// child index.  An icon-only button carries no button text at all, but the control
// still answers accessibility with the name a screen reader reads -- normally the
// button's own tooltip.  Reading it here is what lets an icon-only toolbar project
// with a real name instead of being refused.
std::wstring ToolbarAccessibleName(HWND toolbar, int childIndex) noexcept {
    return AccessibleChildName(toolbar, childIndex);
}

bool CaptureToolbarState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const LRESULT rawCount = SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0);
    if (rawCount <= 0 || rawCount > static_cast<LRESULT>(Ipc::kMaxToolbarItems))
        return Reject(reason, L"ToolbarWindow32 button count is empty, invalid, or exceeds 64");
    if (SendMessageW(hwnd, TB_GETROWS, 0, 0) != 1)
        return Reject(reason, L"ToolbarWindow32 TB_GETROWS reports multiple rows");

    // Image lists are resolved per button rather than once for the toolbar: a control
    // may own several, and then a button's iBitmap carries the list id in its high word
    // and the image index in its low word.  A text-only toolbar owns none at all, which
    // is not a defect as long as every button carries a name.
    const auto resolveImageList =
        [hwnd](int listId, int& width, int& height, int& count) -> HIMAGELIST {
        const HIMAGELIST list = reinterpret_cast<HIMAGELIST>(
            SendMessageW(hwnd, TB_GETIMAGELIST, static_cast<WPARAM>(listId), 0));
        width = 0;
        height = 0;
        count = list ? ImageList_GetImageCount(list) : 0;
        if (!list || count <= 0 || !ImageList_GetIconSize(list, &width, &height)) return nullptr;
        return list;
    };
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
        // dwData is application-private storage that the control never interprets: the
        // projection posts the same WM_COMMAND a real click produces and the application
        // looks up its own data exactly as it always does.  Owner-draw semantics are
        // refused through the control's style bits instead, where they are actually
        // declared.
        // TBSTATE_PRESSED is the transient look of a button under the pointer right
        // now; the projection owns its own pressed visual, and the next capture drops
        // it.  TBSTATE_MARKED is an application-chosen highlight the control paints
        // with no semantic of its own, and TBSTATE_ELLIPSES only says the control
        // trimmed the label it already reported.  None of them change what the button
        // does, so none of them is a reason to refuse the window.
        constexpr BYTE kAcceptedStates = TBSTATE_ENABLED | TBSTATE_HIDDEN | TBSTATE_CHECKED |
            TBSTATE_PRESSED | TBSTATE_MARKED | TBSTATE_ELLIPSES;
        if ((button.fsState & ~kAcceptedStates) != 0) {
            if ((button.fsState & TBSTATE_WRAP) != 0)
                return Reject(reason, L"ToolbarWindow32 button has TBSTATE_WRAP multi-row semantics");
            wchar_t evidence[144]{};
            swprintf_s(evidence,
                L"ToolbarWindow32 button has unsupported state 0x%02X (style=0x%02X)",
                static_cast<unsigned>(button.fsState), static_cast<unsigned>(button.fsStyle));
            return Reject(reason, evidence);
        }

        ToolbarItemSnapshot item;
        item.enabled = (button.fsState & TBSTATE_ENABLED) != 0;
        item.hidden = (button.fsState & TBSTATE_HIDDEN) != 0;
        item.checked = (button.fsState & TBSTATE_CHECKED) != 0;
        const BYTE type = button.fsStyle & (BTNS_SEP | BTNS_CHECK | BTNS_GROUP | BTNS_DROPDOWN);
        if (type == BTNS_SEP) {
            if ((button.fsStyle & ~BTNS_SEP) != 0)
                return Reject(reason, L"ToolbarWindow32 separator has unsupported style semantics");
            item.kind = ToolbarItemKind::Separator;
        } else {
            // A check-style button owns its latched state, and an application that
            // manages the latch itself sets TBSTATE_CHECKED on an ordinary button.  Both
            // are projected with the state the control currently reports; the projection
            // posts the same WM_COMMAND and reads back whatever the control settled on,
            // so neither the control nor the application loses ownership of it.
            const bool toggle = (button.fsStyle & BTNS_CHECK) != 0;
            constexpr BYTE acceptedButtonStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT | BTNS_CHECK |
                BTNS_DROPDOWN | BTNS_WHOLEDROPDOWN;
            if ((type & ~(BTNS_CHECK | BTNS_DROPDOWN)) != BTNS_BUTTON ||
                (button.fsStyle & ~acceptedButtonStyle) != 0) {
                wchar_t evidence[152]{};
                swprintf_s(evidence,
                    L"ToolbarWindow32 group or custom button style 0x%02X is not supported",
                    static_cast<unsigned>(button.fsStyle));
                return Reject(reason, evidence);
            }
            item.dropDown = (button.fsStyle & BTNS_DROPDOWN) != 0;
            item.wholeDropDown = item.dropDown &&
                (button.fsStyle & BTNS_WHOLEDROPDOWN) == BTNS_WHOLEDROPDOWN;
            if (!item.dropDown && (button.fsStyle & BTNS_WHOLEDROPDOWN) != 0) {
                return Reject(reason,
                    L"ToolbarWindow32 whole-dropdown button has no BTNS_DROPDOWN arrow");
            }
            if (toggle && item.dropDown) {
                return Reject(reason,
                    L"ToolbarWindow32 button combines a latch with a dropdown arrow");
            }
            item.kind = toggle ? ToolbarItemKind::ToggleButton : ToolbarItemKind::PushButton;
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
            if (length == 0) {
                // No button text: fall back to the name the control itself publishes to
                // accessibility clients, which for an icon-only button is its tooltip.
                item.text = ToolbarAccessibleName(hwnd, static_cast<int>(index) + 1);
                if (item.text.empty()) {
                    return Reject(reason,
                        L"ToolbarWindow32 push button has neither a label nor an accessible name");
                }
                if (item.text.size() >= kMaxTabLabelChars) {
                    return Reject(reason,
                        L"ToolbarWindow32 push button accessible name exceeds the bounded text cap");
                }
                if (totalText > kMaxStructuredTextChars - item.text.size()) {
                    return Reject(reason, L"ToolbarWindow32 labels exceed the aggregate text cap");
                }
                totalText += item.text.size();
            } else {
                if (length >= kMaxTabLabelChars)
                    return Reject(reason, L"ToolbarWindow32 push button label exceeds the bounded text cap");
                if (totalText > kMaxStructuredTextChars - length)
                    return Reject(reason, L"ToolbarWindow32 labels exceed the aggregate text cap");
                totalText += length;
                item.text.assign(label.data(), length);
            }

            const bool drawsItsOwnLabel = length != 0;
            if (button.iBitmap == I_IMAGECALLBACK) {
                // The control asks its owner for the image index at draw time, so the
                // projection asks the same question through the same notification rather
                // than refusing the button.  A rebar-hosted toolbar's handler often lives
                // on the frame rather than on the immediate parent, and some owners only
                // implement the ANSI notification, so each contracted form is tried.
                const auto askOwner = [&](HWND owner, UINT code, int& answered) {
                    if (!owner) return false;
                    NMTBDISPINFOW dispInfo{};
                    dispInfo.hdr.hwndFrom = hwnd;
                    dispInfo.hdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(hwnd));
                    dispInfo.hdr.code = code;
                    dispInfo.dwMask = TBNF_IMAGE;
                    dispInfo.idCommand = button.idCommand;
                    dispInfo.lParam = button.dwData;
                    dispInfo.iImage = I_IMAGECALLBACK;
                    SendMessageW(owner, WM_NOTIFY,
                        static_cast<WPARAM>(dispInfo.hdr.idFrom),
                        reinterpret_cast<LPARAM>(&dispInfo));
                    if ((dispInfo.dwMask & TBNF_IMAGE) == 0) return false;
                    if (dispInfo.iImage == I_IMAGECALLBACK) return false;
                    answered = dispInfo.iImage;
                    return true;
                };
                const HWND parent = GetParent(hwnd);
                const HWND frame = GetAncestor(hwnd, GA_ROOT);
                int answered = I_IMAGECALLBACK;
                const bool resolved =
                    askOwner(parent, TBN_GETDISPINFOW, answered) ||
                    askOwner(frame, TBN_GETDISPINFOW, answered) ||
                    askOwner(parent, TBN_GETDISPINFOA, answered) ||
                    askOwner(frame, TBN_GETDISPINFOA, answered);
                // An owner that answers nothing draws the face itself, which the
                // projection reproduces from the control's own paint below -- unless the
                // button carries its own label, in which case that label *is* the face
                // and a cropped bitmap of it would only be a worse copy of the text.
                button.iBitmap = resolved ? answered : I_IMAGENONE;
                if (!resolved && !drawsItsOwnLabel) item.paintedFace = true;
            }
            // A button that draws no icon is admitted only when it carries its own
            // label, which the label read above already guaranteed.
            if (button.iBitmap != I_IMAGENONE && button.iBitmap >= 0) {
                const auto raw = static_cast<DWORD>(button.iBitmap);
                const int listId = static_cast<int>(HIWORD(raw));
                const int imageIndex = static_cast<int>(LOWORD(raw));
                int listWidth = 0;
                int listHeight = 0;
                int listCount = 0;
                const HIMAGELIST list = resolveImageList(listId, listWidth, listHeight, listCount);
                // A toolbar that owns no image list for that id draws no icon from one, no
                // matter what iBitmap says: a button added without an icon keeps the
                // default index of zero, and a control that custom-draws its faces keeps
                // an empty list.  Both are reproduced from the control's own paint.
                if (!list || imageIndex >= listCount) {
                    item.paintedFace = !drawsItsOwnLabel;
                } else if (listWidth <= 0 || listHeight <= 0 ||
                           listWidth > static_cast<int>(Ipc::kMaxImageDimension) ||
                           listHeight > static_cast<int>(Ipc::kMaxImageDimension)) {
                    wchar_t evidence[176]{};
                    swprintf_s(evidence,
                        L"ToolbarWindow32 image list %d is %dx%d, outside the image cap",
                        listId, listWidth, listHeight);
                    return Reject(reason, evidence);
                } else {
                    HICON icon = ImageList_GetIcon(list, imageIndex, ILD_NORMAL);
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
    // A button whose face the control draws itself -- a custom-draw toolbar, or one
    // whose image callback answers nothing -- is reproduced from the control's own
    // paint, cropped to that button's rectangle.  One render serves every such button,
    // and the pixels are part of the snapshot fingerprint, so a repaint reaches the
    // renderer as an ordinary patch.
    const bool needsPaint = std::any_of(node.toolbarItems.begin(), node.toolbarItems.end(),
        [](const ToolbarItemSnapshot& item) {
            return item.paintedFace && !item.hidden;
        });
    if (needsPaint) {
        const PaintedClientSurface surface(hwnd);
        if (!surface.Rendered()) {
            return Reject(reason, L"ToolbarWindow32 could not render its own button faces");
        }
        for (ToolbarItemSnapshot& item : node.toolbarItems) {
            if (!item.paintedFace || item.hidden) continue;
            if (item.rect.right - item.rect.left > static_cast<LONG>(Ipc::kMaxImageDimension) ||
                item.rect.bottom - item.rect.top > static_cast<LONG>(Ipc::kMaxImageDimension)) {
                return Reject(reason,
                    L"ToolbarWindow32 custom-drawn button face is larger than the image cap");
            }
            if (!surface.Crop(item.rect, item.imageWidth, item.imageHeight,
                    item.imageFormat, item.imageData)) {
                return Reject(reason,
                    L"ToolbarWindow32 custom-drawn button face could not be cropped");
            }
        }
    }
    return true;
}

// One flattened TreeView item: its native handle and its nesting level.  The
// order is the depth-first, native sibling order every projected TreeView index
// means, on both the capture and the action side.
struct TreeViewItemHandle final {
    HTREEITEM item = nullptr;
    int depth = 0;
};

HTREEITEM TreeViewRelative(HWND hwnd, WPARAM relation, HTREEITEM item) noexcept {
    return reinterpret_cast<HTREEITEM>(SendMessageW(
        hwnd, TVM_GETNEXTITEM, relation, reinterpret_cast<LPARAM>(item)));
}

// Walks the whole inserted tree, not only the rows currently on screen: a
// collapsed parent's children are canonical state the projection has to carry so
// expanding it does not have to wait for a fresh capture.  A lazily populated
// tree has no children inserted yet, which is exactly what itemHasChildren is
// for.
bool CollectTreeViewItems(
    HWND hwnd,
    std::vector<TreeViewItemHandle>& handles,
    std::wstring& reason) {
    handles.clear();
    HTREEITEM current = TreeViewRelative(hwnd, TVGN_ROOT, nullptr);
    int depth = 0;
    while (current) {
        if (handles.size() >= Ipc::kMaxListItems)
            return Reject(reason, L"excessive TreeView item count");
        if (depth > Ipc::kMaxTreeDepth)
            return Reject(reason, L"TreeView nesting exceeds the projected depth cap");
        handles.push_back({ current, depth });
        if (const HTREEITEM child = TreeViewRelative(hwnd, TVGN_CHILD, current)) {
            current = child;
            ++depth;
            continue;
        }
        for (;;) {
            if (const HTREEITEM next = TreeViewRelative(hwnd, TVGN_NEXT, current)) {
                current = next;
                break;
            }
            if (depth == 0) {
                current = nullptr;
                break;
            }
            current = TreeViewRelative(hwnd, TVGN_PARENT, current);
            --depth;
            if (!current) break;
        }
    }
    return true;
}

bool CaptureTreeViewState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    std::vector<TreeViewItemHandle> handles;
    if (!CollectTreeViewItems(hwnd, handles, reason)) return false;
    if (handles.empty())
        return Reject(reason, L"TreeView has no items to project");
    // A TreeView projects as its items, so the HWND's own text would only
    // duplicate them in the UIA name.
    node.text.clear();
    node.automationName.clear();
    node.items.clear();
    node.itemDepths.clear();
    node.itemExpanded.clear();
    node.itemHasChildren.clear();
    node.itemImages.clear();
    node.itemSelectedImages.clear();
    node.items.reserve(handles.size());
    node.itemDepths.reserve(handles.size());
    node.itemExpanded.reserve(handles.size());
    node.itemHasChildren.reserve(handles.size());
    node.itemImages.reserve(handles.size());
    node.itemSelectedImages.reserve(handles.size());
    if (!CaptureImageList(reinterpret_cast<HIMAGELIST>(
            SendMessageW(hwnd, TVM_GETIMAGELIST, TVSIL_NORMAL, 0)),
            node.imageList, reason)) return false;
    node.editableLabels = (static_cast<DWORD>(node.style) & TVS_EDITLABELS) != 0;
    const HWND editControl = reinterpret_cast<HWND>(
        SendMessageW(hwnd, TVM_GETEDITCONTROL, 0, 0));
    const HTREEITEM editing = editControl && IsWindow(editControl)
        ? TreeViewRelative(hwnd, TVGN_CARET, nullptr)
        : nullptr;
    node.editingIndex = -1;
    const HTREEITEM selected = TreeViewRelative(hwnd, TVGN_CARET, nullptr);
    node.selectedIndex = -1;
    node.multiSelect = false;
    size_t totalText = 0;
    std::vector<wchar_t> buffer(kMaxTreeLabelChars + 1);
    for (size_t index = 0; index < handles.size(); ++index) {
        std::fill(buffer.begin(), buffer.end(), L'\0');
        TVITEMW item{};
        item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN |
            TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        item.hItem = handles[index].item;
        item.stateMask = 0xffffu;
        item.pszText = buffer.data();
        item.cchTextMax = static_cast<int>(buffer.size());
        // TVM_GETITEMW is the control's own read path; a callback-text item is
        // resolved by the application's TVN_GETDISPINFO handler on this thread,
        // which is the same string the control would draw.
        if (!SendMessageW(hwnd, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
            return Reject(reason, L"TreeView item state is unavailable");
        if (item.cChildren == I_CHILDRENCALLBACK)
            return Reject(reason, L"callback-child TreeView item is not supported");
        if ((item.state & (TVIS_STATEIMAGEMASK | TVIS_OVERLAYMASK)) != 0)
            return Reject(reason, L"TreeView item state or overlay image is not supported");
        const size_t length = wcsnlen_s(buffer.data(), buffer.size());
        if (length == 0)
            return Reject(reason, L"TreeView requires nonempty textual labels");
        if (length >= kMaxTreeLabelChars)
            return Reject(reason, L"TreeView item label exceeds the text cap");
        if (totalText > kMaxStructuredTextChars - length)
            return Reject(reason, L"TreeView labels exceed the aggregate text cap");
        totalText += length;
        int image = -1;
        int selectedImage = -1;
        if (!ResolveItemImage(item.iImage, node.imageList.size(), image, reason) ||
            !ResolveItemImage(item.iSelectedImage, node.imageList.size(),
                selectedImage, reason)) return false;
        node.items.emplace_back(buffer.data(), length);
        node.itemDepths.push_back(handles[index].depth);
        node.itemExpanded.push_back((item.state & TVIS_EXPANDED) != 0);
        node.itemHasChildren.push_back(item.cChildren > 0 ||
            TreeViewRelative(hwnd, TVGN_CHILD, handles[index].item) != nullptr);
        node.itemImages.push_back(image);
        node.itemSelectedImages.push_back(selectedImage);
        if (handles[index].item == selected) node.selectedIndex = static_cast<int>(index);
        if (editing && handles[index].item == editing)
            node.editingIndex = static_cast<int>(index);
    }
    return true;
}

bool CaptureTrackbarState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto style = static_cast<DWORD>(node.style);
    node.minimum = static_cast<int>(SendMessageW(hwnd, TBM_GETRANGEMIN, 0, 0));
    node.maximum = static_cast<int>(SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0));
    node.position = static_cast<int>(SendMessageW(hwnd, TBM_GETPOS, 0, 0));
    node.smallChange = static_cast<int>(SendMessageW(hwnd, TBM_GETLINESIZE, 0, 0));
    node.largeChange = static_cast<int>(SendMessageW(hwnd, TBM_GETPAGESIZE, 0, 0));
    node.vertical = (style & TBS_VERT) != 0;
    node.reversed = (style & TBS_REVERSED) != 0;
    if (node.maximum <= node.minimum || node.position < node.minimum ||
        node.position > node.maximum)
        return Reject(reason, L"Trackbar has an invalid native range or position");
    // Zero is the documented "no keyboard step" value and is projected as such;
    // a negative step would mean the control is reporting something else.
    if (node.smallChange < 0 || node.largeChange < 0)
        return Reject(reason, L"Trackbar line or page size is negative");
    return true;
}

bool CaptureMdiChildState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto style = static_cast<DWORD>(node.style);
    const bool minimized = (style & WS_MINIMIZE) != 0;
    const bool maximized = (style & WS_MAXIMIZE) != 0;
    if (minimized && maximized)
        return Reject(reason, L"MDI child reports both minimized and maximized state");
    node.windowState = minimized ? L"minimized" : (maximized ? L"maximized" : L"normal");
    // WM_MDIGETACTIVE is the client's own answer to which child owns activation,
    // so the projection never has to infer it from z-order.
    const HWND client = GetParent(hwnd);
    const HWND active = client
        ? reinterpret_cast<HWND>(SendMessageW(client, WM_MDIGETACTIVE, 0, 0))
        : nullptr;
    node.active = active == hwnd;
    node.automationName = node.text;
    RECT clientArea{};
    RECT window{};
    POINT clientOrigin{};
    if (!GetClientRect(hwnd, &clientArea) || !GetWindowRect(hwnd, &window) ||
        !ClientToScreen(hwnd, &clientOrigin))
        return Reject(reason, L"MDI child client geometry is unavailable");
    const LONG insetX = clientOrigin.x - window.left;
    const LONG insetY = clientOrigin.y - window.top;
    if (insetX < 0 || insetY < 0 || clientArea.right < 0 || clientArea.bottom < 0)
        return Reject(reason, L"MDI child client inset is not inside its frame");
    node.clientRect = {
        insetX, insetY, insetX + clientArea.right, insetY + clientArea.bottom };
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
    at(ControlKind::TreeView) = &CaptureTreeViewState;
    at(ControlKind::StaticIcon) = &CaptureStaticIconState;
    at(ControlKind::TabControl) = &CaptureTabControlState;
    at(ControlKind::Slider) = &CaptureTrackbarState;
    at(ControlKind::StatusBar) = &CaptureStatusBarState;
    at(ControlKind::Toolbar) = &CaptureToolbarState;
    at(ControlKind::MdiChild) = &CaptureMdiChildState;
    at(ControlKind::PaneContainer) = &CapturePaneContainerState;
    at(ControlKind::AccessibleIsland) = &CaptureAccessibleIslandState;
    // StaticText, Separator, GroupBox, DialogContainer, and MdiClient are fully
    // described by the common facets, so they need no reader of their own.
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
    // An MDI child frame is identified by the role its frame gave it rather than
    // by its class name: the class belongs to the application, while the caption,
    // state, and system commands the projection replaces belong to
    // DefMDIChildProc.  Its client content still goes through the registry below,
    // one child window at a time.
    if ((static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)) & WS_EX_MDICHILD) != 0) {
        return ProbeMdiChild(hwnd, style, kind, reason);
    }
    for (const auto& adapter : kClassAdapters) {
        if (FluentShell::EqualsIgnoreCase(className, adapter.className)) {
            if (!adapter.probe(hwnd, style, kind, reason)) return false;
            // A control that owns visible child windows is not drawing its own content
            // there: a Static with children is a panel the application used as a frame,
            // which is a long-standing Win32 idiom.  Reclassify it geometrically so the
            // children it frames have a container to live in; if it does not qualify as
            // one either, the original refusal stands.
            if (kind == ControlKind::StaticText && HasVisibleChildWindow(hwnd)) {
                std::wstring panelReason;
                if (ProbePaneContainer(hwnd, style, kind, panelReason)) return true;
                kind = ControlKind::StaticText;
                reason = L"Static frames child windows but is not a usable container: " +
                    panelReason;
                return false;
            }
            return true;
        }
    }
    // A class the registry does not know can still be nothing but a frame around
    // other windows, or a host for content that owns no HWND at all.  Both are decided
    // by evidence rather than by name, so the rejection below reports whichever refusal
    // is the real one for this window.
    std::wstring containerReason;
    if (IsAccessibleIslandClass(className)) {
        if (ProbeAccessibleIsland(hwnd, style, kind, containerReason)) return true;
    } else if (ProbePaneContainer(hwnd, style, kind, containerReason)) {
        return true;
    }
    reason = L"unsupported visible control class: ";
    reason.append(className);
    reason.append(L" (");
    reason.append(containerReason);
    reason.append(L")");
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

HTREEITEM ResolveTreeViewItem(HWND treeView, int index) noexcept {
    if (!treeView || index < 0) return nullptr;
    try {
        std::vector<TreeViewItemHandle> handles;
        std::wstring ignored;
        if (!CollectTreeViewItems(treeView, handles, ignored) ||
            static_cast<size_t>(index) >= handles.size()) return nullptr;
        return handles[static_cast<size_t>(index)].item;
    } catch (...) {
        return nullptr;
    }
}

int ProportionalSplitTarget(
    int position,
    int fromExtent,
    int toExtent,
    int minimum,
    int maximum) noexcept {
    // A degenerate measurement carries no proportion, so the split stays where the
    // container currently has it as far as this function is concerned.
    if (fromExtent <= 0 || toExtent <= 0 || maximum < minimum) return position;
    const int64_t scaled =
        static_cast<int64_t>(position) * toExtent / fromExtent;
    return static_cast<int>(std::clamp<int64_t>(scaled, minimum, maximum));
}

bool SetPaneSplit(HWND container, int index, int position) noexcept {
    if (!container || index < 0) return false;
    try {
        RECT client{};
        if (!GetClientRect(container, &client)) return false;
        std::vector<PaneGap> gaps;
        std::wstring ignored;
        PaneSurface surface;
        if (!CollectPaneSurface(container, client, surface, ignored)) return false;
        gaps = std::move(surface.gaps);
        int found = 0;
        for (const PaneGap& gap : gaps) {
            const PaneNeighbours neighbours = ResolvePaneNeighbours(container, client, gap);
            if (!neighbours.before || !neighbours.after) continue;
            const int thickness = gap.vertical
                ? gap.rect.right - gap.rect.left
                : gap.rect.bottom - gap.rect.top;
            const int minimum =
                (gap.vertical ? neighbours.beforeRect.left : neighbours.beforeRect.top) +
                kMinPaneExtent;
            const int maximum =
                (gap.vertical ? neighbours.afterRect.right : neighbours.afterRect.bottom) -
                kMinPaneExtent - thickness;
            if (maximum < minimum) continue;
            if (found++ != index) continue;
            if (position < minimum || position > maximum) return false;
            RECT before = neighbours.beforeRect;
            RECT after = neighbours.afterRect;
            if (gap.vertical) {
                before.right = position;
                after.left = position + thickness;
            } else {
                before.bottom = position;
                after.top = position + thickness;
            }
            const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            const bool movedBefore = SetWindowPos(neighbours.before, nullptr,
                before.left, before.top, before.right - before.left, before.bottom - before.top,
                flags) != FALSE;
            const bool movedAfter = SetWindowPos(neighbours.after, nullptr,
                after.left, after.top, after.right - after.left, after.bottom - after.top,
                flags) != FALSE;
            return movedBefore && movedAfter;
        }
        return false;
    } catch (...) {
        return false;
    }
}

// When an MDI child is maximized, the frame relocates that child's caption
// buttons into its own menu band.  A projected surface draws the child's caption
// itself, with the same minimize, restore, and close commands, so the frame's
// duplicate is chrome the projection has already replaced -- exactly like the
// bitmap chrome items the MDI menu bar skips.  The evidence is deliberately
// narrow: a leaf window in the frame band, no text, no bigger than three caption
// buttons, and a maximized active MDI child to belong to.
bool SetListViewColumnOrder(HWND listView, const std::vector<int>& order) noexcept {
    if (!listView || order.empty()) return false;
    try {
        const int columnCount = static_cast<int>(order.size());
        std::vector<bool> seen(order.size(), false);
        for (const int logical : order) {
            if (logical < 0 || logical >= columnCount || seen[static_cast<size_t>(logical)]) {
                return false;
            }
            seen[static_cast<size_t>(logical)] = true;
        }
        std::vector<int> requested(order.begin(), order.end());
        if (!SendMessageW(listView, LVM_SETCOLUMNORDERARRAY,
                static_cast<WPARAM>(requested.size()),
                reinterpret_cast<LPARAM>(requested.data()))) {
            return false;
        }
        InvalidateRect(listView, nullptr, TRUE);
        if (const HWND header = reinterpret_cast<HWND>(
                SendMessageW(listView, LVM_GETHEADER, 0, 0))) {
            InvalidateRect(header, nullptr, TRUE);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool IsMaximizedMdiCaptionCluster(HWND hwnd, HWND parent) noexcept {
    if (GetWindow(hwnd, GW_CHILD) != nullptr) return false;
    if (GetWindowTextLengthW(hwnd) > 0) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || width > 144 || height > 48) return false;
    // Walk up to the frame that owns an MDI client, without crossing into the
    // client itself: the cluster lives in the frame's own band.
    HWND frame = parent;
    for (int depth = 0; frame && depth < 8; ++depth) {
        wchar_t buffer[kMaxClassNameChars]{};
        if (FluentShell::EqualsIgnoreCase(ClassNameOf(frame, buffer), L"MDIClient")) return false;
        const HWND client32 = FindWindowExW(frame, nullptr, L"MDIClient", nullptr);
        if (client32) {
            HWND active = reinterpret_cast<HWND>(
                SendMessageW(client32, WM_MDIGETACTIVE, 0, 0));
            return active != nullptr && IsZoomed(active);
        }
        frame = GetParent(frame);
    }
    return false;
}

bool IsCompositeImplementationChild(HWND hwnd) noexcept {
    const HWND parent = GetParent(hwnd);
    if (!parent) return false;
    if (IsMaximizedMdiCaptionCluster(hwnd, parent)) return true;
    wchar_t parentBuffer[kMaxClassNameChars]{};
    const auto parentClass = ClassNameOf(parent, parentBuffer);
    if (FluentShell::EqualsIgnoreCase(parentClass, L"ComboBox")) {
        COMBOBOXINFO info{sizeof(info)};
        return GetComboBoxInfo(parent, &info) &&
            (hwnd == info.hwndItem || hwnd == info.hwndList);
    }
    // An open label-edit session belongs to the control that owns it: the native
    // edit control is part of the tree's or list's own contract, and the
    // projection drives it through that control rather than projecting it as a
    // separate text box.
    if (FluentShell::EqualsIgnoreCase(parentClass, WC_TREEVIEWW)) {
        return hwnd == reinterpret_cast<HWND>(
            SendMessageW(parent, TVM_GETEDITCONTROL, 0, 0));
    }
    wchar_t childBuffer[kMaxClassNameChars]{};
    if (FluentShell::EqualsIgnoreCase(parentClass, L"MonitorPaletteClass")) {
        return FluentShell::EqualsIgnoreCase(
            ClassNameOf(hwnd, childBuffer), L"MonitorPaletteEntryClass");
    }
    // A status bar's text lives in its own parts.  An application that parks an empty
    // child window inside one -- a placeholder for text it will set later, or a
    // progress bar it keeps hidden -- contributes nothing the projection can lose, so
    // that child is absorbed.  One carrying text is not: that would be content.
    if (FluentShell::EqualsIgnoreCase(parentClass, STATUSCLASSNAMEW)) {
        return GetWindowTextLengthW(hwnd) == 0;
    }
    if (!FluentShell::EqualsIgnoreCase(parentClass, WC_LISTVIEWW)) return false;
    return FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, childBuffer), WC_HEADERW) ||
        hwnd == reinterpret_cast<HWND>(SendMessageW(parent, LVM_GETEDITCONTROL, 0, 0));
}

} // namespace FluentShell::Bridge::Translation
