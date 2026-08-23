#include "WindowCapture.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <unordered_set>
#include <utility>

namespace FluentShell::Bridge::Translation {
namespace {

constexpr wchar_t kNodeGenerationProperty[] = L"FluentShell.Bridge.NodeGeneration";
constexpr size_t kMaxMenuDepth = 8;
constexpr size_t kMaxMenuItems = 256;
constexpr size_t kMaxListViewColumns = 64;
constexpr size_t kMaxStructuredTextChars = 256 * 1024;

class PhysicalCoordinateScope final {
public:
    PhysicalCoordinateScope() noexcept
        : previous_(SetThreadDpiAwarenessContext(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinateScope() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    bool IsValid() const noexcept { return previous_ != nullptr; }

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

bool IsUsableTopLevelBounds(const RECT& bounds) {
    return bounds.right > bounds.left && bounds.bottom > bounds.top &&
        !(bounds.left == -32000 && bounds.top == -32000);
}

bool CaptureTopLevelBounds(HWND root, bool minimized, bool maximized, RECT& bounds) {
    if (!GetWindowRect(root, &bounds)) return false;
    if (!minimized && !maximized) return IsUsableTopLevelBounds(bounds);

    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(root, &placement)) return false;

    RECT normal = placement.rcNormalPosition;
    if (!IsUsableTopLevelBounds(normal)) return false;
    const auto exStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
    if ((exStyle & WS_EX_TOOLWINDOW) == 0) {
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const HMONITOR monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return false;
        OffsetRect(&normal,
            monitorInfo.rcWork.left - monitorInfo.rcMonitor.left,
            monitorInfo.rcWork.top - monitorInfo.rcMonitor.top);
    }
    bounds = normal;
    return true;
}

std::wstring WindowClass(HWND hwnd) {
    wchar_t value[256]{};
    const int length = GetClassNameW(hwnd, value, static_cast<int>(std::size(value)));
    return length > 0 ? std::wstring(value, static_cast<size_t>(length)) : std::wstring();
}

void AppendWindowEvidence(HWND hwnd, std::wstring& reason) {
    reason.append(L" [hwnd=");
    reason.append(std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)));
    reason.append(L" class=");
    reason.append(WindowClass(hwnd));
    reason.append(L" style=");
    reason.append(std::to_wstring(
        static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_STYLE))));
    reason.append(L" exStyle=");
    reason.append(std::to_wstring(
        static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE))));
    reason.push_back(L']');
}

bool WindowText(HWND hwnd, std::wstring& value, std::wstring& reason) {
    const int reported = GetWindowTextLengthW(hwnd);
    if (reported < 0 || static_cast<size_t>(reported) > Ipc::kMaxStringChars) {
        reason = L"window text exceeds the 64 KiB protocol string limit";
        return false;
    }
    std::wstring captured(static_cast<size_t>(reported) + 1, L'\0');
    const int length = GetWindowTextW(hwnd, captured.data(), static_cast<int>(captured.size()));
    if (length < 0 || static_cast<size_t>(length) > Ipc::kMaxStringChars) {
        reason = L"window text exceeds the 64 KiB protocol string limit";
        return false;
    }
    captured.resize(static_cast<size_t>(length));
    value = std::move(captured);
    return true;
}

bool IsStandardTopLevel(HWND root, std::wstring& reason) {
    if (!root || !IsWindow(root) || GetAncestor(root, GA_ROOT) != root) {
        reason = L"not a top-level window";
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(root, &processId);
    if (processId != GetCurrentProcessId()) {
        reason = L"foreign top-level window";
        return false;
    }
    const auto style = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_STYLE));
    const auto exStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
    if ((style & WS_CHILD) != 0 || (exStyle & WS_EX_LAYERED) != 0) {
        reason = L"child or layered top-level window";
        return false;
    }
    if (GetWindow(root, GW_OWNER) != nullptr) {
        reason = L"owned top-level window is outside the v1 owner graph";
        return false;
    }
    if ((exStyle & WS_EX_MDICHILD) != 0) {
        reason = L"MDI child top-level window";
        return false;
    }
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int regionType = region ? GetWindowRgn(root, region) : ERROR;
    if (region) DeleteObject(region);
    if (regionType != ERROR) {
        reason = L"nonrectangular top-level window";
        return false;
    }
    return true;
}

