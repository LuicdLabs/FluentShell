#include "MenuBarCapture.h"

#include "../../Common/FluentShell.h"
#include "WindowCapture.h"

#include <oleacc.h>
#include <wrl/client.h>

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <unordered_set>

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

// A menu bar is small by construction: a bar with more top-level menus than this is
// not the shape this lane exists for.
constexpr int kMaxMenuBarButtons = 24;
constexpr size_t kMaxProjectedMenuItems = 256;
constexpr size_t kMaxProjectedMenuDepth = 8;
constexpr UINT kTrackingCommandFlags = TPM_RETURNCMD | TPM_NONOTIFY;
constexpr wchar_t kReadOwnerProperty[] = L"FluentShell.Bridge.MenuBarReadOwner";

// Per thread, because interception has to bracket exactly the call that drives one
// button.  A popup the application opens for its own reasons on another thread must
// still be shown by whatever path already handles it.
thread_local int g_interceptionDepth = 0;
thread_local bool g_menuBarReadInProgress = false;
// What the hook captured out of the popup while it was still alive, plus the identity
// prefix the arming call asked for.
thread_local std::wstring g_recordedPath;
thread_local std::vector<MenuItemSnapshot> g_recordedItems;
thread_local std::wstring g_recordedReason;
thread_local bool g_recordedSystemMenu = false;
thread_local bool g_recordedAny = false;
thread_local HWND g_recordedOwner = nullptr;
thread_local UINT g_recordedFlags = 0;
thread_local const MenuBarCommandBinding* g_selectedCommand = nullptr;
thread_local const std::atomic<bool>* g_selectionCancelled = nullptr;
thread_local ULONGLONG g_selectionDeadline = 0;
thread_local HWND g_selectionRoot = nullptr;
thread_local HWND g_selectionToolbar = nullptr;
thread_local bool g_selectionAccepted = false;
thread_local void (*g_finishSelection)(void*) noexcept = nullptr;
thread_local void* g_selectionScope = nullptr;
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

bool MenuBarAncestorsReady(HWND root, HWND toolbar) noexcept {
    if (!IsWindowVisible(root) || !IsWindowVisible(toolbar)) return false;
    for (HWND current = toolbar; current; current = GetParent(current)) {
        if (!IsWindowEnabled(current)) return false;
        if (current == root) return true;
    }
    return false;
}

bool MatchesSelectedCommand(const std::vector<MenuItemSnapshot>& items,
    const MenuBarCommandBinding& command, size_t depth = 0) noexcept {
    if (depth >= kMaxProjectedMenuDepth) return false;
    for (const auto& item : items) {
        if (!item.enabled) continue;
        if (item.kind == MenuItemKind::Command && item.itemId == command.itemId)
            return item.commandId == command.nativeCommandId && item.text == command.text &&
                depth == command.ancestors.size();
        if (item.kind == MenuItemKind::Popup && depth < command.ancestors.size() &&
            item.text == command.ancestors[depth] &&
            MatchesSelectedCommand(item.items, command, depth + 1)) return true;
    }
    return false;
}

bool BindMenuCommands(std::vector<MenuItemSnapshot>& items, int toolbarIndex,
    HWND owner, UINT flags, std::vector<MenuBarCommandBinding>& commands,
    std::vector<std::wstring>& ancestors, size_t& count, size_t depth) {
    if (depth > kMaxProjectedMenuDepth) return false;
    for (auto& item : items) {
        if (++count > kMaxProjectedMenuItems) return false;
        if (item.kind == MenuItemKind::Command) {
            MenuBarCommandBinding binding;
            binding.commandId = item.commandId;
            binding.nativeCommandId = item.commandId;
            binding.toolbarIndex = toolbarIndex;
            binding.itemId = item.itemId;
            binding.text = item.text;
            binding.ancestors = ancestors;
            binding.owner = owner;
            binding.trackingFlags = flags & kTrackingCommandFlags;
            commands.push_back(std::move(binding));
        } else if (item.kind == MenuItemKind::Popup) {
            ancestors.push_back(item.text);
            const bool valid = BindMenuCommands(item.items, toolbarIndex, owner, flags,
                commands, ancestors, count, depth + 1);
            ancestors.pop_back();
            if (!valid) return false;
        }
    }
    return true;
}

