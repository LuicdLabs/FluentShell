#include "ControlAdapters.h"
#include "CaptureTrace.h"
#include "DateTimePickerValue.h"
#include "PaintedPixel.h"
#include "OwnerDrawComboBox.h"
#include "GuiThreadDispatch.h"

#include "AccessibleIsland.h"
#include "NativeDocument.h"
#include "DiskView.h"
#include "PerformanceChart.h"
#include "RichText.h"
#include "Sysmon.h"
#include "MmcHtmlDocument.h"
#include "ListViewActivation.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>
#include <ole2.h>
#include <oleacc.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
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

// The control sits in a Microsoft Management Console frame: its result controls open an item
// on a double-click, and answer the Enter key the same way.
bool IsInsideMmcFrame(HWND hwnd) noexcept {
    const HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    wchar_t name[32]{};
    return root && GetClassNameW(root, name, static_cast<int>(std::size(name))) &&
        wcscmp(name, L"MMCMainFrame") == 0;
}

// MMC's scope tree is the SysTreeView32 its view window parents directly. A snap-in's own
// tree (Device Manager's) sits deeper, inside the OCX host, and is a result control.
bool IsMmcScopeTree(HWND tree) noexcept {
    const HWND parent = tree ? GetParent(tree) : nullptr;
    wchar_t name[32]{};
    return parent && GetClassNameW(parent, name, static_cast<int>(std::size(name))) &&
        wcscmp(name, L"MMCViewWindow") == 0;
}

// A control answers a geometry message in the coordinate space of its own window.
// The capture pass reads window rectangles inside a per-monitor-aware scope, so
// those are physical pixels, while a DPI-virtualized process answers in the
// smaller space DWM stretches onto the screen -- a 96-DPI dialog on a 150%
// monitor reports an 18 px tab band that occupies 27 physical px.  Everything
// that travels in a snapshot is physical, so message-derived geometry is brought
// into that space here.  Measuring one client rectangle in both spaces gives the
// factor without inferring it from DPI semantics, and yields exactly 1 for a
// window that is not virtualized, so a per-monitor-aware target is untouched.
struct MessageGeometryScale final {
    double x = 1.0;
    double y = 1.0;

    static MessageGeometryScale For(HWND hwnd) noexcept {
        RECT physical{};
        if (!GetClientRect(hwnd, &physical) || physical.right <= 0 || physical.bottom <= 0) {
            return {};
        }
        const DPI_AWARENESS_CONTEXT ownContext = GetWindowDpiAwarenessContext(hwnd);
        if (!ownContext) return {};
        const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(ownContext);
        if (!previous) return {};
        RECT own{};
        const bool read = GetClientRect(hwnd, &own) != FALSE;
        SetThreadDpiAwarenessContext(previous);
        if (!read || own.right <= 0 || own.bottom <= 0) return {};
        return MessageGeometryScale{
            static_cast<double>(physical.right) / static_cast<double>(own.right),
            static_cast<double>(physical.bottom) / static_cast<double>(own.bottom),
        };
    }

    bool IsIdentity() const noexcept { return x == 1.0 && y == 1.0; }

    int Horizontal(int value) const noexcept {
        return IsIdentity() ? value : static_cast<int>(std::lround(value * x));
    }

    int Vertical(int value) const noexcept {
        return IsIdentity() ? value : static_cast<int>(std::lround(value * y));
    }

    RECT Rect(const RECT& rect) const noexcept {
        if (IsIdentity()) return rect;
        return RECT{
            static_cast<LONG>(std::lround(rect.left * x)),
            static_cast<LONG>(std::lround(rect.top * y)),
            static_cast<LONG>(std::lround(rect.right * x)),
            static_cast<LONG>(std::lround(rect.bottom * y)),
        };
    }
};

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

// MMC draws its result pane's description bar itself: an owner-draw Static under its
// own view window whose paint is exactly the window text on a caption band.  That is a
// text label, so it projects as one; any other owner-draw Static keeps its refusal.
bool IsMmcDescriptionBar(HWND hwnd, DWORD style) noexcept {
    if ((style & (SS_NOTIFY | WS_TABSTOP)) != 0) return false;
    const HWND parent = GetParent(hwnd);
    wchar_t parentClass[32]{};
    return parent && GetClassNameW(parent, parentClass, 32) > 0 &&
        FluentShell::EqualsIgnoreCase(parentClass, L"MMCViewWindow") &&
        WindowClassRegisteredBySystemModule(parent, L"mmc.exe");
}

// Windows Forms registers per-AppDomain subclasses of the standard controls
// ("WindowsForms10.STATIC.app.0.<hash>"). With the default FlatStyle.Standard a
// Label or Button is owner-drawn at the Win32 level and painted by WinForms itself,
// so its style bits no longer say what it is. WinForms publishes that through the
// window's own MSAA object, which is the evidence these adapters rely on.
bool IsWindowsFormsWindow(HWND hwnd) noexcept {
    wchar_t raw[kMaxClassNameChars]{};
    const int length = GetClassNameW(hwnd, raw, static_cast<int>(kMaxClassNameChars));
    return length > 0 && std::wstring_view(raw, static_cast<size_t>(length)).starts_with(L"WindowsForms10.");
}

bool ReadWindowAccessible(HWND hwnd, long& role, long& state) noexcept {
    role = state = 0;
    DWORD process = 0;
    if (!hwnd || GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() ||
        process != GetCurrentProcessId()) return false;
    Microsoft::WRL::ComPtr<IAccessible> accessible;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_PPV_ARGS(accessible.GetAddressOf()))) ||
        !accessible) return false;
    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VARIANT roleValue{};
    VARIANT stateValue{};
    const bool read = SUCCEEDED(accessible->get_accRole(self, &roleValue)) && roleValue.vt == VT_I4 &&
        SUCCEEDED(accessible->get_accState(self, &stateValue)) && stateValue.vt == VT_I4;
    if (read) {
        role = roleValue.lVal;
        state = stateValue.lVal;
    }
    VariantClear(&roleValue);
    VariantClear(&stateValue);
    return read;
}

// A leaf Windows Forms surface (Panel, PictureBox) that hosts nothing, is no tab stop
// and offers no accessible action: a header strip, a separator or an icon. Its own
// painted pixels are all it contributes, like Disk Management's legend. A tab-stop or
// actionable custom control keeps its refusal.
constexpr int kMaxInertSurfaceWidth = 1024;
constexpr int kMaxInertSurfaceHeight = 256;
// A rule a few pixels thick (the divider a wizard draws beside its page) may run the whole
// way: its pixels are few whatever its length, and the picture cap bounds that length.
constexpr int kMaxInertRuleThickness = 4;
constexpr int kMaxInertRuleLength = 1024;

// A tab stop normally marks an interactive custom control and keeps its refusal.  A
// header strip no taller than this that takes focus but publishes no action and frames
// nothing -- the section headers of the Windows Firewall snap-in's overview, where a
// click changes nothing -- is still only its painted band.
constexpr int kMaxFocusableStripHeight = 32;

bool IsWindowsFormsInertSurface(HWND hwnd, DWORD style) noexcept {
    wchar_t raw[kMaxClassNameChars]{};
    const int length = GetClassNameW(hwnd, raw, static_cast<int>(kMaxClassNameChars));
    if (length <= 0 || !std::wstring_view(raw, static_cast<size_t>(length)).starts_with(L"WindowsForms10.Window."))
        return false;
    RECT client{};
    if ((style & (WS_HSCROLL | WS_VSCROLL)) != 0 || GetWindow(hwnd, GW_CHILD) ||
        !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return false;
    const bool rule = (std::min)(client.right, client.bottom) <= kMaxInertRuleThickness &&
        (std::max)(client.right, client.bottom) <= kMaxInertRuleLength;
    if (!rule && (client.right > kMaxInertSurfaceWidth || client.bottom > kMaxInertSurfaceHeight)) return false;
    if ((style & WS_TABSTOP) != 0 && client.bottom > kMaxFocusableStripHeight) return false;
    long role = 0;
    long state = 0;
    // WinForms reports STATE_SYSTEM_FOCUSABLE for every visible, enabled control, so
    // keyboard participation is judged by WS_TABSTOP above.
    // A Panel answers as a grouping (the 1-pixel rules a wizard draws between its header, page
    // and buttons); a PictureBox or a plain Control as a client or a graphic.
    if (!ReadWindowAccessible(hwnd, role, state) ||
        (role != ROLE_SYSTEM_CLIENT && role != ROLE_SYSTEM_GRAPHIC && role != ROLE_SYSTEM_PANE &&
         role != ROLE_SYSTEM_GROUPING) ||
        (state & STATE_SYSTEM_LINKED) != 0) return false;
    Microsoft::WRL::ComPtr<IAccessible> accessible;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_PPV_ARGS(accessible.GetAddressOf()))) ||
        !accessible) return false;
    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    BSTR action = nullptr;
    const bool actionable = SUCCEEDED(accessible->get_accDefaultAction(self, &action)) &&
        action && SysStringLen(action) != 0;
    if (action) SysFreeString(action);
    return !actionable;
}

// The links a Windows Forms LinkLabel publishes as the accessible children of its own
// static-text object, one ROLE_SYSTEM_LINK child per link.  A plain Label has none.
// Each entry keeps the object to act on and the child ID to address it with.
struct AccessibleLink final {
    Microsoft::WRL::ComPtr<IAccessible> object;
    long child = CHILDID_SELF;
    std::wstring name;
    bool enabled = true;
};

bool ReadWindowsFormsLinks(HWND hwnd, std::vector<AccessibleLink>& links) noexcept {
    links.clear();
    try {
        long role = 0;
        long state = 0;
        if (!IsWindowsFormsWindow(hwnd) || GetWindow(hwnd, GW_CHILD) ||
            !ReadWindowAccessible(hwnd, role, state) || role != ROLE_SYSTEM_STATICTEXT) return false;
        Microsoft::WRL::ComPtr<IAccessible> label;
        long count = 0;
        if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_PPV_ARGS(label.GetAddressOf()))) ||
            !label || FAILED(label->get_accChildCount(&count)) || count < 0 || count > 64) return false;
        if (count == 0) return true;
        std::vector<VARIANT> children(static_cast<size_t>(count));
        long obtained = 0;
        if (FAILED(AccessibleChildren(label.Get(), 0, count, children.data(), &obtained))) return false;
        bool valid = obtained == count;
        for (long index = 0; index < obtained; ++index) {
            VARIANT& entry = children[static_cast<size_t>(index)];
            AccessibleLink link;
            if (entry.vt == VT_DISPATCH && entry.pdispVal) {
                valid = valid && SUCCEEDED(entry.pdispVal->QueryInterface(IID_PPV_ARGS(link.object.GetAddressOf())));
            } else if (entry.vt == VT_I4) {
                link.object = label;
                link.child = entry.lVal;
            } else {
                valid = false;
            }
            VariantClear(&entry);
            if (!valid || !link.object) { valid = false; continue; }
            VARIANT id{};
            id.vt = VT_I4;
            id.lVal = link.child;
            VARIANT linkRole{};
            VARIANT linkState{};
            BSTR name = nullptr;
            BSTR action = nullptr;
            valid = SUCCEEDED(link.object->get_accRole(id, &linkRole)) && linkRole.vt == VT_I4 &&
                linkRole.lVal == ROLE_SYSTEM_LINK &&
                SUCCEEDED(link.object->get_accState(id, &linkState)) && linkState.vt == VT_I4 &&
                SUCCEEDED(link.object->get_accName(id, &name)) && name && SysStringLen(name) != 0 &&
                SUCCEEDED(link.object->get_accDefaultAction(id, &action)) && action && SysStringLen(action) != 0;
            if (valid) {
                link.name.assign(name, SysStringLen(name));
                link.enabled = (linkState.lVal & STATE_SYSTEM_UNAVAILABLE) == 0;
                links.push_back(std::move(link));
            }
            VariantClear(&linkRole);
            VariantClear(&linkState);
            if (name) SysFreeString(name);
            if (action) SysFreeString(action);
        }
        if (!valid) links.clear();
        return valid;
    } catch (...) {
        links.clear();
        return false;
    }
}

// A control whose font is a symbol font draws glyphs rather than the characters of
// its text -- Marlett's "5" is a chevron and its "r" a close cross -- so projecting
// that text as text would show the letters.  Its painted pixels are its content.
bool UsesSymbolFont(HWND hwnd) noexcept {
    const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    LOGFONTW face{};
    if (!font || GetObjectW(font, sizeof(face), &face) != sizeof(face)) return false;
    if (face.lfCharSet == SYMBOL_CHARSET) return true;
    constexpr std::wstring_view symbolFaces[] = { L"Marlett", L"Webdings", L"Wingdings",
        L"Wingdings 2", L"Wingdings 3", L"Symbol", L"Segoe MDL2 Assets", L"Segoe Fluent Icons" };
    for (const auto name : symbolFaces) {
        if (FluentShell::EqualsIgnoreCase(face.lfFaceName, name)) return true;
    }
    return false;
}

// A WinForms Label paints exactly its window text.  A LinkLabel answers with the same
// static-text role but publishes its links as accessible children, so it is told apart
// by those rather than by its role.
bool IsWindowsFormsLabel(HWND hwnd, DWORD style) noexcept {
    std::vector<AccessibleLink> links;
    return (style & WS_TABSTOP) == 0 && ReadWindowsFormsLinks(hwnd, links) && links.empty();
}

// A Windows Forms LinkLabel with exactly one link is the bounded SysLink contract: its
// label text with one link run in it, activated through that link's own accessible
// default action, which raises the LinkClicked event the application handles.
bool IsWindowsFormsLinkLabel(HWND hwnd) noexcept {
    std::vector<AccessibleLink> links;
    return ReadWindowsFormsLinks(hwnd, links) && links.size() == 1;
}

bool ProbeStatic(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    switch (style & SS_TYPEMASK) {
    case SS_OWNERDRAW:
        if (IsWindowsFormsLinkLabel(hwnd)) {
            kind = ControlKind::SysLink;
            return true;
        }
        // A WinForms Label drawn in a symbol font (a Marlett chevron) is its glyphs.
        if (IsWindowsFormsLabel(hwnd, style) && UsesSymbolFont(hwnd)) {
            kind = ControlKind::StaticBitmap;
            return true;
        }
        if (!IsMmcDescriptionBar(hwnd, style) && !IsWindowsFormsLabel(hwnd, style))
            return Reject(reason, L"unsupported Static draw style");
        kind = ControlKind::StaticText;
        return true;
    case SS_LEFT:
    case SS_CENTER:
    case SS_RIGHT:
    case SS_SIMPLE:
    case SS_LEFTNOWORDWRAP: {
        // A label has no text to be tabbed to: a Static that takes the focus and says nothing
        // is a control the dialog procedure subclassed and paints itself (the palettes of the
        // Color chooser), whose pixels and clicks a text label could not carry.
        if ((style & WS_TABSTOP) != 0 && GetWindowTextLengthW(hwnd) == 0)
            return Reject(reason, L"focusable Static without text is a custom-drawn control");
        kind = ControlKind::StaticText;
        return true;
    }
    case SS_ETCHEDHORZ:
    case SS_ETCHEDVERT:
        kind = ControlKind::Separator;
        return true;
    case SS_BLACKRECT:
    case SS_GRAYRECT:
    case SS_WHITERECT:
    case SS_BLACKFRAME:
    case SS_GRAYFRAME:
    case SS_WHITEFRAME:
    case SS_ETCHEDFRAME: {
        if ((style & (SS_NOTIFY | WS_TABSTOP)) != 0)
            return Reject(reason, L"interactive Static decoration is not supported");
        constexpr DWORD acceptedStyle = WS_CHILD | WS_VISIBLE | WS_DISABLED |
            WS_GROUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | SS_TYPEMASK | SS_SUNKEN;
        constexpr DWORD acceptedExStyle = WS_EX_STATICEDGE | WS_EX_NOPARENTNOTIFY;
        const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        if ((style & WS_CHILD) == 0 || (style & ~acceptedStyle) != 0 ||
            (exStyle & ~acceptedExStyle) != 0)
            return Reject(reason, L"Static decoration has unsupported window chrome or composition");
        kind = ControlKind::StaticDecoration;
        return true;
    }
    case SS_BITMAP:
        // A wizard's side art and similar pictures: inert pixels with no semantics of
        // their own.  An interactive one would need a click contract.
        if ((style & (SS_NOTIFY | WS_TABSTOP)) != 0)
            return Reject(reason, L"interactive Static bitmap is not supported");
        kind = ControlKind::StaticBitmap;
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

bool ProbeButton(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    // A push button may show an icon or a bitmap instead of its text; the picture is
    // captured and the text stays its accessible name.  A check box or radio button
    // drawn as a picture has no projected toggle contract.
    const DWORD type = style & BS_TYPEMASK;
    const DWORD picture = style & (BS_BITMAP | BS_ICON);
    const bool pushButton = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
    if (type == BS_OWNERDRAW && picture == 0 && IsWindowsFormsWindow(hwnd)) {
        // WinForms Button, CheckBox and RadioButton are all BS_OWNERDRAW here; the
        // role tells them apart and the check state is read from the same object.
        long role = 0;
        long state = 0;
        if (!ReadWindowAccessible(hwnd, role, state))
            return Reject(reason, L"owner-draw WinForms button publishes no accessible role");
        switch (role) {
        case ROLE_SYSTEM_PUSHBUTTON: kind = ControlKind::Button; return true;
        case ROLE_SYSTEM_CHECKBUTTON:
            kind = (state & STATE_SYSTEM_MIXED) != 0 ? ControlKind::ThreeState : ControlKind::CheckBox;
            return true;
        case ROLE_SYSTEM_RADIOBUTTON: kind = ControlKind::RadioButton; return true;
        default: return Reject(reason, L"owner-draw WinForms button has an unsupported accessible role");
        }
    }
    if (type == BS_OWNERDRAW) {
        // A WinForms button that reaches here carries picture bits as well, which has no contract. Any
        // other owner-draw button is a push button whose whole face, frame included, its owner paints: it
        // is carried as that picture, and pressing it clicks the control. A button picture is capped at an
        // icon's size, so a wider face -- Performance Monitor's colour swatch, which no key reaches -- is
        // the inert picture it shows; a wider face that takes focus has no contract.
        if (IsWindowsFormsWindow(hwnd)) return Reject(reason, L"unsupported Button draw style");
        RECT client{};
        const bool fitsButtonPicture = GetClientRect(hwnd, &client) &&
            client.right <= static_cast<LONG>(Ipc::kMaxImageDimension) &&
            client.bottom <= static_cast<LONG>(Ipc::kMaxImageDimension);
        if (fitsButtonPicture) kind = ControlKind::Button;
        else if ((style & WS_TABSTOP) == 0) kind = ControlKind::StaticBitmap;
        else return Reject(reason, L"owner-draw button face exceeds the picture cap");
        return true;
    }
    if (picture == (BS_BITMAP | BS_ICON) || (picture != 0 && !pushButton)) {
        return Reject(reason, L"unsupported Button draw style");
    }
    switch (type) {
    case BS_PUSHBUTTON:
    case BS_DEFPUSHBUTTON:
    // A command link is a push button that draws its label, a note under it and a
    // leading arrow (Windows Memory Diagnostic's two choices): the note is read with it.
    case BS_COMMANDLINK:
    case BS_DEFCOMMANDLINK:
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
        // A group box never takes the focus, so a WS_TABSTOP a dialog template gave it (MMC's
        // Customize View has one) is a stop on nothing: capture drops it from the traversal.
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

// A rich edit control reads exactly like a plain text box when nothing about its
// content needs rich rendering: one size and colour, no emphasis, no hidden, raised or
// lowered run, no link, and no embedded object.  Font faces may differ: RichEdit binds
// a CJK run and a Latin run to different fonts on its own (Event Viewer's descriptions
// alternate 微軟正黑體 and Calibri), which a plain text box does the same way.  The
// text object model answers all of it for the whole story without touching the
// selection; a property that varies across the range reads back as tomUndefined.
// `decorationOnly`: underline and colour may vary too, as they do in an edit box that underlines
// what it recognised (the object picker's resolved names); nothing else may.
bool ReadRichEditPlainContent(HWND hwnd, std::wstring& reason, bool* formatted = nullptr,
    bool decorationOnly = false) {
    if (formatted) *formatted = false;
    IRichEditOle* raw = nullptr;
    if (!SendMessageW(hwnd, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&raw)) || !raw)
        return Reject(reason, L"RichEdit publishes no text object model");
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    ole.Attach(raw);
    if (ole->GetObjectCount() != 0)
        return Reject(reason, L"RichEdit embeds objects the projection cannot draw");
    Microsoft::WRL::ComPtr<ITextDocument> document;
    Microsoft::WRL::ComPtr<ITextRange> story;
    long storyLength = 0;
    if (FAILED(ole.As(&document)) || FAILED(document->Range(0, 0, story.GetAddressOf())) || !story ||
        FAILED(story->GetStoryLength(&storyLength)))
        return Reject(reason, L"RichEdit character formats could not be read");
    // The story's final paragraph mark keeps a format of its own and draws nothing.
    if (storyLength <= 1) return true;
    Microsoft::WRL::ComPtr<ITextRange> text;
    Microsoft::WRL::ComPtr<ITextFont> font;
    if (FAILED(document->Range(0, storyLength - 1, text.GetAddressOf())) || !text ||
        FAILED(text->GetFont(font.GetAddressOf())) || !font)
        return Reject(reason, L"RichEdit character formats could not be read");
    long bold = tomUndefined, italic = tomUndefined, underline = tomUndefined, strike = tomUndefined;
    long hidden = tomUndefined, superscript = tomUndefined, subscript = tomUndefined;
    long foreground = tomUndefined, background = tomUndefined;
    float size = static_cast<float>(tomUndefined);
    if (FAILED(font->GetBold(&bold)) || FAILED(font->GetItalic(&italic)) ||
        FAILED(font->GetUnderline(&underline)) || FAILED(font->GetStrikeThrough(&strike)) ||
        FAILED(font->GetHidden(&hidden)) || FAILED(font->GetSuperscript(&superscript)) ||
        FAILED(font->GetSubscript(&subscript)) || FAILED(font->GetForeColor(&foreground)) ||
        FAILED(font->GetBackColor(&background)) || FAILED(font->GetSize(&size)))
        return Reject(reason, L"RichEdit character formats could not be read");
    const auto plain = [](long value) { return value == tomFalse || value == tomNone; };
    const bool decorated = decorationOnly && (underline == tomUndefined || underline == tomSingle ||
        foreground == tomUndefined || background == tomUndefined);
    if (!plain(bold) || !plain(italic) || (!plain(underline) && !decorated) || !plain(strike) || !plain(hidden) ||
        !plain(superscript) || !plain(subscript) || (foreground == tomUndefined && !decorationOnly) ||
        (background == tomUndefined && !decorationOnly) || size == static_cast<float>(tomUndefined)) {
        // Formatted, not unreadable: the rich text adapter decides whether it can draw it.
        if (formatted) *formatted = true;
        return Reject(reason, L"RichEdit text mixes character formats, links, or effects");
    }
    // A link draws as a link and acts on a click; only the TOM 2 font reports one.
    Microsoft::WRL::ComPtr<ITextRange2> text2;
    Microsoft::WRL::ComPtr<ITextFont2> font2;
    long link = tomUndefined;
    if (FAILED(text.As(&text2)) || FAILED(text2->GetFont2(font2.GetAddressOf())) || !font2 ||
        FAILED(font2->GetLinkType(&link)))
        return Reject(reason, L"RichEdit cannot report whether its text holds links");
    if (link != tomNoLink)
        return Reject(reason, L"RichEdit text mixes character formats, links, or effects");
    return true;
}

// Read-only, because an edit made through the projection would write plain text over
// the application's formatting.  Windows Forms' RichTextBox showing Event Viewer's
// event description is this shape.
bool ProbeRichEdit(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & ES_PASSWORD) != 0) return Reject(reason, L"password RichEdit is not supported");
    if ((style & ES_READONLY) == 0) {
        // An editable RichEdit is an edit box while its text is plain, or only underlined or
        // coloured (the object picker underlines the names it resolved); anything else would
        // lose its formatting through a plain-text edit.
        std::wstring plainReason;
        if (!ReadRichEditPlainContent(hwnd, plainReason, nullptr, true))
            return Reject(reason, L"editable RichEdit would lose its formatting through a plain-text edit");
        kind = ControlKind::Edit;
        return true;
    }
    bool formatted = false;
    if (ReadRichEditPlainContent(hwnd, reason, &formatted)) {
        kind = ControlKind::Edit;
        return true;
    }
    // Text in more than one format (a certificate's bold labels) is carried as formatted
    // runs, read-only, when every run is one the projection can draw.
    std::vector<RichParagraphSnapshot> paragraphs;
    std::wstring richReason;
    if (!formatted || !ReadRichText(hwnd, paragraphs, richReason)) {
        if (formatted) reason = richReason;
        return false;
    }
    reason.clear();
    kind = ControlKind::RichText;
    return true;
}

bool ProbeComboBox(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    const DWORD ownerDraw = style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE);
    if (ownerDraw == (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE))
        return Reject(reason, L"owner-draw ComboBox has conflicting fixed and variable styles");
    const DWORD type = style & 0x0003u;
    if (type != CBS_DROPDOWNLIST && type != CBS_DROPDOWN) {
        return Reject(reason,
            L"ComboBox is simple or has an unsupported type");
    }
    kind = ControlKind::ComboBox;
    return true;
}

