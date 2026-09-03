#include "SourceThreadAgent.h"
#include "DirectUiEngine.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>
#include <prsht.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <thread>
#include <new>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace FluentShell::Bridge::Translation {
namespace {

constexpr UINT_PTR kRootSubclassId = 0xAF110001u;
constexpr UINT_PTR kControlSubclassId = 0xAF110002u;
constexpr UINT kCommandCapture = 1;
constexpr UINT kCommandInvoke = 2;
constexpr UINT kCommandCloak = 3;
constexpr UINT kCommandRestore = 4;
constexpr UINT kCommandShutdown = 5;
constexpr UINT kCommandCaptureAndCloak = 6;
constexpr UINT kCommandCaptureDirectUiEvidence = 7;
constexpr UINT kCommandVerifyDirectUiAndCloak = 8;
constexpr UINT kCommandRestoreThenDirectUiClick = 9;
constexpr UINT kCommandDirectUiToggle = 10;
constexpr UINT kCommandDirectUiMove = 11;
constexpr UINT kCommandCaptureDirectUiBootstrap = 12;
constexpr UINT kCommandPostDirectUiPropertySheetButton = 13;
constexpr UINT kCommandPlaceBehind = 14;
constexpr UINT kCommandRestoreDirectUiActivation = 15;
constexpr UINT kCommandDirectUiNodeAction = 16;
constexpr UINT kCommandNavigateDirectUiProjected = 17;
constexpr wchar_t kNodeGenerationProperty[] = L"FluentShell.Bridge.NodeGeneration";
constexpr wchar_t kDirectUiGenerationProperty[] = L"FluentShell.Bridge.DirectUiGeneration";

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

struct Command final {
    std::atomic<long> references{ 1 };
    // Distinguish a failed PostThreadMessageW from a queued command whose
    // callback may still be waiting in the source thread's message pump.
    std::atomic<bool> queued{ false };
    HANDLE started = nullptr;
    HANDLE completed = nullptr;
    UINT kind = 0;
    SourceThreadAgent* agent = nullptr;
    ActionRequest action;
    CaptureContext capture;
    WindowSnapshot snapshot;
    DirectUiNativeEvidence directUiEvidence;
    DirectUiBootstrapEvidence directUiBootstrapEvidence;
    DirectUiNativeEvidence expectedDirectUiEvidence;
    DirectUiActionBinding directUiBinding;
    const DirectUiWindowProfile* profile = nullptr;
    HWND sibling = nullptr;
    ActionOutcome outcome;
    bool captured = false;
    bool cloaked = false;
    uint64_t expectedFingerprint = 0;
    bool success = false;
    std::atomic<bool> cancelled{ false };
    std::wstring error;
};

Command* CreateCommand(UINT kind, SourceThreadAgent* agent) noexcept {
    auto* command = new (std::nothrow) Command();
    if (!command) return nullptr;
    command->started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    command->completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!command->started || !command->completed) {
        if (command->started) CloseHandle(command->started);
        if (command->completed) CloseHandle(command->completed);
        delete command;
        return nullptr;
    }
    command->kind = kind;
    command->agent = agent;
    return command;
}

std::mutex g_agentsMutex;
std::unordered_map<UINT, SourceThreadAgent*> g_agents;
std::mutex g_commandsMutex;
std::unordered_set<Command*> g_pendingCommands;
std::vector<std::shared_ptr<SourceThreadAgent>> g_retainedAgents;
std::atomic<UINT> g_nextMessage{ WM_APP + 0x4A1 };
std::atomic<uint64_t> g_nextGeneration{ 1 };

void AddRef(Command* command) noexcept {
    command->references.fetch_add(1, std::memory_order_relaxed);
}

void Release(Command* command) noexcept {
    if (command->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (command->started) CloseHandle(command->started);
        if (command->completed) CloseHandle(command->completed);
        delete command;
    }
}

void Complete(Command* command) noexcept {
    SetEvent(command->completed);
}

bool AbortIfCancelled(Command* command) noexcept {
    if (!command || !command->cancelled.load(std::memory_order_acquire)) return false;
    try {
        command->success = false;
        command->captured = false;
        command->outcome.accepted = false;
        command->error = L"source-thread command cancelled";
    } catch (...) {}
    Complete(command);
    return true;
}

void ScheduleTimedOutAttachCleanup(
    std::shared_ptr<SourceThreadAgent> agent,
    Command* command) noexcept {
    if (!agent || !command) return;

    // Keep the command alive independently of the Attach caller.  A queued
    // command must be consumed before its dispatch hooks and map entry go
    // away, otherwise SourceHook can dereference a stale agent or leak it.
    AddRef(command);
    try {
        std::thread([agent = std::move(agent), command]() mutable {
            bool released = false;
            try {
                if (command->queued.load(std::memory_order_acquire)) {
                    WaitForSingleObject(command->completed, INFINITE);
                }
                Release(command);
                released = true;
                if (!agent->Shutdown()) {
                    // A permanently stalled source thread still owns the raw
                    // pointer in g_agents; retain the agent rather than allowing
                    // a later hook callback to use freed memory.
                    RetainSourceThreadAgent(std::move(agent));
                }
            } catch (...) {
                if (!released) Release(command);
                RetainSourceThreadAgent(std::move(agent));
            }
        }).detach();
    } catch (...) {
        Release(command);
        RetainSourceThreadAgent(std::move(agent));
    }
}

bool TrackCommand(Command* command) noexcept {
    try {
        std::scoped_lock lock(g_commandsMutex);
        return g_pendingCommands.insert(command).second;
    } catch (...) {
        return false;
    }
}

void UntrackCommand(Command* command) noexcept {
    try {
        std::scoped_lock lock(g_commandsMutex);
        g_pendingCommands.erase(command);
    } catch (...) {}
}

bool IsTrackedCommand(Command* command, SourceThreadAgent* agent) noexcept {
    if (!command) return false;
    try {
        std::scoped_lock lock(g_commandsMutex);
        const auto found = g_pendingCommands.find(command);
        return found != g_pendingCommands.end() && command->agent == agent;
    } catch (...) {
        return false;
    }
}