void AssignProjectedCommandIds(std::vector<MenuItemSnapshot>& menu,
    std::vector<MenuBarCommandBinding>& commands) {
    std::unordered_set<uint32_t> reserved;
    for (const auto& command : commands) reserved.insert(command.nativeCommandId);
    std::unordered_set<uint32_t> used;
    uint32_t available = 1;
    size_t remapped = 0;
    size_t index = 0;
    const auto assign = [&](const auto& self, std::vector<MenuItemSnapshot>& items) -> void {
        for (auto& item : items) {
            if (item.kind == MenuItemKind::Command) {
                if (!used.insert(item.commandId).second) {
                    while (reserved.contains(available) || used.contains(available)) ++available;
                    item.commandId = available++;
                    used.insert(item.commandId);
                    ++remapped;
                }
                commands[index++].commandId = item.commandId;
            }
            self(self, item.items);
        }
    };
    assign(assign, menu);
    if (remapped) FluentShell::Log(L"Menu-bar scoped " + std::to_wstring(remapped) +
        L" reused native command ID(s) to their original popup bindings");
}

} // namespace

bool PopupInterceptionArmed() noexcept {
    return g_interceptionDepth > 0;
}

bool PopupSuppressionActive() noexcept {
    return g_suppressionDepth.load(std::memory_order_acquire) > 0;
}

bool ShouldSuppressPopup(HWND owner) noexcept {
    if (PopupSuppressionActive()) return true;
    const HWND root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    DWORD process = 0;
    if (!root || GetWindowThreadProcessId(root, &process) != GetCurrentThreadId() ||
        process != GetCurrentProcessId() || !GetPropW(root, kReadOwnerProperty)) return false;
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        (cloaked & DWM_CLOAKED_APP) != 0;
}

PopupSuppressionScope::PopupSuppressionScope() noexcept {
    g_suppressionDepth.fetch_add(1, std::memory_order_acq_rel);
}

PopupSuppressionScope::~PopupSuppressionScope() {
    g_suppressionDepth.fetch_sub(1, std::memory_order_acq_rel);
}

BOOL RecordInterceptedPopup(HMENU menu, UINT flags, HWND owner) noexcept {
    if (g_interceptionDepth <= 0 || g_recordedAny) return FALSE;
    g_recordedAny = true;
    g_recordedOwner = owner;
    g_recordedFlags = flags;
    try {
        FluentShell::Log(L"Intercepted a popup menu for the menu-bar read");
    } catch (...) {}
    // Captured here, inside the tracking call, because the application frees the popup as
    // soon as that call answers.
    if (IsWindowSystemMenu(menu)) {
        g_recordedSystemMenu = true;
        return FALSE;
    }
    try {
        // Duplicate native IDs are safe only because the complete toolbar bar gets
        // unique projected IDs and retains one binding for every native item path.
        if (!CaptureMenuHandle(menu, g_recordedPath, g_recordedItems, g_recordedReason, false)) {
            g_recordedItems.clear();
        }
        if (g_selectedCommand) {
            if (!g_selectionCancelled || g_selectionCancelled->load(std::memory_order_acquire) ||
                GetTickCount64() >= g_selectionDeadline || !IsWindow(owner) ||
                !MenuBarAncestorsReady(g_selectionRoot, g_selectionToolbar) ||
                GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId() ||
                owner != g_selectedCommand->owner ||
                (flags & kTrackingCommandFlags) != g_selectedCommand->trackingFlags ||
                !MatchesSelectedCommand(g_recordedItems, *g_selectedCommand)) {
                g_recordedReason = L"menu command no longer matches its native tracking context";
                return FALSE;
            }
            const auto commandId = g_selectedCommand->nativeCommandId;
            const auto finish = []() noexcept {
                const auto callback = g_finishSelection;
                void* scope = g_selectionScope;
                g_finishSelection = nullptr;
                g_selectionScope = nullptr;
                if (callback) callback(scope);
            };
            if ((flags & TPM_RETURNCMD) != 0) {
                g_selectionAccepted = true;
                finish();
                return static_cast<BOOL>(commandId);
            }
            if ((flags & TPM_NONOTIFY) == 0) {
                g_selectionAccepted = PostMessageW(owner, WM_COMMAND, MAKEWPARAM(commandId, 0), 0) != FALSE;
                if (!g_selectionAccepted) g_recordedReason = L"native tracking owner refused the menu command";
                if (!g_selectionAccepted) return FALSE;
                finish();
                return TRUE;
            }
            g_selectionAccepted = true;
            finish();
            return TRUE;
        }
    } catch (...) {
        g_recordedItems.clear();
        try { g_recordedReason = L"exception while capturing an intercepted menu"; }
        catch (...) {}
    }
    return FALSE;
}

