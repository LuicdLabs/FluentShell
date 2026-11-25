#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// HostScope is the one owner of what the Bridge may touch in its host.  Its verdicts are pure
// functions of the windows they are asked about, so both hosts' rules run here without Explorer
// or a watcher; the choke points (the process-wide show hook, cross-thread dispatch) are driven
// under a scoped override of the process scope.

namespace HostScopeFixture {

// The protocol's action names, from the schema: a new action fails this test until the Explorer
// scope classifies it.
std::vector<std::wstring> SchemaActionNames() {
    std::vector<std::wstring> names;
    std::ifstream file(L"src\\Protocol\\protocol-v1.schema.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Conditional sub-schemas name a few actions each; the action property's own enum is the
    // longest list.
    const std::string anchor = "\"action\": { \"enum\": [";
    for (size_t start = text.find(anchor); start != std::string::npos; start = text.find(anchor, start + 1)) {
        std::vector<std::wstring> list;
        const size_t end = text.find(']', start);
        size_t cursor = start + anchor.size();
        while (end != std::string::npos && cursor < end) {
            const size_t open = text.find('"', cursor);
            if (open == std::string::npos || open >= end) break;
            const size_t close = text.find('"', open + 1);
            if (close == std::string::npos || close >= end) break;
            list.emplace_back(text.begin() + static_cast<std::ptrdiff_t>(open + 1),
                text.begin() + static_cast<std::ptrdiff_t>(close));
            cursor = close + 1;
        }
        if (list.size() > names.size()) names = std::move(list);
    }
    return names;
}

struct Window final {
    HWND handle = nullptr;
    ~Window() { if (handle) DestroyWindow(handle); }
};

// A window owned by a thread of its own, pumping until released.
struct ForeignThreadWindow final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> window{ nullptr };
    std::thread thread;
    ForeignThreadWindow() {
        thread = std::thread([this] {
            const HWND created = CreateWindowExW(0, L"Static", L"foreign", WS_POPUP, 0, 0, 10, 10,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            window.store(created);
            SetEvent(ready);
            while (WaitForSingleObject(release, 0) == WAIT_TIMEOUT) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                MsgWaitForMultipleObjects(1, &release, FALSE, 20, QS_ALLINPUT);
            }
            if (created) DestroyWindow(created);
        });
        WaitForSingleObject(ready, 5000);
    }
    ~ForeignThreadWindow() {
        SetEvent(release);
        if (thread.joinable()) thread.join();
        CloseHandle(ready);
        CloseHandle(release);
    }
};

} // namespace HostScopeFixture

