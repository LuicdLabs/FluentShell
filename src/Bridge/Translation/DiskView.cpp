#include "DiskView.h"
#include "AccessibleIsland.h"
#include "GuiThreadDispatch.h"
#include "../../Common/FluentShell.h"

#include <oleacc.h>
#include <wrl/client.h>
#include <algorithm>
#include <unordered_set>

namespace FluentShell::Bridge::Translation {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kMaxDiskItems = 256;
constexpr size_t kMaxDiskText = 256 * 1024;

struct Text final {
    BSTR value = nullptr;
    ~Text() { SysFreeString(value); }
    std::wstring String() const { return value ? std::wstring(value, SysStringLen(value)) : L""; }
};
struct Variant final {
    VARIANT value{};
    ~Variant() { VariantClear(&value); }
};
VARIANT Child(long id) { VARIANT value{}; value.vt = VT_I4; value.lVal = id; return value; }
bool Reject(std::wstring& reason, std::wstring_view detail) {
    reason = L"Disk Management view: "; reason += detail; return false;
}
bool Number(IAccessible* object, long id, bool role, long& number) {
    Variant value;
    const HRESULT hr = role ? object->get_accRole(Child(id), &value.value) :
        object->get_accState(Child(id), &value.value);
    if (FAILED(hr) || value.value.vt != VT_I4) return false;
    number = value.value.lVal; return true;
}
bool Bounded(const std::wstring& value, bool empty = false) {
    return (empty || !value.empty()) && value.size() <= 65536 &&
        value.find(L'\0') == std::wstring::npos;
}
bool NamedHost(HWND window, const wchar_t* name) {
    wchar_t text[64]{}, className[64]{};
    if (!window || GetWindowTextW(window, text, 64) <= 0 || wcscmp(text, name) != 0 ||
        !GetClassNameW(window, className, 64) || wcscmp(className, L"AfxWnd42u") != 0) return false;
    // MFC may register a shared class from its runtime. Pin the OLE control
    // ancestor's Windows module instead of trusting a reusable MFC class name.
    HWND host = window;
    for (int depth = 0; host && depth < 4; ++depth, host = GetParent(host))
        if (WindowClassRegisteredBySystemModule(host, L"dmview.ocx")) return true;
    return false;
}
bool Open(HWND window, ComPtr<IAccessible>& object, RECT& screen, std::wstring& reason) {
    if (!IsDiskManagementView(window) || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        return Reject(reason, L"native window provenance or GUI-thread ownership is unavailable");
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        if (IsWindowVisible(child)) return Reject(reason, L"graphical view also owns visible child windows");
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin) ||
        client.right <= 0 || client.bottom <= 0 ||
        client.right > Ipc::kMaxCoordinate || client.bottom > Ipc::kMaxCoordinate)
        return Reject(reason, L"client viewport is unavailable or exceeds its bound");
    screen = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    if (FAILED(AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT),
            IID_PPV_ARGS(&object))) || !object)
        return Reject(reason, L"native accessibility table is unavailable");
    return true;
}
bool ReadScroll(HWND window, DiskViewSnapshot& snapshot, std::wstring& reason) {
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
    if ((style & WS_HSCROLL) && GetScrollInfo(window, SB_HORZ, &horizontal) &&
        static_cast<int64_t>(horizontal.nMax) - horizontal.nMin + 1 > horizontal.nPage)
        return Reject(reason, L"horizontal graphical scrolling is not represented");
    snapshot.scrollMinimum = snapshot.scrollMaximum = snapshot.scrollPosition = 0;
    snapshot.scrollPage = 0;
    snapshot.scrollRect = {};
    SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS};
    SCROLLBARINFO bar{sizeof(bar)};
    if (!(style & WS_VSCROLL) || !GetScrollBarInfo(window, OBJID_VSCROLL, &bar) ||
        (bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) return true;
    if (!GetScrollInfo(window, SB_VERT, &vertical))
        return Reject(reason, L"native vertical scroll range is unavailable");
    const int64_t maximum = (std::max)(static_cast<int64_t>(vertical.nMin),
        static_cast<int64_t>(vertical.nMax) - (std::max)(static_cast<int64_t>(vertical.nPage) - 1, int64_t{0}));
    if (vertical.nMin < 0 || maximum > 65535 || vertical.nPos < vertical.nMin || vertical.nPos > maximum)
        return Reject(reason, L"native scroll range exceeds the WM_VSCROLL position contract");
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect)) return Reject(reason, L"scrollbar geometry is unavailable");
    snapshot.scrollRect = bar.rcScrollBar;
    OffsetRect(&snapshot.scrollRect, -windowRect.left, -windowRect.top);
    snapshot.scrollMinimum = vertical.nMin;
    snapshot.scrollMaximum = static_cast<int>(maximum);
    snapshot.scrollPosition = vertical.nPos;
    snapshot.scrollPage = static_cast<int>((std::min)(vertical.nPage, UINT{65535}));
    return true;
}
}

