#pragma once

#include "../../Common/FluentShell.h"

#include <string>
#include <string_view>

namespace FluentShell::Bridge::Translation {

// Where this Bridge runs, and so what it may touch.
//
// An application Bridge owns its process: it installs process-wide detours and a
// WinEvent hook, manages its windows' taskbar buttons, follows dormancy and the watch
// lease, and offers every standard top-level window to the projection.
//
// Inside explorer.exe -- the shell itself, admitted only by global apply's watcher
// grant -- the Bridge is a guest.  It touches the dialogs it projects and nothing else:
// no process-wide hook or detour, nothing sent to the taskbar, desktop or folder
// windows, no work on a shell thread beyond the projected dialog's own windows, and
// only the actions a plain dialog needs.  A fault there ends the shell, so whatever is
// not needed is not done.
//
// Every decision that differs between the two hosts is a question asked of this type.
// Nothing else in the Bridge reads the Explorer dialog policy (src/Common) or tests for Explorer, and
// test.ps1 holds that line.
enum class HostKind : unsigned char { Application, ExplorerDialogs };

// How a projected root's GUI thread is shared with windows outside the scope.
enum class SourceThreadMode : unsigned char {
    // Every visible top-level on the thread is a window the scope admits: an
    // application's thread, or the folder Properties sheet's own shell thread.
    Dedicated,
    // The thread also runs windows outside the scope -- the security warning is shown on
    // the thread of the folder window that opened the file.  The thread hooks then act
    // only for the projected roots' own windows and ignore the rest of the thread.
    Shared,
};

class HostScope final {
public:
    static constexpr HostScope Application() noexcept { return HostScope(HostKind::Application); }
    static constexpr HostScope ExplorerDialogs() noexcept { return HostScope(HostKind::ExplorerDialogs); }
    // Decided once when the Bridge starts: the system explorer.exe (by file identity)
    // under a live watcher grant is a guest; anything else is an application.
    static HostScope ForCurrentProcess(const wchar_t* executablePath) noexcept;

    constexpr HostKind Kind() const noexcept { return kind_; }
    constexpr bool IsExplorer() const noexcept { return kind_ == HostKind::ExplorerDialogs; }
    const wchar_t* Name() const noexcept;

    // The scope still holds: always for an application; for Explorer while the
    // watcher's grant lasts.  Losing it ends the guest session.
    bool Active() const noexcept;

    // --- Process-wide machinery: a guest installs none of it. ---------------------
    // The deny list and the "already hosts XAML" refusal; Explorer hosts XAML itself and
    // is denied by name, and only the grant lets it through.
    constexpr bool RefusesDeniedOrXamlHost() const noexcept { return !IsExplorer(); }
    // MessageBox/TaskDialog/TrackPopupMenu detours.  Without them no translated dialog
    // and no popup interception exists, so the actions that depend on it are refused.
    constexpr bool InstallsProcessDetours() const noexcept { return !IsExplorer(); }
    // The in-context EVENT_OBJECT_SHOW hook over every thread of the process.  In the
    // shell that is the taskbar's, the desktop's and every folder's thread.
    constexpr bool InstallsProcessShowHook() const noexcept { return !IsExplorer(); }
    // The shell's taskbar list from inside the host.  Inside Explorer that is the shell editing its
    // own taskbar from a Bridge thread; the renderer swaps the button out of process.
    constexpr bool ManagesTaskbarButtons() const noexcept { return !IsExplorer(); }
    // The shared dormancy event and the watch lease.  A guest's revocation is the grant,
    // and the shell never exits, so it must not keep (or set) the shared event.
    constexpr bool FollowsDormancyAndLease() const noexcept { return !IsExplorer(); }
    constexpr bool TranslatesDialogs() const noexcept { return !IsExplorer(); }
    constexpr bool AdmitsGenericDirectUi() const noexcept { return !IsExplorer(); }
    // Reading a menu-bar toolbar opens the application's real menus; only the detour
    // keeps them off screen.
    constexpr bool ReadsMenuBars() const noexcept { return !IsExplorer(); }
    // Hooking another thread to reach a child window that thread owns.
    constexpr bool DispatchesToForeignThreads() const noexcept { return !IsExplorer(); }
    // Sending a message (WM_GETTEXT for the census log) to a window the scope does not
    // admit.
    constexpr bool MessagesForeignWindows() const noexcept { return !IsExplorer(); }
    // The UIA isolation check may enumerate the desktop's top-levels; a guest checks the
    // native root by its handle only.
    constexpr bool SweepsDesktopForIsolation() const noexcept { return !IsExplorer(); }
    // How many times a root refused on a timing condition is offered again.  Each attempt
    // hooks the dialog's thread and holds it for up to the capture deadline.
    unsigned TransientRetryLimit() const noexcept;

    // --- Windows. -------------------------------------------------------------------
    // A top-level window the projection may take.  An application admits any; Explorer
    // admits a visible #32770 root of its own whose direct children are all standard
    // Win32 controls (a shell view, DirectUI host or browser at the root means a common
    // item dialog or a task dialog, which get no hooks at all).
    bool AdmitsRoot(HWND window, std::wstring* reason = nullptr) const noexcept;
    // A window discovery should even look at: what AdmitsRoot judges, before the
    // child-class check (which needs the window's children to exist).
    bool ConsidersRoot(HWND window) const noexcept;
    // The child-class half of Explorer's root admission: every direct child of the root is a
    // standard Win32 control or a child dialog page.  Reads classes only; sends nothing.
    static bool HasOnlyStandardDialogChildren(HWND root, std::wstring* reason = nullptr) noexcept;
    // The native owner a projected dialog may keep without projecting it: Explorer's
    // disabled folder window for a dialog modal to it.  Null for an application.
    HWND NativeModalOwner(HWND dialog) const noexcept;
    // How the root's GUI thread is shared.  An application's is always dedicated.
    SourceThreadMode ThreadModeFor(HWND root) const noexcept;
    // A renderer action the scope delivers.  An application delivers every action the
    // protocol defines; Explorer delivers what a plain dialog needs and refuses anything
    // that opens a native popup, drives a shell view or edits a label.
    bool AdmitsAction(std::wstring_view action) const noexcept;
    // Joining the dialog thread's input to the foreground thread for a press.  A guest
    // joins only with the renderer proxy's own thread, never with a shell thread.
    bool AllowsInputJoinWith(DWORD foregroundThread, HWND proxy) const noexcept;

private:
    explicit constexpr HostScope(HostKind kind) noexcept : kind_(kind) {}
    HostKind kind_;
};

// The scope hook callbacks, cross-thread dispatch and capture consult.  Published once
// by the Bridge worker before any session or agent exists.  Trivially destructible: it
// is read by hook callbacks that can run while the process exits.
const HostScope& ProcessHostScope() noexcept;
void PublishProcessHostScope(const HostScope& scope) noexcept;

// Tests run both hosts' rules in one process.
class ScopedProcessHostScope final {
public:
    explicit ScopedProcessHostScope(const HostScope& scope) noexcept;
    ~ScopedProcessHostScope();
    ScopedProcessHostScope(const ScopedProcessHostScope&) = delete;
    ScopedProcessHostScope& operator=(const ScopedProcessHostScope&) = delete;

private:
    HostKind previous_;
};

} // namespace FluentShell::Bridge::Translation
