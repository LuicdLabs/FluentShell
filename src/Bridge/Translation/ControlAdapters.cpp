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

bool ProbeProgressBar(HWND, DWORD style, ControlKind& kind, std::wstring& reason) {
    if ((style & (PBS_MARQUEE | PBS_VERTICAL | WS_TABSTOP)) != 0) {
        return Reject(reason,
            L"marquee, vertical, or tab-stop ProgressBar is not supported");
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
    if ((style & LVS_TYPEMASK) != LVS_REPORT || (style & LVS_NOCOLUMNHEADER) != 0 ||
        (style & (LVS_OWNERDATA | LVS_OWNERDRAWFIXED | LVS_EDITLABELS)) != 0) {
        return Reject(reason,
            L"ListView is virtual, owner-draw, label-editable, headerless, or not in report view");
    }
    if (SendMessageW(hwnd, LVM_ISGROUPVIEWENABLED, 0, 0) != FALSE) {
        return Reject(reason, L"grouped ListView is not supported");
    }
    constexpr DWORD unsupportedExtended = LVS_EX_CHECKBOXES | LVS_EX_HEADERDRAGDROP |
        LVS_EX_TRACKSELECT | LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE;
    const auto extended = static_cast<DWORD>(
        SendMessageW(hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
    if ((extended & unsupportedExtended) != 0) {
        return Reject(reason,
            L"ListView requires unsupported checkbox, header-drag, or activation semantics");
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

using ProbeFn = bool (*)(HWND, DWORD, ControlKind&, std::wstring&);

struct ClassAdapter final {
    std::wstring_view className;
    ProbeFn probe;
};

// The registry. Class names select a candidate adapter; only its probe can
// establish support.
constexpr std::array kClassAdapters{
    ClassAdapter{ L"Static", &ProbeStatic },
    ClassAdapter{ L"Button", &ProbeButton },
    ClassAdapter{ L"Edit", &ProbeEdit },
    ClassAdapter{ L"ComboBox", &ProbeComboBox },
    ClassAdapter{ L"ListBox", &ProbeListBox },
    ClassAdapter{ PROGRESS_CLASSW, &ProbeProgressBar },
    ClassAdapter{ WC_LINK, &ProbeSysLink },
    ClassAdapter{ WC_LISTVIEWW, &ProbeListView },
    ClassAdapter{ STATUSCLASSNAMEW, &ProbeStatusBar },
};

// ---------------------------------------------------------------------------
// Capture(HWND) -> canonical typed state
//
// One function per projected kind, reading only what that kind adds on top of
// the facets every control shares.
// ---------------------------------------------------------------------------

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

bool CaptureListViewState(HWND hwnd, ControlNode& node, std::wstring& reason) {
    const LRESULT count = SendMessageW(hwnd, LVM_GETITEMCOUNT, 0, 0);
    if (count < 0 || count > static_cast<LRESULT>(Ipc::kMaxListItems)) {
        return Reject(reason, L"invalid or excessive ListView item count");
    }
    const HWND header = reinterpret_cast<HWND>(SendMessageW(hwnd, LVM_GETHEADER, 0, 0));
    const int columnCount = header && IsWindow(header) ? Header_GetItemCount(header) : 0;
    if (columnCount <= 0 || static_cast<size_t>(columnCount) > kMaxListViewColumns) {
        return Reject(reason, L"ListView has no bounded report columns");
    }
    if (!ValidateListViewColumns(hwnd, header, columnCount, reason)) return false;

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
    if (node.maximum <= node.minimum || node.position < node.minimum ||
        node.position > node.maximum) {
        return Reject(reason, L"ProgressBar has an invalid native range or position");
    }
    return true;
}

using CaptureFn = bool (*)(HWND, ControlNode&, std::wstring&);

// Sized from the last ControlKind enumerator.  A kind declared past StatusBar
// falls outside the table and is rejected by CaptureControlDetail rather than
// silently capturing nothing, so the failure is loud but never unsafe.
constexpr size_t kControlKindCount = static_cast<size_t>(ControlKind::StatusBar) + 1;

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
    at(ControlKind::RadioButton) = &CaptureToggleState;
    at(ControlKind::Edit) = &CaptureTextEntryState;
    at(ControlKind::Password) = &CaptureTextEntryState;
    at(ControlKind::ComboBox) = &CaptureStringListState;
    at(ControlKind::ListBox) = &CaptureStringListState;
    at(ControlKind::ProgressBar) = &CaptureProgressState;
    at(ControlKind::SysLink) = &CaptureSysLinkState;
    at(ControlKind::ListView) = &CaptureListViewState;
    at(ControlKind::StatusBar) = &CaptureStatusBarState;
    // StaticText, Separator, and GroupBox are fully described by the common
    // facets.  TreeView, TabControl, and Slider have no adapter yet, so no
    // probe can produce them.
    return table;
}

constexpr auto kCaptureTable = MakeCaptureTable();

} // namespace

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
    if (!FluentShell::EqualsIgnoreCase(parentClass, WC_LISTVIEWW)) return false;
    wchar_t childBuffer[kMaxClassNameChars]{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(hwnd, childBuffer), WC_HEADERW);
}

} // namespace FluentShell::Bridge::Translation