// A ComboBoxEx (mstsc's computer box) is a combo box that stores and draws its items itself:
// its inner ComboBox is owner-drawn without strings, and typing goes into an Edit of its own.
// It is a comboBox node read through what the ComboBoxEx answers about its items, and its
// inner parts are absorbed by it.
bool ProbeComboBoxEx(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    const DWORD type = style & 0x0003u;
    if (type != CBS_DROPDOWNLIST && type != CBS_DROPDOWN)
        return Reject(reason, L"ComboBoxEx is simple or has an unsupported type");
    kind = ControlKind::ComboBox;
    return true;
}

bool ProbeListBox(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    // A multi-select list (LBS_MULTIPLESEL toggles each click, LBS_EXTENDEDSEL takes ctrl and shift)
    // carries its selection as an index list; the rest of the unsupported styles change what an item
    // is, which is not something the projection can carry.
    constexpr DWORD unsupported = LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE |
        LBS_NODATA | LBS_MULTICOLUMN | LBS_NOSEL;
    if ((style & unsupported) != 0) {
        return Reject(reason,
            L"owner-draw, virtual, or multi-column ListBox");
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
    // WS_TABSTOP is tolerated: a dialog template can flag a progress bar as a stop, but
    // the control takes no keyboard input, so capture keeps it out of traversal.
    if ((style & PBS_VERTICAL) != 0) {
        return Reject(reason, L"vertical ProgressBar is not supported");
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

bool ReadListViewMode(HWND hwnd, DWORD style, std::wstring& mode, std::wstring& reason) {
    const LRESULT view = SendMessageW(hwnd, LVM_GETVIEW, 0, 0);
    if (view == LV_VIEW_TILE) return Reject(reason, L"tile ListView is not supported");
    if (view < LV_VIEW_ICON || view > LV_VIEW_LIST)
        return Reject(reason, L"ListView returned an unknown presentation mode");
    const DWORD legacyView = style & LVS_TYPEMASK;
    // Older common controls return zero for an unimplemented LVM_GETVIEW. The
    // four original views remain represented by LVS_TYPEMASK. A modern control
    // implements the stable-ID messages as well, whose invalid-index sentinel
    // distinguishes its real icon-view zero from that legacy fallback. Prefer
    // the current view so a menu's LVM_SETVIEW change is not inferred from style.
    const bool modernView = view != LV_VIEW_ICON ||
        static_cast<uint32_t>(SendMessageW(hwnd, LVM_MAPINDEXTOID,
            static_cast<WPARAM>(-1), 0)) == std::numeric_limits<uint32_t>::max();
    switch (modernView ? static_cast<DWORD>(view) : legacyView) {
    case LVS_REPORT: mode = L"report"; break;
    case LVS_ICON: mode = L"largeIcon"; break;
    case LVS_SMALLICON: mode = L"smallIcon"; break;
    case LVS_LIST: mode = L"list"; break;
    default: return Reject(reason, L"ListView style has an unknown presentation mode");
    }
    return true;
}

bool ProbeListView(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & LVS_OWNERDRAWFIXED) != 0)
        return Reject(reason, L"owner-draw ListView is not supported (LVS_OWNERDRAWFIXED)");
    std::wstring mode;
    if (!ReadListViewMode(hwnd, style, mode, reason)) return false;
    if ((style & LVS_OWNERDATA) != 0 && SendMessageW(hwnd, LVM_ISGROUPVIEWENABLED, 0, 0))
        return Reject(reason, L"owner-data groups require an application-owned group membership contract");
    // LVS_EX_HEADERDRAGDROP only says the user may reorder the columns.  The
    // projection captures the display order and offers the same reordering, so the
    // flag is evidence to carry rather than a reason to refuse.
    constexpr DWORD unsupportedExtended = LVS_EX_TRACKSELECT |
        LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
    // Virtual lists keep only selection and focus in comctl32. The existing
    // checkbox action writes stored state images, so it cannot truthfully offer
    // application-owned check state through that same contract.
    if ((style & LVS_OWNERDATA) != 0 && (extended & LVS_EX_CHECKBOXES) != 0)
        return Reject(reason, L"owner-data ListView checkboxes require application-owned state semantics (LVS_OWNERDATA | LVS_EX_CHECKBOXES)");
    if ((style & LVS_OWNERDATA) != 0) {
        const auto callbackMask = static_cast<UINT>(SendMessageW(hwnd, LVM_GETCALLBACKMASK, 0, 0));
        UINT nativeState = LVIS_SELECTED | LVIS_FOCUSED;
        // Windows Forms puts LVIS_STATEIMAGEMASK in every virtual list's callback mask
        // so it can answer check states on demand.  Without a state image list comctl32
        // draws no state image whatever the owner answers, so those bits are invisible
        // and there is nothing for the projection to carry.
        if (!SendMessageW(hwnd, LVM_GETIMAGELIST, LVSIL_STATE, 0)) nativeState |= LVIS_STATEIMAGEMASK;
        if (const UINT rejected = callbackMask & ~nativeState; rejected != 0) {
            static constexpr NamedFlag flags[] = {
                { LVIS_STATEIMAGEMASK, L"LVIS_STATEIMAGEMASK" },
                { LVIS_OVERLAYMASK, L"LVIS_OVERLAYMASK" },
                { LVIS_CUT, L"LVIS_CUT" },
                { LVIS_DROPHILITED, L"LVIS_DROPHILITED" },
            };
            return RejectFlags(reason, L"owner-data ListView has unsupported callback state flag(s) ",
                rejected, callbackMask, flags, std::size(flags));
        }
    }
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
    // CCS_NODIVIDER only drops the top divider, and the remaining CCS_* bits -- top or
    // bottom alignment, NOMOVEY, NORESIZE, NOPARENTALIGN -- only say how the control
    // places itself when its parent resizes; the projection lays the toolbar out from
    // its native rectangle either way.  TBSTYLE_LIST puts the label beside the icon,
    // which the projected button already does.  TBSTYLE_ALTDRAG only matters to a
    // customizable (CCS_ADJUSTABLE) toolbar, which stays refused.
    constexpr DWORD accepted = CCS_TOP | CCS_NOMOVEY | CCS_NODIVIDER | CCS_NORESIZE |
        CCS_NOPARENTALIGN | TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE | TBSTYLE_FLAT |
        TBSTYLE_TRANSPARENT | TBSTYLE_LIST | TBSTYLE_ALTDRAG;
    if ((toolbarStyle & ~accepted) != 0) {
        wchar_t text[128]{};
        swprintf_s(text, L"ToolbarWindow32 has unsupported style bits 0x%04lX (style=0x%08lX)",
            toolbarStyle & ~accepted, style);
        reason = text;
        return false;
    }
    // TBSTYLE_EX_DRAWDDARROWS only draws the arrow of a BTNS_DROPDOWN button as a separate part
    // of it, which is how the snapshot already carries such a button; every other extended style
    // changes what a button looks like or does.
    const auto extended = static_cast<DWORD>(SendMessageW(hwnd, TB_GETEXTENDEDSTYLE, 0, 0));
    if ((extended & ~static_cast<DWORD>(TBSTYLE_EX_DRAWDDARROWS)) != 0) {
        wchar_t text[112]{};
        swprintf_s(text, L"ToolbarWindow32 has unsupported extended style 0x%08lX", extended);
        reason = text;
        return false;
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

    // TCS_RAGGEDRIGHT (WinForms' default) only changes how a multiline row is
    // justified, and the projection uses each tab's native rectangle anyway.
    constexpr DWORD accepted = TCS_MULTILINE | TCS_HOTTRACK | TCS_RAGGEDRIGHT;
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

// The bounded TreeView subset: a single-selection tree whose hierarchy,
// expansion, selection, item icons, overlays, and state images are all readable
// from the control itself. Checkbox toggles, hover selection, auto-collapse,
// and callback tooltips each own behavior the projection cannot reproduce, so
// each keeps the whole window native by name.
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
    // A state image list is painted beside each item that names one. Checkbox
    // styles stay refused above: those glyphs are a toggle, not decoration.
    // The control's built-in tooltip only re-renders a truncated item label the
    // adapter already captures, so its presence is not interrogated.  Application
    // tooltip *content* arrives through TVS_INFOTIP and TVS_EX_RICHTOOLTIP, and
    // both are refused above.
    kind = ControlKind::TreeView;
    return true;
}

// The bounded Trackbar subset: a plain range control whose value the projection
// can both read and drive through the control's own notification.  A selection
// range, a thumbless bar, and pre-move veto snapping are each contracts the
// projection would have to invent, so they stay native.  TBS_TOOLTIPS only shows
// the position beside the thumb while dragging, which the projected Slider's thumb
// tooltip reproduces; a tooltip control attached any other way is refused.
bool ProbeTrackbar(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    constexpr DWORD acceptedStyle = TBS_AUTOTICKS | TBS_VERT | TBS_TOP | TBS_BOTH |
        TBS_NOTICKS | TBS_FIXEDLENGTH | TBS_REVERSED | TBS_DOWNISLEFT |
        TBS_TRANSPARENTBKGND | TBS_TOOLTIPS;
    if (const DWORD rejected = style & 0xffffu & ~acceptedStyle; rejected != 0) {
        static constexpr NamedFlag flags[] = {
            { TBS_ENABLESELRANGE, L"TBS_ENABLESELRANGE" },
            { TBS_NOTHUMB, L"TBS_NOTHUMB" },
            { TBS_NOTIFYBEFOREMOVE, L"TBS_NOTIFYBEFOREMOVE" },
        };
        return RejectFlags(reason, L"Trackbar has unsupported style bit(s) ",
            rejected, style, flags, std::size(flags));
    }
    if ((style & TBS_TOOLTIPS) == 0 && SendMessageW(hwnd, TBM_GETTOOLTIPS, 0, 0) != 0)
        return Reject(reason, L"Trackbar tooltips are not supported");
    kind = ControlKind::Slider;
    return true;
}

// The UpDown (spin) control: a pair of arrows that step a bounded position, usually
// mirrored into a buddy Edit.  Every UDS_* bit is layout or behavior the control itself
// carries out when it is stepped -- wrapping, buddy text, alignment beside the buddy,
// orientation -- so none of them changes the contract the projection drives.
bool ProbeUpDown(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    constexpr DWORD accepted = UDS_WRAP | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ALIGNLEFT |
        UDS_AUTOBUDDY | UDS_ARROWKEYS | UDS_HORZ | UDS_NOTHOUSANDS | UDS_HOTTRACK;
    if (const DWORD rejected = style & 0xffffu & ~accepted; rejected != 0) {
        wchar_t text[112]{};
        swprintf_s(text, L"UpDown has unsupported style bits 0x%04lX (style=0x%08lX)",
            rejected, style);
        reason = text;
        return false;
    }
    kind = ControlKind::UpDown;
    return true;
}

// The date and time picker (SysDateTimePick32).  Its value is the one integer a node
// carries for a position -- a date picker's day number, a time picker's second of the day
// (DateTimePickerValue.h) -- and setValue changes it.  A custom format cannot be read back
// from the control and may show anything, so only the stock ones are taken: short, short
// with century and long dates, and time.  DTS_SHOWNONE (a check box that says there is no
// value) and DTS_APPCANPARSE (the application parses what the user types) are contracts the
// projection would have to invent.
bool ProbeDateTimePicker(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & DTS_SHOWNONE) != 0)
        return Reject(reason, L"DateTimePicker has a no-date check box (DTS_SHOWNONE)");
    if ((style & DTS_APPCANPARSE) != 0)
        return Reject(reason, L"DateTimePicker parses its own text (DTS_APPCANPARSE)");
    switch (style & 0x000Fu) {
    case DTS_TIMEFORMAT:
        kind = ControlKind::TimePicker;
        return true;
    case DTS_SHORTDATEFORMAT:
    case DTS_UPDOWN:
    case DTS_LONGDATEFORMAT:
    case DTS_LONGDATEFORMAT | DTS_UPDOWN:
    case DTS_SHORTDATECENTURYFORMAT:
    case DTS_SHORTDATECENTURYFORMAT | DTS_UPDOWN: {
        // A Windows Forms Custom format keeps the stock style bits; a date picker that shows
        // a time of day is one.
        wchar_t shown[64]{};
        GetWindowTextW(hwnd, shown, static_cast<int>(std::size(shown)));
        if (std::wcschr(shown, L':'))
            return Reject(reason, L"DateTimePicker shows a time of day in a date format");
        kind = ControlKind::DatePicker;
        return true;
    }
    default:
        return Reject(reason, L"DateTimePicker has an unsupported format style");
    }
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

// A child dialog is a container of controls, never a control. One with DS_CONTROL and WS_EX_CONTROLPARENT
// belongs to its parent's dialog-manager walk; one without them -- the pages of an OLE property frame, such
// as Performance Monitor's chart Properties -- runs a dialog manager of its own, so its controls form a
// navigation scope of their own (see IsIndependentDialogPage in WindowCapture.cpp).
bool ProbeDialogContainer(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if ((style & WS_CHILD) == 0) return Reject(reason, L"dialog is not a child window");
    constexpr DWORD unsupportedStyle = WS_CAPTION | WS_THICKFRAME | WS_BORDER |
        WS_DLGFRAME | WS_HSCROLL | WS_VSCROLL | WS_SYSMENU | DS_MODALFRAME;
    constexpr DWORD unsupportedExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
        WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_LAYERED;
    if ((style & unsupportedStyle) != 0 || (exStyle & unsupportedExStyle) != 0) {
        return Reject(reason, L"child dialog has unsupported visible chrome");
    }
    if (const HMENU menu = GetMenu(hwnd); menu && IsMenu(menu)) {
        return Reject(reason, L"child dialog has a menu");
    }
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int regionType = region ? GetWindowRgn(hwnd, region) : ERROR;
    if (region) DeleteObject(region);
    if (regionType != ERROR) {
        return Reject(reason, L"child dialog has a custom region");
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
        // MDI caption cluster in the rebar band is exactly that case, and so is a host
        // a sibling above covers but for its edges.
        if (IsCompositeImplementationChild(child) || ObscuredBySiblingAbove(child)) continue;
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

bool IsPlainSurfaceBand(HWND container, const RECT& band) noexcept;

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
        const size_t bytes = static_cast<size_t>(width) * height * 4;
        if (width > static_cast<int>(Ipc::kMaxChromeRegionDimension) ||
            height > static_cast<int>(Ipc::kMaxChromeRegionDimension) ||
            surface.chrome.size() >= Ipc::kMaxChromeRegions ||
            chromeBytes + bytes > Ipc::kMaxChromeRegionBytes) {
            // More than the caps carry is still nothing when it is only the surface the
            // projection draws behind every control (the empty room beside a button row).
            if (IsPlainSurfaceBand(container, gap)) continue;
            if (chromeBytes + bytes > Ipc::kMaxChromeRegionBytes &&
                width <= static_cast<int>(Ipc::kMaxChromeRegionDimension) &&
                height <= static_cast<int>(Ipc::kMaxChromeRegionDimension) &&
                surface.chrome.size() < Ipc::kMaxChromeRegions) {
                return Reject(reason, L"container chrome exceeds the protocol pixel cap");
            }
            wchar_t evidence[176]{};
            swprintf_s(evidence,
                L"container paints more than the projection can reproduce: %ldx%ld at %ld,%ld",
                width, height, gap.left, gap.top);
            return Reject(reason, evidence);
        }
        chromeBytes += bytes;
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

// A splitter runs the whole way across its container: from one edge to the other, where an
// edge may be a band the container paints itself (a caption above the panes), and it covers
// most of that extent.  A row of a form has a strip between its label and its field too, but
// that strip is one row tall and stops at the next row; dragging it would move nothing the
// user sees as one pane.
bool GapSpansContainer(const PaneGap& gap, const RECT& client, const std::vector<RECT>& chrome) noexcept {
    const int from = gap.vertical ? gap.rect.top : gap.rect.left;
    const int to = gap.vertical ? gap.rect.bottom : gap.rect.right;
    const int low = gap.vertical ? client.top : client.left;
    const int high = gap.vertical ? client.bottom : client.right;
    if ((to - from) * 2 < high - low) return false;
    const auto reaches = [&](int edge, bool leading) noexcept {
        if (leading ? edge - low <= kMaxPaneGap : high - edge <= kMaxPaneGap) return true;
        for (const RECT& band : chrome) {
            // A painted band alongside the gap ends exactly where the gap begins (or begins
            // where it ends).
            const bool alongside = gap.vertical
                ? band.left < gap.rect.right && band.right > gap.rect.left
                : band.top < gap.rect.bottom && band.bottom > gap.rect.top;
            const int bandEdge = leading ? (gap.vertical ? band.bottom : band.right)
                                         : (gap.vertical ? band.top : band.left);
            if (alongside && std::abs(bandEdge - edge) <= 1) return true;
        }
        return false;
    };
    return reaches(from, true) && reaches(to, false);
}

// The splits a container offers, in the order it offers them: each is a gap that spans the
// container, with a pane on each side that is wide enough to give way.  Capture publishes
// them and SetPaneSplit moves them by index, so both read the same list.
struct ResolvedSplit final {
    PaneGap gap;
    PaneNeighbours neighbours;
    int thickness = 0;
    int minimum = 0;
    int maximum = 0;
};

std::vector<ResolvedSplit> ResolveSplits(HWND container, const RECT& client, const PaneSurface& surface) {
    std::vector<ResolvedSplit> splits;
    // Performance Monitor's System Monitor control lays its toolbar, graph, value strip and legend
    // out itself, and puts a few pixels between them. Those strips are margins, not splitters:
    // nothing in the control follows a drag.
    if (IsSystemMonitorWindow(container)) return splits;
    for (const PaneGap& gap : surface.gaps) {
        if (!GapSpansContainer(gap, client, surface.chrome)) continue;
        ResolvedSplit split;
        split.gap = gap;
        split.neighbours = ResolvePaneNeighbours(container, client, gap);
        if (!split.neighbours.before || !split.neighbours.after) continue;
        split.thickness = gap.vertical ? gap.rect.right - gap.rect.left : gap.rect.bottom - gap.rect.top;
        // The range is bounded by the two panes' far edges: moving the split cannot
        // collapse either of them, and nothing outside the pair ever moves.
        split.minimum = (gap.vertical ? split.neighbours.beforeRect.left : split.neighbours.beforeRect.top) +
            kMinPaneExtent;
        split.maximum = (gap.vertical ? split.neighbours.afterRect.right : split.neighbours.afterRect.bottom) -
            kMinPaneExtent - split.thickness;
        if (split.maximum < split.minimum) continue;
        splits.push_back(split);
    }
    return splits;
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
        // Windows Forms paints every control through WM_PRINTCLIENT (OnPrint), so its
        // windows render straight into the bitmap in a few milliseconds, cloaked or not.
        if (IsWindowsFormsWindow(window)) {
            rendered_ = RenderOver(window, RGB(0, 0, 0), black_, false) &&
                RenderOver(window, RGB(255, 255, 255), white_, false) && AnyPainted();
            if (rendered_) return;
        }
        // Anything else is taken from what DWM composes, which is what makes this
        // work while the native window is cloaked -- exactly when a reconcile capture
        // runs -- at the cost of a compositor round trip per render.  The first such
        // render of a freshly painted window can come back before DWM holds its
        // content -- the fill untouched, as if the window painted nothing -- so one
        // render is discarded before the pair that is compared.
        PrintWindow(window, memory_, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        rendered_ = RenderOver(window, RGB(0, 0, 0), black_, true) &&
            RenderOver(window, RGB(255, 255, 255), white_, true);
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

    // True when every pixel of a client-relative rectangle came out opaque and the
    // same colour in both renders.  `color` carries the fill across calls: empty on
    // entry means any colour may start it, and every later pixel must match it.
    bool UniformOpaqueFill(const RECT& area, std::optional<COLORREF>& color) const noexcept {
        if (!rendered_ || area.left < client_.left || area.top < client_.top ||
            area.right > client_.right || area.bottom > client_.bottom) return false;
        const size_t stride = static_cast<size_t>(width_) * 4;
        for (int row = area.top; row < area.bottom; ++row) {
            const size_t offset = static_cast<size_t>(row - client_.top) * stride;
            for (int column = area.left; column < area.right; ++column) {
                const size_t pixel = offset + static_cast<size_t>(column - client_.left) * 4;
                const uint8_t* black = black_.data() + pixel;
                const uint8_t* white = white_.data() + pixel;
                if (black[0] != white[0] || black[1] != white[1] || black[2] != white[2])
                    return false;
                const COLORREF value = RGB(black[2], black[1], black[0]);
                if (!color) color = value;
                else if (*color != value) return false;
            }
        }
        return true;
    }

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
                for (size_t channel = 0; channel < 3; ++channel) {
                    imageData[target + channel] = black_[source + channel];
                }
                imageData[target + 3] = RecoverPaintAlpha(
                    black_.data() + source, white_.data() + source);
            }
        }
        return true;
    }

private:
    // Whether the window painted anything at all: a window that ignores WM_PRINT leaves
    // both fills untouched, and is then taken from the compositor instead.
    bool AnyPainted() const noexcept {
        for (size_t offset = 0; offset + 3 < black_.size() && offset + 3 < white_.size(); offset += 4) {
            if (black_[offset] != 0 || black_[offset + 1] != 0 || black_[offset + 2] != 0 ||
                white_[offset] != 255 || white_[offset + 1] != 255 || white_[offset + 2] != 255) return true;
        }
        return false;
    }

    // One render of the window's own paint over a known background: from the
    // compositor, or through the window's own WM_PRINT handling (client and
    // background; no child is ever read, since every caller crops the window's own
    // uncovered pixels).
    bool RenderOver(HWND window, COLORREF background, std::vector<uint8_t>& into, bool fullContent) noexcept {
        RECT full{ 0, 0, width_, height_ };
        HBRUSH brush = CreateSolidBrush(background);
        if (!brush) return false;
        FillRect(memory_, &full, brush);
        DeleteObject(brush);
        if (fullContent) {
            if (!PrintWindow(window, memory_, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) return false;
        } else {
            SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory_), PRF_CLIENT | PRF_ERASEBKGND);
        }
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

// The colours a window's own background paints: the dialog face and the window background,
// and a themed page of the same kind (a tab control's body is #F9F9F9 under the Windows 11
// theme, not the dialog face) -- a neutral, nearly white fill. The projection draws its own
// surface behind every control, so such a fill is nothing to carry.
bool IsSurfaceColor(COLORREF color) noexcept {
    if (color == GetSysColor(COLOR_3DFACE) || color == GetSysColor(COLOR_WINDOW)) return true;
    const int red = GetRValue(color);
    const int green = GetGValue(color);
    const int blue = GetBValue(color);
    const int lowest = std::min({ red, green, blue });
    const int highest = std::max({ red, green, blue });
    return lowest >= 0xE8 && highest - lowest <= 3;
}

// A band that is nothing but the system surface colour -- the dialog face or the window
// background -- is the background the projection already draws behind every control. Carried
// as pixels it would sit on the projection's own surface as a patch of a slightly different
// shade, and wider than the caps it would refuse a frame that has nothing custom in it.
bool IsSurfaceFillBand(const PaintedClientSurface& surface, const RECT& band) noexcept {
    std::optional<COLORREF> fill;
    return surface.UniformOpaqueFill(band, fill) && fill && IsSurfaceColor(*fill);
}

bool IsPlainSurfaceBand(HWND container, const RECT& band) noexcept {
    try {
        const PaintedClientSurface surface(container);
        return surface.Rendered() && IsSurfaceFillBand(surface, band);
    } catch (...) {
        return false;
    }
}

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
        if (IsSurfaceFillBand(surface, band)) continue;
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

// The pixels a control paints for its own face, as a picture: rendered once, with the
// face's background -- the colour of its corner pixel -- keyed out, so a glyph or an
// icon sits on whatever the projection draws behind it instead of on a native tile.
bool CaptureKeyedFace(
    HWND hwnd,
    uint32_t maxDimension,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData) {
    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0 ||
        client.right > static_cast<LONG>(maxDimension) || client.bottom > static_cast<LONG>(maxDimension))
        return false;
    const PaintedClientSurface paint(hwnd);
    if (!paint.Rendered() || !paint.Crop(client, imageWidth, imageHeight, imageFormat, imageData) ||
        imageData.size() < 4) return false;
    uint32_t corner = 0;
    std::memcpy(&corner, imageData.data(), sizeof(corner));
    for (size_t offset = 0; offset + 3 < imageData.size(); offset += 4) {
        uint32_t pixel = 0;
        std::memcpy(&pixel, imageData.data() + offset, sizeof(pixel));
        if (pixel == corner) std::memset(imageData.data() + offset, 0, sizeof(pixel));
    }
    return true;
}

// The pixels a control paints for its own face, exactly as painted: the whole client area, with the
// alpha the two renders recovered.
bool CapturePaintedFace(
    HWND hwnd,
    uint32_t maxDimension,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData) {
    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0 ||
        client.right > static_cast<LONG>(maxDimension) || client.bottom > static_cast<LONG>(maxDimension))
        return false;
    const PaintedClientSurface paint(hwnd);
    return paint.Rendered() && paint.Crop(client, imageWidth, imageHeight, imageFormat, imageData);
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

// The standard window scroll bars of a container that scrolls its child windows,
// in the container's own window-relative coordinates. A bar the application hid
// (ShowScrollBar keeps the style bit on some frameworks) is skipped.
bool ReadContainerScrollBars(HWND hwnd, DWORD style, std::vector<ScrollBarSnapshot>& bars,
    std::wstring& reason) {
    bars.clear();
    RECT window{};
    if (!GetWindowRect(hwnd, &window)) return Reject(reason, L"scrolling container bounds are unavailable");
    for (const bool vertical : { true, false }) {
        if ((style & (vertical ? WS_VSCROLL : WS_HSCROLL)) == 0) continue;
        SCROLLBARINFO info{ sizeof(info) };
        if (!GetScrollBarInfo(hwnd, vertical ? OBJID_VSCROLL : OBJID_HSCROLL, &info))
            return Reject(reason, L"container scroll bar geometry is unavailable");
        if ((info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) != 0) continue;
        SCROLLINFO scroll{ sizeof(scroll), SIF_ALL };
        if (!GetScrollInfo(hwnd, vertical ? SB_VERT : SB_HORZ, &scroll))
            return Reject(reason, L"container scroll range is unavailable");
        ScrollBarSnapshot bar;
        bar.vertical = vertical;
        bar.enabled = (info.rgstate[0] & STATE_SYSTEM_UNAVAILABLE) == 0;
        bar.minimum = scroll.nMin;
        bar.maximum = scroll.nMax;
        bar.page = static_cast<int>(std::min<UINT>(scroll.nPage, static_cast<UINT>(Ipc::kMaxCoordinate)));
        bar.position = scroll.nPos;
        bar.rect = { info.rcScrollBar.left - window.left, info.rcScrollBar.top - window.top,
            info.rcScrollBar.right - window.left, info.rcScrollBar.bottom - window.top };
        const auto outside = [](int64_t value) { return value < -Ipc::kMaxCoordinate || value > Ipc::kMaxCoordinate; };
        if (bar.maximum < bar.minimum || outside(bar.minimum) || outside(bar.maximum) ||
            bar.position < bar.minimum || bar.position > bar.maximum ||
            bar.rect.right <= bar.rect.left || bar.rect.bottom <= bar.rect.top ||
            bar.rect.left < 0 || bar.rect.top < 0 ||
            bar.rect.right > window.right - window.left || bar.rect.bottom > window.bottom - window.top)
            return Reject(reason, L"container scroll bar state is outside the projection's bounds");
        bars.push_back(bar);
    }
    return true;
}

// The part of a window's client area its ancestors leave on screen, in the window's
// own client coordinates.  What a window paints outside it is never seen natively, so
// it is no evidence about what the window draws.
RECT VisibleClientPart(HWND hwnd, const RECT& client) noexcept {
    POINT origin{ 0, 0 };
    if (!ClientToScreen(hwnd, &origin)) return RECT{};
    RECT visible = client;
    OffsetRect(&visible, origin.x, origin.y);
    const HWND desktop = GetDesktopWindow();
    for (HWND ancestor = GetAncestor(hwnd, GA_PARENT); ancestor && ancestor != desktop;
         ancestor = GetAncestor(ancestor, GA_PARENT)) {
        RECT area{};
        POINT corner{ 0, 0 };
        if (!GetClientRect(ancestor, &area) || !ClientToScreen(ancestor, &corner)) return RECT{};
        OffsetRect(&area, corner.x, corner.y);
        if (!IntersectRect(&visible, &visible, &area)) return RECT{};
    }
    OffsetRect(&visible, -origin.x, -origin.y);
    return visible;
}

// A container whose own paint is nothing but one system surface colour -- the dialog
// face or the window background -- around children laid out like a form (a Windows
// Forms panel of labels and fields), or behind children that are all hidden.  That
// fill is the surface the projection already draws behind every control, so the
// container is a plain frame with nothing of its own to reproduce: no chrome, and no
// splits, because a gap between two fields is nothing the user can drag natively.
// Only what is on screen counts; the scrolled-away part of a panel is never seen.  An
// empty tab stop is an interactive control that has merely not painted, not a frame.
bool IsSurfaceFilledContainer(HWND hwnd, DWORD style, const RECT& client) noexcept {
    try {
        std::vector<RECT> children;
        CollectVisibleChildRects(hwnd, children);
        if (children.empty() && (style & WS_TABSTOP) != 0) return false;
        const RECT visible = VisibleClientPart(hwnd, client);
        if (IsRectEmpty(&visible)) return false;
        struct RegionGuard final {
            HRGN handle = nullptr;
            ~RegionGuard() { if (handle) DeleteObject(handle); }
        };
        RegionGuard uncovered{ CreateRectRgnIndirect(&visible) };
        if (!uncovered.handle) return false;
        for (const RECT& child : children) {
            RegionGuard covered{ CreateRectRgnIndirect(&child) };
            if (!covered.handle) return false;
            CombineRgn(uncovered.handle, uncovered.handle, covered.handle, RGN_DIFF);
        }
        const DWORD size = GetRegionData(uncovered.handle, 0, nullptr);
        if (size == 0) return false;
        std::vector<uint8_t> buffer(size);
        auto* data = reinterpret_cast<RGNDATA*>(buffer.data());
        if (GetRegionData(uncovered.handle, size, data) == 0) return false;
        // Every visible pixel belongs to a child: the container shows nothing of its own.
        if (data->rdh.nCount == 0) return true;
        const PaintedClientSurface surface(hwnd);
        if (!surface.Rendered()) return false;
        std::optional<COLORREF> fill;
        const auto* rects = reinterpret_cast<const RECT*>(data->Buffer);
        for (DWORD index = 0; index < data->rdh.nCount; ++index) {
            if (!surface.UniformOpaqueFill(rects[index], fill)) return false;
        }
        return fill && IsSurfaceColor(*fill);
    } catch (...) {
        return false;
    }
}

bool ProbePaneContainer(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) {
        return Reject(reason, L"private container is not a child window");
    }
    if ((style & (WS_HSCROLL | WS_VSCROLL)) != 0) {
        // A container whose application scrolls its child windows (a WinForms
        // AutoScroll panel) projects its bars; the native handler moves the children.
        std::vector<ScrollBarSnapshot> bars;
        std::wstring scrollReason;
        if (!ReadContainerScrollBars(hwnd, style, bars, scrollReason))
            return Reject(reason, (L"private container scrolls content beyond its client area: " +
                scrollReason).c_str());
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
        if (!IsSurfaceFilledContainer(hwnd, style, client))
            return Reject(reason, L"private container has no visible children to frame");
        kind = ControlKind::PaneContainer;
        return true;
    }
    PaneSurface surface;
    if (!CollectPaneSurface(hwnd, client, surface, reason)) {
        if (!IsSurfaceFilledContainer(hwnd, style, client)) return false;
        reason.clear();
    }
    kind = ControlKind::PaneContainer;
    return true;
}