bool ClassifyControl(
    HWND hwnd,
    ControlKind& kind,
    std::wstring& reason) {
    const auto className = WindowClass(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));

    if (FluentShell::EqualsIgnoreCase(className, L"Static")) {
        const DWORD type = style & SS_TYPEMASK;
        if (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT &&
            type != SS_SIMPLE && type != SS_LEFTNOWORDWRAP &&
            type != SS_ETCHEDHORZ && type != SS_ETCHEDVERT) {
            reason = L"unsupported Static draw style";
            return false;
        }
        kind = (type == SS_ETCHEDHORZ || type == SS_ETCHEDVERT)
            ? ControlKind::Separator
            : ControlKind::StaticText;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, L"Button")) {
        const DWORD type = style & BS_TYPEMASK;
        if (type == BS_OWNERDRAW || (style & (BS_BITMAP | BS_ICON)) != 0) {
            reason = L"unsupported Button draw style";
            return false;
        }
        switch (type) {
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
                reason = L"tab-stop GroupBox is not supported";
                return false;
            }
            kind = ControlKind::GroupBox;
            return true;
        default:
            reason = L"unsupported Button type";
            return false;
        }
    }

    if (FluentShell::EqualsIgnoreCase(className, L"Edit")) {
        kind = (style & ES_PASSWORD) != 0 ? ControlKind::Password : ControlKind::Edit;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, L"ComboBox")) {
        if ((style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0) {
            reason = L"owner-draw ComboBox";
            return false;
        }
        const DWORD type = style & 0x0003u;
        if (type != CBS_DROPDOWNLIST &&
            (type != CBS_DROPDOWN || (style & CBS_HASSTRINGS) == 0)) {
            reason = L"ComboBox is simple, non-string-backed, or has an unsupported type";
            return false;
        }
        kind = ControlKind::ComboBox;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, L"ListBox")) {
        if ((style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE | LBS_NODATA |
                      LBS_MULTIPLESEL | LBS_EXTENDEDSEL | LBS_MULTICOLUMN | LBS_NOSEL)) != 0) {
            reason = L"owner-draw, virtual, multi-select, or multi-column ListBox";
            return false;
        }
        kind = ControlKind::ListBox;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, PROGRESS_CLASSW)) {
        if ((style & (PBS_MARQUEE | PBS_VERTICAL | WS_TABSTOP)) != 0) {
            reason = L"marquee, vertical, or tab-stop ProgressBar is not supported";
            return false;
        }
        kind = ControlKind::ProgressBar;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, WC_LINK)) {
        constexpr DWORD unsupportedLinkStyles = LWS_IGNORERETURN |
            LWS_USECUSTOMTEXT | LWS_RIGHT | LWS_NOPREFIX;
        if ((style & unsupportedLinkStyles) != 0) {
            reason = L"SysLink requires unsupported callback, alignment, prefix, or keyboard semantics";
            return false;
        }
        kind = ControlKind::SysLink;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, WC_LISTVIEWW)) {
        const DWORD type = style & LVS_TYPEMASK;
        if (type != LVS_REPORT || (style & LVS_NOCOLUMNHEADER) != 0 ||
            (style & (LVS_OWNERDATA | LVS_OWNERDRAWFIXED | LVS_EDITLABELS)) != 0) {
            reason = L"ListView is virtual, owner-draw, label-editable, headerless, or not in report view";
            return false;
        }
        if (SendMessageW(hwnd, LVM_ISGROUPVIEWENABLED, 0, 0) != FALSE) {
            reason = L"grouped ListView is not supported";
            return false;
        }
        const DWORD extended = static_cast<DWORD>(
            SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
        constexpr DWORD unsupportedExtended = LVS_EX_CHECKBOXES |
            LVS_EX_HEADERDRAGDROP | LVS_EX_TRACKSELECT | LVS_EX_ONECLICKACTIVATE |
            LVS_EX_TWOCLICKACTIVATE;
        if ((extended & unsupportedExtended) != 0) {
            reason = L"ListView requires unsupported checkbox, header-drag, or activation semantics";
            return false;
        }
        kind = ControlKind::ListView;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, STATUSCLASSNAMEW)) {
        if ((style & WS_TABSTOP) != 0) {
            reason = L"tab-stop StatusBar is not supported";
            return false;
        }
        kind = ControlKind::StatusBar;
        return true;
    }

    reason = L"unsupported visible control class: " + className;
    return false;
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
        reason = L"invalid or excessive string item count";
        return false;
    }
    items.reserve(static_cast<size_t>(rawCount));
    for (LRESULT index = 0; index < rawCount; ++index) {
        const LRESULT length = SendMessageW(hwnd, lengthMessage, index, 0);
        if (length < 0 || length > static_cast<LRESULT>(Ipc::kMaxStringChars)) {
            reason = L"invalid string item length";
            return false;
        }
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        const LRESULT copied = SendMessageW(
            hwnd, textMessage, index, reinterpret_cast<LPARAM>(text.data()));
        if (copied < 0) {
            reason = L"failed to read string item";
            return false;
        }
        text.resize(static_cast<size_t>(copied));
        items.push_back(std::move(text));
    }
    return true;
}

