#include "HostScope.h"

#include "../../Common/ExplorerDialogPolicy.h"

#include <array>
#include <atomic>
#include <climits>

namespace FluentShell::Bridge::Translation {
namespace {

std::atomic<HostKind> g_processHostKind{ HostKind::Application };

constexpr HostScope kApplicationScope = HostScope::Application();
constexpr HostScope kExplorerScope = HostScope::ExplorerDialogs();

// The window classes a dialog the guest may hook carries directly under its root: the
// standard Win32 controls and child dialog pages.  A shell view, a DirectUI host, a
// browser or a XAML island at the root is a common item dialog, a task dialog or a
// shell surface, which the projection cannot take, and finding that out would cost
// three thread hooks and a subclass on every control first.  Deeper descendants are
// the capture's business (a page's shell view is judged by its own adapter).
constexpr std::array<std::wstring_view, 18> kExplorerDialogChildClasses{
    L"Button", L"Static", L"Edit", L"ComboBox", L"ComboBoxEx32", L"ListBox",
    L"SysTabControl32", L"#32770", L"SysLink", L"Link Window", L"ScrollBar",
    L"msctls_progress32", L"msctls_trackbar32", L"msctls_updown32",
    L"SysListView32", L"SysTreeView32", L"SysDateTimePick32", L"SysAnimate32",
};

// What a plain dialog needs.  Everything that opens a native popup (there is no
// TrackPopupMenu detour in the shell to keep it off screen), drives a shell or HTML
// view, edits an item label, or addresses a menu bar or MDI frame is left out.
constexpr std::array<std::wstring_view, 20> kExplorerActions{
    L"invoke", L"close", L"setCheck", L"setText", L"select", L"setSelection",
    L"setFocusedIndex", L"setItemCheck", L"setValue", L"setExpand", L"setGroupExpanded",
    L"focus", L"setScroll", L"setTopIndex", L"scrollBy",
    L"move", L"resize", L"minimize", L"maximize", L"restore",
};

bool IsExplorerDialogChildClass(HWND child) noexcept {
    wchar_t name[64]{};
    if (GetClassNameW(child, name, static_cast<int>(std::size(name))) <= 0) return false;
    for (const auto& allowed : kExplorerDialogChildClasses) {
        if (FluentShell::EqualsIgnoreCase(name, allowed)) return true;
    }
    return false;
}

} // namespace

HostScope HostScope::ForCurrentProcess(const wchar_t* executablePath) noexcept {
    try {
        if (executablePath && FluentShell::IsSystemExplorerImage(executablePath) &&
            FluentShell::CurrentExplorerDialogGrant())
            return ExplorerDialogs();
    } catch (...) {}
    return Application();
}

const wchar_t* HostScope::Name() const noexcept {
    return IsExplorer() ? L"explorer-dialogs" : L"application";
}

bool HostScope::Active() const noexcept {
    return !IsExplorer() || FluentShell::CurrentExplorerDialogGrant();
}

unsigned HostScope::TransientRetryLimit() const noexcept {
    return IsExplorer() ? 1u : UINT_MAX;
}

bool HostScope::ConsidersRoot(HWND window) const noexcept {
    if (!window) return false;
    if (!IsExplorer()) return true;
    return FluentShell::IsExplorerDialogWindow(window, GetCurrentProcessId());
}

bool HostScope::AdmitsRoot(HWND window, std::wstring* reason) const noexcept {
    const auto refuse = [&](const wchar_t* text) noexcept {
        if (reason) {
            try { *reason = text; } catch (...) {}
        }
        return false;
    };
    if (!IsExplorer()) return window != nullptr || refuse(L"no window");
    if (!FluentShell::CurrentExplorerDialogGrant())
        return refuse(L"the global Explorer dialog grant has ended");
    if (!ConsidersRoot(window))
        return refuse(L"outside the global Explorer dialog scope (not a visible #32770 root of this process)");
    return HasOnlyStandardDialogChildren(window, reason);
}

bool HostScope::HasOnlyStandardDialogChildren(HWND root, std::wstring* reason) noexcept {
    const auto refuse = [&](const wchar_t* text) noexcept {
        if (reason) {
            try { *reason = text; } catch (...) {}
        }
        return false;
    };
    if (!root || !IsWindow(root)) return refuse(L"no window");
    unsigned children = 0;
    for (HWND child = GetWindow(root, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (++children > 512) return refuse(L"Explorer dialog has too many child windows");
        if (!IsExplorerDialogChildClass(child))
            return refuse(L"Explorer dialog hosts a non-standard window at its root "
                L"(shell view, DirectUI or browser host)");
    }
    return true;
}

HWND HostScope::NativeModalOwner(HWND dialog) const noexcept {
    return IsExplorer() ? FluentShell::ExplorerNativeModalOwner(dialog, GetCurrentProcessId()) : nullptr;
}

SourceThreadMode HostScope::ThreadModeFor(HWND root) const noexcept {
    if (!IsExplorer() || !root) return SourceThreadMode::Dedicated;
    const DWORD thread = GetWindowThreadProcessId(root, nullptr);
    if (!thread) return SourceThreadMode::Shared;
    struct Census final {
        const HostScope* scope;
        HWND root;
        bool shared;
    } census{ this, root, false };
    EnumThreadWindows(thread, [](HWND window, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Census*>(parameter);
        if (window == state.root || !IsWindowVisible(window)) return TRUE;
        if (state.scope->ConsidersRoot(window)) return TRUE;
        state.shared = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&census));
    return census.shared ? SourceThreadMode::Shared : SourceThreadMode::Dedicated;
}

bool HostScope::AdmitsAction(std::wstring_view action) const noexcept {
    if (!IsExplorer()) return true;
    for (const auto& allowed : kExplorerActions) {
        if (action == allowed) return true;
    }
    return false;
}

bool HostScope::AllowsInputJoinWith(DWORD foregroundThread, HWND proxy) const noexcept {
    if (!IsExplorer()) return true;
    return proxy && foregroundThread != 0 && IsWindow(proxy) &&
        GetWindowThreadProcessId(proxy, nullptr) == foregroundThread;
}

const HostScope& ProcessHostScope() noexcept {
    return g_processHostKind.load(std::memory_order_acquire) == HostKind::ExplorerDialogs
        ? kExplorerScope : kApplicationScope;
}

void PublishProcessHostScope(const HostScope& scope) noexcept {
    g_processHostKind.store(scope.Kind(), std::memory_order_release);
}

ScopedProcessHostScope::ScopedProcessHostScope(const HostScope& scope) noexcept
    : previous_(g_processHostKind.exchange(scope.Kind(), std::memory_order_acq_rel)) {}

ScopedProcessHostScope::~ScopedProcessHostScope() {
    g_processHostKind.store(previous_, std::memory_order_release);
}

} // namespace FluentShell::Bridge::Translation