void TestHostScopeVerdicts() {
    using namespace HostScopeFixture;
    using Translation::HostScope;
    using Translation::SourceThreadMode;
    constexpr HostScope application = HostScope::Application();
    constexpr HostScope explorer = HostScope::ExplorerDialogs();
    Check(!application.IsExplorer() && explorer.IsExplorer(), "host scope kinds are confused");

    // A guest in the shell installs no process-wide machinery and keeps out of every
    // shell window; an application keeps everything it had.
    Check(application.RefusesDeniedOrXamlHost() && application.InstallsProcessDetours() &&
          application.InstallsProcessShowHook() && application.ManagesTaskbarButtons() &&
          application.FollowsDormancyAndLease() && application.TranslatesDialogs() &&
          application.AdmitsGenericDirectUi() && application.ReadsMenuBars() &&
          application.DispatchesToForeignThreads() && application.MessagesForeignWindows() &&
          application.SweepsDesktopForIsolation(),
        "the application scope lost a capability it always had");
    Check(!explorer.RefusesDeniedOrXamlHost() && !explorer.InstallsProcessDetours() &&
          !explorer.InstallsProcessShowHook() && !explorer.ManagesTaskbarButtons() &&
          !explorer.FollowsDormancyAndLease() && !explorer.TranslatesDialogs() &&
          !explorer.AdmitsGenericDirectUi() && !explorer.ReadsMenuBars() &&
          !explorer.DispatchesToForeignThreads() && !explorer.MessagesForeignWindows() &&
          !explorer.SweepsDesktopForIsolation(),
        "the Explorer scope kept process-wide machinery");
    Check(application.TransientRetryLimit() > 1000 && explorer.TransientRetryLimit() == 1,
        "the Explorer scope's transient retry bound is wrong");
    Check(application.Active(), "an application scope must always be active");

    // Actions: every protocol action is either one a plain dialog needs or refused, explicitly.
    const std::vector<std::wstring> admitted{
        L"invoke", L"close", L"setCheck", L"setText", L"select", L"setSelection", L"setFocusedIndex",
        L"setItemCheck", L"setValue", L"setExpand", L"setGroupExpanded", L"focus", L"setScroll",
        L"setTopIndex", L"scrollBy", L"move", L"resize", L"minimize", L"maximize", L"restore",
    };
    const std::vector<std::wstring> refused{
        L"activate", L"setItemText", L"setSplit", L"setColumnOrder", L"islandInvoke", L"htmlInvoke",
        L"htmlScroll", L"diskSelect", L"diskContext", L"diskScroll", L"contextMenu", L"openItem",
        L"activateItem", L"toolbarCommand", L"splitDropDown", L"menuCommand", L"popupCommand", L"mdiCommand",
    };
    const auto actions = SchemaActionNames();
    Check(actions.size() >= admitted.size() + refused.size(), "the schema's action names could not be read");
    for (const auto& action : actions) {
        const bool inAdmitted = std::find(admitted.begin(), admitted.end(), action) != admitted.end();
        const bool inRefused = std::find(refused.begin(), refused.end(), action) != refused.end();
        if (inAdmitted == inRefused)
            std::wcerr << L"unclassified protocol action for the Explorer scope: " << action << L'\n';
        Check(inAdmitted != inRefused, "a protocol action is not classified for the Explorer scope");
        Check(explorer.AdmitsAction(action) == inAdmitted, "the Explorer scope's action verdict is wrong");
        Check(application.AdmitsAction(action), "the application scope refused a protocol action");
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    Window dialog{ CreateWindowExW(0, WC_DIALOG, L"Scoped dialog", WS_POPUP | WS_VISIBLE, 60, 60, 200, 120,
        nullptr, nullptr, instance, nullptr) };
    Window folder{ CreateWindowExW(0, L"Static", L"folder stand-in", WS_POPUP | WS_VISIBLE, 300, 60, 200, 120,
        nullptr, nullptr, instance, nullptr) };
    Window stub{ CreateWindowExW(0, L"Static", L"hidden stub owner", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, instance, nullptr) };
    Check(dialog.handle && folder.handle && stub.handle, "host scope window fixtures were not created");
    if (!dialog.handle || !folder.handle || !stub.handle) return;

    // A dialog sharing its thread with a visible non-dialog window (the security warning on a
    // folder's thread) is on a shared thread; with only hidden windows beside it (the Properties
    // sheet and its hidden stub owner) its thread is its own.  An application's always is.
    Check(explorer.ThreadModeFor(dialog.handle) == SourceThreadMode::Shared,
        "a dialog sharing its thread with a visible shell window was judged dedicated");
    ShowWindow(folder.handle, SW_HIDE);
    Check(explorer.ThreadModeFor(dialog.handle) == SourceThreadMode::Dedicated,
        "a dialog alone on its thread (hidden stub beside it) was judged shared");
    ShowWindow(folder.handle, SW_SHOWNOACTIVATE);
    Check(application.ThreadModeFor(dialog.handle) == SourceThreadMode::Dedicated,
        "an application's thread was judged shared");

    // Input joins: a guest joins only the renderer proxy's own thread.
    const HWND proxyStandIn = folder.handle;
    Check(explorer.AllowsInputJoinWith(GetCurrentThreadId(), proxyStandIn),
        "the Explorer scope refused a join with the proxy's thread");
    Check(!explorer.AllowsInputJoinWith(GetCurrentThreadId() + 1, proxyStandIn) &&
          !explorer.AllowsInputJoinWith(GetCurrentThreadId(), nullptr),
        "the Explorer scope allowed a join with a thread that is not the proxy's");
    Check(application.AllowsInputJoinWith(GetCurrentThreadId() + 1, nullptr),
        "the application scope lost its input join");

    // Root admission: only standard controls directly under the root, and nothing without a grant.
    CreateWindowExW(0, L"Button", L"OK", WS_CHILD | WS_VISIBLE, 10, 10, 60, 24, dialog.handle,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance, nullptr);
    CreateWindowExW(0, L"Static", L"Label", WS_CHILD | WS_VISIBLE, 10, 40, 60, 16, dialog.handle,
        nullptr, instance, nullptr);
    CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 80, 10, 100, 60, dialog.handle,
        nullptr, instance, nullptr);
    std::wstring reason;
    Check(HostScope::HasOnlyStandardDialogChildren(dialog.handle, &reason),
        "a dialog of standard controls was refused at its root");
    WNDCLASSW hostClass{};
    hostClass.lpfnWndProc = DefWindowProcW;
    hostClass.hInstance = instance;
    hostClass.lpszClassName = L"DirectUIHWND";
    RegisterClassW(&hostClass);
    CreateWindowExW(0, L"DirectUIHWND", L"", WS_CHILD | WS_VISIBLE, 10, 70, 60, 30, dialog.handle,
        nullptr, instance, nullptr);
    Check(!HostScope::HasOnlyStandardDialogChildren(dialog.handle, &reason) && !reason.empty(),
        "a dialog hosting DirectUI at its root (a task or common item dialog) was admitted");
    // No watcher, no grant: the guest admits nothing, whatever the window.
    Check(!explorer.AdmitsRoot(dialog.handle, &reason) && reason.find(L"grant") != std::wstring::npos,
        "the Explorer scope admitted a root without the watcher's grant");
    Check(application.AdmitsRoot(dialog.handle), "the application scope refused a root");
    Check(application.NativeModalOwner(dialog.handle) == nullptr,
        "the application scope named a native modal owner");
    Check(!explorer.ConsidersRoot(folder.handle) && explorer.ConsidersRoot(dialog.handle),
        "the Explorer scope's discovery filter is wrong");
}