bool CaptureSingleSysLink(
    HWND hwnd,
    ControlNode& node,
    std::wstring& reason) {
    LITEM item{};
    item.mask = LIF_ITEMINDEX | LIF_STATE | LIF_ITEMID | LIF_URL;
    item.iLink = 0;
    item.stateMask = LIS_ENABLED;
    if (!SendMessageW(hwnd, LM_GETITEM, 0, reinterpret_cast<LPARAM>(&item))) {
        reason = L"SysLink has no interrogable link item";
        return false;
    }
    LITEM second{};
    second.mask = LIF_ITEMINDEX | LIF_STATE;
    second.iLink = 1;
    second.stateMask = LIS_ENABLED;
    if (SendMessageW(hwnd, LM_GETITEM, 0, reinterpret_cast<LPARAM>(&second))) {
        reason = L"multi-link SysLink is outside the bounded adapter";
        return false;
    }

    std::wstring markup = node.text;
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
        reason = L"SysLink markup is not a single bounded hyperlink";
        return false;
    }
    const std::wstring prefix = markup.substr(0, open);
    const std::wstring label = markup.substr(openEnd + 1, close - openEnd - 1);
    std::wstring suffix = markup.substr(close + 4);
    while (!suffix.empty() && suffix.back() <= L' ') suffix.pop_back();
    const auto containsMarkup = [](std::wstring_view value) {
        return value.find(L'<') != std::wstring_view::npos ||
            value.find(L'>') != std::wstring_view::npos;
    };
    if (containsMarkup(prefix) || containsMarkup(label) || containsMarkup(suffix) ||
        prefix.find(L'&') != std::wstring::npos ||
        label.find(L'&') != std::wstring::npos ||
        suffix.find(L'&') != std::wstring::npos) {
        reason = L"SysLink contains unsupported nested markup or mnemonic text";
        return false;
    }
    const std::wstring flattened = prefix + label + suffix;
    const size_t labelAt = flattened.find(label);
    if (labelAt != prefix.size() ||
        flattened.find(label, labelAt + label.size()) != std::wstring::npos) {
        reason = L"SysLink label is ambiguous in its flattened text";
        return false;
    }
    node.text = flattened;
    node.automationName = node.text;
    node.items.push_back(label);
    node.enabled = node.enabled && (item.state & LIS_ENABLED) != 0;
    return true;
}

bool ReadListViewCell(
    HWND hwnd,
    int row,
    int column,
    std::wstring& text,
    std::wstring& reason) {
    size_t capacity = 256;
    while (capacity <= Ipc::kMaxStringChars + 1) {
        std::wstring buffer(capacity, L'\0');
        LVITEMW item{};
        item.iSubItem = column;
        item.pszText = buffer.data();
        item.cchTextMax = static_cast<int>(buffer.size());
        const int copied = static_cast<int>(SendMessageW(
            hwnd, LVM_GETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item)));
        if (copied < 0) {
            reason = L"ListView item text read failed";
            return false;
        }
        if (static_cast<size_t>(copied) + 1 < capacity) {
            buffer.resize(static_cast<size_t>(copied));
            text = std::move(buffer);
            return true;
        }
        if (capacity == Ipc::kMaxStringChars + 1) break;
        capacity = std::min(Ipc::kMaxStringChars + 1, capacity * 2);
    }
    reason = L"ListView item text exceeds the protocol string limit";
    return false;
}

bool ReadListViewColumn(
    HWND hwnd,
    int index,
    std::wstring& text,
    int& width,
    std::wstring& reason) {
    size_t capacity = 256;
    while (capacity <= Ipc::kMaxStringChars + 1) {
        std::wstring buffer(capacity, L'\0');
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = buffer.data();
        column.cchTextMax = static_cast<int>(buffer.size());
        if (!SendMessageW(hwnd, LVM_GETCOLUMNW, index,
                reinterpret_cast<LPARAM>(&column))) {
            reason = L"ListView column read failed";
            return false;
        }
        const size_t length = static_cast<size_t>(
            std::find(buffer.begin(), buffer.end(), L'\0') - buffer.begin());
        if (length + 1 < capacity) {
            buffer.resize(length);
            text = std::move(buffer);
            width = std::max(0, column.cx);
            return true;
        }
        if (capacity == Ipc::kMaxStringChars + 1) break;
        capacity = std::min(Ipc::kMaxStringChars + 1, capacity * 2);
    }
    reason = L"ListView column text exceeds the protocol string limit";
    return false;
}