SourceThreadAgent* AgentForMessage(UINT message) noexcept {
    try {
        std::scoped_lock lock(g_agentsMutex);
        const auto found = g_agents.find(message);
        return found == g_agents.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

void MarkCurrentThreadAgentsDirty(HWND window = nullptr, UINT message = 0) noexcept {
    const DWORD threadId = GetCurrentThreadId();
    try {
        std::scoped_lock lock(g_agentsMutex);
        for (const auto& [_, agent] : g_agents) {
            if (agent && agent->ThreadId() == threadId) agent->MarkDirty(window, message);
        }
    } catch (...) {}
}

void MarkWindowCloseCompleted(HWND window) noexcept {
    if (!window) return;
    try {
        // Resolve through the lifetime-protected registry after the native
        // WndProc returns. A nested fallback may have removed and destroyed the
        // agent while WM_CLOSE was inside a modal loop, so the subclass's raw
        // refData must not be dereferenced for this completion notification.
        std::scoped_lock lock(g_agentsMutex);
        for (const auto& [_, agent] : g_agents) {
            if (agent && agent->Root() == window) {
                agent->MarkCloseRequestCompleted();
                break;
            }
        }
    } catch (...) {}
}

bool RelevantMessage(UINT message) noexcept {
    // Common-control message values overlap heavily inside WM_USER. Keep the
    // Toolbar mutators out of the switch so aliases cannot create duplicate cases.
    if (message == TB_ENABLEBUTTON || message == TB_HIDEBUTTON ||
        message == TB_INDETERMINATE || message == TB_MARKBUTTON ||
        message == TB_PRESSBUTTON || message == TB_CHECKBUTTON ||
        message == TB_SETSTATE || message == TB_ADDBUTTONSA ||
        message == TB_ADDBUTTONSW || message == TB_INSERTBUTTONA ||
        message == TB_INSERTBUTTONW || message == TB_DELETEBUTTON ||
        message == TB_SETBUTTONINFOA || message == TB_SETBUTTONINFOW ||
        message == TB_SETIMAGELIST || message == TB_SETHOTIMAGELIST ||
        message == TB_SETDISABLEDIMAGELIST || message == TB_SETPRESSEDIMAGELIST ||
        message == TB_SETEXTENDEDSTYLE || message == TB_SETBUTTONSIZE ||
        message == TB_SETBITMAPSIZE || message == TB_SETROWS ||
        message == TB_MOVEBUTTON || message == TB_AUTOSIZE) return true;
    switch (message) {
    case WM_SETTEXT:
    case WM_ENABLE:
    case WM_WINDOWPOSCHANGED:
    case WM_STYLECHANGED:
    case WM_COMMAND:
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
    case WM_MENUSELECT:
    case WM_NOTIFY:
    case WM_PARENTNOTIFY:
    case WM_DPICHANGED:
    case WM_SHOWWINDOW:
    case WM_UPDATEUISTATE:
    case WM_SETFONT:
    case BM_SETCHECK:
    case STM_SETICON:
    case STM_SETIMAGE:
    case CB_ADDSTRING:
    case CB_DELETESTRING:
    case CB_RESETCONTENT:
    case CB_SETCURSEL:
    case LB_ADDSTRING:
    case LB_DELETESTRING:
    case LB_RESETCONTENT:
    case LB_SETCURSEL:
    case LM_SETITEM:
    case LVM_SETITEMA:
    case LVM_SETITEMW:
    case LVM_SETITEMTEXTA:
    case LVM_SETITEMTEXTW:
    case LVM_SETITEMSTATE:
    case LVM_INSERTITEMA:
    case LVM_INSERTITEMW:
    case LVM_DELETEITEM:
    case LVM_DELETEALLITEMS:
    case LVM_SETCOLUMNA:
    case LVM_SETCOLUMNW:
    case LVM_INSERTCOLUMNA:
    case LVM_INSERTCOLUMNW:
    case LVM_DELETECOLUMN:
    case LVM_SETCOLUMNWIDTH:
    case LVM_SETCOLUMNORDERARRAY:
    case LVM_SORTITEMS:
    case LVM_SORTITEMSEX:
    case LVM_SETEXTENDEDLISTVIEWSTYLE:
    case LVM_ENABLEGROUPVIEW:
    case LVM_SETVIEW:
    case TCM_SETCURSEL:
    case TCM_INSERTITEMA:
    case TCM_INSERTITEMW:
    case TCM_DELETEITEM:
    case TCM_DELETEALLITEMS:
    case TCM_SETITEMA:
    case TCM_SETITEMW:
    case TCM_SETIMAGELIST:
    case TCM_SETITEMSIZE:
    case TCM_SETITEMEXTRA:
    case TCM_SETPADDING:
    case TCM_REMOVEIMAGE:
    case TCM_SETTOOLTIPS:
    case TCM_SETMINTABWIDTH:
    case TCM_SETEXTENDEDSTYLE:
    case SB_SIMPLE:
    case SB_SETTEXTW:
    // ProgressBar state is driven entirely by these messages.  Without them a
    // dirty-gated reconcile would never notice a native progress update.
    case PBM_SETPOS:
    case PBM_DELTAPOS:
    case PBM_STEPIT:
    case PBM_SETRANGE:
    case PBM_SETRANGE32:
    case PBM_SETSTEP:
    case PBM_SETSTATE:
    case PBM_SETMARQUEE:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK ControlSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData) {
    const bool statusBar = (refData & 1u) != 0;
    auto* owner = reinterpret_cast<SourceThreadAgent*>(refData & ~DWORD_PTR{ 1 });
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (owner && (RelevantMessage(message) || (statusBar && message == SB_SETMINHEIGHT)))
        owner->MarkDirty(window, message);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ControlSubclassProc, subclassId);
    }
    return result;
}

LRESULT CALLBACK RootSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData) {
    auto* owner = reinterpret_cast<SourceThreadAgent*>(refData);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_CLOSE) MarkWindowCloseCompleted(window);
    if (owner && RelevantMessage(message)) owner->MarkDirty(window, message);
    if (message == WM_NCDESTROY && owner) {
        owner->MarkDestroyed();
        RemoveWindowSubclass(window, RootSubclassProc, subclassId);
    }
    return result;
}

void InstallChildSubclass(SourceThreadAgent* agent, HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        wchar_t className[64]{};
        const bool statusBar = GetClassNameW(hwnd, className,
            static_cast<int>(std::size(className))) > 0 &&
            FluentShell::EqualsIgnoreCase(className, STATUSCLASSNAMEW);
        const DWORD_PTR refData = reinterpret_cast<DWORD_PTR>(agent) |
            static_cast<DWORD_PTR>(statusBar);
        SetWindowSubclass(hwnd, ControlSubclassProc, kControlSubclassId,
            refData);
    }
}

void InstallRootSubclass(SourceThreadAgent* agent) {
    SetWindowSubclass(agent->Root(), RootSubclassProc, kRootSubclassId,
        reinterpret_cast<DWORD_PTR>(agent));
}

// ---------------------------------------------------------------------------
// Source-thread command handlers
//
// Every stage below runs on the target window's own UI thread inside the
// bounded dispatcher callback.  A stage returns true while the command is still
// running and false once AbortIfCancelled has finished it, in which case the
// caller must return immediately and never touch the command again.
// ---------------------------------------------------------------------------

bool ExecuteCapture(Command* command) {
    auto* agent = command->agent;
    command->success = agent->CaptureOnSourceThread(
        command->capture.surfaceId,
        command->capture.revision,
        command->snapshot,
        command->error);
    if (AbortIfCancelled(command)) return false;
    if (!command->success) return true;

    // Observation is installed only for a capture that succeeded, so a rejected
    // window never leaves subclasses behind.
    InstallRootSubclass(agent);
    for (const auto& node : command->snapshot.nodes) {
        if (command->cancelled.load(std::memory_order_acquire)) break;
        InstallChildSubclass(agent, node.hwnd);
    }
    if (command->cancelled.load(std::memory_order_acquire)) {
        for (const auto& node : command->snapshot.nodes) {
            RemoveWindowSubclass(node.hwnd, ControlSubclassProc, kControlSubclassId);
        }
        RemoveWindowSubclass(agent->Root(), RootSubclassProc, kRootSubclassId);
        AbortIfCancelled(command);
        return false;
    }
    agent->ClearDirty();
    return true;
}

// --- Window-level actions ---------------------------------------------------

bool ApplyActivate(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    command->success = SetForegroundWindow(agent->Root()) != FALSE;
    return true;
}

bool ApplyGeometry(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    const RECT bounds = command->action.rect;
    command->success = SetWindowPos(agent->Root(), nullptr,
        bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    return true;
}

bool ApplyMinimize(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    ShowWindow(agent->Root(), SW_MINIMIZE);
    command->success = IsIconic(agent->Root()) != FALSE;
    return true;
}

bool ApplyMaximize(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    ShowWindow(agent->Root(), SW_MAXIMIZE);
    command->success = IsZoomed(agent->Root()) != FALSE;
    return true;
}

bool ApplyRestoreState(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    ShowWindow(agent->Root(), SW_RESTORE);
    command->success = !IsIconic(agent->Root()) && !IsZoomed(agent->Root());
    return true;
}

bool ApplyClose(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    // WM_CLOSE is allowed to enter an application-owned modal loop (for example
    // an unsaved-document prompt).  Sending it from this bounded command would
    // keep Invoke blocked until the user answers and make the Bridge tear down a
    // healthy projection at its 2 s deadline.  Queue the request for the native
    // message loop just as we do for buttons and menu commands.  Destruction is
    // observed by the root subclass and the reconcile path; if the handler
    // returns without destroying the root, reconcile reports closeRejected only
    // after that complete modal/veto lifetime.
    command->success = PostMessageW(agent->Root(), WM_CLOSE, 0, 0) != FALSE;
    if (command->success) {
        // This hook is executing on the source UI thread, so the posted message
        // cannot reach RootSubclassProc until this command returns.  Registering
        // after a successful post is therefore race-free.
        command->outcome.closeSequence = agent->RegisterCloseRequest();
    }
    return true;
}

bool ApplyMenuCommand(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    // Menu handlers may enter a modal loop.  Queue the validated WM_COMMAND so
    // this bounded source-thread command can return; periodic reconciliation
    // captures the resulting native state.
    command->success = PostMessageW(agent->Root(), WM_COMMAND,
        MAKEWPARAM(command->action.menuCommandId, 0), 0) != FALSE;
    return true;
}

using WindowAction = bool (*)(Command*, SourceThreadAgent*);

struct WindowActionEntry final {
    std::wstring_view name;
    WindowAction apply;
};

constexpr std::array kWindowActions{
    WindowActionEntry{ L"activate", &ApplyActivate },
    WindowActionEntry{ L"move", &ApplyGeometry },
    WindowActionEntry{ L"resize", &ApplyGeometry },
    WindowActionEntry{ L"minimize", &ApplyMinimize },
    WindowActionEntry{ L"maximize", &ApplyMaximize },
    WindowActionEntry{ L"restore", &ApplyRestoreState },
    WindowActionEntry{ L"close", &ApplyClose },
    WindowActionEntry{ L"menuCommand", &ApplyMenuCommand },
};

WindowAction FindWindowAction(std::wstring_view action) noexcept {
    for (const auto& entry : kWindowActions) {
        if (entry.name == action) return entry.apply;
    }
    return nullptr;
}

// --- Node-level actions -----------------------------------------------------

bool ClickButton(Command* command, HWND target) {
    // A button handler is allowed to enter a synchronous MessageBox/TaskDialog.
    // Queue BM_CLICK so this command can acknowledge within the bounded
    // dispatcher deadline; the native message loop then runs the modal API
    // normally after the hook returns.
    if (AbortIfCancelled(command)) return false;
    command->success = PostMessageW(target, BM_CLICK, 0, 0) != FALSE;
    return true;
}

bool ActivateSysLink(Command* command, HWND target) {
    // The bounded SysLink adapter accepts exactly one link.  Let the native
    // control generate its normal NM_RETURN notification instead of fabricating
    // a parent callback, and queue the keystrokes so this command is not held
    // across a handler that may open another window.
    if (AbortIfCancelled(command)) return false;
    LITEM item{};
    item.mask = LIF_ITEMINDEX | LIF_STATE;
    item.iLink = 0;
    item.stateMask = LIS_FOCUSED;
    item.state = LIS_FOCUSED;
    const bool focused = SendMessageW(
        target, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item)) != FALSE;
    // The native HWND is cloaked and the renderer owns the real keyboard focus,
    // so a bounded synthetic focus lifetime is posted to the SysLink itself.
    // Its standard WM_KEYDOWN handler then emits NM_RETURN without stealing the
    // desktop focus from the proxy.
    const bool focusEntered = focused &&
        PostMessageW(target, WM_SETFOCUS, 0, 0) != FALSE;
    const bool down = focusEntered &&
        PostMessageW(target, WM_KEYDOWN, VK_RETURN, 1) != FALSE;
    const bool up = down &&
        PostMessageW(target, WM_KEYUP, VK_RETURN, 1 | (1ll << 30) | (1ll << 31)) != FALSE;
    const bool focusLeft = up && PostMessageW(target, WM_KILLFOCUS, 0, 0) != FALSE;
    command->success = focusLeft;
    return true;
}