bool CapturePaneContainerState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    node.splits.clear();
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        return Reject(reason, L"private container has no client area");
    }
    if (!ReadContainerScrollBars(hwnd, static_cast<DWORD>(node.style), node.scrollBars, reason))
        return false;
    node.clientRect = {};
    if (!node.scrollBars.empty()) {
        // The viewport the scrolled children are clipped to, beside the bars.
        RECT window{};
        POINT origin{ client.left, client.top };
        if (!GetWindowRect(hwnd, &window) || !ClientToScreen(hwnd, &origin))
            return Reject(reason, L"scrolling container viewport is unavailable");
        node.clientRect = { origin.x - window.left, origin.y - window.top,
            origin.x - window.left + client.right - client.left,
            origin.y - window.top + client.bottom - client.top };
    }
    std::vector<RECT> children;
    CollectVisibleChildRects(hwnd, children);
    PaneSurface surface;
    std::wstring tilingReason;
    if (children.empty() || !CollectPaneSurface(hwnd, client, surface, tilingReason)) {
        // The probe admitted a surface-filled frame: its fill is the projection's own
        // surface, and it offers no splits.
        node.chromeRegions.clear();
        if (IsSurfaceFilledContainer(hwnd, static_cast<DWORD>(node.style), client)) return true;
        return Reject(reason, children.empty()
            ? L"private container has no visible children to frame" : tilingReason.c_str());
    }
    if (!CaptureContainerChrome(hwnd, client, surface.chrome, node.chromeRegions, reason)) {
        return false;
    }
    for (const ResolvedSplit& resolved : ResolveSplits(hwnd, client, surface)) {
        PaneSplitSnapshot split;
        split.vertical = resolved.gap.vertical;
        split.position = resolved.gap.vertical ? resolved.gap.rect.left : resolved.gap.rect.top;
        split.thickness = resolved.thickness;
        // A pane the application already laid out narrower than the drag minimum
        // still has its split where it is; the range includes that position so the
        // published split is valid and can be dragged back out.
        split.minimum = std::min(resolved.minimum, split.position);
        split.maximum = std::max(resolved.maximum, split.position);
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
        if (IsWindowVisible(child) && !IsDirectUiScrollChrome(child)) {
            return Reject(reason, L"accessible island also owns visible child windows");
        }
    }
    std::vector<AccessibleIslandItem> items;
    if (!ReadAccessibleIslandItems(hwnd, items, reason)) return false;
    kind = ControlKind::AccessibleIsland;
    return true;
}

bool ProbeNativeDocument(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) return Reject(reason, L"native document is not a child window");
    NativeDocumentSnapshot document;
    if (ReadNativeDocument(hwnd, document, reason)) {
        kind = ControlKind::NativeDocument;
        return true;
    }
    // Preserve the existing, narrower MMC extended-view resource contract.
    std::wstring legacyReason;
    if (ProbeAccessibleIsland(hwnd, style, kind, legacyReason)) return true;
    if (reason.empty()) reason = L"native document could not be read";
    // The document is only readable from inside the process, so its source travels
    // with the refusal when diagnostics are switched on for the target.
    wchar_t diagnostics[2]{};
    if (GetEnvironmentVariableW(L"FLUENTSHELL_DIAG_ACCESSIBILITY", diagnostics, 2) == 1 &&
        diagnostics[0] == L'1') {
        reason.append(L" HTML:");
        reason.append(DescribeMmcHtmlDocument(hwnd));
    }
    return false;
}

bool CaptureNativeDocumentState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    NativeDocumentSnapshot document;
    if (!ReadNativeDocument(hwnd, document, reason)) return false;
    node.document = std::move(document);
    node.text.clear();
    node.automationName.clear();
    node.tabStop = false;
    node.tabIndex = -1;
    return true;
}

bool ProbeDiskManagement(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if (IsDiskManagementView(hwnd)) {
        DiskViewSnapshot view;
        if (!ReadDiskView(hwnd, view, reason)) return false;
        kind = ControlKind::DiskView;
        return true;
    }
    if (IsDiskManagementLegend(hwnd) && (style & (WS_TABSTOP | WS_HSCROLL | WS_VSCROLL)) == 0 &&
        !GetWindow(hwnd, GW_CHILD)) {
        kind = ControlKind::StaticBitmap;
        return true;
    }
    return ProbePaneContainer(hwnd, style, kind, reason);
}

bool CaptureDiskViewState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    DiskViewSnapshot view;
    if (!ReadDiskView(hwnd, view, reason)) return false;
    node.text.clear();
    node.automationName = view.name;
    node.diskView = std::move(view);
    node.tabStop = false;
    node.tabIndex = -1;
    return true;
}

// Performance Monitor's chart view: the graph, the value strip and the legend of the System Monitor
// control. Each is a window sysmon.ocx registered, directly inside the control, and what it shows
// is read from the control (PerformanceChart.h), never from its pixels.
bool ProbePerformanceChartGraph(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) return Reject(reason, L"performance graph is not a child window");
    if (!ProbePerformanceGraph(hwnd, reason)) return false;
    kind = ControlKind::PerformanceGraph;
    return true;
}

bool ProbePerformanceChartLegend(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) return Reject(reason, L"performance legend is not a child window");
    if (!ProbePerformanceLegend(hwnd, reason)) return false;
    kind = ControlKind::PerformanceLegend;
    return true;
}

bool ProbePerformanceChartValueBar(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0) return Reject(reason, L"performance value strip is not a child window");
    if (!ProbePerformanceValueBar(hwnd, reason)) return false;
    kind = ControlKind::PerformanceValueBar;
    return true;
}

// None of the three takes keyboard focus as a window: the legend's list is the control's own.
void ClearPerformanceChartFocus(ControlNode& node) noexcept {
    node.text.clear();
    node.tabStop = false;
    node.tabIndex = -1;
}

bool CapturePerformanceGraphState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    PerformanceGraphSnapshot graph;
    if (!ReadPerformanceGraph(hwnd, graph, reason)) return false;
    node.performanceGraph = std::move(graph);
    ClearPerformanceChartFocus(node);
    return true;
}

bool CapturePerformanceLegendState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    PerformanceLegendSnapshot legend;
    if (!ReadPerformanceLegend(hwnd, legend, reason)) return false;
    node.performanceLegend = std::move(legend);
    ClearPerformanceChartFocus(node);
    return true;
}

bool CapturePerformanceValueBarState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    PerformanceValueBarSnapshot bar;
    if (!ReadPerformanceValueBar(hwnd, bar, reason)) return false;
    node.performanceValueBar = std::move(bar);
    ClearPerformanceChartFocus(node);
    return true;
}

bool CaptureAccessibleIslandState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    std::vector<AccessibleIslandItem> items;
    if (!ReadAccessibleIslandItems(hwnd, items, reason)) return false;
    node.islandItems.clear();
    node.islandItems.reserve(items.size());
    // A graphic element's provider describes it but owns no pixels, so its image is
    // what the host itself painted there.  One render serves every graphic.  The host
    // fills its background before painting the graphic, and the island never projects
    // that background anywhere else, so pixels exactly matching it -- sampled from the
    // client corner -- travel transparent instead of drawing a box around the image.
    std::optional<PaintedClientSurface> paint;
    std::vector<uint8_t> background;
    const auto clearBackground = [&](std::vector<uint8_t>& pixels) {
        if (background.size() != 4 || background[3] != 0xff) return;
        for (size_t offset = 0; offset + 4 <= pixels.size(); offset += 4) {
            if (std::equal(background.begin(), background.end(), pixels.begin() + offset))
                std::fill_n(pixels.begin() + offset, 4, uint8_t{ 0 });
        }
    };
    for (const AccessibleIslandItem& item : items) {
        AccessibleIslandItemSnapshot published;
        switch (item.kind) {
        case AccessibleItemKind::Text: published.kind = L"text"; break;
        case AccessibleItemKind::Heading: published.kind = L"heading"; break;
        case AccessibleItemKind::Image: published.kind = L"image"; break;
        case AccessibleItemKind::Link: published.kind = L"link"; break;
        case AccessibleItemKind::PageTab: published.kind = L"pageTab"; break;
        case AccessibleItemKind::Button: published.kind = L"button"; break;
        }
        published.rect = item.rect;
        published.name = item.name;
        published.description = item.description;
        published.actionName = item.actionName;
        published.enabled = item.enabled;
        published.dropDown = item.dropDown;
        published.selected = item.selected;
        published.expanded = item.expanded;
        if (item.kind == AccessibleItemKind::Image) {
            if (!paint) {
                paint.emplace(hwnd);
                uint32_t width = 0, height = 0;
                std::wstring format;
                if (paint->Rendered() && !paint->Crop(RECT{ 0, 0, 1, 1 }, width, height,
                        format, background)) background.clear();
            }
            if (!paint->Rendered() || !paint->Crop(item.rect, published.imageWidth,
                    published.imageHeight, published.imageFormat, published.imageData)) {
                node.islandItems.clear();
                return Reject(reason, L"accessible island graphic could not be rendered");
            }
            clearBackground(published.imageData);
        }
        node.islandItems.push_back(std::move(published));
    }
    return true;
}

// The Time Range bar on the Source page of Performance Monitor's chart Properties: a window sysmon.ocx
// registered that draws the total and the viewed range of a log. It has no child windows and nothing the
// projection could drive, so it is carried as the picture it paints (as the Disk Management legend is); the
// range of a log is not something this projection edits.
bool IsSysmonIntervalBar(HWND hwnd) noexcept {
    wchar_t className[kMaxClassNameChars]{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), L"IntervalBar") &&
        WindowClassRegisteredBySystemModule(hwnd, L"sysmon.ocx");
}

// An owner-draw button too wide for a button picture that no key reaches (ProbeButton): the picture
// its owner paints, carried as inert pixels.
bool IsInertOwnerDrawButton(HWND hwnd) noexcept {
    wchar_t className[kMaxClassNameChars]{};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    RECT client{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), L"Button") &&
        (style & BS_TYPEMASK) == BS_OWNERDRAW && (style & WS_TABSTOP) == 0 && !IsWindowsFormsWindow(hwnd) &&
        GetClientRect(hwnd, &client) &&
        (client.right > static_cast<LONG>(Ipc::kMaxImageDimension) ||
         client.bottom > static_cast<LONG>(Ipc::kMaxImageDimension));
}

bool ProbeIntervalBar(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0 || GetWindow(hwnd, GW_CHILD) || !IsSysmonIntervalBar(hwnd))
        return Reject(reason, L"IntervalBar is not the System Monitor's time range bar");
    kind = ControlKind::StaticBitmap;
    return true;
}

// An animation control (SysAnimate32: a search's flashlight, a copy's flying pages).  It plays a clip on its
// own and takes no input, so it is carried as the frame it shows when captured -- nothing, while it is not
// playing, which is where a dialog starts -- and a later frame arrives with the next capture.  A tab stop
// would be a focus target the projection has no picture of.
bool ProbeAnimation(HWND hwnd, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & WS_CHILD) == 0 || (style & WS_TABSTOP) != 0 || GetWindow(hwnd, GW_CHILD))
        return Reject(reason, L"Animation control is a tab stop or frames child windows");
    kind = ControlKind::StaticBitmap;
    return true;
}

bool IsAnimationControl(HWND hwnd) noexcept {
    wchar_t className[kMaxClassNameChars]{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), ANIMATE_CLASSW);
}

using ProbeFn = bool (*)(HWND, DWORD, ControlKind&, std::wstring&);

struct ClassAdapter final {
    std::wstring_view className;
    ProbeFn probe;
};