bool CaptureListView(
    HWND hwnd,
    ControlNode& node,
    std::wstring& reason) {
    const LRESULT count = SendMessageW(hwnd, LVM_GETITEMCOUNT, 0, 0);
    if (count < 0 || count > static_cast<LRESULT>(Ipc::kMaxListItems)) {
        reason = L"invalid or excessive ListView item count";
        return false;
    }
    const HWND header = reinterpret_cast<HWND>(
        SendMessageW(hwnd, LVM_GETHEADER, 0, 0));
    const int columnCount = header && IsWindow(header)
        ? Header_GetItemCount(header) : 0;
    if (columnCount <= 0 ||
        static_cast<size_t>(columnCount) > kMaxListViewColumns) {
        reason = L"ListView has no bounded report columns";
        return false;
    }
    std::vector<int> columnOrder(static_cast<size_t>(columnCount), -1);
    if (!SendMessageW(hwnd, LVM_GETCOLUMNORDERARRAY, columnCount,
            reinterpret_cast<LPARAM>(columnOrder.data()))) {
        reason = L"ListView column order read failed";
        return false;
    }
    for (int index = 0; index < columnCount; ++index) {
        if (columnOrder[static_cast<size_t>(index)] != index) {
            reason = L"reordered ListView columns are outside the bounded adapter";
            return false;
        }
        HDITEMW headerItem{};
        headerItem.mask = HDI_FORMAT;
        if (!Header_GetItem(header, index, &headerItem) ||
            (headerItem.fmt & (HDF_OWNERDRAW | HDF_BITMAP |
                HDF_BITMAP_ON_RIGHT | HDF_IMAGE)) != 0) {
            reason = L"ListView header requires owner-draw, bitmap, or image semantics";
            return false;
        }
    }

    size_t totalText = 0;
    node.columns.reserve(static_cast<size_t>(columnCount));
    node.columnWidths.reserve(static_cast<size_t>(columnCount));
    for (int column = 0; column < columnCount; ++column) {
        std::wstring label;
        int width = 0;
        if (!ReadListViewColumn(hwnd, column, label, width, reason)) return false;
        totalText += label.size();
        if (totalText > kMaxStructuredTextChars) {
            reason = L"ListView text exceeds the bounded adapter payload";
            return false;
        }
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
            totalText += text.size();
            if (totalText > kMaxStructuredTextChars) {
                reason = L"ListView text exceeds the bounded adapter payload";
                return false;
            }
            cells.push_back(std::move(text));
        }
        node.items.push_back(cells.empty() ? std::wstring() : cells.front());
        node.rows.push_back(std::move(cells));
    }

    int previousSelected = -1;
    while (true) {
        const int selected = static_cast<int>(SendMessageW(
            hwnd, LVM_GETNEXTITEM, previousSelected, LVNI_SELECTED));
        if (selected < 0) break;
        if (selected <= previousSelected ||
            static_cast<size_t>(selected) >= node.rows.size() ||
            node.selectedIndices.size() >= node.rows.size()) {
            reason = L"ListView selected index enumeration is invalid";
            return false;
        }
        node.selectedIndices.push_back(selected);
        previousSelected = selected;
    }
    node.focusedIndex = static_cast<int>(SendMessageW(
        hwnd, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));
    if (node.focusedIndex < -1 ||
        (node.focusedIndex >= 0 &&
            static_cast<size_t>(node.focusedIndex) >= node.rows.size())) {
        reason = L"ListView focused index is outside the item range";
        return false;
    }
    node.multiSelect =
        (static_cast<DWORD>(node.style) & LVS_SINGLESEL) == 0;
    node.selectedIndex = node.selectedIndices.empty()
        ? -1 : node.selectedIndices.front();
    return true;
}

bool CaptureStatusBar(
    HWND hwnd,
    ControlNode& node,
    std::wstring& reason) {
    constexpr size_t kMaxStatusParts = 64;
    node.text.clear();
    node.automationName.clear();
    const bool simple = SendMessageW(hwnd, SB_ISSIMPLE, 0, 0) != FALSE;
    int count = simple ? 1 : static_cast<int>(SendMessageW(hwnd, SB_GETPARTS, 0, 0));
    if (count <= 0 || static_cast<size_t>(count) > kMaxStatusParts) {
        reason = L"StatusBar has no bounded part collection";
        return false;
    }
    std::vector<int> rightEdges(static_cast<size_t>(count), -1);
    if (!simple && SendMessageW(hwnd, SB_GETPARTS, count,
            reinterpret_cast<LPARAM>(rightEdges.data())) != count) {
        reason = L"StatusBar part geometry read failed";
        return false;
    }
    RECT client{};
    if (!GetClientRect(hwnd, &client)) {
        reason = L"StatusBar client geometry read failed";
        return false;
    }
    const int64_t clientWidth = static_cast<int64_t>(client.right) - client.left;
    if (clientWidth < 0 || clientWidth > std::numeric_limits<int>::max()) {
        reason = L"StatusBar client width is outside the bounded adapter";
        return false;
    }

    size_t totalText = 0;
    int64_t previousRight = 0;
    node.items.reserve(static_cast<size_t>(count));
    node.columnWidths.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        const WPARAM part = simple ? SB_SIMPLEID : static_cast<WPARAM>(index);
        const LRESULT lengthAndType = SendMessageW(hwnd, SB_GETTEXTLENGTHW, part, 0);
        const size_t length = LOWORD(lengthAndType);
        const UINT type = HIWORD(lengthAndType);
        if ((type & SBT_OWNERDRAW) != 0 || length > Ipc::kMaxStringChars) {
            reason = L"StatusBar part is owner-draw or exceeds the text limit";
            return false;
        }
        std::wstring text(length + 1, L'\0');
        const LRESULT copiedAndType = SendMessageW(
            hwnd, SB_GETTEXTW, part, reinterpret_cast<LPARAM>(text.data()));
        const size_t copied = LOWORD(copiedAndType);
        if (copied > length || (HIWORD(copiedAndType) & SBT_OWNERDRAW) != 0) {
            reason = L"StatusBar part text read failed";
            return false;
        }
        text.resize(copied);
        totalText += text.size();
        if (totalText > kMaxStructuredTextChars) {
            reason = L"StatusBar text exceeds the bounded adapter payload";
            return false;
        }
        node.items.push_back(std::move(text));
        int64_t right = simple
            ? clientWidth
            : rightEdges[static_cast<size_t>(index)];
        if (right == -1) {
            if (index + 1 != count) {
                reason = L"StatusBar stretch part is not the final part";
                return false;
            }
            right = clientWidth;
        }
        if (right < previousRight || right > clientWidth || right < 0) {
            reason = L"StatusBar part edges are not monotonic client coordinates";
            return false;
        }
        const int64_t width = right - previousRight;
        if (width > std::numeric_limits<int>::max()) {
            reason = L"StatusBar part width exceeds the protocol range";
            return false;
        }
        node.columnWidths.push_back(static_cast<int>(width));
        previousRight = right;
    }
    return true;
}