bool IsDiskManagementView(HWND window) noexcept { return NamedHost(window, L"DMDiskView"); }
bool IsDiskManagementLegend(HWND window) noexcept { return NamedHost(window, L"DMLegend"); }

static bool ReadDiskItemsOnce(IAccessible* table, const RECT& screenClient,
    std::vector<DiskViewItemSnapshot>& items, std::wstring& name, std::wstring& reason) noexcept {
    try {
        items.clear(); name.clear();
        long role = 0, state = 0, count = 0;
        if (!table || !Number(table, CHILDID_SELF, true, role) || role != ROLE_SYSTEM_TABLE ||
            !Number(table, CHILDID_SELF, false, state) ||
            (state & (STATE_SYSTEM_MULTISELECTABLE | STATE_SYSTEM_EXTSELECTABLE)) != 0 ||
            FAILED(table->get_accChildCount(&count)) || count <= 0 || count > kMaxDiskItems)
            return Reject(reason, L"provider is not a complete bounded accessibility table");
        Text rootName;
        if (FAILED(table->get_accName(Child(CHILDID_SELF), &rootName.value)) ||
            !Bounded(name = rootName.String()))
            return Reject(reason, L"table name is unavailable or exceeds its bound");
        std::vector<VARIANT> children(static_cast<size_t>(count));
        long obtained = 0;
        const HRESULT enumeration = AccessibleChildren(table, 0, count, children.data(), &obtained);
        std::vector<long> ids;
        bool simple = SUCCEEDED(enumeration) && obtained == count;
        for (auto& child : children) {
            simple = simple && child.vt == VT_I4 && child.lVal > 0;
            if (child.vt == VT_I4) ids.push_back(child.lVal);
            VariantClear(&child);
        }
        if (!simple) return Reject(reason, L"table membership is incomplete or lacks native child IDs");
        std::unordered_set<long> unique;
        size_t textSize = name.size();
        long currentDisk = 0;
        size_t selections = 0;
        for (long id : ids) {
            if (!unique.insert(id).second || !Number(table, id, true, role) ||
                (role != ROLE_SYSTEM_ROWHEADER && role != ROLE_SYSTEM_CELL) ||
                !Number(table, id, false, state) || (state & STATE_SYSTEM_INVISIBLE) ||
                !(state & STATE_SYSTEM_SELECTABLE))
                return Reject(reason, L"disk/partition membership, role, or selection contract is unavailable");
            DiskViewItemSnapshot item;
            item.itemId = static_cast<uint32_t>(id);
            item.kind = role == ROLE_SYSTEM_ROWHEADER ? L"disk" : L"partition";
            if (role == ROLE_SYSTEM_ROWHEADER) currentDisk = id;
            if (!currentDisk) return Reject(reason, L"partition precedes its native disk header");
            item.diskId = static_cast<uint32_t>(currentDisk);
            Text label, value, action;
            if (FAILED(table->get_accName(Child(id), &label.value)) ||
                FAILED(table->get_accValue(Child(id), &value.value)) ||
                !Bounded(item.name = label.String()) || !Bounded(item.value = value.String()))
                return Reject(reason, L"disk/partition labels or values are unavailable");
            if (SUCCEEDED(table->get_accDefaultAction(Child(id), &action.value)) && !action.String().empty())
                return Reject(reason, L"disk/partition provider exposes an additional unrepresented default action");
            textSize += item.name.size() + item.value.size();
            if (textSize > kMaxDiskText) return Reject(reason, L"table text exceeds the aggregate bound");
            long x = 0, y = 0, width = 0, height = 0;
            if (FAILED(table->accLocation(&x, &y, &width, &height, Child(id))) || width <= 0 || height <= 0 ||
                width > Ipc::kMaxCoordinate || height > Ipc::kMaxCoordinate ||
                std::abs(static_cast<int64_t>(x) - screenClient.left) > Ipc::kMaxCoordinate ||
                std::abs(static_cast<int64_t>(y) - screenClient.top) > Ipc::kMaxCoordinate)
                return Reject(reason, L"disk/partition native rectangle is unavailable or exceeds its bound");
            const LONG left = x - screenClient.left, top = y - screenClient.top;
            item.rect = {left, top, left + width, top + height};
            item.enabled = (state & STATE_SYSTEM_UNAVAILABLE) == 0;
            item.selected = (state & STATE_SYSTEM_SELECTED) != 0;
            if (item.selected && ++selections > 1)
                return Reject(reason, L"multiple native selections require an additional selection contract");
            item.focused = (state & STATE_SYSTEM_FOCUSED) != 0;
            items.push_back(std::move(item));
        }
        if (FAILED(table->get_accChildCount(&count)) || count != static_cast<long>(items.size()))
            return Reject(reason, L"native table membership changed during capture");
        return true;
    } catch (...) { return Reject(reason, L"accessibility table capture failed"); }
}

