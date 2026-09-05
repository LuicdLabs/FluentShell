#include "MenuBarCapture.h"

#include "../../Common/FluentShell.h"
#include "WindowCapture.h"

#include <oleacc.h>
#include <wrl/client.h>

#include <commctrl.h>

#include <algorithm>
#include <atomic>

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

// A menu bar is small by construction: a bar with more top-level menus than this is
// not the shape this lane exists for.
constexpr int kMaxMenuBarButtons = 24;

// Per thread, because interception has to bracket exactly the call that drives one
// button.  A popup the application opens for its own reasons on another thread must
// still be shown by whatever path already handles it.
thread_local int g_interceptionDepth = 0;
thread_local HMENU g_recordedPopup = nullptr;
// What the hook captured out of the popup while it was still alive, plus the identity
// prefix the arming call asked for.
thread_local std::wstring g_recordedPath;
thread_local std::vector<MenuItemSnapshot> g_recordedItems;
thread_local std::wstring g_recordedReason;
thread_local bool g_recordedSystemMenu = false;
thread_local bool g_recordedAny = false;
// Process-wide, and deliberately not per thread: an application can open its popup from
// a different thread than the one that was asked, and a popup that escapes is a native
// window over the projection.
std::atomic<int> g_suppressionDepth{ 0 };

// The accessible object a toolbar publishes for itself, plus its child count.
bool ToolbarAccessible(HWND toolbar, ComPtr<IAccessible>& accessible, long& children) noexcept {
    accessible.Reset();
    children = 0;
    if (!toolbar || !IsWindow(toolbar)) return false;
    if (FAILED(AccessibleObjectFromWindow(toolbar, OBJID_CLIENT,
            IID_PPV_ARGS(accessible.GetAddressOf()))) || !accessible) {
        return false;
    }
    return SUCCEEDED(accessible->get_accChildCount(&children)) && children > 0;
}

long ChildRole(IAccessible* accessible, long child) noexcept {
    VARIANT id{};
    VariantInit(&id);
    id.vt = VT_I4;
    id.lVal = child;
    VARIANT role{};
    VariantInit(&role);
    const bool ok = SUCCEEDED(accessible->get_accRole(id, &role)) && role.vt == VT_I4;
    const long value = ok ? role.lVal : 0;
    VariantClear(&role);
    return value;
}

std::wstring ChildName(IAccessible* accessible, long child) noexcept {
    VARIANT id{};
    VariantInit(&id);
    id.vt = VT_I4;
    id.lVal = child;
    BSTR raw = nullptr;
    std::wstring name;
    if (SUCCEEDED(accessible->get_accName(id, &raw)) && raw) name.assign(raw);
    if (raw) SysFreeString(raw);
    return name;
}

// Performs one top-level button's own default action with popups intercepted, and
// returns the HMENU the application opened for it.
HMENU OpenPopupForButton(IAccessible* accessible, long child) noexcept {
    PopupInterceptionScope scope;
    VARIANT id{};
    VariantInit(&id);
    id.vt = VT_I4;
    id.lVal = child;
    if (FAILED(accessible->accDoDefaultAction(id))) return nullptr;
    return scope.Recorded();
}

} // namespace

bool PopupInterceptionArmed() noexcept {
    return g_interceptionDepth > 0;
}

bool PopupSuppressionActive() noexcept {
    return g_suppressionDepth.load(std::memory_order_acquire) > 0;
}

PopupSuppressionScope::PopupSuppressionScope() noexcept {
    g_suppressionDepth.fetch_add(1, std::memory_order_acq_rel);
}

PopupSuppressionScope::~PopupSuppressionScope() {
    g_suppressionDepth.fetch_sub(1, std::memory_order_acq_rel);
}