void ArmPopupInterception(std::wstring itemIdPath) noexcept {
    ++g_interceptionDepth;
    g_recordedAny = false;
    g_recordedSystemMenu = false;
    g_recordedItems.clear();
    g_recordedReason.clear();
    g_recordedOwner = nullptr;
    g_recordedFlags = 0;
    g_selectionAccepted = false;
    try {
        g_recordedPath = std::move(itemIdPath);
    } catch (...) {
        g_recordedPath.clear();
    }
}

void DisarmPopupInterception() noexcept {
    if (g_interceptionDepth > 0) --g_interceptionDepth;
    g_recordedAny = false;
    g_recordedSystemMenu = false;
    g_recordedItems.clear();
    g_recordedReason.clear();
    g_selectedCommand = nullptr;
    g_selectionCancelled = nullptr;
    g_selectionDeadline = 0;
    g_selectionRoot = nullptr;
    g_selectionToolbar = nullptr;
    g_finishSelection = nullptr;
    g_selectionScope = nullptr;
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

bool ReadMenuBarButtons(HWND toolbar, std::vector<MenuBarButton>& buttons) noexcept {
    try {
        buttons.clear();
        ComPtr<IAccessible> accessible;
        long children = 0;
        if (!ToolbarAccessible(toolbar, accessible, children) ||
            children > kMaxMenuBarButtons) return false;
        for (long child = 1; child <= children; ++child) {
            if (ChildRole(accessible.Get(), child) != ROLE_SYSTEM_MENUITEM) return false;
            MenuBarButton button;
            button.text = ChildName(accessible.Get(), child);
            if (button.text.empty()) return false;
            VARIANT id{};
            id.vt = VT_I4;
            id.lVal = child;
            VARIANT state{};
            const HRESULT read = accessible->get_accState(id, &state);
            if (SUCCEEDED(read) && state.vt == VT_I4)
                button.enabled = (state.lVal & STATE_SYSTEM_UNAVAILABLE) == 0;
            VariantClear(&state);
            buttons.push_back(std::move(button));
        }
        return true;
    } catch (...) {
        buttons.clear();
        return false;
    }
}

void MenuBarRefreshPolicy::Invalidate(uint64_t now) noexcept {
    pending = true;
    attempts = 0;
    due = std::max(nextAllowed, now + 250);
}

bool MenuBarRefreshPolicy::ShouldRead(uint64_t now) const noexcept {
    return pending && now >= due;
}

void MenuBarRefreshPolicy::Complete(uint64_t now, bool success) noexcept {
    nextAllowed = now + 750;
    pending = !success && ++attempts < 3;
    due = now + 1000 * attempts;
}

bool MenuBarReadInProgress() noexcept {
    return g_menuBarReadInProgress;
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
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) continue;
        // A popup item's ID may be its HMENU value (AppendMenu's uIDNewItem)
        // or arbitrary application metadata. Only executable leaves carry SC_*
        // commands; treating a submenu handle as an ID hid ordinary nested menus.
        if ((info.fType & MFT_SEPARATOR) != 0 || info.hSubMenu) continue;
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

static bool ReadOrInvokeMenuBarToolbar(
    HWND root,
    HWND toolbar,
    const std::vector<MenuBarButton>& buttons,
    DWORD popupWaitMs,
    DWORD totalWaitMs,
    const std::atomic<bool>& cancelled,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& reason,
    std::vector<MenuBarCommandBinding>* commands,
    const MenuBarCommandBinding* selection) noexcept {
    // This scope outlives every pump, including cancellation/quit cleanup. Menus that
    // arrive late are swallowed while WM_CANCELMODE unwinds application tracking.
    struct ReadScope final {
        HWND root;
        HWND toolbar;
        HWND foreground = GetForegroundWindow();
        HWND active = GetActiveWindow();
        DWORD foreignThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        bool attached = false;
        bool quit = false;
        int quitCode = 0;
        bool released = false;
        bool accepted = false;
        std::optional<PopupSuppressionScope> suppression{std::in_place};

        ReadScope(HWND rootWindow, HWND bar) : root(rootWindow), toolbar(bar) {
            g_menuBarReadInProgress = true;
            const DWORD thread = GetCurrentThreadId();
            attached = foreignThread && foreignThread != thread &&
                AttachThreadInput(thread, foreignThread, TRUE) != FALSE;
            SetForegroundWindow(root);
            SetActiveWindow(root);
        }
        void CancelTracking() noexcept {
            if (released) return;
            DisarmPopupInterception();
            if (IsWindow(toolbar)) SendMessageW(toolbar, WM_CANCELMODE, 0, 0);
            if (IsWindow(root)) SendMessageW(root, WM_CANCELMODE, 0, 0);
        }
        void Release() noexcept {
            if (released) return;
            released = true;
            // The read-owner marker deliberately outlives this bounded read. A
            // cancelled or finished read can leave popup callbacks queued in the
            // application's own message queue, and those callbacks fire after the
            // read scope is gone. While the native root stays cloaked (that is,
            // the projection is still covering it) those late popups must stay
            // suppressed, so the marker is left in place. ShouldSuppressPopup also
            // requires DWM_CLOAKED_APP, so restoring the native root ends the
            // suppression on its own and the marker is harmless once uncloaked.
            suppression.reset();
            g_menuBarReadInProgress = false;
            if (active && IsWindow(active)) SetActiveWindow(active);
            if (foreground && IsWindow(foreground)) SetForegroundWindow(foreground);
            if (attached) AttachThreadInput(GetCurrentThreadId(), foreignThread, FALSE);
        }
        // The caller can run a modal command immediately after TrackPopupMenu
        // returns. Release all read guards while still inside the hook, so its
        // nested dialog can be discovered and invoked on this same source thread.
        static void FinishSelection(void* context) noexcept {
            auto& scope = *static_cast<ReadScope*>(context);
            scope.accepted = true;
            DisarmPopupInterception();
            scope.Release();
        }
        ~ReadScope() {
            CancelTracking();
            Release();
            // PeekMessage removes WM_QUIT. Preserve it for the application's outer
            // loop exactly once, after all pumping and cleanup have stopped.
            if (quit) PostQuitMessage(quitCode);
        }
    };

    try {
        menu.clear();
        if (commands) commands->clear();
        reason.clear();
        if (MenuBarReadInProgress() || PopupInterceptionArmed() ||
            buttons.empty() || buttons.size() > kMaxMenuBarButtons ||
            GetWindowThreadProcessId(root, nullptr) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(toolbar, nullptr) != GetCurrentThreadId() ||
            !IsChild(root, toolbar)) {
            reason = L"menu-bar read requires an idle owning source thread";
            return false;
        }
        if (selection && (!MenuBarAncestorsReady(root, toolbar) || selection->toolbarIndex < 1 ||
                static_cast<size_t>(selection->toolbarIndex) > buttons.size() ||
                !buttons[selection->toolbarIndex - 1].enabled || !IsWindow(selection->owner) ||
                GetAncestor(selection->owner, GA_ROOT) != root)) {
            reason = L"menu command's native toolbar slot or owner changed";
            return false;
        }
        if (cancelled.load(std::memory_order_acquire)) {
            reason = L"menu-bar read cancelled";
            return false;
        }
        // WM_CANCELMODE cannot revoke application-owned queued callbacks. The
        // tracking hooks keep late popups hidden until this root is restored.
        if (!SetPropW(root, kReadOwnerProperty, reinterpret_cast<HANDLE>(1))) {
            reason = L"cannot protect the native owner from late menu callbacks";
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + totalWaitMs;
        ReadScope scope(root, toolbar);
        // The inner drain checks both bounds for every message. A stream of posted
        // messages (including requeued Bridge commands) cannot extend the deadline.
        const auto pump = [&](ULONGLONG until, bool stopOnPopup) {
            while (GetTickCount64() < until) {
                if (scope.accepted) return true;
                if (cancelled.load(std::memory_order_acquire) || scope.quit) return false;
                if (stopOnPopup && MenuPopupRecorded()) return true;
                MSG message{};
                unsigned drained = 0;
                while (drained++ < 32 && GetTickCount64() < until &&
                       !cancelled.load(std::memory_order_acquire) &&
                       !scope.accepted &&
                       !(stopOnPopup && MenuPopupRecorded()) &&
                       PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) {
                        scope.quit = true;
                        scope.quitCode = static_cast<int>(message.wParam);
                        return false;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (scope.accepted) return true;
                if (stopOnPopup && MenuPopupRecorded()) return true;
                const ULONGLONG now = GetTickCount64();
                if (now < until) MsgWaitForMultipleObjectsEx(0, nullptr,
                    static_cast<DWORD>(std::min<ULONGLONG>(16, until - now)),
                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            return !scope.quit && !cancelled.load(std::memory_order_acquire);
        };
        std::vector<MenuItemSnapshot> next;
        std::vector<MenuBarCommandBinding> nextCommands;
        size_t itemCount = 0;
        for (size_t index = 0; index < buttons.size(); ++index) {
            if (selection && index + 1 != static_cast<size_t>(selection->toolbarIndex)) continue;
            if (GetTickCount64() >= deadline ||
                cancelled.load(std::memory_order_acquire) || scope.quit) {
                reason = L"menu-bar read exceeded its deadline or was cancelled";
                return false;
            }
            // Pumping can destroy/recreate the bar or change its button ordering. A
            // partial read must never bind later commands to the old positions.
            std::vector<MenuBarButton> current;
            if (!IsWindow(root) || !IsWindow(toolbar) ||
                !ReadMenuBarButtons(toolbar, current) || current != buttons) {
                reason = L"menu-bar toolbar changed during its read";
                return false;
            }
            MenuItemSnapshot top;
            top.kind = MenuItemKind::Popup;
            top.text = buttons[index].text;
            top.enabled = buttons[index].enabled;
            top.itemId = selection ? selection->itemId.substr(0, selection->itemId.find(L'.'))
                : std::to_wstring(next.size());
            bool systemMenu = false;
            if (top.enabled) {
                g_selectedCommand = selection;
                g_selectionCancelled = selection ? &cancelled : nullptr;
                g_selectionDeadline = deadline;
                g_selectionRoot = root;
                g_selectionToolbar = toolbar;
                g_finishSelection = selection ? &ReadScope::FinishSelection : nullptr;
                g_selectionScope = selection ? &scope : nullptr;
                if (!DriveMenuBarButton(toolbar, static_cast<int>(index + 1),
                        top.itemId, reason)) return false;
                if (scope.accepted) return true;
                if (!pump(std::min(deadline, GetTickCount64() + popupWaitMs), true)) {
                    if (scope.accepted) return true;
                    reason = scope.quit ? L"menu-bar read interrupted by WM_QUIT"
                        : L"menu-bar read cancelled";
                    return false;
                }
                if (scope.accepted) return true;
                std::wstring captureReason;
                const bool recorded = TakeRecordedMenu(top.items, systemMenu, captureReason);
                const bool accepted = g_selectionAccepted;
                const HWND owner = g_recordedOwner;
                const UINT flags = g_recordedFlags;
                scope.CancelTracking();
                if (selection) {
                    if (!recorded || !accepted) {
                        reason = captureReason.empty() ? L"native menu did not accept the selected command"
                            : std::move(captureReason);
                        return false;
                    }
                    return true;
                }
                if (recorded && !systemMenu && top.items.empty() && !captureReason.empty()) {
                    reason = std::move(captureReason);
                    return false;
                }
                if (recorded && !systemMenu) {
                    std::vector<std::wstring> ancestors;
                    if (!BindMenuCommands(top.items, static_cast<int>(index + 1), owner,
                            flags, nextCommands, ancestors, itemCount, 2)) {
                        reason = L"complete menu bar exceeds its item or depth cap";
                        return false;
                    }
                }
                // Keep late popups suppressed while the bar leaves its tracking state.
                if (!pump(std::min(deadline, GetTickCount64() + 120), false)) {
                    reason = scope.quit ? L"menu-bar cleanup interrupted by WM_QUIT"
                        : L"menu-bar cleanup cancelled";
                    return false;
                }
            }
            if (systemMenu) continue;
            if (++itemCount > kMaxProjectedMenuItems) {
                reason = L"complete menu bar exceeds its item cap";
                return false;
            }
            // An empty native Window menu still has a title. A disabled empty popup
            // preserves it and cannot dispatch a command the application did not offer.
            if (top.items.empty()) top.enabled = false;
            next.push_back(std::move(top));
        }
        if (next.empty() || GetTickCount64() >= deadline) {
            reason = L"menu-bar read produced no complete menu within its deadline";
            return false;
        }
        AssignProjectedCommandIds(next, nextCommands);
        menu = std::move(next);
        if (commands) *commands = std::move(nextCommands);
        return true;
    } catch (...) {
        try { reason = L"exception while reading a menu-bar toolbar"; } catch (...) {}
        return false;
    }
}

bool CaptureMenuBarToolbar(
    HWND root, HWND toolbar, const std::vector<MenuBarButton>& buttons,
    DWORD popupWaitMs, DWORD totalWaitMs, const std::atomic<bool>& cancelled,
    std::vector<MenuItemSnapshot>& menu, std::wstring& reason,
    std::vector<MenuBarCommandBinding>* commands) noexcept {
    return ReadOrInvokeMenuBarToolbar(root, toolbar, buttons, popupWaitMs, totalWaitMs,
        cancelled, menu, reason, commands, nullptr);
}

bool InvokeMenuBarToolbarCommand(
    HWND root, HWND toolbar, const std::vector<MenuBarButton>& buttons,
    const MenuBarCommandBinding& command, const std::atomic<bool>& cancelled,
    std::wstring& reason) noexcept {
    std::vector<MenuItemSnapshot> unused;
    return ReadOrInvokeMenuBarToolbar(root, toolbar, buttons, 1000, 1400,
        cancelled, unused, reason, nullptr, &command);
}

} // namespace FluentShell::Bridge::Translation