bool IsCompositeImplementationChild(HWND hwnd) {
    const HWND parent = GetParent(hwnd);
    if (!parent) return false;
    if (FluentShell::EqualsIgnoreCase(WindowClass(parent), L"ComboBox")) {
        COMBOBOXINFO info{sizeof(info)};
        return GetComboBoxInfo(parent, &info) &&
            (hwnd == info.hwndItem || hwnd == info.hwndList);
    }
    return FluentShell::EqualsIgnoreCase(WindowClass(parent), WC_LISTVIEWW) &&
        FluentShell::EqualsIgnoreCase(WindowClass(hwnd), WC_HEADERW);
}

struct Enumeration final {
    std::vector<HWND> handles;
};

BOOL CALLBACK CollectChild(HWND hwnd, LPARAM raw) {
    auto& enumeration = *reinterpret_cast<Enumeration*>(raw);
    enumeration.handles.push_back(hwnd);
    return enumeration.handles.size() <= Ipc::kMaxNodes;
}

bool CaptureNativeTabOrder(
    HWND root,
    const std::vector<HWND>& handles,
    std::unordered_map<HWND, int>& tabIndexes,
    std::wstring& reason) {
    std::unordered_set<HWND> candidates;
    for (const HWND child : handles) {
        if (IsCompositeImplementationChild(child)) continue;
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
        if (IsWindowVisible(child) && IsWindowEnabled(child) &&
            (style & WS_TABSTOP) != 0) {
            candidates.insert(child);
        }
    }
    if (candidates.empty()) return true;

    // GetNextDlgTabItem is the native dialog manager's source of truth. Do
    // not silently substitute EnumChildWindows order when it cannot describe
    // every currently focusable candidate; that would produce a misleading
    // focus contract and should instead trigger whole-window fallback.
    std::unordered_set<HWND> visited;
    HWND current = nullptr;
    for (size_t attempt = 0; attempt <= handles.size() + candidates.size(); ++attempt) {
        const HWND next = GetNextDlgTabItem(root, current, FALSE);
        if (!next) {
            reason = L"native dialog manager did not expose a complete tab order";
            return false;
        }
        if (!visited.insert(next).second) break;
        if (candidates.contains(next)) {
            tabIndexes.emplace(next, static_cast<int>(tabIndexes.size()));
        }
        current = next;
    }
    if (tabIndexes.size() != candidates.size()) {
        reason = L"native dialog manager tab order omitted a focusable control";
        return false;
    }
    return true;
}

template <typename T>
void HashBytes(uint64_t& hash, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t index = 0; index < sizeof(T); ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

void HashString(uint64_t& hash, std::wstring_view value) noexcept {
    for (const wchar_t character : value) HashBytes(hash, character);
}

bool HasMdiClient(HWND root) {
    return FindWindowExW(root, nullptr, L"MDIClient", nullptr) != nullptr;
}

struct MenuCaptureState final {
    size_t count = 0;
    std::unordered_set<HMENU> menus;
    std::unordered_set<uint32_t> commandIds;
};

bool CaptureMenuLevel(
    HMENU menu,
    size_t depth,
    std::wstring_view path,
    bool topLevel,
    bool ancestorEnabled,
    MenuCaptureState& state,
    std::vector<MenuItemSnapshot>& result,
    std::wstring& reason) {
    if (!menu || !IsMenu(menu) || depth > kMaxMenuDepth || !state.menus.insert(menu).second) {
        reason = L"invalid, deep, or cyclic menu shape";
        return false;
    }
    MENUINFO menuInfo{sizeof(menuInfo)};
    menuInfo.fMask = MIM_STYLE;
    if (!GetMenuInfo(menu, &menuInfo) ||
        (menuInfo.dwStyle & (MNS_NOTIFYBYPOS | MNS_MODELESS | MNS_DRAGDROP)) != 0) {
        reason = L"callback or modeless menu semantics are not supported";
        return false;
    }
    const int count = GetMenuItemCount(menu);
    if (count < 0 || state.count + static_cast<size_t>(count) > kMaxMenuItems) {
        reason = L"invalid or excessive menu item count";
        return false;
    }
    bool foundDefault = false;
    result.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW info{sizeof(info)};
        std::wstring text(Ipc::kMaxStringChars + 1, L'\0');
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU |
            MIIM_STRING | MIIM_BITMAP | MIIM_CHECKMARKS | MIIM_DATA;
        info.dwTypeData = text.data();
        info.cch = static_cast<UINT>(text.size());
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info) ||
            info.cch > Ipc::kMaxStringChars) {
            reason = L"invalid or excessive menu label";
            return false;
        }
        text.resize(info.cch);
        const UINT unsupportedType = MFT_OWNERDRAW | MFT_BITMAP | MFT_MENUBARBREAK |
            MFT_MENUBREAK | MFT_RIGHTJUSTIFY;
        if ((info.fType & unsupportedType) != 0 || info.hbmpItem != nullptr ||
            info.hbmpChecked != nullptr || info.hbmpUnchecked != nullptr ||
            info.dwItemData != 0) {
            reason = L"owner-draw, bitmap, or custom menu item";
            return false;
        }
        MenuItemSnapshot item;
        item.itemId = path.empty()
            ? std::to_wstring(index)
            : std::wstring(path) + L"." + std::to_wstring(index);
        item.text = std::move(text);
        item.enabled = ancestorEnabled &&
            (info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
        item.checked = (info.fState & MFS_CHECKED) != 0;
        item.radio = (info.fType & MFT_RADIOCHECK) != 0;
        item.isDefault = (info.fState & MFS_DEFAULT) != 0;
        if (item.isDefault && foundDefault) {
            reason = L"menu level has multiple default items";
            return false;
        }
        foundDefault = foundDefault || item.isDefault;
        ++state.count;
        if ((info.fType & MFT_SEPARATOR) != 0) {
            if (topLevel || info.hSubMenu || !item.text.empty()) {
                reason = L"invalid menu separator shape";
                return false;
            }
            item.kind = MenuItemKind::Separator;
        } else if (info.hSubMenu) {
            if (item.text.empty()) {
                reason = L"popup menu has no textual label";
                return false;
            }
            item.kind = MenuItemKind::Popup;
            if (!CaptureMenuLevel(info.hSubMenu, depth + 1, item.itemId,
                    false, item.enabled, state, item.items, reason) || item.items.empty()) return false;
        } else {
            if (topLevel || item.text.empty() || info.wID == 0 || info.wID >= 0xf000) {
                reason = L"menu command cannot use standard WM_COMMAND semantics";
                return false;
            }
            item.kind = MenuItemKind::Command;
            item.commandId = info.wID;
            if (!state.commandIds.insert(item.commandId).second) {
                reason = L"ambiguous duplicate executable menu command ID";
                return false;
            }
        }
        result.push_back(std::move(item));
    }
    return true;
}

} // namespace