void RecordInterceptedPopup(HMENU menu) noexcept {
    if (g_interceptionDepth <= 0 || g_recordedAny) return;
    g_recordedAny = true;
    g_recordedPopup = menu;
    try {
        FluentShell::Log(L"Intercepted a popup menu for the menu-bar read");
    } catch (...) {}
    // Captured here, inside the tracking call, because the application frees the popup as
    // soon as that call answers.
    if (IsWindowSystemMenu(menu)) {
        g_recordedSystemMenu = true;
        return;
    }
    try {
        if (!CaptureMenuHandle(menu, g_recordedPath, g_recordedItems, g_recordedReason)) {
            g_recordedItems.clear();
        }
    } catch (...) {
        g_recordedItems.clear();
        try { g_recordedReason = L"exception while capturing an intercepted menu"; }
        catch (...) {}
    }
}

PopupInterceptionScope::PopupInterceptionScope() noexcept {
    ++g_interceptionDepth;
    g_recordedPopup = nullptr;
}

PopupInterceptionScope::~PopupInterceptionScope() {
    if (g_interceptionDepth > 0) --g_interceptionDepth;
}

HMENU PopupInterceptionScope::Recorded() const noexcept {
    return g_recordedPopup;
}

void ArmPopupInterception(std::wstring itemIdPath) noexcept {
    ++g_interceptionDepth;
    g_recordedPopup = nullptr;
    g_recordedAny = false;
    g_recordedSystemMenu = false;
    g_recordedItems.clear();
    g_recordedReason.clear();
    try {
        g_recordedPath = std::move(itemIdPath);
    } catch (...) {
        g_recordedPath.clear();
    }
}

void DisarmPopupInterception() noexcept {
    if (g_interceptionDepth > 0) --g_interceptionDepth;
    g_recordedPopup = nullptr;
    g_recordedAny = false;
    g_recordedSystemMenu = false;
    g_recordedItems.clear();
    g_recordedReason.clear();
}

bool MenuPopupRecorded() noexcept {
    return g_recordedAny;
}

bool TakeRecordedMenu(
    std::vector<MenuItemSnapshot>& items,
    bool& systemMenu,
    std::wstring& reason) noexcept {
    systemMenu = g_recordedSystemMenu;
    reason = g_recordedReason;
    items = std::move(g_recordedItems);
    g_recordedItems.clear();
    return g_recordedAny;
}

int MenuBarButtonCount(HWND toolbar) noexcept {
    ComPtr<IAccessible> accessible;
    long children = 0;
    if (!ToolbarAccessible(toolbar, accessible, children)) return 0;
    if (children > kMaxMenuBarButtons) return 0;
    return static_cast<int>(children);
}

std::wstring MenuBarButtonName(HWND toolbar, int index) noexcept {
    ComPtr<IAccessible> accessible;
    long children = 0;
    if (!ToolbarAccessible(toolbar, accessible, children)) return {};
    if (index < 1 || index > children) return {};
    if (ChildRole(accessible.Get(), index) != ROLE_SYSTEM_MENUITEM) return {};
    return ChildName(accessible.Get(), index);
}

bool DriveMenuBarButton(
    HWND toolbar,
    int index,
    std::wstring itemIdPath,
    std::wstring& reason) noexcept {
    ComPtr<IAccessible> accessible;
    long children = 0;
    if (!ToolbarAccessible(toolbar, accessible, children)) {
        reason = L"menu-bar toolbar publishes no accessible buttons";
        return false;
    }
    if (index < 1 || index > children) {
        reason = L"menu-bar toolbar index is outside its published buttons";
        return false;
    }
    if (ChildRole(accessible.Get(), index) != ROLE_SYSTEM_MENUITEM) {
        reason = L"menu-bar toolbar child is not published as a menu item";
        return false;
    }
    // Armed before the drive and left armed: the application opens its popup from its own
    // message loop, so the hook captures it after this call has already returned.
    ArmPopupInterception(std::move(itemIdPath));
    VARIANT id{};
    VariantInit(&id);
    id.vt = VT_I4;
    id.lVal = index;
    if (FAILED(accessible->accDoDefaultAction(id))) {
        DisarmPopupInterception();
        reason = L"menu-bar toolbar refused its own default action";
        return false;
    }
    return true;
}