// The registry. Class names select a candidate adapter; only its probe can
// establish support.
constexpr std::array kClassAdapters{
    ClassAdapter{ L"AfxWnd42u", &ProbeDiskManagement },
    ClassAdapter{ L"Internet Explorer_Server", &ProbeNativeDocument },
    ClassAdapter{ L"ChartGraphClass", &ProbePerformanceChartGraph },
    ClassAdapter{ L"PerfLegend", &ProbePerformanceChartLegend },
    ClassAdapter{ L"PerfStatus", &ProbePerformanceChartValueBar },
    ClassAdapter{ L"IntervalBar", &ProbeIntervalBar },
    ClassAdapter{ ANIMATE_CLASSW, &ProbeAnimation },
    ClassAdapter{ L"Static", &ProbeStatic },
    ClassAdapter{ L"BitmapDisplayClass", &ProbeBitmapDisplay },
    ClassAdapter{ L"BitmapSwitchClass", &ProbeBitmapSwitch },
    ClassAdapter{ L"MonitorPaletteClass", &ProbeMonitorPalette },
    ClassAdapter{ L"Button", &ProbeButton },
    ClassAdapter{ L"Edit", &ProbeEdit },
    ClassAdapter{ MSFTEDIT_CLASS, &ProbeRichEdit },
    ClassAdapter{ RICHEDIT_CLASSW, &ProbeRichEdit },
    ClassAdapter{ L"ComboBox", &ProbeComboBox },
    ClassAdapter{ WC_COMBOBOXEXW, &ProbeComboBoxEx },
    ClassAdapter{ L"ListBox", &ProbeListBox },
    ClassAdapter{ PROGRESS_CLASSW, &ProbeProgressBar },
    ClassAdapter{ WC_LINK, &ProbeSysLink },
    // The shell's older link control (the object picker's), answering the same LM_ messages.
    ClassAdapter{ L"Link Window", &ProbeSysLink },
    ClassAdapter{ WC_LISTVIEWW, &ProbeListView },
    ClassAdapter{ WC_TREEVIEWW, &ProbeTreeView },
    ClassAdapter{ WC_TABCONTROLW, &ProbeTabControl },
    ClassAdapter{ TRACKBAR_CLASSW, &ProbeTrackbar },
    ClassAdapter{ UPDOWN_CLASSW, &ProbeUpDown },
    ClassAdapter{ DATETIMEPICK_CLASSW, &ProbeDateTimePicker },
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

// Resolve the value returned by the native item provider before compacting it.
// A missing list cannot draw an icon; otherwise an unresolved callback or an
// index outside the native list must not become a silently missing image.
bool ResolveItemImage(
    int nativeIndex,
    size_t imageCount,
    int& resolved,
    std::wstring& reason) {
    if (imageCount == 0 || nativeIndex == I_IMAGENONE) {
        resolved = -1;
        return true;
    }
    if (nativeIndex == I_IMAGECALLBACK)
        return Reject(reason, L"callback item image is not supported");
    if (nativeIndex < 0 || static_cast<size_t>(nativeIndex) >= imageCount)
        return Reject(reason, L"item image index is outside the native image list");
    resolved = nativeIndex;
    return true;
}

// One referenced icon. Overlay 0 is the plain image; a nonzero overlay is the
// image the control draws with that image-list overlay composited on top.
struct IconReference final {
    int index = 0;
    int overlay = 0;
    bool operator==(const IconReference&) const noexcept = default;
    bool operator<(const IconReference& other) const noexcept {
        return index < other.index || (index == other.index && overlay < other.overlay);
    }
};

// Item providers have already been read exactly once. Copy only the native icons
// those item values reference, then remap both arrays into this snapshot's owned
// list. MMC keeps icons for unloaded snap-ins in the same HIMAGELIST; unused
// entries do not consume the projection's count or decoded-pixel budget.
// itemOverlays, when present, is one overlay index per itemImages slot and is
// applied to the selected-state icon of that same item.
bool CaptureReferencedImageList(
    HIMAGELIST images,
    std::vector<int>& itemImages,
    std::vector<int>* itemSelectedImages,
    const std::vector<int>* itemOverlays,
    std::vector<ImageListEntry>& imageList,
    std::wstring& reason) {
    imageList.clear();
    const int count = images ? ImageList_GetImageCount(images) : 0;
    if (count < 0)
        return Reject(reason, L"image list metadata is unavailable");
    if (itemOverlays && (itemOverlays->size() != itemImages.size() ||
            (itemSelectedImages && itemSelectedImages->size() != itemOverlays->size())))
        return Reject(reason, L"item overlay count does not match the items");
    std::vector<IconReference> references;
    references.reserve(itemImages.size() +
        (itemSelectedImages ? itemSelectedImages->size() : 0));
    const auto collect = [&](std::vector<int>& indexes) {
        for (size_t slot = 0; slot < indexes.size(); ++slot) {
            int& index = indexes[slot];
            if (!ResolveItemImage(index, static_cast<size_t>(count), index, reason))
                return false;
            if (index < 0) continue;
            int overlay = 0;
            if (itemOverlays) {
                overlay = (*itemOverlays)[slot];
                if (overlay < 0 || overlay > 15)
                    return Reject(reason, L"TreeView overlay image index is outside 0..15");
            }
            references.push_back({ index, overlay });
        }
        return true;
    };
    if (!collect(itemImages) ||
        (itemSelectedImages && !collect(*itemSelectedImages))) return false;
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()), references.end());
    if (references.empty()) return true;
    if (references.size() > Ipc::kMaxImageListImages)
        return Reject(reason, L"referenced image list exceeds the icon cap");
    int width = 0;
    int height = 0;
    if (!ImageList_GetIconSize(images, &width, &height))
        return Reject(reason, L"image list metadata is unavailable");
    if (width <= 0 || height <= 0 ||
        width > static_cast<int>(Ipc::kMaxImageListDimension) ||
        height > static_cast<int>(Ipc::kMaxImageListDimension)) {
        wchar_t text[128]{};
        swprintf_s(text, L"image list icons are %dx%d, outside the %ux%u cap",
            width, height, Ipc::kMaxImageListDimension, Ipc::kMaxImageListDimension);
        reason = text;
        return false;
    }
    const size_t iconBytes = static_cast<size_t>(width) * height * 4u;
    if (references.size() > Ipc::kMaxImageListBytes / iconBytes)
        return Reject(reason, L"referenced image list exceeds the decoded pixel budget");
    imageList.reserve(references.size());
    for (const IconReference& reference : references) {
        const UINT flags = reference.overlay == 0
            ? static_cast<UINT>(ILD_NORMAL)
            : ILD_NORMAL | INDEXTOOVERLAYMASK(reference.overlay);
        HICON icon = ImageList_GetIcon(images, reference.index, flags);
        if (!icon) {
            return Reject(reason, reference.overlay == 0
                ? L"image list icon copy failed"
                : L"TreeView overlay image could not be copied");
        }
        ImageListEntry entry;
        const bool copied = CaptureIconPixels(icon, entry.imageWidth, entry.imageHeight,
            entry.imageFormat, entry.imageData, reason);
        DestroyIcon(icon);
        if (!copied) {
            reason = L"image list icon: " + reason;
            return false;
        }
        if (entry.imageWidth != static_cast<uint32_t>(width) ||
            entry.imageHeight != static_cast<uint32_t>(height) ||
            entry.imageData.size() != iconBytes)
            return Reject(reason, L"image list icon dimensions changed during capture");
        imageList.push_back(std::move(entry));
    }
    const auto remap = [&](std::vector<int>& indexes) {
        for (size_t slot = 0; slot < indexes.size(); ++slot) {
            int& index = indexes[slot];
            if (index < 0) continue;
            const IconReference key{ index, itemOverlays ? (*itemOverlays)[slot] : 0 };
            index = static_cast<int>(
                std::lower_bound(references.begin(), references.end(), key) - references.begin());
        }
    };
    remap(itemImages);
    if (itemSelectedImages) remap(*itemSelectedImages);
    return true;
}

size_t ImageListBytes(const std::vector<ImageListEntry>& images) noexcept {
    size_t bytes = 0;
    for (const auto& entry : images) bytes += entry.imageData.size();
    return bytes;
}