void TestHostScopeChokePoints() {
    using namespace HostScopeFixture;
    using Translation::HostScope;
    Check(!Translation::ProcessHostScope().IsExplorer(), "the test process starts outside the Explorer scope");
    ForeignThreadWindow foreign;
    const HWND remote = foreign.window.load();
    Check(remote != nullptr, "foreign-thread window fixture was not created");
    {
        Translation::ScopedProcessHostScope guest(HostScope::ExplorerDialogs());
        Check(Translation::ProcessHostScope().IsExplorer(), "the scoped override did not publish the Explorer scope");
        // The in-context show hook covers every thread of the process: never inside the shell.
        Check(!Translation::InstallTopLevelShownHook(GetModuleHandleW(nullptr)),
            "the process-wide show hook was installed in the Explorer scope");
        // Reaching a child another thread owns means hooking that thread: refused.
        if (remote) {
            bool ran = false;
            std::wstring error;
            Check(!Translation::DispatchToWindowThread(remote, [&] { ran = true; }, error, 1000) && !ran &&
                  error.find(L"scope") != std::wstring::npos,
                "the Explorer scope dispatched work to another thread");
        }
    }
    Check(!Translation::ProcessHostScope().IsExplorer(), "the scoped override did not restore the process scope");
    if (remote) {
        bool ran = false;
        std::wstring error;
        Check(Translation::DispatchToWindowThread(remote, [&] { ran = true; }, error, 2000) && ran,
            "the application scope lost cross-thread dispatch");
    }
}