bool IsMenuBarToolbar(HWND toolbar) noexcept {
    ComPtr<IAccessible> accessible;
    long children = 0;
    if (!ToolbarAccessible(toolbar, accessible, children)) return false;
    if (children > kMaxMenuBarButtons) return false;
    // A menu bar draws words; an icon toolbar draws pictures.  A control that owns any
    // image is the second kind whatever its accessible role says, and driving one of its
    // buttons would run a command rather than open a menu.  This is the guard that keeps
    // the menu-bar lane away from ordinary toolbars.
    for (int listId = 0; listId <= 1; ++listId) {
        const auto list = reinterpret_cast<HIMAGELIST>(SendMessageW(
            toolbar, TB_GETIMAGELIST, static_cast<WPARAM>(listId), 0));
        if (list && ImageList_GetImageCount(list) > 0) return false;
    }
    // Every button has to agree: a bar that mixes menu items with push buttons is a
    // toolbar with one odd child, not a menu bar.  Each also has to carry a label, because
    // that label is the only thing the projected menu can show.
    for (long child = 1; child <= children; ++child) {
        if (ChildRole(accessible.Get(), child) != ROLE_SYSTEM_MENUITEM) return false;
        if (ChildName(accessible.Get(), child).empty()) return false;
    }
    return true;
}

bool IsWindowSystemMenu(HMENU menu) noexcept {
    if (!menu || !IsMenu(menu)) return false;
    const int count = GetMenuItemCount(menu);
    if (count <= 0) return false;
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW info{ sizeof(info) };
        info.fMask = MIIM_ID | MIIM_FTYPE;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) continue;
        if ((info.fType & MFT_SEPARATOR) != 0) continue;
        // SC_* commands live at 0xF000 and above and are delivered as WM_SYSCOMMAND.
        if (info.wID >= 0xF000) return true;
    }
    return false;
}

HWND FindMenuBarToolbar(HWND root) noexcept {
    struct Search final {
        HWND found = nullptr;
    } search;
    EnumChildWindows(root, [](HWND child, LPARAM param) -> BOOL {
        if (!IsWindowVisible(child)) return TRUE;
        wchar_t buffer[64]{};
        GetClassNameW(child, buffer, static_cast<int>(std::size(buffer)));
        if (_wcsicmp(buffer, TOOLBARCLASSNAMEW) != 0) return TRUE;
        if (!IsMenuBarToolbar(child)) return TRUE;
        reinterpret_cast<Search*>(param)->found = child;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool CaptureMenuBarToolbar(
    HWND toolbar,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& reason) {
    menu.clear();
    ComPtr<IAccessible> accessible;
    long children = 0;
    if (!ToolbarAccessible(toolbar, accessible, children)) {
        reason = L"menu-bar toolbar publishes no accessible buttons";
        return false;
    }
    if (children > kMaxMenuBarButtons) {
        reason = L"menu-bar toolbar has more top-level menus than the bounded adapter";
        return false;
    }
    for (long child = 1; child <= children; ++child) {
        if (ChildRole(accessible.Get(), child) != ROLE_SYSTEM_MENUITEM) {
            reason = L"menu-bar toolbar child is not published as a menu item";
            return false;
        }
        std::wstring name = ChildName(accessible.Get(), child);
        if (name.empty()) {
            reason = L"menu-bar toolbar publishes an unnamed top-level menu";
            return false;
        }
        const HMENU popup = OpenPopupForButton(accessible.Get(), child);
        if (!popup || !IsMenu(popup)) {
            reason = L"menu-bar toolbar button opened no readable popup";
            return false;
        }
        MenuItemSnapshot top;
        top.kind = MenuItemKind::Popup;
        top.text = std::move(name);
        top.enabled = true;
        top.itemId = L"menubar." + std::to_wstring(child);
        if (!CaptureMenuHandle(popup, top.itemId, top.items, reason)) return false;
        if (top.items.empty()) {
            reason = L"menu-bar toolbar popup has no projectable items";
            return false;
        }
        menu.push_back(std::move(top));
    }
    if (menu.empty()) {
        reason = L"menu-bar toolbar has no projectable menus";
        return false;
    }
    return true;
}

} // namespace FluentShell::Bridge::Translation