// The normal list and the state list share one icon budget. Each list is
// already capped on its own; this is what stops the pair from doubling it.
bool ImageListsFitBudget(
    const std::vector<ImageListEntry>& first,
    const std::vector<ImageListEntry>& second,
    std::wstring& reason) {
    if (second.size() > Ipc::kMaxImageListImages - first.size())
        return Reject(reason, L"referenced image list exceeds the icon cap");
    const size_t bytes = ImageListBytes(first);
    if (ImageListBytes(second) > Ipc::kMaxImageListBytes - bytes)
        return Reject(reason, L"referenced image list exceeds the decoded pixel budget");
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

// The 32-bit top-down BGRX pixels of an HBITMAP the control owns.  A bitmap a
// control BitBlts carries no alpha, so callers treat every pixel as opaque.
bool ReadBitmapPixels(HBITMAP bitmap, int& width, int& height,
    std::vector<uint8_t>& pixels) noexcept {
    try {
        BITMAP source{};
        if (GetObjectW(bitmap, sizeof(source), &source) != sizeof(source) ||
            source.bmWidth <= 0 || source.bmHeight == 0 ||
            source.bmWidth > static_cast<LONG>(Ipc::kMaxDirectUiBitmapDimension) ||
            std::abs(source.bmHeight) > static_cast<LONG>(Ipc::kMaxDirectUiBitmapDimension))
            return false;
        width = source.bmWidth;
        height = std::abs(source.bmHeight);
        pixels.assign(static_cast<size_t>(width) * height * 4, 0);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        const int rows = screen ? GetDIBits(screen, bitmap, 0, static_cast<UINT>(height),
            pixels.data(), &info, DIB_RGB_COLORS) : 0;
        if (screen) ReleaseDC(nullptr, screen);
        return rows == height;
    } catch (...) {
        return false;
    }
}

// An SS_BITMAP control BitBlts its HBITMAP at the client origin and leaves the rest
// of its client area to the background, so the projection carries exactly that: a
// client-sized image with the bitmap's pixels opaque at (0,0) and nothing elsewhere.
// With SS_CENTERIMAGE the bitmap is centered instead and the documented fill -- the
// colour of the bitmap's top-left pixel -- covers the rest of the client area.
// Scaling (SS_REALSIZECONTROL) is a layout the adapter does not model.
bool CaptureStaticBitmapState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    if (IsSysmonIntervalBar(hwnd) || IsInertOwnerDrawButton(hwnd) || IsAnimationControl(hwnd)) {
        RECT client{};
        if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0 ||
            client.right > 1024 || client.bottom > 128)
            return Reject(reason, L"owner-painted surface exceeds its inert painted band bound");
        PaintedClientSurface paint(hwnd);
        return paint.Rendered() && paint.Crop(client, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData) ? true :
            Reject(reason, L"owner-painted surface could not render its owned pixels");
    }
    if (IsDiskManagementLegend(hwnd)) {
        RECT client{};
        if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0 ||
            client.right > 1024 || client.bottom > 128)
            return Reject(reason, L"Disk Management legend exceeds its inert painted band bound");
        PaintedClientSurface paint(hwnd);
        return paint.Rendered() && paint.Crop(client, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData) ? true :
            Reject(reason, L"Disk Management legend could not render its owned pixels");
    }
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    if ((style & SS_TYPEMASK) == SS_OWNERDRAW && IsWindowsFormsLabel(hwnd, style) && UsesSymbolFont(hwnd)) {
        return CaptureKeyedFace(hwnd, Ipc::kMaxDirectUiBitmapDimension, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData) ? true :
            Reject(reason, L"symbol-font Windows Forms label could not render its glyphs");
    }
    if (IsWindowsFormsInertSurface(hwnd, style)) {
        RECT client{};
        GetClientRect(hwnd, &client);
        PaintedClientSurface paint(hwnd);
        return paint.Rendered() && paint.Crop(client, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData) ? true :
            Reject(reason, L"Windows Forms surface could not render its owned pixels");
    }
    if ((style & SS_REALSIZECONTROL) != 0)
        return Reject(reason, L"scaled Static bitmap is not supported");
    const bool centered = (style & SS_CENTERIMAGE) != 0;
    const auto bitmap = reinterpret_cast<HBITMAP>(SendMessageW(hwnd, STM_GETIMAGE, IMAGE_BITMAP, 0));
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::vector<uint8_t> pixels;
    if (!bitmap || !ReadBitmapPixels(bitmap, sourceWidth, sourceHeight, pixels))
        return Reject(reason, L"Static bitmap has no readable HBITMAP within the bitmap cap");
    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0)
        return Reject(reason, L"Static bitmap has no client area");
    const int width = client.right;
    const int height = client.bottom;
    if (width > static_cast<int>(Ipc::kMaxDirectUiBitmapDimension) ||
        height > static_cast<int>(Ipc::kMaxDirectUiBitmapDimension) ||
        static_cast<size_t>(width) * height * 4 > Ipc::kMaxDirectUiBitmapBytes)
        return Reject(reason, L"Static bitmap is larger than the bitmap cap");
    node.imageWidth = static_cast<uint32_t>(width);
    node.imageHeight = static_cast<uint32_t>(height);
    node.imageFormat = L"bgra8-premultiplied";
    node.imageData.assign(static_cast<size_t>(width) * height * 4, 0);
    // Destination of the bitmap's (0,0); negative when a centered bitmap is larger
    // than the control and is cropped on both sides.
    const int left = centered ? (width - sourceWidth) / 2 : 0;
    const int top = centered ? (height - sourceHeight) / 2 : 0;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const int sourceRow = row - top;
            const int sourceColumn = column - left;
            const bool inside = sourceRow >= 0 && sourceRow < sourceHeight &&
                sourceColumn >= 0 && sourceColumn < sourceWidth;
            if (!inside && !centered) continue;
            const size_t from = inside
                ? (static_cast<size_t>(sourceRow) * sourceWidth + sourceColumn) * 4 : 0;
            const size_t to = (static_cast<size_t>(row) * width + column) * 4;
            node.imageData[to] = pixels[from];
            node.imageData[to + 1] = pixels[from + 1];
            node.imageData[to + 2] = pixels[from + 2];
            node.imageData[to + 3] = 0xff;
        }
    }
    return true;
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
    const bool ownerData = (GetWindowLongPtrW(hwnd, GWL_STYLE) & LVS_OWNERDATA) != 0;
    return ReadGrowingText(text, reason,
        L"ListView item text exceeds the protocol string limit",
        [&](std::wstring& buffer, size_t& length, std::wstring& error) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.iSubItem = column;
            item.pszText = buffer.data();
            item.cchTextMax = static_cast<int>(buffer.size());
            // LVM_GETITEMTEXT is explicitly unsupported for LVS_OWNERDATA.
            // GETITEM asks the control to obtain text through the owner's own
            // LVN_GETDISPINFO callback, on this HWND's GUI thread.
            if (ownerData) {
                if (!SendMessageW(hwnd, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
                    return Reject(error, L"owner-data ListView item text read failed");
                if (!item.pszText || item.pszText == LPSTR_TEXTCALLBACKW)
                    return Reject(error, L"owner-data ListView did not return item text");
                length = wcsnlen_s(item.pszText, buffer.size());
                // GETITEM may return a pointer to provider-owned text instead
                // of filling the caller's buffer. Own a bounded copy before the
                // next callback can replace that storage.
                if (item.pszText != buffer.data() && length < buffer.size())
                    std::copy_n(item.pszText, length, buffer.data());
                return true;
            }
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
        // HDF_BITMAP_ON_RIGHT only places a picture that HDF_BITMAP or HDF_IMAGE would
        // draw; on its own (Windows Forms sets it on every column) nothing is drawn.
        constexpr int unsupportedFormat = HDF_OWNERDRAW | HDF_BITMAP | HDF_IMAGE;
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

// Enumerates selected items through the native LVM_GETNEXTITEM walk, rejecting
// any enumeration that is not strictly increasing and inside `itemCount`.
bool ReadListViewSelection(
    HWND hwnd,
    size_t itemCount,
    std::vector<int>& selected,
    std::wstring& reason) {
    selected.clear();
    int previous = -1;
    for (;;) {
        const int index = static_cast<int>(
            SendMessageW(hwnd, LVM_GETNEXTITEM, previous, LVNI_SELECTED));
        if (index < 0) return true;
        if (index <= previous || static_cast<size_t>(index) >= itemCount ||
            selected.size() >= itemCount) {
            return Reject(reason, L"ListView selected index enumeration is invalid");
        }
        if (selected.size() >= Ipc::kMaxListItems)
            return Reject(reason, L"ListView selects more rows than the projection carries");
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

// comctl32 (6.10.26100) copies a group's header and footer into the caller's buffers without first
// checking that the group has them: a group inserted without a footer, as every group of Performance
// Monitor's Add Counters list is, is a null read inside comctl32, and the unhandled fault ends the whole
// application. A group query therefore calls the class window procedure directly under a structured
// handler -- an exception raised below SendMessage crosses a kernel callback and never reaches the
// sender -- and a null read means "the group has no such text".
enum class GroupQuery { Answered, Refused, NullText, Faulted };

int GroupQueryFilter(EXCEPTION_POINTERS* pointers, bool* nullRead) noexcept {
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // ExceptionInformation[0] is 0 for a read and [1] the address that was read.
    *nullRead = record.NumberParameters >= 2 && record.ExceptionInformation[0] == 0 &&
        record.ExceptionInformation[1] < 0x10000;
    return EXCEPTION_EXECUTE_HANDLER;
}

GroupQuery QueryGroupInfo(WNDPROC procedure, HWND hwnd, int index, LVGROUP& group) noexcept {
    bool nullRead = false;
    __try {
        return CallWindowProcW(procedure, hwnd, LVM_GETGROUPINFOBYINDEX, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&group)) ? GroupQuery::Answered : GroupQuery::Refused;
    } __except (GroupQueryFilter(GetExceptionInformation(), &nullRead)) {
        return nullRead ? GroupQuery::NullText : GroupQuery::Faulted;
    }
}

// One text of one group, read by its own query so that an absent text cannot hide the other. The buffer
// starts small and grows only while the text could have been cut short, like every other ListView text read
// (ReadGrowingText); a text the protocol cannot carry is returned whole for the caller's size check.
bool ReadListViewGroupText(WNDPROC procedure, HWND hwnd, int index, bool footer, std::wstring& text,
                           std::wstring& reason) {
    text.clear();
    for (size_t capacity = 256;; capacity = (std::min)(Ipc::kMaxStringChars + 2, capacity * 2)) {
        std::wstring buffer(capacity, L'\0');
        LVGROUP native{ sizeof(native) };
        native.mask = footer ? LVGF_FOOTER : LVGF_HEADER;
        (footer ? native.pszFooter : native.pszHeader) = buffer.data();
        (footer ? native.cchFooter : native.cchHeader) = static_cast<int>(capacity);
        switch (QueryGroupInfo(procedure, hwnd, index, native)) {
        case GroupQuery::Answered: break;
        case GroupQuery::NullText:
            if (CaptureTraceEnabled()) CaptureTrace(L"ListView group " + std::to_wstring(index) +
                (footer ? L" has no footer" : L" has no header"));
            return true;
        case GroupQuery::Refused: return Reject(reason, L"ListView group text read failed");
        case GroupQuery::Faulted: return Reject(reason, L"ListView group text read faulted");
        }
        buffer.back() = L'\0';
        const size_t length = wcsnlen_s(buffer.c_str(), capacity);
        if (length + 1 < capacity || capacity >= Ipc::kMaxStringChars + 2) {
            buffer.resize(length);
            text = std::move(buffer);
            return true;
        }
    }
}

bool ReadListViewGroups(HWND hwnd, ControlNode& node, std::wstring& reason) {
    node.groupViewEnabled = SendMessageW(hwnd, LVM_ISGROUPVIEWENABLED, 0, 0) != FALSE;
    node.listViewGroups.clear();
    node.itemGroupIds.clear();
    if (!node.groupViewEnabled) return true;
    const auto count = SendMessageW(hwnd, LVM_GETGROUPCOUNT, 0, 0);
    if (CaptureTraceEnabled()) CaptureTrace(L"ListView groups=" + std::to_wstring(count));
    if (count < 0 || count > 256) return Reject(reason, L"ListView group count exceeds 256");
    const auto procedure = reinterpret_cast<WNDPROC>(GetClassLongPtrW(hwnd, GCLP_WNDPROC));
    if (!procedure) return Reject(reason, L"ListView class window procedure is unavailable");
    const auto geometry = MessageGeometryScale::For(hwnd);
    std::unordered_set<int> ids;
    size_t textSize = 0;
    for (int index = 0; index < count; ++index) {
        // LVGF_SUBTITLE and LVGF_TASK are deliberately not requested: they are copied the same
        // unchecked way, and MMC never sets them.
        LVGROUP native{ sizeof(native) };
        native.mask = LVGF_GROUPID | LVGF_STATE | LVGF_ALIGN | LVGF_TITLEIMAGE | LVGF_EXTENDEDIMAGE;
        native.iTitleImage = native.iExtendedImage = -1;
        native.stateMask = ~0u;
        switch (QueryGroupInfo(procedure, hwnd, index, native)) {
        case GroupQuery::Answered: break;
        case GroupQuery::Refused: return Reject(reason, L"ListView group read failed");
        default: return Reject(reason, L"ListView group read faulted");
        }
        std::wstring header, footer;
        if (!ReadListViewGroupText(procedure, hwnd, index, false, header, reason) ||
            !ReadListViewGroupText(procedure, hwnd, index, true, footer, reason)) return false;
        if (native.iGroupId == I_GROUPIDCALLBACK || native.iGroupId == I_GROUPIDNONE ||
            !ids.insert(native.iGroupId).second)
            return Reject(reason, L"ListView group identity is invalid or duplicated");
        if (native.iTitleImage >= 0 || native.iExtendedImage >= 0 ||
            // LVGS_SELECTED only mirrors the selected row's group highlight.
            (native.state & (LVGS_SUBSETED | LVGS_SUBSETLINKFOCUSED))) {
            reason = L"ListView group requires image or subset semantics (state=" +
                std::to_wstring(native.state) + L" titleImage=" + std::to_wstring(native.iTitleImage) +
                L" extendedImage=" + std::to_wstring(native.iExtendedImage) + L")";
            return false;
        }
        ListViewGroupSnapshot group;
        group.groupId = native.iGroupId;
        group.header = std::move(header); group.footer = std::move(footer);
        group.headerAlignment = native.uAlign & LVGA_HEADER_CENTER ? L"center" :
            native.uAlign & LVGA_HEADER_RIGHT ? L"right" : L"left";
        group.footerAlignment = native.uAlign & LVGA_FOOTER_CENTER ? L"center" :
            native.uAlign & LVGA_FOOTER_RIGHT ? L"right" : L"left";
        textSize += group.header.size() + group.footer.size();
        if (group.header.size() > Ipc::kMaxStringChars || group.footer.size() > Ipc::kMaxStringChars ||
            textSize > kMaxStructuredTextChars)
            return Reject(reason, L"ListView group text exceeds protocol limits");
        group.collapsible = (native.state & LVGS_COLLAPSIBLE) != 0;
        group.collapsed = (native.state & LVGS_COLLAPSED) != 0;
        group.hidden = (native.state & LVGS_HIDDEN) != 0;
        if ((native.state & LVGS_NOHEADER) != 0) group.header.clear();
        // Each band remains in the native display order and physical position.
        auto readRect = [&](int part, RECT& target) {
            if (group.hidden) { target = {}; return true; }
            RECT rect{}; rect.top = part;
            if (!SendMessageW(hwnd, LVM_GETGROUPRECT, group.groupId, reinterpret_cast<LPARAM>(&rect)))
                return Reject(reason, L"ListView group geometry read failed");
            target = geometry.Rect(rect);
            return target.right >= target.left && target.bottom >= target.top &&
                std::abs(static_cast<int64_t>(target.left)) <= Ipc::kMaxCoordinate &&
                std::abs(static_cast<int64_t>(target.top)) <= Ipc::kMaxCoordinate &&
                static_cast<int64_t>(target.right) - target.left <= Ipc::kMaxCoordinate &&
                static_cast<int64_t>(target.bottom) - target.top <= Ipc::kMaxCoordinate;
        };
        if (!readRect(LVGGR_HEADER, group.headerRect))
            return Reject(reason, L"ListView group bounds exceed the bounded projection");
        if (!group.hidden && !group.collapsed && !group.footer.empty()) {
            // The documented rectangle selectors expose the whole group and its
            // header, but no footer selector. Reserve the footer's native font
            // extent at the bottom of the group, inside its border metrics.
            RECT whole{};
            if (!readRect(LVGGR_GROUP, whole))
                return Reject(reason, L"ListView whole-group bounds are unavailable");
            LVGROUPMETRICS metrics{ sizeof(metrics), LVGMF_BORDERSIZE };
            SendMessageW(hwnd, LVM_GETGROUPMETRICS, 0, reinterpret_cast<LPARAM>(&metrics));
            if (metrics.Bottom > Ipc::kMaxCoordinate)
                return Reject(reason, L"ListView group footer border exceeds coordinate limits");
            HDC dc = GetDC(hwnd);
            if (!dc) return Reject(reason, L"ListView footer font measurement failed");
            const HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            const HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
            TEXTMETRICW textMetrics{};
            const bool measured = GetTextMetricsW(dc, &textMetrics) != FALSE;
            if (old && old != HGDI_ERROR) SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
            if (!measured) return Reject(reason, L"ListView footer font measurement failed");
            const LONG bottom = std::max(group.headerRect.bottom,
                whole.bottom - geometry.Vertical(static_cast<int>(metrics.Bottom)));
            group.footerRect = { whole.left, std::max(group.headerRect.bottom,
                bottom - geometry.Vertical(textMetrics.tmHeight)), whole.right, bottom };
        }
        node.listViewGroups.push_back(std::move(group));
    }
    return true;
}

// A virtual report list longer than the threshold is carried as a window of this many
// rows around its native top row: a page above and the rest below, enough that the
// projection scrolls a page either way before it asks for the next window.
constexpr int kListViewWindowThreshold = 512;
constexpr int kListViewWindowRows = 192;

bool CaptureListViewState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    ControlKind admittedKind{};
    // CaptureControlDetail is also used directly by native-backed adapters and
    // tests, whose node may not yet carry the shared window facets. The HWND is
    // authoritative for its current view, including a recent native mode change.
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    node.style = style;
    if (!ProbeListView(hwnd, style, admittedKind, reason) ||
        !ReadListViewMode(hwnd, style, node.listViewMode, reason)) return false;
    bool report = node.listViewMode == L"report";
    // The mode the control itself is in, for the end-of-capture consistency check below.
    const std::wstring nativeMode = node.listViewMode;
    const LRESULT count = SendMessageW(hwnd, LVM_GETITEMCOUNT, 0, 0);
    // A large virtual report list (Event Viewer's logs) answers every row through its
    // owner, so it is carried as a window of rows around its native top row rather
    // than read whole on every capture.  Labels it cannot edit and the absence of
    // checkboxes keep every per-row action inside the rows the window carries; owner
    // data never has groups.
    const bool windowed = report && (style & (LVS_OWNERDATA | LVS_EDITLABELS)) == LVS_OWNERDATA &&
        count > kListViewWindowThreshold && count <= static_cast<LRESULT>(Ipc::kMaxVirtualListItems) &&
        (SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0) & LVS_EX_CHECKBOXES) == 0;
    if (count < 0 || (!windowed && count > static_cast<LRESULT>(Ipc::kMaxListItems))) {
        return Reject(reason, L"invalid or excessive ListView item count");
    }
    int windowStart = 0;
    int windowEnd = static_cast<int>(count);
    if (windowed) {
        const int top = std::clamp(static_cast<int>(SendMessageW(hwnd, LVM_GETTOPINDEX, 0, 0)),
            0, static_cast<int>(count) - 1);
        const int perPage = std::clamp(static_cast<int>(SendMessageW(hwnd, LVM_GETCOUNTPERPAGE, 0, 0)),
            1, kListViewWindowRows / 3);
        windowStart = std::max(0, top - perPage);
        windowEnd = std::min(static_cast<int>(count), windowStart + kListViewWindowRows);
        windowStart = std::max(0, windowEnd - kListViewWindowRows);
    }
    const auto windowRows = static_cast<size_t>(windowEnd - windowStart);
    const HWND header = report
        ? reinterpret_cast<HWND>(SendMessageW(hwnd, LVM_GETHEADER, 0, 0)) : nullptr;
    const int columnCount = header && IsWindow(header) ? Header_GetItemCount(header) : 0;
    if (CaptureTraceEnabled())
        CaptureTrace(L"ListView items=" + std::to_wstring(count) + L" columns=" + std::to_wstring(columnCount) +
            L" mode=" + node.listViewMode + L" style=" + std::to_wstring(style) + L" windowed=" +
            std::to_wstring(windowed));
    if (report && columnCount <= 0) {
        // A report list with no columns paints nothing, whatever it holds (Event Viewer's
        // Subscriptions list before the collector service answers).  With no rows either
        // it is an empty list, and the projection carries exactly that; rows without
        // columns have no faithful presentation and stay refused.
        if (count != 0 || windowed) {
            return Reject(reason,
                L"ListView has no native report columns for faithful bounded projection");
        }
        report = false;
        node.listViewMode = L"list";
    }
    if (static_cast<size_t>(columnCount) > kMaxListViewColumns) {
        return Reject(reason, L"ListView has excessive report columns");
    }
    if (report && !ValidateListViewColumns(hwnd, header, columnCount, reason)) return false;
    node.columnHeadersVisible = report && (style & LVS_NOCOLUMNHEADER) == 0;
    node.editableLabels = (style & LVS_EDITLABELS) != 0;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
    node.checkBoxes = (extended & LVS_EX_CHECKBOXES) != 0;
    if (!ReadListViewGroups(hwnd, node, reason)) return false;

    size_t totalText = 0;
    const auto withinTextBudget = [&](size_t added) {
        totalText += added;
        if (totalText <= kMaxStructuredTextChars) return true;
        return Reject(reason, L"ListView text exceeds the bounded adapter payload");
    };

    node.columns.clear();
    node.columnWidths.clear();
    node.columnOrder.clear();
    node.rows.clear();
    node.items.clear();
    node.itemRects.clear();
    node.itemNativeIds.clear();
    node.columns.reserve(static_cast<size_t>(columnCount));
    node.columnWidths.reserve(static_cast<size_t>(columnCount));
    if (report && !ReadListViewColumnOrder(hwnd, columnCount, node.columnOrder, reason)) return false;
    const MessageGeometryScale geometry = MessageGeometryScale::For(hwnd);
    for (int column = 0; column < columnCount; ++column) {
        std::wstring label;
        int width = 0;
        if (!ReadListViewColumn(hwnd, column, label, width, reason)) return false;
        if (!withinTextBudget(label.size())) return false;
        node.columns.push_back(std::move(label));
        node.columnWidths.push_back(geometry.Horizontal(width));
    }

    node.itemCount = windowed ? static_cast<size_t>(count) : 0;
    node.itemWindowStart = static_cast<size_t>(windowStart);
    if (report) node.rows.reserve(windowRows);
    else node.itemRects.reserve(windowRows);
    node.items.reserve(windowRows);
    node.itemImages.clear();
    node.itemImages.reserve(windowRows);
    const uint32_t invalidId = std::numeric_limits<uint32_t>::max();
    // Owner-data indices are positions in the application's current data, not
    // stable item identities. Some common-control versions answer the ID
    // messages anyway; never let that authorize deferred activation.
    const bool stableIds = (style & LVS_OWNERDATA) == 0 && (count == 0 ||
        (static_cast<uint32_t>(SendMessageW(hwnd, LVM_MAPINDEXTOID,
            static_cast<WPARAM>(-1), 0)) == invalidId &&
         static_cast<int>(SendMessageW(hwnd, LVM_MAPIDTOINDEX, invalidId, 0)) == -1));
    // Older controls may lack this extension. Display and selection remain
    // admissible, but an empty ID vector cannot authorize item activation.
    if (stableIds) node.itemNativeIds.reserve(static_cast<size_t>(count));
    std::unordered_set<uint32_t> nativeIds;
    CaptureTrace(L"ListView columns read; reading rows");
    for (int row = windowStart; row < windowEnd; ++row) {
        if (CaptureTraceEnabled()) CaptureTrace(L"ListView row " + std::to_wstring(row));
        if (stableIds) {
            const auto nativeId = static_cast<uint32_t>(SendMessageW(
                hwnd, LVM_MAPINDEXTOID, static_cast<WPARAM>(row), 0));
            if (nativeId == invalidId || !nativeIds.insert(nativeId).second ||
                ResolveListViewItemByNativeId(hwnd, nativeId) != row)
                return Reject(reason, L"ListView stable item identity did not round-trip");
            node.itemNativeIds.push_back(nativeId);
        }
        std::vector<std::wstring> cells;
        const int textColumns = report ? columnCount : 1;
        cells.reserve(static_cast<size_t>(textColumns));
        for (int column = 0; column < textColumns; ++column) {
            std::wstring text;
            if (!ReadListViewCell(hwnd, row, column, text, reason)) return false;
            if (!withinTextBudget(text.size())) return false;
            cells.push_back(std::move(text));
        }
        LVITEMW image{};
        image.mask = LVIF_IMAGE | (node.groupViewEnabled ? LVIF_GROUPID : 0);
        image.iItem = row;
        image.iImage = I_IMAGENONE;
        if (!SendMessageW(hwnd, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&image)))
            return Reject(reason, L"ListView item image read failed");
        if (image.iImage == I_IMAGECALLBACK) {
            // The control resolves a callback icon by asking its parent while it
            // paints and draws nothing when the parent leaves it unanswered (Windows
            // Forms stores ImageIndex -1 this way). Ask the same question.
            LVITEMW lookup{};
            lookup.mask = LVIF_PARAM;
            lookup.iItem = row;
            SendMessageW(hwnd, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lookup));
            NMLVDISPINFOW query{};
            query.hdr = { hwnd, static_cast<UINT_PTR>(GetDlgCtrlID(hwnd)), static_cast<UINT>(LVN_GETDISPINFOW) };
            query.item.mask = LVIF_IMAGE;
            query.item.iItem = row;
            query.item.lParam = lookup.lParam;
            query.item.iImage = I_IMAGECALLBACK;
            SendMessageW(GetParent(hwnd), WM_NOTIFY, query.hdr.idFrom, reinterpret_cast<LPARAM>(&query));
            image.iImage = query.item.iImage == I_IMAGECALLBACK ? I_IMAGENONE : query.item.iImage;
        }
        node.itemImages.push_back(image.iImage);
        bool groupVisible = true;
        if (node.groupViewEnabled) {
            node.itemGroupIds.push_back(image.iGroupId);
            const auto group = std::find_if(node.listViewGroups.begin(), node.listViewGroups.end(),
                [&](const auto& entry) { return entry.groupId == image.iGroupId; });
            if (group == node.listViewGroups.end() && image.iGroupId != I_GROUPIDNONE)
                return Reject(reason, L"ListView item names an unknown group");
            groupVisible = group != node.listViewGroups.end() && !group->collapsed && !group->hidden;
        }
        node.items.push_back(cells.front());
        if (report) node.rows.push_back(std::move(cells));
        if (!report || node.groupViewEnabled) {
            if (!groupVisible) { node.itemRects.push_back({}); continue; }
            RECT bounds{ LVIR_BOUNDS };
            if (!SendMessageW(hwnd, LVM_GETITEMRECT, static_cast<WPARAM>(row),
                    reinterpret_cast<LPARAM>(&bounds)))
                return Reject(reason, L"ListView item geometry read failed");
            bounds = geometry.Rect(bounds);
            const auto width = static_cast<int64_t>(bounds.right) - bounds.left;
            const auto height = static_cast<int64_t>(bounds.bottom) - bounds.top;
            if (width <= 0 || height <= 0 || width > Ipc::kMaxCoordinate ||
                height > Ipc::kMaxCoordinate ||
                bounds.left < -Ipc::kMaxCoordinate || bounds.left > Ipc::kMaxCoordinate ||
                bounds.top < -Ipc::kMaxCoordinate || bounds.top > Ipc::kMaxCoordinate)
                return Reject(reason, L"ListView item geometry exceeds the bounded projection");
            node.itemRects.push_back(bounds);
        }
    }
    CaptureTrace(L"ListView rows read");
    // Read the current list after the item callbacks, which can populate it.
    // Large icons use LVSIL_NORMAL; all other admitted views draw LVSIL_SMALL.
    if (!CaptureReferencedImageList(reinterpret_cast<HIMAGELIST>(
            SendMessageW(hwnd, LVM_GETIMAGELIST,
                node.listViewMode == L"largeIcon" ? LVSIL_NORMAL : LVSIL_SMALL, 0)),
            node.itemImages, nullptr, nullptr, node.imageList, reason)) return false;
    const HWND editControl = reinterpret_cast<HWND>(
        SendMessageW(hwnd, LVM_GETEDITCONTROL, 0, 0));
    node.editingIndex = editControl && IsWindow(editControl)
        ? static_cast<int>(SendMessageW(
              hwnd, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED))
        : -1;
    if (node.editingIndex >= 0 &&
        static_cast<size_t>(node.editingIndex) >= ListViewItemCount(node))
        return Reject(reason, L"ListView edit session names a row outside its items");

    if (!ReadListViewSelection(hwnd, ListViewItemCount(node), node.selectedIndices, reason)) {
        return false;
    }
    if (!ReadListViewChecks(
            hwnd, ListViewItemCount(node), node.checkBoxes, node.checkedIndices, reason)) {
        return false;
    }
    node.focusedIndex = static_cast<int>(SendMessageW(
        hwnd, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
    if (node.focusedIndex < -1 ||
        (node.focusedIndex >= 0 &&
            static_cast<size_t>(node.focusedIndex) >= ListViewItemCount(node))) {
        return Reject(reason, L"ListView focused index is outside the item range");
    }
    node.multiSelect = (static_cast<DWORD>(node.style) & LVS_SINGLESEL) == 0;
    node.selectedIndex = node.selectedIndices.empty() ? -1 : node.selectedIndices.front();
    CaptureTrace(L"ListView selection read; asking for native default actions");
    // A Windows Forms list inside MMC (Task Scheduler's tasks) answers the accessible default
    // action without opening anything; its Enter is what raises ItemActivate.
    node.itemActivationSupported = stableIds &&
        !(IsWindowsFormsWindow(hwnd) && IsInsideMmcFrame(hwnd)) &&
        ListViewItemsHaveNativeDefaultActions(hwnd, node);
    CaptureTrace(L"ListView default actions answered");
    // A list without that identity contract (virtual rows: Services, an event log) is still
    // opened by a double-click: the projection selects the row and sends the list Enter.
    node.itemDefaultActivation = !node.itemActivationSupported && IsInsideMmcFrame(hwnd);
    // A virtual text/image callback can also change the control's state
    // contract. Do not publish a snapshot that omitted state installed during
    // those callbacks, even when the row count and presentation stayed fixed.
    if ((style & LVS_OWNERDATA) != 0 && !ProbeListView(hwnd,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), admittedKind, reason))
        return false;
    std::wstring finalMode;
    if (SendMessageW(hwnd, LVM_GETITEMCOUNT, 0, 0) != count ||
        !ReadListViewMode(hwnd, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
            finalMode, reason) || finalMode != nativeMode)
        return Reject(reason, L"ListView item count or mode changed during capture");
    for (size_t index = 0; index < node.itemNativeIds.size(); ++index) {
        if (ResolveListViewItemByNativeId(hwnd, node.itemNativeIds[index]) != static_cast<int>(index))
            return Reject(reason, L"ListView item identity changed during capture");
    }
    if (node.groupViewEnabled) {
        ControlNode finalGroups;
        if (!ReadListViewGroups(hwnd, finalGroups, reason) || !finalGroups.groupViewEnabled ||
            finalGroups.listViewGroups.size() != node.listViewGroups.size())
            return Reject(reason, L"ListView group structure changed during capture");
        for (size_t index = 0; index < node.listViewGroups.size(); ++index) {
            const auto& before = node.listViewGroups[index];
            const auto& after = finalGroups.listViewGroups[index];
            if (before.groupId != after.groupId || before.header != after.header || before.footer != after.footer ||
                before.headerAlignment != after.headerAlignment || before.footerAlignment != after.footerAlignment ||
                before.collapsible != after.collapsible || before.collapsed != after.collapsed || before.hidden != after.hidden ||
                !EqualRect(&before.headerRect, &after.headerRect) || !EqualRect(&before.footerRect, &after.footerRect))
                return Reject(reason, L"ListView group state changed during capture");
        }
        for (int index = 0; index < count; ++index) {
            LVITEMW item{}; item.mask = LVIF_GROUPID; item.iItem = index;
            if (!SendMessageW(hwnd, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ||
                item.iGroupId != node.itemGroupIds[index])
                return Reject(reason, L"ListView group membership changed during capture");
        }
    }
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
    // SB_GETPARTS answers in the control's own coordinate space; -1 is the native
    // stretch marker and stays as it is.
    const MessageGeometryScale geometry = MessageGeometryScale::For(hwnd);
    for (int& edge : rightEdges) {
        if (edge > 0) edge = geometry.Horizontal(edge);
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

// A Windows Forms LinkLabel carries plain text (no markup) and names its one link
// through the link's accessible object.  A mnemonic would change what is drawn, and a
// link run that occurs twice would leave the projected link without one position.
bool CaptureWindowsFormsLinkState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    std::vector<AccessibleLink> links;
    if (!ReadWindowsFormsLinks(hwnd, links) || links.size() != 1)
        return Reject(reason, L"LinkLabel no longer publishes exactly one accessible link");
    const std::wstring& label = links.front().name;
    if (node.text.find(L'&') != std::wstring::npos)
        return Reject(reason, L"LinkLabel text carries a mnemonic the projection does not draw");
    const size_t at = node.text.find(label);
    if (at == std::wstring::npos || node.text.find(label, at + label.size()) != std::wstring::npos)
        return Reject(reason, L"LinkLabel link text is not one unambiguous run of its label");
    node.automationName = node.text;
    node.items.assign(1, label);
    node.enabled = node.enabled && links.front().enabled;
    return true;
}

// The bounded SysLink adapter accepts exactly one hyperlink with no nested
// markup, so its label has an unambiguous position in the flattened text.
bool CaptureSysLinkState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    if (IsWindowsFormsWindow(hwnd)) return CaptureWindowsFormsLinkState(hwnd, node, reason);
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
    std::wstring prefix = markup.substr(0, open);
    const std::wstring label = markup.substr(openEnd + 1, close - openEnd - 1);
    std::wstring suffix = markup.substr(close + 4);
    while (!suffix.empty() && suffix.back() <= L' ') suffix.pop_back();
    const auto hasMarkup = [](std::wstring_view value) {
        return value.find_first_of(L"<>") != std::wstring_view::npos;
    };
    if (hasMarkup(prefix) || hasMarkup(label) || hasMarkup(suffix) ||
        label.find(L'&') != std::wstring::npos) {
        return Reject(reason,
            L"SysLink contains unsupported nested markup or mnemonic text");
    }
    // The text around the link may name the next control's access key, as a label does (the
    // object picker's "Enter the object name (&E):"); it is carried as the text it shows.
    const auto shown = [](const std::wstring& value) {
        std::wstring result;
        for (size_t index = 0; index < value.size(); ++index) {
            if (value[index] == L'&' && index + 1 < value.size()) ++index;
            else if (value[index] == L'&') continue;
            result.push_back(value[index]);
        }
        return result;
    };
    prefix = shown(prefix);
    suffix = shown(suffix);
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

// A button too small to hold a word, whose text is a character or two, is a glyph button: a
// drop-down arrow or a browse "..." that a Windows Forms control draws in a symbol font it
// keeps to itself (an owner-draw button answers WM_GETFONT with nothing). Its painted face is
// what it shows; projecting its text would show the characters of the glyph codes ("67").
bool IsTinyGlyphButton(HWND hwnd, const std::wstring& text) noexcept {
    RECT client{};
    return text.size() <= 3 && GetClientRect(hwnd, &client) && client.right <= 36 && client.bottom <= 36;
}

// BS_ICON / BS_BITMAP push buttons draw their picture centred instead of their text.
bool CaptureButtonState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto style = static_cast<DWORD>(node.style);
    node.isDefault = (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON ||
        (style & BS_TYPEMASK) == BS_DEFCOMMANDLINK;
    if ((style & BS_TYPEMASK) == BS_COMMANDLINK || (style & BS_TYPEMASK) == BS_DEFCOMMANDLINK) {
        node.presentationVariant = L"commandLink";
        node.helpText.clear();
        const auto length = static_cast<DWORD>(SendMessageW(hwnd, BCM_GETNOTELENGTH, 0, 0));
        if (length > Ipc::kMaxStringChars) return Reject(reason, L"command link note exceeds the protocol cap");
        if (length != 0) {
            std::wstring note(static_cast<size_t>(length) + 1, L'\0');
            DWORD capacity = length + 1;
            if (!SendMessageW(hwnd, BCM_GETNOTE, reinterpret_cast<WPARAM>(&capacity),
                    reinterpret_cast<LPARAM>(note.data())))
                return Reject(reason, L"command link note is unavailable");
            note.resize(wcsnlen(note.c_str(), note.size()));
            node.helpText = std::move(note);
        }
        return true;
    }
    if ((style & BS_TYPEMASK) == BS_OWNERDRAW && !IsWindowsFormsWindow(hwnd)) {
        // The owner paints the whole face, so the picture is the face as painted, not a glyph on a
        // background to key out (a colour swatch is all background); the text stays the accessible name.
        if (!CapturePaintedFace(hwnd, Ipc::kMaxImageDimension, node.imageWidth, node.imageHeight,
                node.imageFormat, node.imageData))
            return Reject(reason, L"owner-draw button face exceeds the picture cap or did not render");
        return true;
    }
    if ((style & BS_TYPEMASK) == BS_OWNERDRAW) {
        // A WinForms form's AcceptButton is reported as the default through MSAA.
        long role = 0;
        long state = 0;
        if (!ReadWindowAccessible(hwnd, role, state) || role != ROLE_SYSTEM_PUSHBUTTON)
            return Reject(reason, L"owner-draw WinForms button changed its accessible role");
        node.isDefault = (state & STATE_SYSTEM_DEFAULT) != 0;
        // A button with no text (an image button, such as a section's collapse
        // chevron) or with symbol-font text (Marlett's close cross) shows a picture:
        // the face it paints, which the text stays the accessible name of.
        if (node.text.empty() || UsesSymbolFont(hwnd) || IsTinyGlyphButton(hwnd, node.text)) {
            if (!CaptureKeyedFace(hwnd, Ipc::kMaxImageDimension, node.imageWidth, node.imageHeight,
                    node.imageFormat, node.imageData))
                return Reject(reason, L"WinForms picture button face exceeds the picture cap or did not render");
        }
        return true;
    }
    if ((style & (BS_ICON | BS_BITMAP)) == 0 &&
        ((style & BS_TYPEMASK) == BS_PUSHBUTTON || (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) &&
        !node.text.empty() && (UsesSymbolFont(hwnd) || IsTinyGlyphButton(hwnd, node.text))) {
        // A standard push button whose label is glyph codes (Task Scheduler's up/down arrows
        // are Marlett "5" and "6") shows the face it paints. A face that does not render is
        // not a refusal: the button is still a button, with the characters as its text.
        CaptureKeyedFace(hwnd, Ipc::kMaxImageDimension, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData);
        return true;
    }
    if ((style & BS_ICON) != 0) {
        const auto icon = reinterpret_cast<HICON>(SendMessageW(hwnd, BM_GETIMAGE, IMAGE_ICON, 0));
        if (!icon) return Reject(reason, L"icon Button has no current HICON");
        return CaptureIconPixels(icon, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData, reason);
    }
    if ((style & BS_BITMAP) != 0) {
        const auto bitmap = reinterpret_cast<HBITMAP>(SendMessageW(hwnd, BM_GETIMAGE, IMAGE_BITMAP, 0));
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
        if (!bitmap || !ReadBitmapPixels(bitmap, width, height, pixels))
            return Reject(reason, L"bitmap Button has no readable HBITMAP");
        if (width > static_cast<int>(Ipc::kMaxImageDimension) ||
            height > static_cast<int>(Ipc::kMaxImageDimension))
            return Reject(reason, L"bitmap Button picture is larger than the image cap");
        for (size_t offset = 3; offset < pixels.size(); offset += 4) pixels[offset] = 0xff;
        node.imageWidth = static_cast<uint32_t>(width);
        node.imageHeight = static_cast<uint32_t>(height);
        node.imageFormat = L"bgra8-premultiplied";
        node.imageData = std::move(pixels);
    }
    return true;
}

bool CaptureToggleState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    node.checked = ReadButtonCheckState(hwnd);
    if (node.checked < 0) return Reject(reason, L"owner-draw WinForms toggle state is unavailable");
    return true;
}

// The grey hint an empty edit box shows.  Both messages exist from common controls 6 and
// answer FALSE when there is none; the hint is bounded like any short label.
std::wstring ReadCueBanner(HWND hwnd, UINT message) {
    std::vector<wchar_t> buffer(kMaxTabLabelChars + 1, L'\0');
    if (!hwnd || !SendMessageW(hwnd, message, reinterpret_cast<WPARAM>(buffer.data()),
            static_cast<LPARAM>(buffer.size())))
        return {};
    buffer.back() = L'\0';
    return std::wstring(buffer.data(), wcsnlen_s(buffer.data(), buffer.size() - 1));
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
    node.cueBanner = node.multiline ? std::wstring() : ReadCueBanner(hwnd, EM_GETCUEBANNER);
    return true;
}

// A read-only rich edit shown in its formats: the control's text as paragraphs of runs.
bool CaptureRichTextState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto style = static_cast<DWORD>(node.style);
    node.readOnly = true;
    node.multiline = (style & ES_MULTILINE) != 0;
    return ReadRichText(hwnd, node.richParagraphs, reason);
}

// The items of a ComboBoxEx as it stores them.  Only text items are projected: an item that
// draws an image (any item does once the control has an image list), is indented, or asks its
// owner for its text stays native.
bool CaptureComboBoxExState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    node.editable = (static_cast<DWORD>(node.style) & 0x0003u) == CBS_DROPDOWN;
    if (SendMessageW(hwnd, CBEM_GETIMAGELIST, 0, 0))
        return Reject(reason, L"ComboBoxEx item images are not projected");
    const LRESULT rawCount = SendMessageW(hwnd, CB_GETCOUNT, 0, 0);
    if (rawCount < 0 || rawCount > static_cast<LRESULT>(Ipc::kMaxListItems))
        return Reject(reason, L"invalid or excessive ComboBoxEx item count");
    node.items.clear();
    node.items.reserve(static_cast<size_t>(rawCount));
    size_t totalText = 0;
    std::vector<wchar_t> buffer(Ipc::kMaxStringChars + 1);
    for (LRESULT index = 0; index < rawCount; ++index) {
        std::fill(buffer.begin(), buffer.end(), L'\0');
        COMBOBOXEXITEMW item{};
        item.mask = CBEIF_TEXT | CBEIF_INDENT;
        item.iItem = static_cast<INT_PTR>(index);
        item.pszText = buffer.data();
        item.cchTextMax = static_cast<int>(buffer.size());
        if (!SendMessageW(hwnd, CBEM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
            return Reject(reason, L"ComboBoxEx item is unavailable");
        if (item.pszText == LPSTR_TEXTCALLBACKW)
            return Reject(reason, L"ComboBoxEx callback text is not supported");
        if (item.iIndent != 0)
            return Reject(reason, L"indented ComboBoxEx items are not projected");
        const wchar_t* text = item.pszText ? item.pszText : buffer.data();
        const size_t length = wcsnlen_s(text, buffer.size());
        if (length > Ipc::kMaxStringChars || totalText > kMaxStructuredTextChars - length)
            return Reject(reason, L"ComboBoxEx item text exceeds the text cap");
        totalText += length;
        node.items.emplace_back(text, length);
    }
    node.selectedIndex = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    if (node.selectedIndex < -1 || node.selectedIndex >= static_cast<int>(node.items.size()))
        return Reject(reason, L"ComboBoxEx selection is outside its items");
    // What the box shows: the typed text of an editable one, the selected item otherwise.
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(hwnd, CBEM_GETEDITCONTROL, 0, 0));
    if (node.editable) {
        if (!edit || !IsWindow(edit)) return Reject(reason, L"editable ComboBoxEx has no edit box");
        const int length = GetWindowTextLengthW(edit);
        if (length < 0 || static_cast<size_t>(length) > Ipc::kMaxStringChars)
            return Reject(reason, L"ComboBoxEx edit text exceeds the text cap");
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(edit, text.data(), length + 1)));
        node.text = std::move(text);
        node.cueBanner = ReadCueBanner(edit, EM_GETCUEBANNER);
        if (node.cueBanner.empty()) node.cueBanner = ReadCueBanner(hwnd, CB_GETCUEBANNER);
    } else {
        node.text = node.selectedIndex < 0 ? std::wstring() : node.items[static_cast<size_t>(node.selectedIndex)];
    }
    return true;
}