bool ApplyInvoke(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind == ControlKind::Button) return ClickButton(command, target);
    if (node.kind == ControlKind::SysLink) return ActivateSysLink(command, target);
    return true;
}

bool ApplySetText(
    Command* command, SourceThreadAgent* agent, HWND target, const ControlNode& node) {
    const bool writableText = node.kind == ControlKind::Edit ||
        node.kind == ControlKind::Password ||
        (node.kind == ControlKind::ComboBox && node.editable);
    if (!writableText || node.readOnly) return true;
    if (AbortIfCancelled(command)) return false;
    command->success = SetWindowTextW(target, command->action.text.c_str()) != FALSE;
    if (!command->success || command->cancelled.load(std::memory_order_acquire)) {
        return true;
    }
    // SetWindowTextW does not notify the parent, so the native handler that
    // would have observed the user typing is invoked explicitly.
    const bool combo = node.kind == ControlKind::ComboBox;
    SendMessageW(SyntheticNotificationTarget(agent->Root(), target), WM_COMMAND,
        MAKEWPARAM(node.controlId, combo ? CBN_EDITCHANGE : EN_CHANGE),
        reinterpret_cast<LPARAM>(target));
    return true;
}

bool ApplySetCheck(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    const bool toggle = node.kind == ControlKind::CheckBox ||
        node.kind == ControlKind::ThreeState ||
        node.kind == ControlKind::RadioButton;
    if (!toggle) return true;
    const int requested = command->action.integerValue;
    const int maximum = node.kind == ControlKind::ThreeState ? 2 : 1;
    const bool validValue = requested >= 0 && requested <= maximum &&
        (node.kind != ControlKind::RadioButton || requested == 1);
    if (!validValue) return true;
    if (AbortIfCancelled(command)) return false;

    // BM_SETCHECK would bypass the application handler, so the native control is
    // clicked through its own state machine until it reports the requested
    // value.  A click that does not move the state ends the walk.
    int current = static_cast<int>(SendMessageW(target, BM_GETCHECK, 0, 0));
    for (int attempt = 0; current != requested && attempt <= maximum; ++attempt) {
        if (AbortIfCancelled(command)) return false;
        SendMessageW(target, BM_CLICK, 0, 0);
        const int next = static_cast<int>(SendMessageW(target, BM_GETCHECK, 0, 0));
        if (next == current) break;
        current = next;
    }
    command->success =
        static_cast<int>(SendMessageW(target, BM_GETCHECK, 0, 0)) == requested;
    return true;
}

bool ApplySelect(
    Command* command, SourceThreadAgent* agent, HWND target, const ControlNode& node) {
    const bool selectable = node.kind == ControlKind::ComboBox ||
        node.kind == ControlKind::ListBox || node.kind == ControlKind::TabControl;
    if (!selectable) return true;
    const int requested = command->action.integerValue;
    const bool tab = node.kind == ControlKind::TabControl;
    const bool validIndex = requested >= (tab ? 0 : -1) &&
        (requested == -1 || static_cast<size_t>(requested) < node.items.size());
    if (!validIndex) return true;
    if (AbortIfCancelled(command)) return false;

    if (tab) {
        if (AbortIfCancelled(command)) return false;
        command->success = SelectTabControl(
            agent->Root(), target, node.controlId, requested,
            static_cast<int>(node.items.size()));
        return true;
    }
    const bool combo = node.kind == ControlKind::ComboBox;
    SendMessageW(target, combo ? CB_SETCURSEL : LB_SETCURSEL, requested, 0);
    const int selected = static_cast<int>(
        SendMessageW(target, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0));
    if (selected != requested || command->cancelled.load(std::memory_order_acquire)) {
        return true;
    }
    SendMessageW(SyntheticNotificationTarget(agent->Root(), target), WM_COMMAND,
        MAKEWPARAM(node.controlId, combo ? CBN_SELCHANGE : LBN_SELCHANGE),
        reinterpret_cast<LPARAM>(target));
    command->success = true;
    return true;
}

bool ApplySetSelection(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::ListView) return true;
    if (AbortIfCancelled(command)) return false;
    const auto& requested = command->action.integerValues;

    LVITEMW state{};
    state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    state.state = 0;
    SendMessageW(target, LVM_SETITEMSTATE,
        static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&state));
    for (size_t index = 0; index < requested.size(); ++index) {
        if (AbortIfCancelled(command)) return false;
        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        state.state = LVIS_SELECTED | (index == 0 ? LVIS_FOCUSED : 0);
        SendMessageW(target, LVM_SETITEMSTATE,
            static_cast<WPARAM>(requested[index]), reinterpret_cast<LPARAM>(&state));
    }

    // Read the selection back through the same enumeration the capture uses, so
    // an accepted result always matches what the next snapshot will report.
    std::vector<int> actual;
    int previous = -1;
    for (;;) {
        const int selected = static_cast<int>(
            SendMessageW(target, LVM_GETNEXTITEM, previous, LVNI_SELECTED));
        if (selected < 0) break;
        if (selected <= previous ||
            static_cast<size_t>(selected) >= node.rows.size() ||
            actual.size() >= node.rows.size()) {
            return true;
        }
        actual.push_back(selected);
        previous = selected;
    }
    command->success = actual == requested;
    return true;
}