bool CaptureTopLevelMenu(
    HWND root,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& rejectionReason) noexcept {
    try {
        menu.clear();
        const HMENU nativeMenu = GetMenu(root);
        if (!nativeMenu) return true;
        if (HasMdiClient(root) || nativeMenu == GetSystemMenu(root, FALSE)) {
            rejectionReason = L"MDI or system menu cannot be projected as a menu bar";
            return false;
        }
        MenuCaptureState state;
        return CaptureMenuLevel(nativeMenu, 1, L"", true, true,
            state, menu, rejectionReason) &&
            !menu.empty();
    } catch (...) {
        rejectionReason = L"exception while capturing native menu";
        return false;
    }
}

bool CaptureWindow(
    HWND root,
    CaptureContext& context,
    WindowSnapshot& snapshot,
    std::wstring& rejectionReason) noexcept {
    try {
        PhysicalCoordinateScope dpiScope;
        if (!dpiScope.IsValid()) {
            rejectionReason = L"cannot establish physical-coordinate DPI context";
            return false;
        }
        if (!IsStandardTopLevel(root, rejectionReason)) {
            AppendWindowEvidence(root, rejectionReason);
            return false;
        }

        WindowSnapshot next;
        next.surfaceId = context.surfaceId;
        next.surfaceKind = SurfaceKind::Window;
        next.modal = false;
        next.canCancel = true;
        next.generation = context.generation;
        next.revision = context.revision;
        next.nativeHwnd = root;
        next.ownerHwnd = GetWindow(root, GW_OWNER);
        if (!WindowText(root, next.title, rejectionReason)) return false;
        next.dpi = GetDpiForWindow(root);
        if (next.dpi == 0) next.dpi = 96;
        const bool minimized = IsIconic(root) != FALSE;
        const bool maximized = IsZoomed(root) != FALSE;
        if (!CaptureTopLevelBounds(root, minimized, maximized, next.bounds)) {
            rejectionReason = L"native window has no valid restore bounds";
            return false;
        }
        RECT client{};
        GetClientRect(root, &client);
        next.clientBounds = client;
        next.windowStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_STYLE));
        next.windowExStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
        next.visible = IsWindowVisible(root) != FALSE;
        next.enabled = IsWindowEnabled(root) != FALSE;
        next.state = minimized ? L"minimized" : (maximized ? L"maximized" : L"normal");
        next.showInTaskbar = next.ownerHwnd == nullptr || (next.windowExStyle & WS_EX_APPWINDOW) != 0;
        next.rtl = (next.windowExStyle & WS_EX_LAYOUTRTL) != 0;
        if (!CaptureTopLevelMenu(root, next.menu, rejectionReason)) return false;

        Enumeration enumeration;
        EnumChildWindows(root, CollectChild, reinterpret_cast<LPARAM>(&enumeration));
        if (enumeration.handles.size() > Ipc::kMaxNodes) {
            rejectionReason = L"window exceeds the 512 node limit";
            return false;
        }

        std::unordered_map<HWND, uint64_t> visibleNodeIds;
        std::unordered_map<HWND, int> nativeTabIndexes;
        if (!CaptureNativeTabOrder(root, enumeration.handles, nativeTabIndexes, rejectionReason)) {
            return false;
        }
        const DWORD rootThread = GetWindowThreadProcessId(root, nullptr);
        int zIndex = 0;
        for (const HWND child : enumeration.handles) {
            if (!IsWindowVisible(child)) continue;
            if (IsCompositeImplementationChild(child)) continue;
            DWORD childProcess = 0;
            const DWORD childThread = GetWindowThreadProcessId(child, &childProcess);
            if (childProcess != GetCurrentProcessId() || childThread != rootThread) {
                rejectionReason = L"foreign-process or foreign-thread child HWND";
                return false;
            }

            ControlNode node;
            if (!ClassifyControl(child, node.kind, rejectionReason)) {
                AppendWindowEvidence(child, rejectionReason);
                return false;
            }
            uint64_t lifecycleGeneration = reinterpret_cast<uintptr_t>(
                GetPropW(child, kNodeGenerationProperty));
            if (lifecycleGeneration == 0) {
                lifecycleGeneration = context.nextNodeGeneration++;
                if (!SetPropW(child, kNodeGenerationProperty,
                        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(lifecycleGeneration)))) {
                    rejectionReason = L"cannot assign bridge-owned child generation";
                    return false;
                }
            }
            auto id = context.nodeIds.find(child);
            if (id == context.nodeIds.end() || id->second.generation != lifecycleGeneration) {
                CaptureContext::NodeIdentity identity{
                    lifecycleGeneration, context.nextNodeId++ };
                id = context.nodeIds.insert_or_assign(child, identity).first;
            }
            node.nodeId = id->second.nodeId;
            node.generation = lifecycleGeneration;
            node.hwnd = child;
            const HWND parent = GetParent(child);
            if (parent != root) {
                const auto parentId = visibleNodeIds.find(parent);
                if (parentId == visibleNodeIds.end()) {
                    rejectionReason = L"unsupported or hidden intermediate parent";
                    return false;
                }
                node.parentNodeId = parentId->second;
            }
            node.controlId = GetDlgCtrlID(child);
            node.zIndex = zIndex++;
            RECT childRect{};
            GetWindowRect(child, &childRect);
            MapWindowPoints(nullptr, root, reinterpret_cast<POINT*>(&childRect), 2);
            node.rect = childRect;
            node.style = static_cast<uint64_t>(GetWindowLongPtrW(child, GWL_STYLE));
            node.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(child, GWL_EXSTYLE));
            node.visible = true;
            node.enabled = IsWindowEnabled(child) != FALSE;
            node.tabStop = (node.style & WS_TABSTOP) != 0;
            if (const auto tab = nativeTabIndexes.find(child); tab != nativeTabIndexes.end()) {
                node.tabIndex = tab->second;
            }
            node.dialogCode = static_cast<uint32_t>(SendMessageW(child, WM_GETDLGCODE, 0, 0));
            const bool standardTextKeyboard =
                (node.kind == ControlKind::Edit || node.kind == ControlKind::Password) &&
                (static_cast<DWORD>(node.style) & ES_MULTILINE) != 0;
            if ((node.dialogCode & (DLGC_WANTALLKEYS | DLGC_WANTMESSAGE)) != 0 &&
                !standardTextKeyboard) {
                rejectionReason = L"control requires custom keyboard routing code=" +
                    std::to_wstring(node.dialogCode);
                AppendWindowEvidence(child, rejectionReason);
                return false;
            }
            if (!WindowText(child, node.text, rejectionReason)) return false;
            // Never place a password's clear text in the UIA Name property.
            // The PasswordBox still receives the canonical value through its
            // typed view model, while accessibility exposes only its role.
            node.automationName = node.kind == ControlKind::Password
                ? L"Password edit"
                : node.text;
            node.groupStart = (node.style & WS_GROUP) != 0;

            if (node.kind == ControlKind::SysLink &&
                !CaptureSingleSysLink(child, node, rejectionReason)) return false;

            const DWORD controlStyle = static_cast<DWORD>(node.style);
            if (node.kind == ControlKind::Button) {
                node.isDefault = (controlStyle & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
            } else if (node.kind == ControlKind::CheckBox ||
                       node.kind == ControlKind::ThreeState ||
                       node.kind == ControlKind::RadioButton) {
                node.checked = static_cast<int>(SendMessageW(child, BM_GETCHECK, 0, 0));
            } else if (node.kind == ControlKind::Edit || node.kind == ControlKind::Password) {
                node.readOnly = (controlStyle & ES_READONLY) != 0;
                node.multiline = (controlStyle & ES_MULTILINE) != 0;
                DWORD start = 0;
                DWORD end = 0;
                SendMessageW(child, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                node.selectionStart = static_cast<int>(start);
                node.selectionLength = static_cast<int>(end >= start ? end - start : 0);
            } else if (node.kind == ControlKind::ComboBox || node.kind == ControlKind::ListBox) {
                const bool combo = node.kind == ControlKind::ComboBox;
                node.editable = combo && (controlStyle & 0x0003u) == CBS_DROPDOWN;
                if (!ReadStringItems(child, combo, node.items, rejectionReason)) return false;
                node.selectedIndex = static_cast<int>(SendMessageW(
                    child, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0));
            } else if (node.kind == ControlKind::ProgressBar) {
                PBRANGE range{};
                SendMessageW(child, PBM_GETRANGE, FALSE, reinterpret_cast<LPARAM>(&range));
                node.minimum = range.iLow;
                node.maximum = range.iHigh;
                node.position = static_cast<int>(SendMessageW(child, PBM_GETPOS, 0, 0));
                if (node.maximum <= node.minimum || node.position < node.minimum ||
                    node.position > node.maximum) {
                    rejectionReason = L"ProgressBar has an invalid native range or position";
                    return false;
                }
            } else if (node.kind == ControlKind::ListView) {
                if (!CaptureListView(child, node, rejectionReason)) return false;
            } else if (node.kind == ControlKind::StatusBar) {
                if (!CaptureStatusBar(child, node, rejectionReason)) return false;
            }

            visibleNodeIds.emplace(child, node.nodeId);
            next.nodes.push_back(std::move(node));
        }

        snapshot = std::move(next);
        return true;
    } catch (...) {
        rejectionReason = L"exception while capturing native window";
        return false;
    }
}