bool CaptureStringListState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const bool combo = node.kind == ControlKind::ComboBox;
    if (combo && IsComboBoxExWindow(hwnd)) return CaptureComboBoxExState(hwnd, node, reason);
    if (combo && (static_cast<DWORD>(node.style) & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)))
        return CaptureOwnerDrawComboBox(hwnd, node, reason);
    node.editable = combo && (static_cast<DWORD>(node.style) & 0x0003u) == CBS_DROPDOWN;
    if (node.editable) node.cueBanner = ReadCueBanner(hwnd, CB_GETCUEBANNER);
    if (!ReadStringItems(hwnd, combo, node.items, reason)) return false;
    if (!combo && (static_cast<DWORD>(node.style) & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0) {
        // A multi-select list answers its selection as a list of indices, not as a current item.
        node.multiSelect = true;
        const LRESULT count = SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
        if (count == LB_ERR || static_cast<size_t>(count) > node.items.size())
            return Reject(reason, L"multi-select ListBox selection could not be read");
        node.selectedIndices.assign(static_cast<size_t>(count), 0);
        if (count > 0 && SendMessageW(hwnd, LB_GETSELITEMS, static_cast<WPARAM>(count),
                reinterpret_cast<LPARAM>(node.selectedIndices.data())) != count)
            return Reject(reason, L"multi-select ListBox selection could not be read");
        std::sort(node.selectedIndices.begin(), node.selectedIndices.end());
        node.selectedIndex = node.selectedIndices.empty() ? -1 : node.selectedIndices.front();
        return true;
    }
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
    const MessageGeometryScale geometry = MessageGeometryScale::For(hwnd);
    // comctl32 scrolls an overflowing single-line strip with an up-down it creates
    // as the control's own visible child; that is the only overflow admitted.
    bool scrollingStrip = false;
    if ((static_cast<DWORD>(node.style) & TCS_MULTILINE) == 0) {
        for (HWND child = GetWindow(hwnd, GW_CHILD); child && !scrollingStrip; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t childClass[32]{};
            scrollingStrip = IsWindowVisible(child) && GetClassNameW(child, childClass, 32) > 0 &&
                FluentShell::EqualsIgnoreCase(childClass, UPDOWN_CLASSW);
        }
    }
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
        if (!SendMessageW(hwnd, TCM_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect)))
            return Reject(reason, L"TabControl item rectangle is malformed or outside client bounds");
        rect = geometry.Rect(rect);
        // A single-line strip wider than the control scrolls with its own up-down;
        // tabs scrolled out lie beyond either side but stay in the strip's band.
        const bool horizontalOverflow = rect.left < 0 || rect.right > client.right;
        if (rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
            rect.bottom > client.bottom || (horizontalOverflow && !scrollingStrip))
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

bool DescribeToolbarButtonStyle(
    BYTE style, ToolbarItemSnapshot& item, std::wstring& reason) {
    if ((style & BTNS_SEP) != 0) {
        if (style != BTNS_SEP)
            return Reject(reason, L"ToolbarWindow32 separator has unsupported style semantics");
        item.kind = ToolbarItemKind::Separator;
        return true;
    }
    const bool toggle = (style & BTNS_CHECK) != 0;
    // comctl32 reads BTNS_GROUP only together with BTNS_CHECK (a radio member); alone
    // it changes nothing about the button, which stays an ordinary push button.
    // Performance Monitor's snap-in toolbar buttons carry it that way.
    const bool grouped = toggle && (style & BTNS_GROUP) != 0;
    constexpr BYTE acceptedButtonStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT | BTNS_CHECK |
        BTNS_GROUP | BTNS_DROPDOWN | BTNS_WHOLEDROPDOWN;
    if ((style & ~acceptedButtonStyle) != 0) {
        wchar_t evidence[152]{};
        swprintf_s(evidence,
            L"ToolbarWindow32 custom button style 0x%02X is not supported",
            static_cast<unsigned>(style));
        return Reject(reason, evidence);
    }
    item.dropDown = (style & BTNS_DROPDOWN) != 0;
    item.wholeDropDown = item.dropDown && (style & BTNS_WHOLEDROPDOWN) != 0;
    if (!item.dropDown && (style & BTNS_WHOLEDROPDOWN) != 0)
        return Reject(reason,
            L"ToolbarWindow32 whole-dropdown button has no BTNS_DROPDOWN arrow");
    if (toggle && item.dropDown)
        return Reject(reason, L"ToolbarWindow32 button combines a latch with a dropdown arrow");
    item.kind = grouped ? ToolbarItemKind::RadioButton
        : toggle ? ToolbarItemKind::ToggleButton : ToolbarItemKind::PushButton;
    return true;
}

bool CaptureToolbarImageVariant(HWND hwnd, UINT message, int listId, int imageIndex,
    uint32_t normalWidth, uint32_t normalHeight, std::optional<ImageListEntry>& image,
    std::wstring& reason) {
    image.reset();
    const auto list = reinterpret_cast<HIMAGELIST>(
        SendMessageW(hwnd, message, static_cast<WPARAM>(listId), 0));
    if (!list) return true;
    int width = 0;
    int height = 0;
    if (imageIndex >= ImageList_GetImageCount(list) ||
        !ImageList_GetIconSize(list, &width, &height) ||
        width != static_cast<int>(normalWidth) || height != static_cast<int>(normalHeight))
        return Reject(reason, L"ToolbarWindow32 state image list has no matching normal icon geometry or index");
    HICON icon = ImageList_GetIcon(list, imageIndex, ILD_NORMAL);
    if (!icon) return Reject(reason, L"ToolbarWindow32 state image-list icon copy failed");
    ImageListEntry pixels;
    const bool copied = CaptureIconPixels(icon, pixels.imageWidth, pixels.imageHeight,
        pixels.imageFormat, pixels.imageData, reason);
    DestroyIcon(icon);
    if (!copied) return false;
    image = std::move(pixels);
    return true;
}

bool CaptureToolbarState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const auto toolbarStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    constexpr std::array<UINT, 4> imageQueries = {
        TB_GETIMAGELIST, TB_GETHOTIMAGELIST, TB_GETDISABLEDIMAGELIST, TB_GETPRESSEDIMAGELIST };
    std::unordered_map<int, std::array<HIMAGELIST, 4>> observedLists;
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
    int radioGroup = 0;
    bool inRadioGroup = false;
    bool groupHasCheckedItem = false;
    std::vector<TBBUTTON> capturedButtons;
    capturedButtons.reserve(static_cast<size_t>(rawCount));
    // Whether the control draws a button's label on its face beside the icon: list mode with
    // text rows, and under TBSTYLE_EX_MIXEDBUTTONS only for the BTNS_SHOWTEXT buttons.
    const bool listLabels = (static_cast<DWORD>(node.style) & TBSTYLE_LIST) != 0 &&
        SendMessageW(hwnd, TB_GETTEXTROWS, 0, 0) > 0;
    const bool mixedButtons = (static_cast<DWORD>(SendMessageW(hwnd, TB_GETEXTENDEDSTYLE, 0, 0)) &
        TBSTYLE_EX_MIXEDBUTTONS) != 0;
    for (int index = 0; index < rawCount; ++index) {
        TBBUTTON button{};
        if (!SendMessageW(hwnd, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&button)))
            return Reject(reason, L"ToolbarWindow32 TB_GETBUTTON failed");
        capturedButtons.push_back(button);
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
        bool ownLabel = false;
        item.enabled = (button.fsState & TBSTATE_ENABLED) != 0;
        item.hidden = (button.fsState & TBSTATE_HIDDEN) != 0;
        item.checked = (button.fsState & TBSTATE_CHECKED) != 0;
        if (!DescribeToolbarButtonStyle(button.fsStyle, item, reason)) return false;
        if (item.kind == ToolbarItemKind::Separator) {
            inRadioGroup = false;
        } else {
            // A check-style button owns its latched state, and an application that
            // manages the latch itself sets TBSTATE_CHECKED on an ordinary button.  Both
            // are projected with the state the control currently reports; the projection
            // posts the same WM_COMMAND and reads back whatever the control settled on,
            // so neither the control nor the application loses ownership of it.
            const bool grouped = item.kind == ToolbarItemKind::RadioButton;
            if (grouped) {
                // Native groups are consecutive GROUP-style buttons, even when
                // a member is hidden. Any other button or separator ends a run.
                if (!inRadioGroup) {
                    ++radioGroup;
                    groupHasCheckedItem = false;
                }
                item.radioGroup = radioGroup;
                if (item.checked && groupHasCheckedItem)
                    return Reject(reason,
                        L"ToolbarWindow32 CHECKGROUP has multiple checked native members");
                groupHasCheckedItem = groupHasCheckedItem || item.checked;
            }
            inRadioGroup = grouped;
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
            ownLabel = drawsItsOwnLabel;
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
                std::array<HIMAGELIST, 4> currentLists{};
                for (size_t state = 0; state < imageQueries.size(); ++state)
                    currentLists[state] = reinterpret_cast<HIMAGELIST>(
                        SendMessageW(hwnd, imageQueries[state], listId, 0));
                const auto [observed, inserted] = observedLists.emplace(listId, currentLists);
                if (!inserted && observed->second != currentLists)
                    return Reject(reason, L"ToolbarWindow32 image lists changed during capture");
                int listWidth = 0;
                int listHeight = 0;
                int listCount = 0;
                const HIMAGELIST list = resolveImageList(listId, listWidth, listHeight, listCount);
                // A toolbar that owns no image list for that id draws no icon from one, no
                // matter what iBitmap says: a button added without an icon keeps the
                // default index of zero, and a control that custom-draws its faces keeps
                // an empty list.  Both are reproduced from the control's own paint.
                if (!list || imageIndex >= listCount) {
                    if (SendMessageW(hwnd, TB_GETHOTIMAGELIST, listId, 0) ||
                        SendMessageW(hwnd, TB_GETDISABLEDIMAGELIST, listId, 0) ||
                        SendMessageW(hwnd, TB_GETPRESSEDIMAGELIST, listId, 0))
                        return Reject(reason, L"ToolbarWindow32 state image has no normal image");
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
                    if (!CaptureToolbarImageVariant(hwnd, TB_GETHOTIMAGELIST, listId, imageIndex,
                            item.imageWidth, item.imageHeight, item.hotImage, reason) ||
                        !CaptureToolbarImageVariant(hwnd, TB_GETDISABLEDIMAGELIST, listId, imageIndex,
                            item.imageWidth, item.imageHeight, item.disabledImage, reason) ||
                        !CaptureToolbarImageVariant(hwnd, TB_GETPRESSEDIMAGELIST, listId, imageIndex,
                            item.imageWidth, item.imageHeight, item.pressedImage, reason)) return false;
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
        item.labelBeside = listLabels && ownLabel && !item.paintedFace && !item.imageData.empty() &&
            (!mixedButtons || (button.fsStyle & BTNS_SHOWTEXT) != 0);
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
    // Keep the original toolbar pixel budget across all states, rather than
    // multiplying it by four when a control supplies alternate image lists.
    size_t pixelBytes = 0;
    for (const auto& item : node.toolbarItems) {
        pixelBytes += item.imageData.size();
        for (const auto* variant : { &item.hotImage, &item.disabledImage, &item.pressedImage })
            if (*variant) pixelBytes += (*variant)->imageData.size();
    }
    if (pixelBytes > Ipc::kMaxToolbarItems * Ipc::kMaxImageBytes)
        return Reject(reason, L"ToolbarWindow32 state images exceed the aggregate pixel budget");
    // Accessible-name and paint callbacks can reenter application code. Publish
    // group membership and checked state only if the native buttons still agree.
    if (SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0) != rawCount)
        return Reject(reason, L"ToolbarWindow32 button count changed during capture");
    if (static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) != toolbarStyle)
        return Reject(reason, L"ToolbarWindow32 style changed during capture");
    for (const auto& [listId, images] : observedLists)
        for (size_t state = 0; state < imageQueries.size(); ++state)
            if (reinterpret_cast<HIMAGELIST>(SendMessageW(hwnd, imageQueries[state], listId, 0)) != images[state])
                return Reject(reason, L"ToolbarWindow32 image lists changed during capture");
    constexpr BYTE stableStates = TBSTATE_ENABLED | TBSTATE_HIDDEN | TBSTATE_CHECKED;
    for (int index = 0; index < rawCount; ++index) {
        TBBUTTON current{};
        const auto& captured = capturedButtons[static_cast<size_t>(index)];
        if (!SendMessageW(hwnd, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&current)) ||
            current.idCommand != captured.idCommand || current.fsStyle != captured.fsStyle ||
            current.iBitmap != captured.iBitmap ||
            (current.fsState & stableStates) != (captured.fsState & stableStates))
            return Reject(reason, L"ToolbarWindow32 button identity or state changed during capture");
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
    // A TreeView projects as its items, so the HWND's own text would only
    // duplicate them in the UIA name. An empty tree is still that projection:
    // a console clears its scope tree while a snap-in reloads, and refusing
    // the moment in between restores the whole window.
    node.text.clear();
    node.automationName.clear();
    node.items.clear();
    node.itemDepths.clear();
    node.itemExpanded.clear();
    node.itemHasChildren.clear();
    node.itemImages.clear();
    node.itemSelectedImages.clear();
    node.itemStateImages.clear();
    node.imageList.clear();
    node.stateImageList.clear();
    node.items.reserve(handles.size());
    node.itemDepths.reserve(handles.size());
    node.itemExpanded.reserve(handles.size());
    node.itemHasChildren.reserve(handles.size());
    node.itemImages.reserve(handles.size());
    node.itemSelectedImages.reserve(handles.size());
    node.itemStateImages.reserve(handles.size());
    node.editableLabels = (static_cast<DWORD>(node.style) & TVS_EDITLABELS) != 0;
    node.editingIndex = -1;
    node.selectedIndex = -1;
    node.multiSelect = false;
    if (handles.empty()) return true;
    const HWND editControl = reinterpret_cast<HWND>(
        SendMessageW(hwnd, TVM_GETEDITCONTROL, 0, 0));
    const HTREEITEM editing = editControl && IsWindow(editControl)
        ? TreeViewRelative(hwnd, TVGN_CARET, nullptr)
        : nullptr;
    const HTREEITEM selected = TreeViewRelative(hwnd, TVGN_CARET, nullptr);
    // State-image index 0 means the item draws no state image. The same bits
    // are TVIS_USERMASK when the control has no state image list, so they are
    // application-private storage and not a reason to refuse the window.
    const auto stateImages = reinterpret_cast<HIMAGELIST>(
        SendMessageW(hwnd, TVM_GETIMAGELIST, TVSIL_STATE, 0));
    const int stateCount = stateImages ? ImageList_GetImageCount(stateImages) : 0;
    if (stateImages && stateCount < 0)
        return Reject(reason, L"TreeView state image list metadata is unavailable");
    size_t totalText = 0;
    std::vector<int> overlays;
    overlays.reserve(handles.size());
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
        const size_t length = wcsnlen_s(buffer.data(), buffer.size());
        if (length == 0)
            return Reject(reason, L"TreeView requires nonempty textual labels");
        if (length >= kMaxTreeLabelChars)
            return Reject(reason, L"TreeView item label exceeds the text cap");
        if (totalText > kMaxStructuredTextChars - length)
            return Reject(reason, L"TreeView labels exceed the aggregate text cap");
        totalText += length;
        node.items.emplace_back(buffer.data(), length);
        node.itemDepths.push_back(handles[index].depth);
        node.itemExpanded.push_back((item.state & TVIS_EXPANDED) != 0);
        node.itemHasChildren.push_back(item.cChildren > 0 ||
            TreeViewRelative(hwnd, TVGN_CHILD, handles[index].item) != nullptr);
        // TreeView stores the no-image sentinel in an unsigned 16-bit slot on
        // current comctl32 builds. TVM_GETITEM may therefore return 65534 for
        // I_IMAGENONE; it is still an absent icon, not a native list reference.
        const auto imageIndex = [](int value) {
            return value == static_cast<unsigned short>(I_IMAGENONE) ? I_IMAGENONE : value;
        };
        node.itemImages.push_back(imageIndex(item.iImage));
        node.itemSelectedImages.push_back(imageIndex(item.iSelectedImage));
        // The overlay index lives in the same normal image list. It is captured
        // already composited, because that is the icon the control paints.
        overlays.push_back(static_cast<int>((item.state & TVIS_OVERLAYMASK) >> 8));
        const int stateImage = static_cast<int>((item.state & TVIS_STATEIMAGEMASK) >> 12);
        // I_IMAGENONE, not -1: -1 is I_IMAGECALLBACK, and the shared image
        // capture rejects that when a list exists. The capture remaps the
        // sentinel to the protocol's -1 afterwards.
        if (stateImage == 0 || !stateImages) {
            node.itemStateImages.push_back(I_IMAGENONE);
        } else if (stateImage >= stateCount) {
            return Reject(reason, L"TreeView state image index is outside the state image list");
        } else {
            node.itemStateImages.push_back(stateImage);
        }
        if (handles[index].item == selected) node.selectedIndex = static_cast<int>(index);
        if (editing && handles[index].item == editing)
            node.editingIndex = static_cast<int>(index);
    }
    if (!CaptureReferencedImageList(reinterpret_cast<HIMAGELIST>(
            SendMessageW(hwnd, TVM_GETIMAGELIST, TVSIL_NORMAL, 0)),
            node.itemImages, &node.itemSelectedImages, &overlays, node.imageList, reason))
        return false;
    if (!CaptureReferencedImageList(
            stateImages, node.itemStateImages, nullptr, nullptr, node.stateImageList, reason))
        return false;
    // A tree MMC hosts for a snap-in's result view (Device Manager's devices) opens an item on a
    // double-click (Enter); MMC's own scope tree only expands and selects.
    node.itemDefaultActivation = IsInsideMmcFrame(hwnd) && !IsMmcScopeTree(hwnd);
    return ImageListsFitBudget(node.imageList, node.stateImageList, reason);
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

// The range may run backwards (the up arrow then decreases the value), so the
// projection carries it ordered and flags the direction.  The position is the
// control's own: with UDS_SETBUDDYINT it is parsed from the buddy, and a buddy that
// does not parse still leaves the control's last position, which is what an arrow
// steps from.  The first acceleration step is the arrow's increment.
bool CaptureUpDownState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    // A Windows Forms spin pair has no range of its own: its position is the integer in the
    // buddy edit and both ends are open, so a step is always one press away.
    if (IsWindowsFormsWindow(hwnd)) {
        WindowsFormsSpin spin;
        if (!ReadWindowsFormsSpin(hwnd, spin, reason)) return false;
        node.minimum = INT_MIN;
        node.maximum = INT_MAX;
        node.position = spin.value;
        node.vertical = true;
        node.reversed = false;
        node.smallChange = 1;
        return true;
    }
    int low = 0;
    int high = 0;
    SendMessageW(hwnd, UDM_GETRANGE32, reinterpret_cast<WPARAM>(&low),
        reinterpret_cast<LPARAM>(&high));
    BOOL failed = FALSE;
    const int position = static_cast<int>(SendMessageW(hwnd, UDM_GETPOS32, 0,
        reinterpret_cast<LPARAM>(&failed)));
    node.reversed = low > high;
    node.minimum = (std::min)(low, high);
    node.maximum = (std::max)(low, high);
    node.position = position;
    node.vertical = (static_cast<DWORD>(node.style) & UDS_HORZ) == 0;
    UDACCEL acceleration{};
    node.smallChange = SendMessageW(hwnd, UDM_GETACCEL, 1, reinterpret_cast<LPARAM>(&acceleration)) > 0 &&
        acceleration.nInc > 0 ? static_cast<int>(acceleration.nInc) : 1;
    if (node.position < node.minimum || node.position > node.maximum)
        return Reject(reason, L"UpDown position is outside its native range");
    return true;
}

// A time picker's value is the second of the day; a date picker's is the day number inside
// the range the native control allows (DTM_GETRANGE) and a projected calendar can show.  The
// date part of a time picker, and the time part of a date picker, stay what they are.
bool CaptureDateTimePickerState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    SYSTEMTIME value{};
    if (SendMessageW(hwnd, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&value)) != GDT_VALID)
        return Reject(reason, L"DateTimePicker has no valid value");
    node.vertical = false;
    node.reversed = false;
    if (node.kind == ControlKind::TimePicker) {
        node.minimum = 0;
        node.maximum = kSecondsPerDay - 1;
        node.position = value.wHour * 3600 + value.wMinute * 60 + value.wSecond;
        node.smallChange = 60;
        node.largeChange = 3600;
        return true;
    }
    if (value.wYear < 1900 || value.wYear > 2200)
        return Reject(reason, L"DateTimePicker date is outside the years a projected calendar shows");
    const int day = DayNumberFromCivil(value.wYear, value.wMonth, value.wDay);
    SYSTEMTIME limits[2]{};
    const auto flags = static_cast<DWORD>(SendMessageW(
        hwnd, DTM_GETRANGE, 0, reinterpret_cast<LPARAM>(limits)));
    int low = kDatePickerMinDay;
    int high = kDatePickerMaxDay;
    if ((flags & GDTR_MIN) != 0)
        low = (std::max)(low, DayNumberFromCivil(limits[0].wYear, limits[0].wMonth, limits[0].wDay));
    if ((flags & GDTR_MAX) != 0)
        high = (std::min)(high, DayNumberFromCivil(limits[1].wYear, limits[1].wMonth, limits[1].wDay));
    if (low > high || day < low || day > high)
        return Reject(reason, L"DateTimePicker date is outside its native range");
    node.minimum = low;
    node.maximum = high;
    node.position = day;
    node.smallChange = 1;
    node.largeChange = 30;
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