bool ExecuteCaptureDirectUiEvidence(Command* command) {
    command->success = command->profile &&
        CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, command->directUiEvidence, command->error);
    if (AbortIfCancelled(command)) return false;
    if (!command->success) return true;
    InstallRootSubclass(command->agent);
    EnumChildWindows(command->agent->Root(), [](HWND child, LPARAM raw) -> BOOL {
        InstallChildSubclass(reinterpret_cast<SourceThreadAgent*>(raw), child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(command->agent));
    command->agent->ClearDirty();
    return true;
}

bool ExecuteCaptureDirectUiBootstrap(Command* command) {
    command->success = CaptureDirectUiBootstrapEvidenceOnSourceThread(
        *command->agent, command->directUiBootstrapEvidence, command->error);
    if (AbortIfCancelled(command)) return false;
    if (!command->success) return true;
    InstallRootSubclass(command->agent);
    EnumChildWindows(command->agent->Root(), [](HWND child, LPARAM raw) -> BOOL {
        InstallChildSubclass(reinterpret_cast<SourceThreadAgent*>(raw), child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(command->agent));
    command->agent->ClearDirty();
    return true;
}

bool ApplySetItemCheck(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::ListView || !node.checkBoxes) return true;
    const int index = command->action.itemIndex;
    if (index < 0 || static_cast<size_t>(index) >= node.rows.size()) return true;
    if (AbortIfCancelled(command)) return false;

    const bool applied = SetListViewItemCheck(
        target, index, command->action.booleanValue);
    if (AbortIfCancelled(command)) return false;
    command->success = applied;
    return true;
}

bool ApplyToolbarCommand(
    Command* command, SourceThreadAgent* agent, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::Toolbar) return true;
    if (AbortIfCancelled(command)) return false;
    command->success = PostMessageW(SyntheticNotificationTarget(agent->Root(), target), WM_COMMAND,
        MAKEWPARAM(command->action.menuCommandId, 0), reinterpret_cast<LPARAM>(target)) != FALSE;
    return true;
}

using NodeAction = bool (*)(Command*, SourceThreadAgent*, HWND, const ControlNode&);

struct NodeActionEntry final {
    std::wstring_view name;
    NodeAction apply;
};

constexpr std::array kNodeActions{
    NodeActionEntry{ L"invoke", &ApplyInvoke },
    NodeActionEntry{ L"setText", &ApplySetText },
    NodeActionEntry{ L"setCheck", &ApplySetCheck },
    NodeActionEntry{ L"select", &ApplySelect },
    NodeActionEntry{ L"setSelection", &ApplySetSelection },
    NodeActionEntry{ L"setItemCheck", &ApplySetItemCheck },
    NodeActionEntry{ L"toolbarCommand", &ApplyToolbarCommand },
};

NodeAction FindNodeAction(std::wstring_view action) noexcept {
    for (const auto& entry : kNodeActions) {
        if (entry.name == action) return entry.apply;
    }
    return nullptr;
}

// Resolves the addressed control in the baseline capture and hands it to its
// action.  A node that is gone, non-interactive, or has no matching action
// leaves command->success false, which the caller reports as a rejection.
bool InvokeOnNode(
    Command* command, SourceThreadAgent* agent, const WindowSnapshot& before) {
    const auto& action = command->action;
    const auto node = std::find_if(before.nodes.begin(), before.nodes.end(),
        [&](const ControlNode& candidate) { return candidate.nodeId == *action.nodeId; });
    if (node == before.nodes.end()) return true;
    if (AbortIfCancelled(command)) return false;
    if (!node->visible || !node->enabled) return true;
    const NodeAction apply = FindNodeAction(action.action);
    return apply == nullptr || apply(command, agent, node->hwnd, *node);
}

bool ExecuteInvoke(Command* command) {
    auto* agent = command->agent;
    const ActionRequest& action = command->action;

    // Resolve the action against a fresh capture.  The caller has already
    // checked the expected revision, so a full scan is the safest read here.
    if (AbortIfCancelled(command)) return false;
    WindowSnapshot before;
    std::wstring captureError;
    if (!agent->CaptureOnSourceThread(
            action.surfaceId, action.expectedRevision, before, captureError)) {
        command->error = captureError;
        return true;
    }
    if (!ValidateActionForSnapshot(action, before, command->error)) return true;
    if (AbortIfCancelled(command)) return false;

    if (const WindowAction apply = FindWindowAction(action.action)) {
        if (!apply(command, agent)) return false;
    } else if (action.nodeId && !InvokeOnNode(command, agent, before)) {
        return false;
    }

    if (command->success && (agent->IsDestroyed() || !IsWindow(agent->Root()))) {
        command->outcome.destroyed = true;
    }
    if (AbortIfCancelled(command)) return false;
    if (command->success && !command->outcome.destroyed) {
        // Verify: the accepted revision is whatever a fresh capture reports, not
        // what the action asked for.
        command->success = agent->CaptureOnSourceThread(
            action.surfaceId, action.expectedRevision + 1,
            command->outcome.snapshot, command->error);
        if (AbortIfCancelled(command)) return false;
        command->outcome.accepted = command->success;
        command->outcome.revision = action.expectedRevision + 1;
    }
    return true;
}

// --- Cloak lifetime ---------------------------------------------------------

// Applies DWMWA_CLOAK and confirms DWM published it.  Cancellation between the
// two must never leave the native window hidden, so a cancelled cloak is undone
// before the command is finished.
bool SetCloakAndVerify(
    Command* command, HWND root, bool cloaked, const wchar_t* failureReason) {
    BOOL value = cloaked ? TRUE : FALSE;
    const HRESULT applied = DwmSetWindowAttribute(root, DWMWA_CLOAK, &value, sizeof(value));
    if (command->cancelled.load(std::memory_order_acquire)) {
        if (cloaked) {
            value = FALSE;
            DwmSetWindowAttribute(root, DWMWA_CLOAK, &value, sizeof(value));
        }
        AbortIfCancelled(command);
        return false;
    }
    DWORD reasons = cloaked ? 0 : DWM_CLOAKED_APP;
    const HRESULT verified = DwmGetWindowAttribute(
        root, DWMWA_CLOAKED, &reasons, sizeof(reasons));
    command->success = SUCCEEDED(applied) && SUCCEEDED(verified) &&
        ((reasons & DWM_CLOAKED_APP) != 0) == cloaked;
    if (!command->success) command->error = failureReason;
    return true;
}

bool ExecuteCloak(Command* command) {
    if (AbortIfCancelled(command)) return false;
    return SetCloakAndVerify(
        command, command->agent->Root(), command->cloaked, L"DWMWA_CLOAK failed");
}

bool ExecuteCaptureAndCloak(Command* command) {
    auto* agent = command->agent;
    if (AbortIfCancelled(command)) return false;
    command->success = agent->CaptureOnSourceThread(
        command->capture.surfaceId,
        command->capture.revision,
        command->snapshot,
        command->error);
    command->captured = command->success;
    // The barrier: cloak only the exact native state the renderer already
    // validated, so a concurrent native change cannot be hidden behind a stale
    // projection.
    if (command->success &&
        SnapshotFingerprint(command->snapshot) != command->expectedFingerprint) {
        command->success = false;
        command->error = L"native revision changed before cloak";
    }
    if (AbortIfCancelled(command)) return false;
    if (!command->success) return true;
    return SetCloakAndVerify(
        command, agent->Root(), true, L"native cloak verification failed");
}

bool ExecuteVerifyDirectUiAndCloak(Command* command) {
    DirectUiNativeEvidence current;
    if (!command->profile ||
        !CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, current, command->error) ||
        !MatchDirectUiMutationBracket(*command->profile,
            command->expectedDirectUiEvidence, current, command->error, false)) {
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    return SetCloakAndVerify(command, command->agent->Root(), true,
        L"DirectUI native cloak verification failed");
}

bool ExecuteRestoreThenDirectUiClick(Command* command) {
    DirectUiNativeEvidence current;
    if (!command->profile ||
        !CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, current, command->error) ||
        !MatchDirectUiMutationBracket(*command->profile,
            command->expectedDirectUiEvidence, current, command->error, false)) {
        return true;
    }
    const DirectUiActionBinding& binding = command->directUiBinding;
    const size_t slot = binding.slotIndex;
    // Both handoff routes are declared by the slot itself, so the binding may
    // only reach the one its own profile row published.
    const bool handoffRoute = binding.action == DirectUiAction::HandoffClick ||
        binding.action == DirectUiAction::HandoffLinkClick;
    bool bindingMatches = slot < command->profile->slotCount && handoffRoute &&
        command->profile->slots[slot].action == binding.action &&
        !command->profile->slots[slot].virtualSource &&
        slot < current.slotWindows.size() &&
        current.slotWindows[slot].hwnd == binding.hwnd &&
        current.slotWindows[slot].generation == binding.generation &&
        current.slotWindows[slot].enabled;
    if (!bindingMatches) {
        command->error = L"DirectUI handoff backing generation changed";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    if (!SetCloakAndVerify(command, command->agent->Root(), false,
            L"DirectUI handoff native uncloak failed")) return false;
    if (!command->success) return true;
    SetWindowPos(command->agent->Root(), nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetForegroundWindow(command->agent->Root());
    DirectUiNativeEvidence restored;
    if (!CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, restored, command->error)) {
        command->success = false;
        return true;
    }
    bindingMatches = slot < restored.slotWindows.size() &&
        restored.slotWindows[slot].hwnd == binding.hwnd &&
        restored.slotWindows[slot].generation == binding.generation &&
        restored.slotWindows[slot].enabled;
    DWORD cloak = DWM_CLOAKED_APP;
    const bool visible = IsWindowVisible(command->agent->Root()) != FALSE;
    const bool uncloaked = SUCCEEDED(DwmGetWindowAttribute(command->agent->Root(),
        DWMWA_CLOAKED, &cloak, sizeof(cloak))) && (cloak & DWM_CLOAKED_APP) == 0;
    if (!DirectUiHandoffMayPost(true, bindingMatches, true, visible, uncloaked)) {
        command->success = false;
        command->error = L"DirectUI handoff native visibility verification failed";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    // Both routes give the page up, so the native control is driven exactly the
    // way the Win32 lane drives it: a push button through BM_CLICK, a link
    // through its own NM_RETURN notification.
    if (binding.action == DirectUiAction::HandoffLinkClick) {
        if (!ActivateSysLink(command, binding.hwnd)) return false;
        if (!command->success)
            command->error = L"DirectUI handoff link activation failed";
        return true;
    }
    command->success = PostMessageW(binding.hwnd, BM_CLICK, 0, 0) != FALSE;
    if (!command->success) command->error = L"DirectUI handoff BM_CLICK post failed";
    return true;
}

bool ExecutePostDirectUiPropertySheetButton(Command* command) {
    DirectUiNativeEvidence current;
    if (!command->profile ||
        !CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, current, command->error) ||
        !MatchDirectUiMutationBracket(*command->profile,
            command->expectedDirectUiEvidence, current, command->error)) {
        return true;
    }
    const auto& binding = command->directUiBinding;
    const size_t slotIndex = binding.slotIndex;
    const bool validButton = binding.propertySheetButton == PSBTN_BACK ||
        binding.propertySheetButton == PSBTN_NEXT ||
        binding.propertySheetButton == PSBTN_FINISH ||
        binding.propertySheetButton == PSBTN_CANCEL;
    const bool bindingMatches = validButton &&
        binding.action == DirectUiAction::HandoffPropertySheetButton &&
        slotIndex < command->profile->slotCount &&
        command->profile->slots[slotIndex].virtualSource &&
        command->profile->slots[slotIndex].uiaEnabled &&
        command->profile->slots[slotIndex].action == binding.action &&
        command->profile->slots[slotIndex].propertySheetButton ==
            binding.propertySheetButton &&
        current.root.hwnd == binding.hwnd &&
        current.root.generation == binding.generation &&
        current.propertySheetPageHwnd != nullptr &&
        std::find(current.pageHosts.begin(), current.pageHosts.end(),
            current.propertySheetPageHwnd) != current.pageHosts.end();
    if (!bindingMatches) {
        command->error = L"DirectUI property-sheet action provenance changed";
        return true;
    }
    DWORD cloak = DWM_CLOAKED_APP;
    const bool visible = IsWindowVisible(command->agent->Root()) != FALSE;
    const bool enabled = IsWindowEnabled(command->agent->Root()) != FALSE;
    const bool uncloaked = SUCCEEDED(DwmGetWindowAttribute(command->agent->Root(),
        DWMWA_CLOAKED, &cloak, sizeof(cloak))) && (cloak & DWM_CLOAKED_APP) == 0;
    if (!visible || !enabled || !uncloaked) {
        command->error = L"DirectUI property-sheet root is not interactive";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    command->success = PostMessageW(command->agent->Root(), PSM_PRESSBUTTON,
        static_cast<WPARAM>(binding.propertySheetButton), 0) != FALSE;
    if (!command->success)
        command->error = L"DirectUI handoff PSM_PRESSBUTTON post failed";
    return true;
}

// Queue one native click and let the application-owned handler return through
// its normal message loop. The action worker captures and verifies the resulting
// canonical state before it acknowledges the renderer.
bool ExecuteDirectUiToggle(Command* command) {
    const HWND target = command->directUiBinding.hwnd;
    if (!target || !IsWindow(target)) {
        command->error = L"DirectUI toggle backing window is gone";
        return true;
    }
    const int requested = command->action.integerValue;
    if (requested != 0 && requested != 1) {
        command->error = L"DirectUI toggle requires a two-state value";
        return true;
    }
    wchar_t className[64]{};
    GetClassNameW(target, className, static_cast<int>(std::size(className)));
    const bool bitmapSwitch = FluentShell::EqualsIgnoreCase(className, L"BitmapSwitchClass");
    int current = 0;
    if (bitmapSwitch) {
        if (requested != 1) {
            command->error = L"DirectUI radio cannot be unchecked";
            return true;
        }
        current = (static_cast<DWORD>(GetWindowLongPtrW(target, GWL_STYLE)) & WS_TABSTOP) != 0;
    } else {
        current = static_cast<int>(SendMessageW(target, BM_GETCHECK, 0, 0));
        if (current != BST_CHECKED && current != BST_UNCHECKED) {
            command->error = L"DirectUI toggle backing state is not two-state";
            return true;
        }
    }
    command->sibling = GetActiveWindow();
    if (current == requested) {
        command->success = true;
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    const HWND root = command->agent->Root();
    if (command->sibling != root) {
        SetActiveWindow(root);
        if (GetActiveWindow() != root) {
            command->error = L"DirectUI toggle could not activate the native dialog";
            return true;
        }
    }
    if (bitmapSwitch) {
        RECT client{};
        if (!GetClientRect(target, &client) ||
            client.right <= client.left || client.bottom <= client.top) {
            command->error = L"DirectUI radio client bounds are unavailable";
            return true;
        }
        const LPARAM hit = MAKELPARAM((client.right - client.left) / 2,
            (client.bottom - client.top) / 2);
        command->success =
            PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, hit) != FALSE &&
            PostMessageW(target, WM_LBUTTONUP, 0, hit) != FALSE;
        if (!command->success) {
            SetActiveWindow(command->sibling);
            command->error = L"DirectUI radio mouse click post failed";
        }
        return true;
    }
    command->success = PostMessageW(target, BM_CLICK, 0, 0) != FALSE;
    if (!command->success) {
        SetActiveWindow(command->sibling);
        command->error = L"DirectUI toggle BM_CLICK post failed";
    }
    return true;
}

// Drives one projected DirectUI slot through its own registered Win32 adapter and
// its own notification path, so the application handler observes exactly the
// transition a user would have produced and the page stays projected. The route
// must be one that slot itself declared, the backing HWND identity and lifecycle
// generation must still match the binding, and the node handed to the action is
// read fresh here rather than trusted from the published snapshot.
bool ExecuteDirectUiNodeAction(Command* command) {
    DirectUiNativeEvidence current;
    if (!command->profile ||
        !CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, current, command->error) ||
        !MatchDirectUiMutationBracket(*command->profile,
            command->expectedDirectUiEvidence, current, command->error, false)) {
        return true;
    }
    const DirectUiActionBinding& binding = command->directUiBinding;
    const size_t slot = binding.slotIndex;
    const DirectUiAction route =
        DirectUiActionForRequest(binding, command->action.action);
    // Checkboxes and radios keep their dedicated route: it posts the click and
    // walks the state machine instead of sending, because a toggle handler is the
    // one in-place case already observed to open application-owned modal windows.
    const bool dedicatedRoute = route == DirectUiAction::ToggleCheck ||
        route == DirectUiAction::SelectRadio;
    const bool bindingMatches = route != DirectUiAction::None && !dedicatedRoute &&
        IsDirectUiInPlaceAction(route) &&
        slot < command->profile->slotCount &&
        !command->profile->slots[slot].virtualSource &&
        command->profile->slots[slot].kind == binding.kind &&
        (command->profile->slots[slot].action == route ||
            command->profile->slots[slot].secondaryAction == route) &&
        slot < current.slotWindows.size() &&
        current.slotWindows[slot].hwnd == binding.hwnd &&
        current.slotWindows[slot].generation == binding.generation &&
        current.slotWindows[slot].visible &&
        current.slotWindows[slot].enabled;
    if (!bindingMatches) {
        command->error = L"DirectUI in-place action provenance changed";
        return true;
    }
    ControlNode node;
    if (!CaptureDirectUiSlotNode(binding.hwnd, binding.kind, node, command->error)) {
        return true;
    }
    if (!node.visible || !node.enabled) {
        command->error = L"DirectUI in-place target is not interactive";
        return true;
    }
    const NodeAction apply = FindNodeAction(command->action.action);
    if (!apply) {
        command->error = L"DirectUI in-place action has no native route";
        return true;
    }
    // The native dialog owns activation while its own handler runs; the renderer
    // proxy takes it back through RestoreDirectUiActivation once the action worker
    // has accepted the resulting canonical state.
    command->sibling = GetActiveWindow();
    if (AbortIfCancelled(command)) return false;
    const HWND root = command->agent->Root();
    if (command->sibling != root) {
        SetActiveWindow(root);
        if (GetActiveWindow() != root) {
            command->error =
                L"DirectUI in-place action could not activate the native dialog";
            return true;
        }
    }
    if (!apply(command, command->agent, binding.hwnd, node)) return false;
    if (!command->success) {
        SetActiveWindow(command->sibling);
        if (command->error.empty())
            command->error = L"DirectUI in-place action was refused by the native control";
    }
    return true;
}

// Presses one handoff-declared DirectUI navigation control while the surface stays
// projected: the root keeps its application cloak, the renderer proxy keeps the
// screen, and the session admits whatever page replaces this one in place. The
// pre-press UIA revalidation the giving-up route runs is deliberately absent -- a
// cloaked root exposes no UIA subtree to walk -- and is not needed here, because
// the press is followed by a full fresh admission of the resulting page, which is
// a stronger check than revalidating the page being left. PSBTN_CANCEL is refused:
// cancel ends the window rather than navigating, so it keeps the handoff route.
bool ExecuteNavigateDirectUiProjected(Command* command) {
    DirectUiNativeEvidence current;
    if (!command->profile ||
        !CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, current, command->error) ||
        !MatchDirectUiMutationBracket(*command->profile,
            command->expectedDirectUiEvidence, current, command->error, false)) {
        return true;
    }
    const DirectUiActionBinding& binding = command->directUiBinding;
    // One definition of "this route navigates rather than ends the window" lives
    // in the engine; refuse anything else here so a terminal route can never take
    // the stay-projected path.
    if (!DirectUiNavigationMayStayProjected(binding)) {
        command->error = L"DirectUI projected navigation route is not a page navigation";
        return true;
    }
    const size_t slot = binding.slotIndex;
    if (slot >= command->profile->slotCount ||
        command->profile->slots[slot].action != binding.action) {
        command->error = L"DirectUI projected navigation provenance changed";
        return true;
    }
    const DirectUiSlot& declared = command->profile->slots[slot];
    bool bindingMatches = false;
    if (binding.action == DirectUiAction::HandoffPropertySheetButton) {
        // A virtual slot has no backing HWND, so its provenance is the property
        // sheet itself: the admitted root identity and lifecycle generation, the
        // button the slot declared, and a page host the sheet still owns.
        bindingMatches = declared.virtualSource && declared.uiaEnabled &&
            declared.propertySheetButton == binding.propertySheetButton &&
            current.root.hwnd == binding.hwnd &&
            current.root.generation == binding.generation &&
            current.propertySheetPageHwnd != nullptr &&
            std::find(current.pageHosts.begin(), current.pageHosts.end(),
                current.propertySheetPageHwnd) != current.pageHosts.end();
    } else if (binding.action == DirectUiAction::HandoffClick ||
        binding.action == DirectUiAction::HandoffLinkClick) {
        bindingMatches = !declared.virtualSource &&
            slot < current.slotWindows.size() &&
            current.slotWindows[slot].hwnd == binding.hwnd &&
            current.slotWindows[slot].generation == binding.generation &&
            current.slotWindows[slot].visible &&
            current.slotWindows[slot].enabled;
    }
    if (!bindingMatches) {
        command->error = L"DirectUI projected navigation provenance changed";
        return true;
    }
    const HWND root = command->agent->Root();
    DWORD cloak = 0;
    const bool visible = IsWindowVisible(root) != FALSE;
    const bool enabled = IsWindowEnabled(root) != FALSE;
    const bool cloaked = SUCCEEDED(DwmGetWindowAttribute(
        root, DWMWA_CLOAKED, &cloak, sizeof(cloak))) &&
        (cloak & DWM_CLOAKED_APP) != 0;
    if (!visible || !enabled || !cloaked) {
        command->error = L"DirectUI projected navigation root is not projected";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    // The native dialog owns activation while its own page handler runs; the proxy
    // takes it back through RestoreDirectUiActivation once the session has
    // accepted the page that replaced this one.
    command->sibling = GetActiveWindow();
    if (command->sibling != root) {
        SetActiveWindow(root);
        if (GetActiveWindow() != root) {
            command->error =
                L"DirectUI projected navigation could not activate the native dialog";
            return true;
        }
    }
    if (binding.action == DirectUiAction::HandoffLinkClick) {
        if (!ActivateSysLink(command, binding.hwnd)) return false;
        if (!command->success) {
            SetActiveWindow(command->sibling);
            command->error = L"DirectUI projected link activation failed";
        }
        return true;
    }
    if (binding.action == DirectUiAction::HandoffClick) {
        command->success = PostMessageW(binding.hwnd, BM_CLICK, 0, 0) != FALSE;
        if (!command->success) {
            SetActiveWindow(command->sibling);
            command->error = L"DirectUI projected BM_CLICK post failed";
        }
        return true;
    }
    command->success = PostMessageW(root, PSM_PRESSBUTTON,
        static_cast<WPARAM>(binding.propertySheetButton), 0) != FALSE;
    if (!command->success) {
        SetActiveWindow(command->sibling);
        command->error = L"DirectUI projected PSM_PRESSBUTTON post failed";
    }
    return true;
}

bool ExecuteRestoreDirectUiActivation(Command* command) {
    if (AbortIfCancelled(command)) return false;
    HWND desired = command->sibling;
    if (desired && (!IsWindow(desired) ||
        GetWindowThreadProcessId(desired, nullptr) != command->agent->ThreadId())) {
        desired = nullptr;
    }
    SetActiveWindow(desired);
    command->success = true;
    return true;
}

bool ExecutePlaceBehind(Command* command) {
    if (!command->sibling || !IsWindow(command->sibling) ||
        GetAncestor(command->sibling, GA_ROOT) != command->sibling) {
        command->error = L"proxy sibling is unavailable for native z-order placement";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    command->success = SetWindowPos(command->agent->Root(), command->sibling,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
    if (!command->success)
        command->error = L"native/proxy relative z-order placement failed";
    return true;
}

bool ExecuteDirectUiMove(Command* command) {
    DirectUiNativeEvidence before;
    if (!command->profile ||
        !CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, before, command->error) ||
        !MatchDirectUiMutationBracket(*command->profile,
            command->expectedDirectUiEvidence, before, command->error, false)) {
        return true;
    }
    const RECT requested = command->action.rect;
    const int64_t beforeWidth = static_cast<int64_t>(before.root.bounds.right) -
        before.root.bounds.left;
    const int64_t beforeHeight = static_cast<int64_t>(before.root.bounds.bottom) -
        before.root.bounds.top;
    const int64_t requestedWidth = static_cast<int64_t>(requested.right) - requested.left;
    const int64_t requestedHeight = static_cast<int64_t>(requested.bottom) - requested.top;
    if (beforeWidth != requestedWidth || beforeHeight != requestedHeight) {
        command->error = L"DirectUI move cannot change the admitted window size";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    if (!SetWindowPos(command->agent->Root(), nullptr,
            requested.left, requested.top,
            static_cast<int>(requestedWidth), static_cast<int>(requestedHeight),
            SWP_NOZORDER | SWP_NOACTIVATE)) {
        command->error = L"DirectUI native move failed";
        return true;
    }
    if (AbortIfCancelled(command)) return false;
    DirectUiNativeEvidence after;
    if (!CaptureDirectUiNativeEvidenceOnSourceThread(
            *command->agent, *command->profile, after, command->error) ||
        !MatchDirectUiMoveTransition(*command->profile, before, after, command->error)) {
        return true;
    }
    command->directUiEvidence = std::move(after);
    command->agent->ClearDirty();
    command->success = true;
    return true;
}

bool ExecuteRestore(Command* command) {
    auto* agent = command->agent;
    if (AbortIfCancelled(command)) return false;
    if (!SetCloakAndVerify(command, agent->Root(), false,
            L"native window remained application-cloaked")) {
        return false;
    }
    if (command->success) {
        // The frame was composited while cloaked; force a non-client repaint and
        // hand activation back to the window the user is looking at again.
        SetWindowPos(agent->Root(), nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetForegroundWindow(agent->Root());
    }
    return true;
}

bool ExecuteShutdown(Command* command) {
    if (AbortIfCancelled(command)) return false;
    EnumChildWindows(command->agent->Root(), [](HWND child, LPARAM) -> BOOL {
        RemoveWindowSubclass(child, ControlSubclassProc, kControlSubclassId);
        RemovePropW(child, kNodeGenerationProperty);
        RemovePropW(child, kDirectUiGenerationProperty);
        return TRUE;
    }, 0);
    RemovePropW(command->agent->Root(), kDirectUiGenerationProperty);
    RemoveWindowSubclass(command->agent->Root(), RootSubclassProc, kRootSubclassId);
    command->success = true;
    return true;
}

using CommandHandler = bool (*)(Command*);

CommandHandler HandlerFor(UINT kind) noexcept {
    switch (kind) {
    case kCommandCapture: return &ExecuteCapture;
    case kCommandInvoke: return &ExecuteInvoke;
    case kCommandCloak: return &ExecuteCloak;
    case kCommandCaptureAndCloak: return &ExecuteCaptureAndCloak;
    case kCommandRestore: return &ExecuteRestore;
    case kCommandShutdown: return &ExecuteShutdown;
    case kCommandCaptureDirectUiEvidence: return &ExecuteCaptureDirectUiEvidence;
    case kCommandVerifyDirectUiAndCloak: return &ExecuteVerifyDirectUiAndCloak;
    case kCommandRestoreThenDirectUiClick: return &ExecuteRestoreThenDirectUiClick;
    case kCommandDirectUiToggle: return &ExecuteDirectUiToggle;
    case kCommandDirectUiMove: return &ExecuteDirectUiMove;
    case kCommandCaptureDirectUiBootstrap: return &ExecuteCaptureDirectUiBootstrap;
    case kCommandPostDirectUiPropertySheetButton:
        return &ExecutePostDirectUiPropertySheetButton;
    case kCommandPlaceBehind: return &ExecutePlaceBehind;
    case kCommandRestoreDirectUiActivation: return &ExecuteRestoreDirectUiActivation;
    case kCommandDirectUiNodeAction: return &ExecuteDirectUiNodeAction;
    case kCommandNavigateDirectUiProjected:
        return &ExecuteNavigateDirectUiProjected;
    default: return nullptr;
    }
}

void ExecuteCommandImpl(Command* command) {
    auto* agent = command->agent;
    if (!agent || !IsWindow(agent->Root())) {
        command->error = L"source window no longer exists";
        command->outcome.destroyed = true;
        Complete(command);
        return;
    }
    // Every handler reads and writes physical pixel coordinates, whatever DPI
    // awareness the injected thread inherited.
    PhysicalCoordinateScope dpiScope;
    if (!dpiScope.IsValid()) {
        command->error = L"cannot establish physical-coordinate DPI context";
        Complete(command);
        return;
    }
    if (AbortIfCancelled(command)) return;

    const CommandHandler handler = HandlerFor(command->kind);
    if (!handler) {
        command->error = L"unknown source-thread command";
    } else if (!handler(command)) {
        // Cancelled mid-flight; the handler already finished the command.
        return;
    }
    if (AbortIfCancelled(command)) return;
    Complete(command);
}

void ExecuteCommand(Command* command) noexcept {
    try {
        ExecuteCommandImpl(command);
    } catch (...) {
        if (command) {
            command->success = false;
            command->captured = false;
            command->outcome.accepted = false;
            try { command->error = L"source-thread command exception"; } catch (...) {}
            Complete(command);
        }
    }
}

LRESULT CALLBACK SourceHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE && lParam) {
        auto* message = reinterpret_cast<MSG*>(lParam);
        auto* agent = AgentForMessage(message->message);
        if (agent && message->message == agent->MessageId()) {
            auto* command = reinterpret_cast<Command*>(message->lParam);
            if (IsTrackedCommand(command, agent)) {
                UntrackCommand(command);
                message->message = WM_NULL;
                SetEvent(command->started);
                if (!command->cancelled.load()) ExecuteCommand(command);
                else Complete(command);
                Release(command);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK CbtHook(int code, WPARAM wParam, LPARAM lParam) {
    switch (code) {
    case HCBT_CREATEWND:
    case HCBT_DESTROYWND:
    case HCBT_MINMAX:
    case HCBT_MOVESIZE:
        MarkCurrentThreadAgentsDirty(reinterpret_cast<HWND>(wParam));
        break;
    default:
        break;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK CallWndRetHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        const auto* message = reinterpret_cast<CWPRETSTRUCT*>(lParam);
        if (RelevantMessage(message->message) || message->message == WM_NCDESTROY) {
            MarkCurrentThreadAgentsDirty(message->hwnd, message->message);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace

SourceThreadAgent::SourceThreadAgent(
    HWND root, HMODULE module, DWORD threadId, UINT message) noexcept
    : root_(root), module_(module), threadId_(threadId), message_(message),
      generation_(g_nextGeneration.fetch_add(1)) {}

uint64_t SourceThreadAgent::DirectUiWindowGeneration(HWND window) noexcept {
    if (!window || GetCurrentThreadId() != threadId_) return 0;
    const auto existing = reinterpret_cast<uintptr_t>(
        GetPropW(window, kDirectUiGenerationProperty));
    if (existing) return existing;
    const uint64_t value = nextDirectUiWindowGeneration_++;
    return SetPropW(window, kDirectUiGenerationProperty,
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value))) ? value : 0;
}

uint64_t SourceThreadAgent::RegisterCloseRequest() noexcept {
    const uint64_t sequence = closeIssued_.fetch_add(1, std::memory_order_acq_rel) + 1;
    try {
        FluentShell::Log(L"Queued native WM_CLOSE sequence=" + std::to_wstring(sequence));
    } catch (...) {}
    return sequence;
}

void SourceThreadAgent::MarkCloseRequestCompleted() noexcept {
    const uint64_t sequence = closeIssued_.load(std::memory_order_acquire);
    if (sequence == 0 || sequence <= closeCompleted_.load(std::memory_order_acquire)) return;
    closeCompleted_.store(sequence, std::memory_order_release);
    MarkDirty();
    try {
        FluentShell::Log(L"Native WM_CLOSE handler completed sequence=" +
            std::to_wstring(sequence));
    } catch (...) {}
}

bool SourceThreadAgent::CaptureOnSourceThread(
    std::wstring_view surfaceId,
    uint64_t revision,
    WindowSnapshot& snapshot,
    std::wstring& error) noexcept {
    try {
        captureContext_.surfaceId = surfaceId;
        captureContext_.generation = generation_;
        captureContext_.revision = revision;
        return CaptureWindow(root_, captureContext_, snapshot, error);
    } catch (...) {
        try { error = L"source-thread capture exception"; } catch (...) {}
        return false;
    }
}

std::shared_ptr<SourceThreadAgent> SourceThreadAgent::Attach(HWND root, HMODULE module) {
    if (!root || !IsWindow(root)) return {};
    DWORD threadId = GetWindowThreadProcessId(root, nullptr);
    if (!threadId) return {};
    const UINT message = g_nextMessage.fetch_add(1);
    auto agent = std::shared_ptr<SourceThreadAgent>(
        new SourceThreadAgent(root, module, threadId, message));
    {
        std::scoped_lock lock(g_agentsMutex);
        g_agents.emplace(message, agent.get());
    }
    agent->hook_ = SetWindowsHookExW(WH_GETMESSAGE, SourceHook, module, threadId);
    const DWORD messageHookError = agent->hook_ ? ERROR_SUCCESS : GetLastError();
    agent->cbtHook_ = SetWindowsHookExW(WH_CBT, CbtHook, module, threadId);
    const DWORD cbtHookError = agent->cbtHook_ ? ERROR_SUCCESS : GetLastError();
    agent->callWndRetHook_ = SetWindowsHookExW(
        WH_CALLWNDPROCRET, CallWndRetHook, module, threadId);
    const DWORD callWndRetHookError = agent->callWndRetHook_ ? ERROR_SUCCESS : GetLastError();
    if (!agent->hook_ || !agent->cbtHook_ || !agent->callWndRetHook_) {
        FluentShell::Log(L"Source hook install failed: getMessage=" +
            std::to_wstring(messageHookError) + L" cbt=" +
            std::to_wstring(cbtHookError) + L" callWndRet=" +
            std::to_wstring(callWndRetHookError));
        agent->UnhookAll();
        std::scoped_lock lock(g_agentsMutex);
        g_agents.erase(message);
        return {};
    }
    Command* command = CreateCommand(kCommandCapture, agent.get());
    if (!command) {
        agent->UnhookAll();
        std::scoped_lock lock(g_agentsMutex);
        g_agents.erase(message);
        return {};
    }
    command->capture.generation = agent->generation_;
    command->capture.surfaceId = L"00000000-0000-0000-0000-000000000000";
    bool posted = agent->Post(command, 2000, nullptr);
    bool success = posted && command->success;
    const std::wstring initialCaptureError = success
        ? std::wstring()
        : (posted ? command->error : L"source UI thread acknowledgement timed out");
    if (!success && !posted) {
        FluentShell::Log(L"Initial source capture failed within the 2 s deadline: " +
            initialCaptureError);
    }
    if (!posted) {
        // The hook may already be inside ExecuteCommand. Keep every pointer used
        // by that callback valid even though Attach must honor its 2 s deadline;
        // cleanup waits for the callback before removing the dispatch hooks.
        ScheduleTimedOutAttachCleanup(agent, command);
    }
    Release(command);
    if (!success && posted) {
        // A generic-capture failure on a profile-matched process falls back to
        // DirectUI admission; the resolved profile travels with the agent so
        // every later evidence/handoff command validates the same contract.
        std::wstring imagePath;
        std::wstring processError;
        const DirectUiWindowProfile* profile =
            ResolveDirectUiWindowProfile(imagePath, processError);
        if (profile) {
            FluentShell::Log(L"Generic capture deferred to DirectUI adapter " +
                std::wstring(profile->adapterId) + L" page=" +
                std::wstring(profile->pageId) + L": " + initialCaptureError);
            agent->directUiProfile_ = profile;
            command = CreateCommand(kCommandCaptureDirectUiEvidence, agent.get());
            if (command) {
                command->profile = profile;
                posted = agent->Post(command, 2000, nullptr);
                success = posted && command->success;
                if (!success) FluentShell::Log(L"Exact DirectUI profile page '" +
                    std::wstring(profile->pageId) +
                    L"' did not match at attach A: " + command->error);
                Release(command);
            }
        } else {
            FluentShell::Log(L"Initial source capture failed within the 2 s deadline: " +
                initialCaptureError);
            if (!processError.empty()) {
                FluentShell::Log(L"DirectUI application adapter not applicable: " + processError);
            }
        }
        if (!success && posted) {
            std::wstring genericImagePath;
            std::wstring genericError;
            if (ResolveGenericDirectUiImage(genericImagePath, genericError)) {
                agent->directUiProfile_ = nullptr;
                agent->genericDirectUiCandidate_ = true;
                success = true;
                FluentShell::Log(
                    L"DirectUI surface deferred to capability-derived generic admission");
            } else {
                FluentShell::Log(L"Generic DirectUI admission not applicable: " + genericError);
            }
        }
    }
    if (!success) {
        if (posted) {
            agent->UnhookAll();
            std::scoped_lock lock(g_agentsMutex);
            g_agents.erase(message);
        }
        return {};
    }
    return agent;
}

SourceThreadAgent::~SourceThreadAgent() {
    Shutdown();
}

void SourceThreadAgent::UnhookAll() noexcept {
    if (callWndRetHook_) {
        UnhookWindowsHookEx(callWndRetHook_);
        callWndRetHook_ = nullptr;
    }
    if (cbtHook_) {
        UnhookWindowsHookEx(cbtHook_);
        cbtHook_ = nullptr;
    }
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
}

bool SourceThreadAgent::Post(void* rawCommand, DWORD timeoutMs, HANDLE cancelEvent) noexcept {
    auto* command = static_cast<Command*>(rawCommand);
    bool dispatchReference = false;
    try {
        AddRef(command);
        dispatchReference = true;
        if (!TrackCommand(command)) {
            Release(command);
            return false;
        }
        // Publish the dispatch lifetime before the source thread can consume
        // the message.  A successful post keeps this true for the entire
        // callback lifetime, including the timeout-cleanup path.
        command->queued.store(true, std::memory_order_release);
        if (!PostThreadMessageW(threadId_, message_, 0, reinterpret_cast<LPARAM>(command))) {
            command->queued.store(false, std::memory_order_release);
            UntrackCommand(command);
            Release(command);
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        const DWORD started = WaitForSingleObject(command->started, timeoutMs);
        if (started != WAIT_OBJECT_0) {
            command->cancelled = true;
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline
            ? 0
            : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, MAXDWORD));
        if (!cancelEvent) {
            const bool completed = WaitForSingleObject(command->completed, remaining) == WAIT_OBJECT_0;
            if (!completed) command->cancelled = true;
            return completed;
        }
        HANDLE waits[] = { command->completed, cancelEvent };
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, remaining);
        if (result != WAIT_OBJECT_0) command->cancelled = true;
        return result == WAIT_OBJECT_0;
    } catch (...) {
        command->cancelled = true;
        if (dispatchReference && !command->queued.load(std::memory_order_acquire)) {
            UntrackCommand(command);
            Release(command);
        }
        return false;
    }
}

bool SourceThreadAgent::Capture(
    WindowSnapshot& snapshot,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    bool* timedOut) {
    if (timedOut) *timedOut = false;
    Command* command = CreateCommand(kCommandCapture, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->capture.surfaceId = snapshot.surfaceId;
    command->capture.generation = generation_;
    command->capture.revision = snapshot.revision;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) {
        error = L"source UI thread did not acknowledge capture";
        if (timedOut) *timedOut = true;
        Release(command);
        return false;
    }
    const bool success = command->success;
    if (success) snapshot = std::move(command->snapshot);
    else error = command->error;
    Release(command);
    return success;
}

bool SourceThreadAgent::Invoke(
    const ActionRequest& action,
    ActionOutcome& outcome,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandInvoke, this);
    if (!command) {
        outcome.error = L"source command allocation failed";
        return false;
    }
    command->action = action;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) {
        outcome.error = L"source UI thread did not acknowledge action";
        Release(command);
        return false;
    }
    outcome = std::move(command->outcome);
    if (!command->success && outcome.error.empty()) outcome.error = command->error;
    const bool success = command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::SetCloaked(
    bool cloaked,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandCloak, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->cloaked = cloaked;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge cloak";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::Restore(
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandRestore, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge restore";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::CaptureDirectUiNativeEvidence(
    const DirectUiWindowProfile& profile,
    DirectUiNativeEvidence& evidence,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    bool* timedOut) {
    if (timedOut) *timedOut = false;
    Command* command = CreateCommand(kCommandCaptureDirectUiEvidence, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) {
        error = L"source UI thread did not acknowledge DirectUI native evidence capture";
        if (timedOut) *timedOut = true;
    }
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    if (success) evidence = std::move(command->directUiEvidence);
    Release(command);
    return success;
}

bool SourceThreadAgent::VerifyDirectUiAndCloak(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& expected,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandVerifyDirectUiAndCloak, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    command->expectedDirectUiEvidence = expected;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge DirectUI cloak barrier";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::RestoreThenDirectUiButtonClick(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& expected,
    const DirectUiActionBinding& binding,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandRestoreThenDirectUiClick, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    command->expectedDirectUiEvidence = expected;
    command->directUiBinding = binding;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge DirectUI handoff";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::PostDirectUiPropertySheetButton(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& expected,
    const DirectUiActionBinding& binding,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandPostDirectUiPropertySheetButton, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    command->expectedDirectUiEvidence = expected;
    command->directUiBinding = binding;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted)
        error = L"source UI thread did not acknowledge DirectUI property-sheet action";
    else if (!command->success)
        error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::NavigateDirectUiProjected(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& expected,
    const DirectUiActionBinding& binding,
    HWND& previousActive,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    previousActive = nullptr;
    Command* command = CreateCommand(kCommandNavigateDirectUiProjected, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    command->expectedDirectUiEvidence = expected;
    command->directUiBinding = binding;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted)
        error = L"source UI thread did not acknowledge DirectUI projected navigation";
    else if (!command->success)
        error = command->error;
    const bool success = posted && command->success;
    // Activation moved to the native dialog before its page handler ran, so the
    // caller gets it back even when the press itself was refused.
    if (posted) previousActive = command->sibling;
    Release(command);
    return success;
}

bool SourceThreadAgent::InvokeDirectUiToggle(
    const DirectUiActionBinding& binding,
    int requested,
    HWND& previousActive,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    previousActive = nullptr;
    Command* command = CreateCommand(kCommandDirectUiToggle, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->directUiBinding = binding;
    command->action.integerValue = requested;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge DirectUI toggle";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    if (success) previousActive = command->sibling;
    Release(command);
    return success;
}

bool SourceThreadAgent::InvokeDirectUiNodeAction(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& expected,
    const DirectUiActionBinding& binding,
    const ActionRequest& request,
    HWND& previousActive,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    previousActive = nullptr;
    Command* command = CreateCommand(kCommandDirectUiNodeAction, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    command->expectedDirectUiEvidence = expected;
    command->directUiBinding = binding;
    command->action = request;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge DirectUI node action";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    // Activation moved to the native dialog before the handler ran, so the caller
    // gets it back even when the action itself was refused.
    if (posted) previousActive = command->sibling;
    Release(command);
    return success;
}

bool SourceThreadAgent::PlaceBehind(
    HWND sibling,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandPlaceBehind, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->sibling = sibling;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge z-order placement";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::RestoreDirectUiActivation(
    HWND previousActive,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandRestoreDirectUiActivation, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->sibling = previousActive;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge activation restore";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::CaptureDirectUiBootstrapEvidence(
    DirectUiBootstrapEvidence& evidence,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandCaptureDirectUiBootstrap, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge DirectUI bootstrap capture";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    if (success) evidence = std::move(command->directUiBootstrapEvidence);
    Release(command);
    return success;
}

bool SourceThreadAgent::MoveDirectUiWindow(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& expected,
    const RECT& bounds,
    DirectUiNativeEvidence& evidence,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandDirectUiMove, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->profile = &profile;
    command->expectedDirectUiEvidence = expected;
    command->action.action = L"move";
    command->action.rect = bounds;
    command->action.hasRect = true;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) error = L"source UI thread did not acknowledge DirectUI move";
    else if (!command->success) error = command->error;
    const bool success = posted && command->success;
    if (success) evidence = std::move(command->directUiEvidence);
    Release(command);
    return success;
}

bool SourceThreadAgent::CaptureAndCloak(
    WindowSnapshot& snapshot,
    uint64_t expectedFingerprint,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    Command* command = CreateCommand(kCommandCaptureAndCloak, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->capture.surfaceId = snapshot.surfaceId;
    command->capture.generation = generation_;
    command->capture.revision = snapshot.revision;
    command->expectedFingerprint = expectedFingerprint;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    if (!posted) {
        error = L"source UI thread did not acknowledge capture-and-cloak";
        Release(command);
        return false;
    }
    const bool success = command->success;
    if (command->captured) snapshot = std::move(command->snapshot);
    if (!success) error = command->error;
    Release(command);
    return success;
}

bool SourceThreadAgent::EnableGenericDirectUiCandidate() noexcept {
    if (genericDirectUiCandidate_) return true;
    std::wstring imagePath;
    std::wstring error;
    if (!ResolveGenericDirectUiImage(imagePath, error)) {
        FluentShell::Log(
            L"DirectUI generic lane refused for this process: " + error);
        return false;
    }
    genericDirectUiCandidate_ = true;
    FluentShell::Log(
        L"DirectUI surface degraded to the capability-derived generic lane");
    return true;
}

void SourceThreadAgent::AdoptDirectUiProfile(
    std::shared_ptr<DirectUiOwnedProfile> profile) noexcept {
    ownedDirectUiProfile_ = std::move(profile);
    directUiProfile_ = ownedDirectUiProfile_ ? &ownedDirectUiProfile_->profile : nullptr;
}

bool SourceThreadAgent::Shutdown() noexcept {
    try {
    if (shuttingDown_.exchange(true)) {
        return hook_ == nullptr && cbtHook_ == nullptr && callWndRetHook_ == nullptr;
    }
    if (hook_) {
        if (!IsWindow(root_)) {
            UnhookAll();
            std::scoped_lock lock(g_agentsMutex);
            g_agents.erase(message_);
            return true;
        }
        Command* command = CreateCommand(kCommandShutdown, this);
        if (!command) return false;
        const bool acknowledged = Post(command, 500);
        const bool succeeded = command->success;
        Release(command);
        if (!acknowledged || !succeeded) return false;
        UnhookAll();
    }
    std::scoped_lock lock(g_agentsMutex);
    g_agents.erase(message_);
    return true;
    } catch (...) {
        return false;
    }
}

void RetainSourceThreadAgent(std::shared_ptr<SourceThreadAgent> agent) noexcept {
    if (!agent) return;
    try {
        std::scoped_lock lock(g_agentsMutex);
        g_retainedAgents.push_back(std::move(agent));
    } catch (...) {
        // The Bridge is pinned; leaking the final reference is safer than leaving
        // a source-thread hook or subclass with a dangling owner pointer.
        try {
            auto* leaked = new std::shared_ptr<SourceThreadAgent>(std::move(agent));
            (void)leaked;
        } catch (...) {
            // There is no allocation-free way to retain a reference after OOM;
            // keep the failure contained at the injected ABI boundary.
        }
    }
}

} // namespace FluentShell::Bridge::Translation