uint64_t SnapshotFingerprint(const WindowSnapshot& snapshot) noexcept {
    uint64_t hash = 1469598103934665603ull;
    HashBytes(hash, snapshot.nativeHwnd);
    HashBytes(hash, snapshot.ownerHwnd);
    HashBytes(hash, snapshot.generation);
    HashString(hash, snapshot.title);
    HashBytes(hash, snapshot.dpi);
    HashBytes(hash, snapshot.bounds);
    HashBytes(hash, snapshot.clientBounds);
    HashBytes(hash, snapshot.windowStyle);
    HashBytes(hash, snapshot.windowExStyle);
    HashBytes(hash, snapshot.visible);
    HashBytes(hash, snapshot.enabled);
    HashString(hash, snapshot.state);
    HashBytes(hash, snapshot.showInTaskbar);
    HashBytes(hash, snapshot.rtl);
    const auto hashMenu = [&](const auto& self,
                              const std::vector<MenuItemSnapshot>& items) -> void {
        HashBytes(hash, items.size());
        for (const auto& item : items) {
            HashString(hash, item.itemId);
            HashBytes(hash, item.kind);
            HashString(hash, item.text);
            HashBytes(hash, item.commandId);
            HashBytes(hash, item.enabled);
            HashBytes(hash, item.checked);
            HashBytes(hash, item.radio);
            HashBytes(hash, item.isDefault);
            self(self, item.items);
        }
    };
    hashMenu(hashMenu, snapshot.menu);
    HashBytes(hash, snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        HashBytes(hash, node.nodeId);
        HashBytes(hash, node.generation);
        HashBytes(hash, node.hwnd);
        const bool hasParent = node.parentNodeId.has_value();
        HashBytes(hash, hasParent);
        if (node.parentNodeId) HashBytes(hash, *node.parentNodeId);
        HashBytes(hash, node.kind);
        HashBytes(hash, node.controlId);
        HashBytes(hash, node.zIndex);
        HashBytes(hash, node.tabIndex);
        HashBytes(hash, node.rect);
        HashBytes(hash, node.style);
        HashBytes(hash, node.exStyle);
        HashBytes(hash, node.visible);
        HashBytes(hash, node.enabled);
        HashBytes(hash, node.tabStop);
        HashBytes(hash, node.dialogCode);
        HashString(hash, node.text);
        HashString(hash, node.automationName);
        HashBytes(hash, node.checked);
        HashBytes(hash, node.selectedIndex);
        HashBytes(hash, node.selectedIndices.size());
        for (const int index : node.selectedIndices) HashBytes(hash, index);
        HashBytes(hash, node.focusedIndex);
        HashBytes(hash, node.multiSelect);
        HashBytes(hash, node.selectionStart);
        HashBytes(hash, node.selectionLength);
        HashBytes(hash, node.readOnly);
        HashBytes(hash, node.multiline);
        HashBytes(hash, node.editable);
        HashBytes(hash, node.isDefault);
        HashBytes(hash, node.groupStart);
        HashBytes(hash, node.minimum);
        HashBytes(hash, node.maximum);
        HashBytes(hash, node.position);
        HashBytes(hash, node.smallChange);
        HashBytes(hash, node.largeChange);
        HashBytes(hash, node.vertical);
        HashBytes(hash, node.reversed);
        HashBytes(hash, node.items.size());
        for (const auto& item : node.items) HashString(hash, item);
        HashBytes(hash, node.columns.size());
        for (const auto& column : node.columns) HashString(hash, column);
        HashBytes(hash, node.columnWidths.size());
        for (const int width : node.columnWidths) HashBytes(hash, width);
        HashBytes(hash, node.rows.size());
        for (const auto& row : node.rows) {
            HashBytes(hash, row.size());
            for (const auto& cell : row) HashString(hash, cell);
        }
        HashBytes(hash, node.itemDepths.size());
        for (const int depth : node.itemDepths) HashBytes(hash, depth);
        HashBytes(hash, node.itemExpanded.size());
        for (const bool expanded : node.itemExpanded) HashBytes(hash, expanded);
    }
    return hash;
}

} // namespace FluentShell::Bridge::Translation