bool CaptureStaticDecorationState(HWND, ControlNode& node, std::wstring& reason) {
    if (node.tabStop || node.tabIndex != -1 || (node.dialogCode & ~DLGC_STATIC) != 0)
        return Reject(reason, L"Static decoration reports interactive keyboard semantics");
    // These built-in draw styles never render their window text or publish a
    // label. Retain the native styles for their frame/fill and edge appearance.
    node.text.clear();
    node.automationName.clear();
    return true;
}

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
    at(ControlKind::StaticBitmap) = &CaptureStaticBitmapState;
    at(ControlKind::TabControl) = &CaptureTabControlState;
    at(ControlKind::Slider) = &CaptureTrackbarState;
    at(ControlKind::UpDown) = &CaptureUpDownState;
    at(ControlKind::DatePicker) = &CaptureDateTimePickerState;
    at(ControlKind::TimePicker) = &CaptureDateTimePickerState;
    at(ControlKind::StatusBar) = &CaptureStatusBarState;
    at(ControlKind::Toolbar) = &CaptureToolbarState;
    at(ControlKind::MdiChild) = &CaptureMdiChildState;
    at(ControlKind::PaneContainer) = &CapturePaneContainerState;
    at(ControlKind::AccessibleIsland) = &CaptureAccessibleIslandState;
    at(ControlKind::NativeDocument) = &CaptureNativeDocumentState;
    at(ControlKind::DiskView) = &CaptureDiskViewState;
    at(ControlKind::RichText) = &CaptureRichTextState;
    at(ControlKind::PerformanceGraph) = &CapturePerformanceGraphState;
    at(ControlKind::PerformanceLegend) = &CapturePerformanceLegendState;
    at(ControlKind::PerformanceValueBar) = &CapturePerformanceValueBarState;
    at(ControlKind::StaticDecoration) = &CaptureStaticDecorationState;
    // StaticText, Separator, GroupBox, DialogContainer, and MdiClient are fully
    // described by the common facets, so they need no reader of their own.
    return table;
}

constexpr auto kCaptureTable = MakeCaptureTable();

} // namespace

bool IsComboBoxExWindow(HWND hwnd) noexcept {
    wchar_t className[kMaxClassNameChars]{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, className), WC_COMBOBOXEXW);
}

bool CaptureDialogPaintedBands(HWND root, std::vector<ChromeRegionSnapshot>& bands, std::wstring& reason) {
    bands.clear();
    wchar_t className[kMaxClassNameChars]{};
    if (!FluentShell::EqualsIgnoreCase(ClassNameOf(root, className), L"#32770")) return true;
    RECT client{};
    if (!GetClientRect(root, &client) || client.right <= 0 || client.bottom <= 0) return true;
    std::vector<RECT> children;
    CollectVisibleChildRects(root, children);
    if (children.empty()) return true;
    LONG top = client.bottom;
    LONG bottom = client.top;
    for (const RECT& child : children) {
        top = std::min(top, child.top);
        bottom = std::max(bottom, child.bottom);
    }
    // Thinner than this is the dialog's margin, not a band; the controls' own edge is theirs.
    constexpr LONG kMinimumBand = 24;
    constexpr LONG kClearance = 2;
    std::vector<RECT> candidates;
    if (top - kClearance - client.top >= kMinimumBand)
        candidates.push_back({ client.left, client.top, client.right, top - kClearance });
    if (client.bottom - (bottom + kClearance) >= kMinimumBand)
        candidates.push_back({ client.left, bottom + kClearance, client.right, client.bottom });
    // A band past the chrome caps stays unprojected rather than keeping the dialog native.
    size_t bytes = 0;
    std::erase_if(candidates, [&](const RECT& band) {
        const LONG width = band.right - band.left;
        const LONG height = band.bottom - band.top;
        const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        if (width > static_cast<LONG>(Ipc::kMaxChromeRegionDimension) ||
            height > static_cast<LONG>(Ipc::kMaxChromeRegionDimension) ||
            size > Ipc::kMaxChromeRegionBytes - bytes) return true;
        bytes += size;
        return false;
    });
    if (candidates.empty()) return true;
    return CaptureContainerChrome(root, client, candidates, bands, reason);
}

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
    if (length <= 0) return {};
    const std::wstring_view observed(buffer, static_cast<size_t>(length));
    // Windows Forms gives standard native subclasses per-AppDomain names.
    // The native class token still has to pass its normal registry probe;
    // custom Window.* classes get no additional admission from this prefix.
    constexpr std::wstring_view prefix = L"WindowsForms10.";
    if (observed.starts_with(prefix)) {
        const auto end = observed.find(L".app.", prefix.size());
        if (end != std::wstring_view::npos && end > prefix.size() && end + 5 < observed.size())
            return observed.substr(prefix.size(), end - prefix.size());
    }
    return observed;
}

// A Windows Forms NumericUpDown keeps its number in an edit and steps it through a small leaf
// control of its own that paints two arrows (UpDownBase.UpDownButtons): an accessible spin box
// whose two push-button children publish no default action, and which steps when its upper or
// lower half is pressed. The projection carries the arrows as an UpDown whose position is the
// number in the buddy edit (the sibling that holds the control's value), and a step is the same
// press the pointer gives. A number the projection cannot read as an integer refuses the control.
namespace {

bool ParseSpinNumber(const std::wstring& text, int& value) noexcept {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && text[begin] == L' ') ++begin;
    while (end > begin && text[end - 1] == L' ') --end;
    bool negative = false;
    if (begin < end && (text[begin] == L'-' || text[begin] == L'+')) negative = text[begin++] == L'-';
    if (begin >= end) return false;
    long long number = 0;
    bool digitSeen = false;
    for (size_t index = begin; index < end; ++index) {
        const wchar_t character = text[index];
        if (character == L',' && digitSeen && index + 1 < end && text[index + 1] >= L'0' && text[index + 1] <= L'9')
            continue; // a thousands separator between digits
        if (character < L'0' || character > L'9') return false;
        digitSeen = true;
        number = number * 10 + (character - L'0');
        if (number > static_cast<long long>(INT_MAX) + 1) return false;
    }
    if (negative) number = -number;
    if (number < INT_MIN || number > INT_MAX) return false;
    value = static_cast<int>(number);
    return true;
}

} // namespace

bool ReadWindowsFormsSpin(HWND hwnd, WindowsFormsSpin& spin, std::wstring& reason) noexcept {
    try {
        spin = {};
        long role = 0;
        long state = 0;
        RECT client{};
        if (!IsWindowsFormsWindow(hwnd) || GetWindow(hwnd, GW_CHILD) || !GetClientRect(hwnd, &client) ||
            client.right <= 0 || client.right > 48 || client.bottom < 8 || client.bottom > 96 ||
            !ReadWindowAccessible(hwnd, role, state) || role != ROLE_SYSTEM_SPINBUTTON)
            return Reject(reason, L"window is not a Windows Forms spin button pair");
        Microsoft::WRL::ComPtr<IAccessible> accessible;
        long count = 0;
        if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_PPV_ARGS(accessible.GetAddressOf()))) ||
            !accessible || FAILED(accessible->get_accChildCount(&count)) || count != 2)
            return Reject(reason, L"spin buttons do not publish an up and a down button");
        VARIANT children[2]{};
        long obtained = 0;
        bool pushButtons = SUCCEEDED(AccessibleChildren(accessible.Get(), 0, 2, children, &obtained)) && obtained == 2;
        for (VARIANT& entry : children) {
            Microsoft::WRL::ComPtr<IAccessible> owner;
            VARIANT id{};
            id.vt = VT_I4;
            id.lVal = CHILDID_SELF;
            if (entry.vt == VT_DISPATCH && entry.pdispVal) {
                pushButtons = pushButtons && SUCCEEDED(entry.pdispVal->QueryInterface(IID_PPV_ARGS(owner.GetAddressOf())));
            } else if (entry.vt == VT_I4) {
                owner = accessible;
                id.lVal = entry.lVal;
            } else {
                pushButtons = false;
            }
            VARIANT childRole{};
            pushButtons = pushButtons && owner && SUCCEEDED(owner->get_accRole(id, &childRole)) &&
                childRole.vt == VT_I4 && childRole.lVal == ROLE_SYSTEM_PUSHBUTTON;
            VariantClear(&childRole);
            VariantClear(&entry);
        }
        if (!pushButtons) return Reject(reason, L"spin buttons do not publish an up and a down button");
        // The buddy: the one visible edit beside the arrows, inside the NumericUpDown's own window.
        const HWND parent = GetParent(hwnd);
        HWND edit = nullptr;
        for (HWND sibling = parent ? GetWindow(parent, GW_CHILD) : nullptr; sibling;
             sibling = GetWindow(sibling, GW_HWNDNEXT)) {
            wchar_t className[kMaxClassNameChars]{};
            if (sibling == hwnd || !IsWindowVisible(sibling) ||
                !FluentShell::EqualsIgnoreCase(ClassNameOf(sibling, className), L"EDIT")) continue;
            if (edit) return Reject(reason, L"spin buttons have more than one candidate edit");
            edit = sibling;
        }
        if (!edit) return Reject(reason, L"spin buttons have no edit beside them");
        wchar_t text[48]{};
        const int length = GetWindowTextW(edit, text, static_cast<int>(std::size(text)));
        int value = 0;
        if (length <= 0 || length >= static_cast<int>(std::size(text)) ||
            !ParseSpinNumber(std::wstring(text, static_cast<size_t>(length)), value))
            return Reject(reason, L"the number beside the spin buttons is not an integer");
        spin.edit = edit;
        spin.value = value;
        return true;
    } catch (...) {
        reason = L"exception while reading spin buttons";
        return false;
    }
}

bool StepWindowsFormsSpin(HWND buttons, bool up) noexcept {
    try {
        if (!buttons || GetWindowThreadProcessId(buttons, nullptr) != GetCurrentThreadId()) return false;
        RECT client{};
        WindowsFormsSpin spin;
        std::wstring ignored;
        if (!GetClientRect(buttons, &client) || !ReadWindowsFormsSpin(buttons, spin, ignored)) return false;
        // Exactly what a click delivers: the control steps on the press (its upper half is Up) and
        // starts its auto-repeat, which the release ends.
        const LPARAM point = MAKELPARAM(static_cast<SHORT>(client.right / 2),
            static_cast<SHORT>(up ? client.bottom / 4 : client.bottom * 3 / 4));
        SendMessageW(buttons, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(buttons, WM_LBUTTONUP, 0, point);
        return true;
    } catch (...) {
        return false;
    }
}

bool ProbeWindowsFormsSpin(HWND hwnd, ControlKind& kind, std::wstring& reason) noexcept {
    WindowsFormsSpin spin;
    if (!ReadWindowsFormsSpin(hwnd, spin, reason)) return false;
    kind = ControlKind::UpDown;
    return true;
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
    } else {
        // mmcndmgr.dll registers several ATL classes; only the message view's accessible
        // shape sets it apart from the hosts that frame child windows, which remain
        // ordinary geometric containers.
        std::wstring messageViewReason;
        if (IsMmcMessageView(hwnd) &&
            ProbeAccessibleIsland(hwnd, style, kind, messageViewReason)) return true;
        // The arrows of a Windows Forms NumericUpDown are a leaf the geometric rules cannot see.
        std::wstring spinReason;
        if (ProbeWindowsFormsSpin(hwnd, kind, spinReason)) return true;
        if (ProbePaneContainer(hwnd, style, kind, containerReason)) return true;
        if (IsWindowsFormsInertSurface(hwnd, style)) {
            kind = ControlKind::StaticBitmap;
            return true;
        }
        if (!messageViewReason.empty())
            containerReason += L"; as an MMC message view: " + messageViewReason;
    }
    reason = L"unsupported visible control class: ";
    reason.append(className);
    reason.append(L" (");
    reason.append(containerReason);
    reason.append(L")");
    wchar_t diagnostics[2]{};
    if (GetEnvironmentVariableW(L"FLUENTSHELL_DIAG_ACCESSIBILITY", diagnostics, 2) == 1 &&
        diagnostics[0] == L'1') {
        reason.append(L" MSAA:");
        reason.append(DescribeAccessibleIsland(hwnd));
        if (FluentShell::EqualsIgnoreCase(className, L"Internet Explorer_Server")) {
            reason.append(L" HTML:");
            reason.append(DescribeMmcHtmlDocument(hwnd));
        }
    }
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
        if (GetWindowThreadProcessId(container, nullptr) != GetCurrentThreadId()) {
            auto moved = std::make_shared<bool>(false);
            std::wstring reason;
            return DispatchToWindowThread(container, [container, index, position, moved] {
                *moved = SetPaneSplit(container, index, position);
            }, reason) && *moved;
        }
        if (GuiThreadOperationCancelled()) return false;
        RECT client{};
        if (!GetClientRect(container, &client)) return false;
        std::wstring ignored;
        PaneSurface surface;
        if (!CollectPaneSurface(container, client, surface, ignored)) return false;
        const std::vector<ResolvedSplit> splits = ResolveSplits(container, client, surface);
        if (static_cast<size_t>(index) >= splits.size()) return false;
        const ResolvedSplit& resolved = splits[static_cast<size_t>(index)];
        if (position < resolved.minimum || position > resolved.maximum) return false;
        RECT before = resolved.neighbours.beforeRect;
        RECT after = resolved.neighbours.afterRect;
        if (resolved.gap.vertical) {
            before.right = position;
            after.left = position + resolved.thickness;
        } else {
            before.bottom = position;
            after.top = position + resolved.thickness;
        }
        const auto move = [container](HWND child, RECT target) {
            auto moved = std::make_shared<bool>(false);
            std::wstring reason;
            return DispatchToWindowThread(child, [container, child, target, moved] {
                if (GuiThreadOperationCancelled() || GetParent(child) != container) return;
                *moved = SetWindowPos(child, nullptr, target.left, target.top,
                    target.right - target.left, target.bottom - target.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
            }, reason) && *moved;
        };
        // Stop on the first refusal. The caller restores the complete native
        // surface on failure; a cancelled operation must not move its peer later.
        return move(resolved.neighbours.before, before) && !GuiThreadOperationCancelled() &&
            move(resolved.neighbours.after, after);
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
bool ApplyToolbarCheckState(
    HWND toolbar, const ControlNode& node, uint32_t commandId, std::wstring& reason) {
    if (!toolbar || !IsWindow(toolbar) || node.kind != ControlKind::Toolbar ||
        !node.enabled || commandId == 0 || commandId > 0xffff ||
        node.toolbarItems.empty() || node.toolbarItems.size() > Ipc::kMaxToolbarItems)
        return Reject(reason, L"Toolbar command has no bounded native button collection");
    wchar_t className[kMaxClassNameChars]{};
    if (ClassNameOf(toolbar, className) != TOOLBARCLASSNAMEW)
        return Reject(reason, L"Toolbar command target is no longer a native toolbar");
    const auto count = static_cast<int>(node.toolbarItems.size());
    if (SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0) != count)
        return Reject(reason, L"Toolbar command button count changed");
    const auto targetIndex = static_cast<int>(
        SendMessageW(toolbar, TB_COMMANDTOINDEX, commandId, 0));
    if (targetIndex < 0 || targetIndex >= count)
        return Reject(reason, L"Toolbar command ID no longer resolves to a native button");

    std::vector<TBBUTTON> buttons(static_cast<size_t>(count));
    std::vector<bool> requestedChecks(static_cast<size_t>(count), false);
    int radioGroup = 0;
    bool inRadioGroup = false;
    bool groupHasCheckedItem = false;
    constexpr BYTE stableStates = TBSTATE_ENABLED | TBSTATE_HIDDEN | TBSTATE_CHECKED;
    for (int index = 0; index < count; ++index) {
        auto& button = buttons[static_cast<size_t>(index)];
        if (!SendMessageW(toolbar, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&button)))
            return Reject(reason, L"Toolbar command button read failed");
        ToolbarItemSnapshot live;
        if (!DescribeToolbarButtonStyle(button.fsStyle, live, reason)) return false;
        const bool grouped = live.kind == ToolbarItemKind::RadioButton;
        if (grouped) {
            if (!inRadioGroup) {
                ++radioGroup;
                groupHasCheckedItem = false;
            }
            live.radioGroup = radioGroup;
        }
        inRadioGroup = grouped;
        live.enabled = (button.fsState & TBSTATE_ENABLED) != 0;
        live.hidden = (button.fsState & TBSTATE_HIDDEN) != 0;
        live.checked = (button.fsState & TBSTATE_CHECKED) != 0;
        if (grouped && live.checked && groupHasCheckedItem)
            return Reject(reason, L"Toolbar command CHECKGROUP has multiple checked members");
        if (grouped) groupHasCheckedItem = groupHasCheckedItem || live.checked;
        const auto& expected = node.toolbarItems[static_cast<size_t>(index)];
        if (live.kind != expected.kind || live.radioGroup != expected.radioGroup ||
            live.dropDown != expected.dropDown || live.wholeDropDown != expected.wholeDropDown ||
            live.enabled != expected.enabled || live.hidden != expected.hidden ||
            live.checked != expected.checked ||
            (live.kind != ToolbarItemKind::Separator &&
                (button.idCommand <= 0 || button.idCommand > 0xffff ||
                 static_cast<uint32_t>(button.idCommand) != expected.commandId ||
                 SendMessageW(toolbar, TB_COMMANDTOINDEX, button.idCommand, 0) != index)))
            return Reject(reason, L"Toolbar command identity, group, or state changed");
        requestedChecks[static_cast<size_t>(index)] = live.checked;
    }
    const auto& target = node.toolbarItems[static_cast<size_t>(targetIndex)];
    if (target.commandId != commandId || target.kind == ToolbarItemKind::Separator ||
        !target.enabled || target.hidden)
        return Reject(reason, L"Toolbar command names an unavailable button");
    if (target.kind == ToolbarItemKind::RadioButton) {
        for (int index = 0; index < count; ++index) {
            if (node.toolbarItems[static_cast<size_t>(index)].radioGroup == target.radioGroup)
                requestedChecks[static_cast<size_t>(index)] = index == targetIndex;
        }
    } else if (target.kind == ToolbarItemKind::ToggleButton) {
        requestedChecks[static_cast<size_t>(targetIndex)] = !target.checked;
    } else {
        return true;
    }

    // TB_CHECKBUTTON documents one button's state, not automatic peer clearing.
    // Clear only checked peers from this validated native group, then select the
    // target. No WM_COMMAND is delivered until every requested bit is verified.
    for (int index = 0; index < count; ++index) {
        if (index == targetIndex || requestedChecks[static_cast<size_t>(index)] ||
            (buttons[static_cast<size_t>(index)].fsState & TBSTATE_CHECKED) == 0) continue;
        if (!SendMessageW(toolbar, TB_CHECKBUTTON,
                node.toolbarItems[static_cast<size_t>(index)].commandId, MAKELONG(FALSE, 0)))
            return Reject(reason, L"Toolbar native radio peer could not be unchecked");
    }
    if (requestedChecks[static_cast<size_t>(targetIndex)] != target.checked &&
        !SendMessageW(toolbar, TB_CHECKBUTTON, commandId,
            MAKELONG(requestedChecks[static_cast<size_t>(targetIndex)] ? TRUE : FALSE, 0)))
        return Reject(reason, L"Toolbar native button check state could not be applied");
    if (SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0) != count)
        return Reject(reason, L"Toolbar button count changed while applying check state");
    for (int index = 0; index < count; ++index) {
        TBBUTTON current{};
        const auto& original = buttons[static_cast<size_t>(index)];
        const BYTE expectedState = static_cast<BYTE>((original.fsState & ~TBSTATE_CHECKED) |
            (requestedChecks[static_cast<size_t>(index)] ? TBSTATE_CHECKED : 0));
        if (!SendMessageW(toolbar, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&current)) ||
            current.idCommand != original.idCommand || current.fsStyle != original.fsStyle ||
            (current.fsState & stableStates) != (expectedState & stableStates))
            return Reject(reason, L"Toolbar native check-state readback disagrees with its command");
    }
    return true;
}

bool ReadListViewItemText(HWND listView, int row, int column,
    std::wstring& text, std::wstring& reason) {
    if (!listView || row < 0 || column < 0 ||
        GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId())
        return Reject(reason, L"ListView text must be read on its owning GUI thread");
    return ReadListViewCell(listView, row, column, text, reason);
}

int ResolveListViewItemByNativeId(HWND listView, uint32_t nativeId) noexcept {
    if (!listView || nativeId == std::numeric_limits<uint32_t>::max()) return -1;
    const auto index = static_cast<int>(SendMessageW(listView, LVM_MAPIDTOINDEX, nativeId, 0));
    if (index < 0 || static_cast<size_t>(index) >= Ipc::kMaxListItems) return -1;
    return static_cast<uint32_t>(SendMessageW(listView, LVM_MAPINDEXTOID,
        static_cast<WPARAM>(index), 0)) == nativeId ? index : -1;
}