bool DiskViewDetail::ReadItems(IAccessible* table, const RECT& screenClient,
    std::vector<DiskViewItemSnapshot>& items, std::wstring& name, std::wstring& reason) noexcept {
    std::vector<DiskViewItemSnapshot> confirmation;
    std::wstring confirmedName;
    if (!ReadDiskItemsOnce(table, screenClient, items, name, reason) ||
        !ReadDiskItemsOnce(table, screenClient, confirmation, confirmedName, reason)) return false;
    if (name != confirmedName || items.size() != confirmation.size())
        return Reject(reason, L"table identity changed during capture");
    for (size_t index = 0; index < items.size(); ++index) {
        const auto& before = items[index];
        const auto& after = confirmation[index];
        if (before.itemId != after.itemId || before.diskId != after.diskId || before.kind != after.kind ||
            before.name != after.name || before.value != after.value || before.selected != after.selected ||
            before.focused != after.focused || before.enabled != after.enabled || !EqualRect(&before.rect, &after.rect))
            return Reject(reason, L"table content changed during capture");
    }
    return true;
}

bool DiskViewDetail::SelectItem(IAccessible* table,
    const std::vector<DiskViewItemSnapshot>& expected, int index,
    const RECT& screenClient, std::wstring& reason) noexcept {
    try {
        if (index < 0 || static_cast<size_t>(index) >= expected.size())
            return Reject(reason, L"selection names no published native item");
        std::vector<DiskViewItemSnapshot> current;
        std::wstring name;
        if (!ReadItems(table, screenClient, current, name, reason) || current.size() != expected.size()) return false;
        for (size_t at = 0; at < current.size(); ++at)
            if (current[at].itemId != expected[at].itemId || current[at].diskId != expected[at].diskId ||
                current[at].kind != expected[at].kind || current[at].name != expected[at].name ||
                current[at].value != expected[at].value)
                return Reject(reason, L"published native item identity changed before selection");
        const auto& target = current[static_cast<size_t>(index)];
        if (!target.enabled || GuiThreadOperationCancelled() ||
            FAILED(table->accSelect(SELFLAG_TAKESELECTION, Child(target.itemId))))
            return Reject(reason, L"native provider declined item selection");
        long state = 0;
        if (!Number(table, target.itemId, false, state) || !(state & STATE_SYSTEM_SELECTED))
            return Reject(reason, L"native provider did not retain the requested selection");
        return true;
    } catch (...) { return Reject(reason, L"native selection failed"); }
}

bool ReadDiskView(HWND window, DiskViewSnapshot& snapshot, std::wstring& reason) noexcept {
    try {
        snapshot = {};
        ComPtr<IAccessible> table;
        RECT screen{};
        if (!Open(window, table, screen, reason) ||
            !DiskViewDetail::ReadItems(table.Get(), screen, snapshot.items, snapshot.name, reason) ||
            !ReadScroll(window, snapshot, reason)) return false;
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return Reject(reason, L"native view geometry disappeared");
        snapshot.clientRect = {screen.left - bounds.left, screen.top - bounds.top,
            screen.right - bounds.left, screen.bottom - bounds.top};
        return true;
    } catch (...) { return Reject(reason, L"native view capture failed"); }
}
bool SelectDiskViewItem(HWND window, const DiskViewSnapshot& expected,
    int index, std::wstring& reason) noexcept {
    ComPtr<IAccessible> table;
    RECT screen{};
    return Open(window, table, screen, reason) &&
        DiskViewDetail::SelectItem(table.Get(), expected.items, index, screen, reason);
}
bool RequestDiskViewContextMenu(HWND window, const DiskViewSnapshot& expected,
    int index, std::wstring& reason) noexcept {
    if (!SelectDiskViewItem(window, expected, index, reason) || GuiThreadOperationCancelled()) return false;
    SendMessageW(window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), MAKELPARAM(-1, -1));
    return true;
}
bool ScrollDiskView(HWND window, const DiskViewSnapshot& expected,
    int position, std::wstring& reason) noexcept {
    if (position < expected.scrollMinimum || position > expected.scrollMaximum ||
        !IsDiskManagementView(window) || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        return Reject(reason, L"scroll position or GUI-thread ownership is invalid");
    DiskViewSnapshot current;
    if (!ReadScroll(window, current, reason) || current.scrollMinimum != expected.scrollMinimum ||
        current.scrollMaximum != expected.scrollMaximum || current.scrollPage != expected.scrollPage)
        return Reject(reason, L"native scroll range changed before the request");
    if (GuiThreadOperationCancelled()) return Reject(reason, L"native scroll request was cancelled");
    SendMessageW(window, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, position), 0);
    SendMessageW(window, WM_VSCROLL, MAKEWPARAM(SB_ENDSCROLL, 0), 0);
    if (!ReadScroll(window, current, reason) || current.scrollPosition != position)
        return Reject(reason, L"native view declined the scroll position");
    return true;
}
}