bool SetListViewColumnOrder(HWND listView, const std::vector<int>& order) noexcept {
    if (!listView || order.empty()) return false;
    try {
        std::wstring mode;
        std::wstring ignored;
        if (!ReadListViewMode(listView, static_cast<DWORD>(GetWindowLongPtrW(listView, GWL_STYLE)),
                mode, ignored) || mode != L"report") return false;
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

bool SetListViewFocusedIndex(HWND listView, int index) noexcept {
    if (!listView || index < -1 ||
        GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId()) return false;
    try {
        const auto count = SendMessageW(listView, LVM_GETITEMCOUNT, 0, 0);
        if (count < 0 || index >= count) return false;
        const auto selectedBefore = static_cast<int>(SendMessageW(
            listView, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        LVITEMW state{};
        state.stateMask = LVIS_FOCUSED;
        state.state = 0;
        SendMessageW(listView, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(&state));
        if (index >= 0) {
            state.state = LVIS_FOCUSED;
            SendMessageW(listView, LVM_SETITEMSTATE, static_cast<WPARAM>(index),
                reinterpret_cast<LPARAM>(&state));
            SendMessageW(listView, LVM_ENSUREVISIBLE, static_cast<WPARAM>(index), FALSE);
        }
        const auto focused = static_cast<int>(SendMessageW(
            listView, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
        const auto selectedAfter = static_cast<int>(SendMessageW(
            listView, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        return focused == index && selectedAfter == selectedBefore;
    } catch (...) {
        return false;
    }
}

bool ScrollPaneContainer(HWND container, bool vertical, bool byLines, int amount) noexcept {
    if (!container || GetWindowThreadProcessId(container, nullptr) != GetCurrentThreadId()) return false;
    try {
        const UINT message = vertical ? WM_VSCROLL : WM_HSCROLL;
        const int bar = vertical ? SB_VERT : SB_HORZ;
        const auto read = [&](SCROLLINFO& info) {
            info = { sizeof(info), SIF_POS | SIF_PAGE | SIF_RANGE };
            return GetScrollInfo(container, bar, &info) != FALSE;
        };
        SCROLLINFO info{};
        if (!read(info)) return false;
        const int start = info.nPos;
        // Only the application's own line/page handling moves its children; a
        // synthetic SB_THUMBPOSITION is ignored by frameworks that read nTrackPos.
        if (byLines) {
            for (int step = 0; step < std::abs(amount) && !GuiThreadOperationCancelled(); ++step)
                SendMessageW(container, message, MAKEWPARAM(amount > 0 ? SB_LINEDOWN : SB_LINEUP, 0), 0);
        } else {
            int current = start;
            for (int step = 0; step < 512 && current != amount && !GuiThreadOperationCancelled(); ++step) {
                const int distance = amount - current;
                const bool page = info.nPage > 0 && static_cast<UINT>(std::abs(distance)) >= info.nPage;
                const int code = distance > 0 ? (page ? SB_PAGEDOWN : SB_LINEDOWN)
                    : (page ? SB_PAGEUP : SB_LINEUP);
                SendMessageW(container, message, MAKEWPARAM(code, 0), 0);
                if (!read(info) || info.nPos == current) break;
                // A line step may not land exactly; stop where it crosses the target.
                const bool crossed = (distance > 0) != (amount - info.nPos > 0) && info.nPos != amount;
                current = info.nPos;
                if (crossed) break;
            }
        }
        SendMessageW(container, message, MAKEWPARAM(SB_ENDSCROLL, 0), 0);
        return read(info) && info.nPos != start;
    } catch (...) {
        return false;
    }
}

int ReadButtonCheckState(HWND button) noexcept {
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(button, GWL_STYLE));
    if ((style & BS_TYPEMASK) != BS_OWNERDRAW)
        return static_cast<int>(SendMessageW(button, BM_GETCHECK, 0, 0));
    long role = 0;
    long state = 0;
    if (!IsWindowsFormsWindow(button) || !ReadWindowAccessible(button, role, state) ||
        (role != ROLE_SYSTEM_CHECKBUTTON && role != ROLE_SYSTEM_RADIOBUTTON)) return -1;
    if ((state & STATE_SYSTEM_MIXED) != 0) return BST_INDETERMINATE;
    return (state & STATE_SYSTEM_CHECKED) != 0 ? BST_CHECKED : BST_UNCHECKED;
}

bool RequestItemContextMenu(HWND control, ControlKind kind, int index,
    const std::wstring& expectedText, std::wstring& reason, ContextMenuTrigger trigger) noexcept {
    try {
        if (!control || index < 0 || GetWindowThreadProcessId(control, nullptr) != GetCurrentThreadId())
            return Reject(reason, L"context-menu owner is not on this GUI thread");
        if (kind == ControlKind::TreeView) {
            const HTREEITEM item = ResolveTreeViewItem(control, index);
            if (!item) return Reject(reason, L"TreeView item no longer exists");
            std::vector<wchar_t> buffer(kMaxTreeLabelChars + 1, L'\0');
            TVITEMW read{};
            read.mask = TVIF_HANDLE | TVIF_TEXT;
            read.hItem = item;
            read.pszText = buffer.data();
            read.cchTextMax = static_cast<int>(buffer.size());
            if (!SendMessageW(control, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&read)) ||
                expectedText != std::wstring(buffer.data(), wcsnlen_s(buffer.data(), buffer.size())))
                return Reject(reason, L"TreeView item changed before its context menu");
            // TVM_SELECTITEM raises the TVN_SELCHANGING/TVN_SELCHANGED pair a click
            // would, so the application may veto the selection like any other.
            if (TreeViewRelative(control, TVGN_CARET, nullptr) != item)
                SendMessageW(control, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item));
            if (TreeViewRelative(control, TVGN_CARET, nullptr) != item)
                return Reject(reason, L"the application refused the TreeView selection");
            SendMessageW(control, TVM_ENSUREVISIBLE, 0, reinterpret_cast<LPARAM>(item));
        } else if (kind == ControlKind::ListView) {
            std::wstring text;
            if (!ReadListViewItemText(control, index, 0, text, reason)) return false;
            if (text != expectedText) return Reject(reason, L"ListView item changed before its context menu");
            // A right-click on an unselected row selects only that row; on a selected
            // row it keeps the existing (possibly multiple) selection.
            const bool selected = (SendMessageW(control, LVM_GETITEMSTATE, static_cast<WPARAM>(index),
                LVIS_SELECTED) & LVIS_SELECTED) != 0;
            LVITEMW state{};
            if (!selected) {
                state.stateMask = LVIS_SELECTED;
                state.state = 0;
                SendMessageW(control, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&state));
            }
            state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            state.state = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(control, LVM_SETITEMSTATE, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&state));
            if ((SendMessageW(control, LVM_GETITEMSTATE, static_cast<WPARAM>(index), LVIS_SELECTED) &
                    LVIS_SELECTED) == 0)
                return Reject(reason, L"the application refused the ListView selection");
            SendMessageW(control, LVM_ENSUREVISIBLE, static_cast<WPARAM>(index), FALSE);
        } else {
            return Reject(reason, L"context menus are offered only for TreeView and ListView items");
        }
        if (GuiThreadOperationCancelled()) return Reject(reason, L"context-menu request was cancelled");
        // A right-click focuses the control it lands on, and MMC answers the
        // keyboard form for whichever pane holds focus (Task Scheduler's focus
        // otherwise rests in its Windows Forms view).
        if (GetFocus() != control) SetFocus(control);
        if (trigger == ContextMenuTrigger::DoubleClick) {
            // What the pointer delivers to the item: the double-click at its label, then the
            // release that completes a Windows Forms control's DoubleClick/MouseDoubleClick.
            RECT label{};
            label.left = LVIR_LABEL;
            if (kind != ControlKind::ListView ||
                !SendMessageW(control, LVM_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&label)))
                return Reject(reason, L"the item has no on-screen label to double-click");
            const LPARAM at = MAKELPARAM(static_cast<SHORT>((label.left + label.right) / 2),
                static_cast<SHORT>((label.top + label.bottom) / 2));
            SendMessageW(control, WM_LBUTTONDBLCLK, MK_LBUTTON, at);
            SendMessageW(control, WM_LBUTTONUP, 0, at);
            return true;
        }
        if (trigger == ContextMenuTrigger::MenuKey || trigger == ContextMenuTrigger::EnterKey) {
            // Exactly what the key delivers to the focused control: the control's own key
            // handling (or the default handling of the key-up) opens the menu, or activates the item.
            const bool enter = trigger == ContextMenuTrigger::EnterKey;
            const WPARAM key = enter ? VK_RETURN : VK_APPS;
            const LPARAM keyDown = 1 | (static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC)) << 16) |
                (enter ? 0 : (1 << 24));
            SendMessageW(control, WM_KEYDOWN, key, keyDown);
            SendMessageW(control, WM_KEYUP, key, keyDown | (1 << 30) | (static_cast<LPARAM>(1) << 31));
            return true;
        }
        // The keyboard form names the selection. MMC hit-tests the cursor rather
        // than this message's point, so a coordinate cannot name the projected item.
        SendMessageW(control, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(control), MAKELPARAM(-1, -1));
        return true;
    } catch (...) {
        reason = L"context-menu request failed";
        return false;
    }
}

bool SetListBoxSelection(HWND listBox, const std::vector<int>& indices) noexcept {
    try {
        if (!listBox || GetWindowThreadProcessId(listBox, nullptr) != GetCurrentThreadId() ||
            (static_cast<DWORD>(GetWindowLongPtrW(listBox, GWL_STYLE)) & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) == 0)
            return false;
        const LRESULT items = SendMessageW(listBox, LB_GETCOUNT, 0, 0);
        if (items == LB_ERR) return false;
        for (size_t at = 0; at < indices.size(); ++at)
            if (indices[at] < 0 || indices[at] >= items || (at > 0 && indices[at] <= indices[at - 1])) return false;
        SendMessageW(listBox, LB_SETSEL, FALSE, -1);
        for (const int index : indices) {
            if (GuiThreadOperationCancelled()) return false;
            SendMessageW(listBox, LB_SETSEL, TRUE, index);
        }
        const LRESULT count = SendMessageW(listBox, LB_GETSELCOUNT, 0, 0);
        if (count == LB_ERR || static_cast<size_t>(count) != indices.size()) return false;
        std::vector<int> actual(static_cast<size_t>(count));
        if (count > 0 && SendMessageW(listBox, LB_GETSELITEMS, static_cast<WPARAM>(count),
                reinterpret_cast<LPARAM>(actual.data())) != count) return false;
        std::sort(actual.begin(), actual.end());
        return actual == indices;
    } catch (...) {
        return false;
    }
}

bool RequestToolbarDropDown(HWND toolbar, int index, uint32_t commandId, std::wstring& reason) noexcept {
    try {
        if (!toolbar || index < 0 || GetWindowThreadProcessId(toolbar, nullptr) != GetCurrentThreadId())
            return Reject(reason, L"toolbar is not on this GUI thread");
        const HWND owner = GetParent(toolbar);
        if (!owner) return Reject(reason, L"toolbar has no parent to notify");
        // The button has to be what the snapshot said: an enabled drop-down with the same command.
        TBBUTTON button{};
        if (!SendMessageW(toolbar, TB_GETBUTTON, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&button)) ||
            static_cast<uint32_t>(button.idCommand) != commandId || (button.fsStyle & BTNS_DROPDOWN) == 0 ||
            (button.fsState & TBSTATE_ENABLED) == 0 || (button.fsState & TBSTATE_HIDDEN) != 0)
            return Reject(reason, L"the toolbar button is no longer an enabled drop-down");
        RECT rect{};
        if (!SendMessageW(toolbar, TB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&rect)))
            return Reject(reason, L"the toolbar button has no rectangle");
        if (GuiThreadOperationCancelled()) return Reject(reason, L"drop-down request was cancelled");
        // What the toolbar's own click on the arrow sends: its parent is told which button wants a
        // menu and where it is, and answers by tracking one under it.
        NMTOOLBARW notification{};
        notification.hdr.hwndFrom = toolbar;
        notification.hdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(toolbar));
        notification.hdr.code = TBN_DROPDOWN;
        notification.iItem = static_cast<int>(commandId);
        notification.rcButton = rect;
        SendMessageW(owner, WM_NOTIFY, notification.hdr.idFrom, reinterpret_cast<LPARAM>(&notification));
        return true;
    } catch (...) {
        reason = L"drop-down request failed";
        return false;
    }
}

namespace {
// The visible captioned top-level windows of this process: what opening an item can put up
// (menus, tooltips and the transient helper windows MMC creates have no caption).
std::vector<HWND> ProcessDialogWindows() {
    std::vector<HWND> windows;
    EnumWindows([](HWND window, LPARAM raw) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window) &&
            (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
            reinterpret_cast<std::vector<HWND>*>(raw)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

bool NewDialogWindowAppeared(const std::vector<HWND>& before) {
    for (const HWND window : ProcessDialogWindows())
        if (std::find(before.begin(), before.end(), window) == before.end()) return true;
    return false;
}

// Waits, dispatching messages the way the application's own modal loop would, for an
// activation that opens its window a moment after the input returned.
bool AwaitNewDialogWindow(const std::vector<HWND>& before, ULONGLONG milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        if (NewDialogWindowAppeared(before) || GuiThreadOperationCancelled()) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>((std::min<ULONGLONG>)(25, deadline - now)),
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

// An activation that opens its item modally keeps its input inside the application's loop for as long
// as the dialog lives; one that returns sooner either opened nothing or opened a window without one.
constexpr ULONGLONG kModalActivationMilliseconds = 300;
constexpr ULONGLONG kActivationWindowMilliseconds = 250;
}

bool RequestItemKeyActivation(HWND control, ControlKind kind, int index,
    const std::wstring& expectedText, std::wstring& reason) noexcept {
    try {
        if (kind != ControlKind::ListView || !IsWindowsFormsWindow(control))
            return RequestItemContextMenu(control, kind, index, expectedText, reason, ContextMenuTrigger::EnterKey);
        // A Windows Forms list opens its item from whichever event the application handles. Enter raises
        // ItemActivate and KeyDown; a double-click raises DoubleClick and MouseDoubleClick, and an
        // application that reads the pointer instead of the event finds nothing under a synthesized one
        // (Event Viewer opens on Enter and ignores the click, Task Scheduler the reverse). Which one the
        // application listens to cannot be read from outside, so Enter (which names the selected item
        // exactly) goes first and the double-click follows only when Enter visibly did nothing.
        const std::vector<HWND> before = ProcessDialogWindows();
        const ULONGLONG started = GetTickCount64();
        if (!RequestItemContextMenu(control, kind, index, expectedText, reason, ContextMenuTrigger::EnterKey))
            return false;
        if (GetTickCount64() - started >= kModalActivationMilliseconds ||
            AwaitNewDialogWindow(before, kActivationWindowMilliseconds)) return true;
        return RequestItemContextMenu(control, kind, index, expectedText, reason, ContextMenuTrigger::DoubleClick);
    } catch (...) {
        reason = L"item activation failed";
        return false;
    }
}

bool SetListViewGroupExpanded(HWND listView, int groupId, bool expanded) noexcept {
    if (!listView || GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId() ||
        !SendMessageW(listView, LVM_ISGROUPVIEWENABLED, 0, 0)) return false;
    LVGROUP group{ sizeof(group) };
    group.mask = LVGF_STATE;
    group.stateMask = LVGS_COLLAPSIBLE | LVGS_HIDDEN;
    if (SendMessageW(listView, LVM_GETGROUPINFO, groupId, reinterpret_cast<LPARAM>(&group)) == -1 ||
        (group.state & LVGS_COLLAPSIBLE) == 0 || (group.state & LVGS_HIDDEN) != 0) return false;
    group.stateMask = LVGS_COLLAPSED;
    group.state = expanded ? 0 : LVGS_COLLAPSED;
    if (SendMessageW(listView, LVM_SETGROUPINFO, groupId, reinterpret_cast<LPARAM>(&group)) == -1) return false;
    group.state = 0;
    return SendMessageW(listView, LVM_GETGROUPINFO, groupId, reinterpret_cast<LPARAM>(&group)) != -1 &&
        ((group.state & LVGS_COLLAPSED) == 0) == expanded;
}

bool ScrollListViewBy(HWND listView, int dx, int dy) noexcept {
    if (!listView || GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId()) return false;
    if (dx < -Ipc::kMaxCoordinate || dx > Ipc::kMaxCoordinate ||
        dy < -Ipc::kMaxCoordinate || dy > Ipc::kMaxCoordinate) return false;
    try {
        std::wstring mode;
        std::wstring ignored;
        if (!ReadListViewMode(listView, static_cast<DWORD>(GetWindowLongPtrW(listView, GWL_STYLE)),
                mode, ignored) || mode.empty() || (mode == L"report" &&
                !SendMessageW(listView, LVM_ISGROUPVIEWENABLED, 0, 0))) return false;
        if (dx == 0 && dy == 0) return true;
        return SendMessageW(listView, LVM_SCROLL, static_cast<WPARAM>(dx),
            static_cast<LPARAM>(dy)) != FALSE;
    } catch (...) {
        return false;
    }
}

bool ScrollListViewToTop(HWND listView, int index) noexcept {
    if (!listView || GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId()) return false;
    try {
        const int count = static_cast<int>(SendMessageW(listView, LVM_GETITEMCOUNT, 0, 0));
        if (index < 0 || index >= count) return false;
        const int top = static_cast<int>(SendMessageW(listView, LVM_GETTOPINDEX, 0, 0));
        if (top == index) return true;
        RECT topRow{ LVIR_BOUNDS };
        RECT targetRow{ LVIR_BOUNDS };
        if (top < 0 || top >= count ||
            !SendMessageW(listView, LVM_GETITEMRECT, static_cast<WPARAM>(top), reinterpret_cast<LPARAM>(&topRow)) ||
            !SendMessageW(listView, LVM_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&targetRow)))
            return false;
        // A report list scrolls by whole rows in its own client pixels; the last page
        // stops where the list's own scroll range ends.
        return SendMessageW(listView, LVM_SCROLL, 0, static_cast<LPARAM>(targetRow.top - topRow.top)) != FALSE;
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

// MMC raises a transparent, unpainted child over its frame's whole client area while
// a snap-in holds the console busy -- a prompt shown from the snap-in's own thread, or
// a long operation -- so the frame takes no mouse input and shows the wait cursor.
// That is input semantics rather than content: the window is never a node, and the
// session declines the frame's actions until MMC removes it.
bool IsMmcBusyShield(HWND hwnd) noexcept {
    wchar_t name[kMaxClassNameChars]{};
    if (!hwnd || GetClassNameW(hwnd, name, static_cast<int>(kMaxClassNameChars)) <= 0 ||
        !FluentShell::EqualsIgnoreCase(name, L"MMCWaitCursorWindow")) return false;
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    return (style & WS_CHILD) != 0 && (exStyle & WS_EX_TRANSPARENT) != 0 &&
        !GetWindow(hwnd, GW_CHILD) && WindowClassRegisteredBySystemModule(hwnd, L"mmcndmgr.dll");
}

// MMC can lay one window over another and leave only a thin edge of the lower one on
// screen: Print Management places its result ListView over the HTML host that
// positions it (the mmcview behavior of its frameset view), covering all but two
// pixels of it.  What such a window and everything inside it show is that edge and
// nothing else, so they are not captured; the projection's surface shows where the
// native background edge was.  An edge wide enough to hold a control keeps the
// ordinary contract.
constexpr int kMaxObscuredEdge = 8;

bool ObscuredBySiblingAbove(HWND window) noexcept {
    if (!window || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CLIPSIBLINGS) == 0) return false;
    RECT own{};
    if (!GetWindowRect(window, &own)) return false;
    RECT core = own;
    InflateRect(&core, -kMaxObscuredEdge, -kMaxObscuredEdge);
    if (IsRectEmpty(&core)) return false;
    for (HWND above = GetWindow(window, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        RECT covering{};
        RECT joined{};
        if (!IsWindowVisible(above) || !GetWindowRect(above, &covering)) continue;
        if ((GetWindowLongPtrW(above, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) != 0) continue;
        if (UnionRect(&joined, &covering, &core) && EqualRect(&joined, &covering)) return true;
    }
    return false;
}

bool InsideObscuredHost(HWND root, HWND window) noexcept {
    for (HWND current = window; current && current != root; current = GetParent(current)) {
        if (ObscuredBySiblingAbove(current)) return true;
    }
    return false;
}

bool IsWindowsFormsControl(HWND hwnd) noexcept {
    return IsWindowsFormsWindow(hwnd);
}

bool IsWindowsFormsLinkLabelWindow(HWND hwnd) noexcept {
    return IsWindowsFormsLinkLabel(hwnd);
}

bool InvokeWindowsFormsLink(HWND linkLabel, const std::wstring& expectedName) noexcept {
    try {
        std::vector<AccessibleLink> links;
        if (!ReadWindowsFormsLinks(linkLabel, links) || links.size() != 1 ||
            links.front().name != expectedName || !links.front().enabled) return false;
        VARIANT id{};
        id.vt = VT_I4;
        id.lVal = links.front().child;
        return SUCCEEDED(links.front().object->accDoDefaultAction(id));
    } catch (...) {
        return false;
    }
}

bool HasVisibleMmcBusyShield(HWND root) noexcept {
    for (HWND child = root ? GetWindow(root, GW_CHILD) : nullptr; child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsWindowVisible(child) && IsMmcBusyShield(child)) return true;
    }
    return false;
}

bool IsCompositeImplementationChild(HWND hwnd) noexcept {
    if (IsMmcBusyShield(hwnd)) return true;
    try {
        struct Child final {
            HWND parent = nullptr;
            std::wstring className;
            bool empty = false;
            bool captionCandidate = false;
            bool sizeGrip = false;
        };
        auto child = std::make_shared<Child>();
        std::wstring reason;
        if (!DispatchToWindowThread(hwnd, [hwnd, child] {
                child->parent = GetParent(hwnd);
                wchar_t name[kMaxClassNameChars]{};
                child->className = ClassNameOf(hwnd, name);
                child->sizeGrip = FluentShell::EqualsIgnoreCase(child->className, L"ScrollBar") &&
                    (static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) & (SBS_SIZEBOX | SBS_SIZEGRIP)) != 0;
                child->empty = GetWindowTextLengthW(hwnd) == 0;
                RECT client{};
                child->captionCandidate = child->empty && !GetWindow(hwnd, GW_CHILD) &&
                    GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0 &&
                    client.right <= 144 && client.bottom <= 48;
            }, reason) || !child->parent) return false;
        // A size grip is the window's resize corner, which the projection's own frame provides.
        if (child->sizeGrip) return true;
        // The caption cluster's own facts belong to the child; MDI activation
        // belongs to the client, which may live on a third GUI thread.
        if (child->captionCandidate) {
            struct Frame final { HWND parent = nullptr; HWND client = nullptr; bool mdi = false; };
            for (HWND frame = child->parent, previous = nullptr; frame && frame != previous;) {
                auto facts = std::make_shared<Frame>();
                if (!DispatchToWindowThread(frame, [frame, facts] {
                    wchar_t name[kMaxClassNameChars]{};
                    facts->mdi = FluentShell::EqualsIgnoreCase(ClassNameOf(frame, name), L"MDIClient");
                    facts->parent = GetParent(frame);
                    facts->client = FindWindowExW(frame, nullptr, L"MDIClient", nullptr);
                }, reason)) return false;
                if (facts->mdi) break;
                if (facts->client) {
                    auto active = std::make_shared<HWND>(nullptr);
                    if (!DispatchToWindowThread(facts->client, [client = facts->client, active] {
                        *active = reinterpret_cast<HWND>(SendMessageW(client, WM_MDIGETACTIVE, 0, 0));
                    }, reason)) return false;
                    if (*active) {
                        auto maximized = std::make_shared<bool>(false);
                        if (!DispatchToWindowThread(*active, [active = *active, maximized] {
                            *maximized = IsZoomed(active) != FALSE;
                        }, reason)) return false;
                        if (*maximized) return true;
                    }
                    break;
                }
                previous = frame;
                frame = facts->parent;
            }
        }
        auto absorbed = std::make_shared<bool>(false);
        const HWND parent = child->parent;
        return DispatchToWindowThread(parent, [parent, hwnd, child, absorbed] {
            wchar_t name[kMaxClassNameChars]{};
            const auto type = ClassNameOf(parent, name);
            if (FluentShell::EqualsIgnoreCase(type, L"ComboBox")) {
                COMBOBOXINFO info{sizeof(info)};
                // A ComboBoxEx's own edit box lives inside its inner combo.
                const HWND outer = GetParent(parent);
                *absorbed = (GetComboBoxInfo(parent, &info) &&
                        (hwnd == info.hwndItem || hwnd == info.hwndList)) ||
                    (outer && IsComboBoxExWindow(outer) &&
                        hwnd == reinterpret_cast<HWND>(SendMessageW(outer, CBEM_GETEDITCONTROL, 0, 0)));
            } else if (FluentShell::EqualsIgnoreCase(type, WC_COMBOBOXEXW)) {
                *absorbed = hwnd == reinterpret_cast<HWND>(SendMessageW(parent, CBEM_GETCOMBOCONTROL, 0, 0));
            } else if (FluentShell::EqualsIgnoreCase(type, WC_TREEVIEWW)) {
                *absorbed = hwnd == reinterpret_cast<HWND>(SendMessageW(parent, TVM_GETEDITCONTROL, 0, 0));
            } else if (FluentShell::EqualsIgnoreCase(type, L"MonitorPaletteClass")) {
                *absorbed = FluentShell::EqualsIgnoreCase(child->className, L"MonitorPaletteEntryClass");
            } else if (FluentShell::EqualsIgnoreCase(type, STATUSCLASSNAMEW)) {
                *absorbed = child->empty;
            } else if (FluentShell::EqualsIgnoreCase(type, DATETIMEPICK_CLASSW)) {
                // The spin arrows of a time picker (or a DTS_UPDOWN date picker) are part of it.
                *absorbed = FluentShell::EqualsIgnoreCase(child->className, UPDOWN_CLASSW);
            } else if (FluentShell::EqualsIgnoreCase(type, WC_LISTVIEWW)) {
                *absorbed = FluentShell::EqualsIgnoreCase(child->className, WC_HEADERW) ||
                    hwnd == reinterpret_cast<HWND>(SendMessageW(parent, LVM_GETEDITCONTROL, 0, 0));
            } else if (FluentShell::EqualsIgnoreCase(type, L"DirectUIHWND")) {
                // A DirectUI scroll viewer's hosted ScrollBar (in its CtrlNotifySink):
                // the projected island scrolls its elements itself.
                *absorbed = IsDirectUiScrollChrome(hwnd);
            } else if (FluentShell::EqualsIgnoreCase(type, L"CtrlNotifySink")) {
                *absorbed = IsDirectUiScrollChrome(parent);
            } else if (FluentShell::EqualsIgnoreCase(type, L"ChartGraphClass") ||
                       FluentShell::EqualsIgnoreCase(type, L"PerfLegend")) {
                // The chart's drawing surface and the legend's list belong to their part's node.
                *absorbed = IsPerformanceChartImplementationChild(hwnd);
            }
        }, reason) && *absorbed;
    } catch (...) { return false; }
}

} // namespace FluentShell::Bridge::Translation
