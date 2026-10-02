#include "SourceThreadAgent.h"
#include "AccessibleIsland.h"
#include "DirectUiEngine.h"
#include "MenuBarCapture.h"
#include "ListViewActivation.h"
#include "WindowCapture.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>
#include <prsht.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <thread>
#include <new>
#include <limits>

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
constexpr UINT kCommandMenuBarRefresh = 18;
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

// Distinguishes a deferred island action from a tracked command in the posted
// message's wParam, which is zero for every command. Its LPARAM is an opaque token;
// the agent owns the request until dispatch or cancellation, never the message.
constexpr WPARAM kDeferredIslandAction = 1;
constexpr WPARAM kDeferredMenuAction = 2;
constexpr WPARAM kDeferredListViewActivation = 3;
constexpr WPARAM kDeferredNativeAction = 4;

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
    DWORD menuBarPopupWaitMs = 0;
    bool menuBarChanged = false;
    ActionOutcome outcome;
    bool captured = false;
    bool cloaked = false;
    uint64_t expectedFingerprint = 0;
    WPARAM deferredActionKind = 0;
    uint64_t deferredActionToken = 0;
    bool success = false;
    // The application ran the operation and declined it.  That is the application
    // working, not the projection failing, so the surface keeps its projection and
    // the renderer is told the action was rejected.
    bool refused = false;
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
thread_local unsigned g_boundedSourceCommandDepth = 0;

struct BoundedSourceCommandScope final {
    BoundedSourceCommandScope() noexcept { ++g_boundedSourceCommandDepth; }
    ~BoundedSourceCommandScope() {
        if (--g_boundedSourceCommandDepth != 0) return;
        // A provider can pump a posted native action while a bounded command is
        // still capturing. Its message is consumed, then rearmed only after the
        // outer command has called Complete. Reposting inside that provider's
        // PeekMessage loop would keep the loop alive indefinitely.
        try {
            std::scoped_lock lock(g_agentsMutex);
            for (const auto& [_, agent] : g_agents) {
                if (agent && agent->ThreadId() == GetCurrentThreadId())
                    agent->RearmDeferredActionsOnSourceThread();
            }
        } catch (...) {}
    }
};

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
    if (command->deferredActionToken != 0 &&
        (!command->success || command->cancelled.load(std::memory_order_acquire))) {
        command->agent->CancelDeferredActionOnSourceThread(
            command->deferredActionKind, command->deferredActionToken);
    }
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

std::shared_ptr<SourceThreadAgent> AgentForMessage(UINT message) noexcept {
    try {
        std::scoped_lock lock(g_agentsMutex);
        const auto found = g_agents.find(message);
        return found == g_agents.end() || !found->second
            ? nullptr : found->second->weak_from_this().lock();
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<SourceThreadAgent> AgentForSubclass(SourceThreadAgent* candidate) noexcept {
    try {
        // Subclass refData is only a registry key. Shutdown can remove this
        // subclass while an earlier callback is inside a native modal loop.
        std::scoped_lock lock(g_agentsMutex);
        for (const auto& [_, agent] : g_agents) {
            if (agent == candidate && agent) return agent->weak_from_this().lock();
        }
    } catch (...) {}
    return nullptr;
}

void MarkCurrentThreadAgentsDirty(
    HWND window = nullptr, UINT message = 0, bool menuChanged = false) noexcept {
    const DWORD threadId = GetCurrentThreadId();
    try {
        std::scoped_lock lock(g_agentsMutex);
        for (const auto& [_, agent] : g_agents) {
            if (agent && agent->ThreadId() == threadId) {
                if (message == WM_CANCELMODE && window &&
                    (window == agent->Root() || GetAncestor(window, GA_ROOT) == agent->Root()))
                    agent->CancelPopupOnSourceThread(true);
                agent->MarkDirty(window, message);
                if (menuChanged && !MenuBarReadInProgress()) agent->RequestMenuBarRefresh();
            }
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
    // Toolbar and Trackbar mutators out of the switch so aliases cannot create
    // duplicate cases: TBM_SETRANGEMIN and PBM_GETRANGE, TBM_SETSELSTART and
    // SB_SETTEXTW, and several others are the same number.
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
    // Trackbar range, position, and step mutators. Without them a dirty-gated
    // reconcile would never notice a native slider update.
    if (message == TBM_SETPOS || message == TBM_SETPOSNOTIFY ||
        message == TBM_SETRANGE || message == TBM_SETRANGEMIN ||
        message == TBM_SETRANGEMAX || message == TBM_SETLINESIZE ||
        message == TBM_SETPAGESIZE || message == TBM_SETTICFREQ ||
        message == TBM_SETSEL || message == TBM_SETSELSTART ||
        message == TBM_SETSELEND || message == TBM_CLEARSEL ||
        message == TBM_SETBUDDY || message == TBM_SETTOOLTIPS) return true;
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
    case LVM_SETITEMCOUNT:
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
    // TreeView hierarchy, selection, and expansion mutators.
    case TVM_INSERTITEMA:
    case TVM_INSERTITEMW:
    case TVM_DELETEITEM:
    case TVM_EXPAND:
    case TVM_SETITEMA:
    case TVM_SETITEMW:
    case TVM_SELECTITEM:
    case TVM_SORTCHILDREN:
    case TVM_SORTCHILDRENCB:
    case TVM_SETIMAGELIST:
    case TVM_SETINDENT:
    case TVM_SETITEMHEIGHT:
    case TVM_SETEXTENDEDSTYLE:
    case TVM_SETTOOLTIPS:
    case TVM_ENDEDITLABELNOW:
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

bool MenuMutationMessage(UINT message, LPARAM lParam) noexcept {
    switch (message) {
    case WM_COMMAND:
    case WM_MDIACTIVATE:
    case WM_MDICREATE:
    case WM_MDIDESTROY:
    case WM_MDIMAXIMIZE:
    case WM_MDIRESTORE:
    case WM_MDISETMENU:
    case WM_MDINEXT:
    case TVM_SELECTITEM:
    case TVM_EXPAND:
        return true;
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
        if (!notification) return false;
        switch (notification->code) {
        case TVN_SELCHANGEDA:
        case TVN_SELCHANGEDW:
        case TVN_ITEMEXPANDEDA:
        case TVN_ITEMEXPANDEDW:
        case LVN_ITEMCHANGED:
        case TCN_SELCHANGE:
            return true;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

LRESULT CALLBACK ControlSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData) {
    const bool statusBar = (refData & 1u) != 0;
    const auto owner = AgentForSubclass(
        reinterpret_cast<SourceThreadAgent*>(refData & ~DWORD_PTR{ 1 }));
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
    const auto owner = AgentForSubclass(reinterpret_cast<SourceThreadAgent*>(refData));
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
    agent->CancelPopupOnSourceThread();
    const RECT bounds = command->action.rect;
    command->success = SetWindowPos(agent->Root(), nullptr,
        bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    return true;
}

bool ApplyMinimize(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    agent->CancelPopupOnSourceThread();
    ShowWindow(agent->Root(), SW_MINIMIZE);
    command->success = IsIconic(agent->Root()) != FALSE;
    return true;
}

bool ApplyMaximize(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    agent->CancelPopupOnSourceThread();
    ShowWindow(agent->Root(), SW_MAXIMIZE);
    command->success = IsZoomed(agent->Root()) != FALSE;
    return true;
}

bool ApplyRestoreState(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    agent->CancelPopupOnSourceThread();
    ShowWindow(agent->Root(), SW_RESTORE);
    command->success = !IsIconic(agent->Root()) && !IsZoomed(agent->Root());
    return true;
}

bool QueueNativeAction(Command* command, SourceThreadAgent* agent, const ControlNode* node = nullptr) {
    if (AbortIfCancelled(command)) return false;
    command->deferredActionKind = kDeferredNativeAction;
    command->success = agent->PostNativeAction(command->action, node,
        command->refused, command->error, &command->deferredActionToken,
        &command->outcome.closeSequence);
    return true;
}

bool ApplyClose(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    agent->CancelPopupOnSourceThread();
    // A post-action capture can pump messages. Own the close until the bounded
    // command succeeds, then run its complete native modal/veto lifetime.
    return QueueNativeAction(command, agent);
}

bool ApplyMenuCommand(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    if (GetMenu(agent->Root())) return QueueNativeAction(command, agent);
    command->deferredActionKind = kDeferredMenuAction;
    command->success = agent->InvokeMenuCommandOnSourceThread(
        command->action.menuCommandId, command->cancelled, command->error, &command->deferredActionToken);
    command->refused = !command->success;
    return true;
}

bool ApplyPopupCommand(Command* command, SourceThreadAgent* agent) {
    if (AbortIfCancelled(command)) return false;
    command->success = agent->CompletePopupOnSourceThread(command->action, command->error);
    command->refused = !command->success;
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
    WindowActionEntry{ L"popupCommand", &ApplyPopupCommand },
};

WindowAction FindWindowAction(std::wstring_view action) noexcept {
    for (const auto& entry : kWindowActions) {
        if (entry.name == action) return entry.apply;
    }
    return nullptr;
}

// --- Node-level actions -----------------------------------------------------

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
    Command* command, SourceThreadAgent* agent, HWND, const ControlNode& node) {
    if (node.kind == ControlKind::Button || node.kind == ControlKind::SysLink)
        return QueueNativeAction(command, agent, &node);
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
        node.kind == ControlKind::ListBox || node.kind == ControlKind::TabControl ||
        node.kind == ControlKind::TreeView;
    if (!selectable) return true;
    const int requested = command->action.integerValue;
    const bool tab = node.kind == ControlKind::TabControl;
    const bool tree = node.kind == ControlKind::TreeView;
    const bool validIndex = requested >= (tab || tree ? 0 : -1) &&
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
    if (tree) {
        // The tree raises its own selection notifications, so nothing is
        // synthesized on its behalf.
        command->success = SelectTreeViewItem(target, requested);
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
    state.stateMask = LVIS_SELECTED;
    state.state = 0;
    SendMessageW(target, LVM_SETITEMSTATE,
        static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&state));
    for (size_t index = 0; index < requested.size(); ++index) {
        if (AbortIfCancelled(command)) return false;
        state.stateMask = LVIS_SELECTED;
        state.state = LVIS_SELECTED;
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
            static_cast<size_t>(selected) >= ListViewItemCount(node) ||
            actual.size() >= ListViewItemCount(node)) {
            return true;
        }
        actual.push_back(selected);
        previous = selected;
    }
    command->success = actual == requested;
    // The control ran the selection and settled somewhere else. That is a
    // refusal of this action, not a broken projection.
    command->refused = !command->success;
    if (!command->success) command->error = L"the list did not keep the requested selection";
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

bool ApplySetFocusedIndex(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::ListView) return true;
    if (AbortIfCancelled(command)) return false;
    const int index = command->action.itemIndex;
    if (index < -1 || (index >= 0 && static_cast<size_t>(index) >= ListViewItemCount(node))) return true;
    command->success = SetListViewFocusedIndex(target, index);
    command->refused = !command->success;
    if (!command->success) command->error = L"the list refused the focus change";
    return true;
}

bool ApplyScrollBy(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::ListView || node.listViewMode == L"report") return true;
    if (AbortIfCancelled(command)) return false;
    command->success = ScrollListViewBy(target, command->action.integerValue, command->action.itemIndex);
    command->refused = !command->success;
    if (!command->success) command->error = L"the list did not scroll";
    return true;
}

bool ApplySetItemCheck(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::ListView || !node.checkBoxes) return true;
    const int index = command->action.itemIndex;
    if (index < 0 || static_cast<size_t>(index) >= ListViewItemCount(node)) return true;
    if (AbortIfCancelled(command)) return false;

    const bool applied = SetListViewItemCheck(
        target, index, command->action.booleanValue);
    if (AbortIfCancelled(command)) return false;
    command->success = applied;
    return true;
}

bool ApplyToolbarCommand(
    Command* command, SourceThreadAgent* agent, HWND, const ControlNode& node) {
    if (node.kind != ControlKind::Toolbar) return true;
    return QueueNativeAction(command, agent, &node);
}

bool ApplySetValue(
    Command* command, SourceThreadAgent* agent, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::Slider) return true;
    const int requested = command->action.integerValue;
    if (requested < node.minimum || requested > node.maximum) return true;
    if (AbortIfCancelled(command)) return false;
    command->success = SetTrackbarPosition(
        agent->Root(), target, node.vertical, requested);
    return true;
}

bool ApplySetExpand(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::TreeView) return true;
    const int index = command->action.itemIndex;
    if (index < 0 || static_cast<size_t>(index) >= node.items.size()) return true;
    if (AbortIfCancelled(command)) return false;
    // Expansion may run an application handler that inserts children, so the
    // command is completed only after the control reports the requested state.
    const bool applied = SetTreeViewItemExpanded(
        target, index, command->action.booleanValue);
    if (AbortIfCancelled(command)) return false;
    command->success = applied;
    return true;
}

bool ApplySetItemText(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    const bool renamable = (node.kind == ControlKind::TreeView ||
        node.kind == ControlKind::ListView) && node.editableLabels;
    if (!renamable) return true;
    const int index = command->action.itemIndex;
    const size_t itemCount = node.kind == ControlKind::TreeView
        ? node.items.size() : ListViewItemCount(node);
    if (index < 0 || static_cast<size_t>(index) >= itemCount) return true;
    if (AbortIfCancelled(command)) return false;
    // The rename opens and closes the control's own label session, so the
    // application's veto and its normalization both apply before this reports
    // success.
    const bool renamed = node.kind == ControlKind::TreeView
        ? RenameTreeViewItem(target, index, command->action.text)
        : RenameListViewItem(target, index, command->action.text);
    if (AbortIfCancelled(command)) return false;
    command->success = renamed;
    // A label session that ran and left the old text is the application refusing
    // the new one, which is a rejected action rather than a broken projection.
    command->refused = !renamed;
    if (!renamed) command->error = L"the application refused the new label";
    return true;
}

bool ApplyIslandInvoke(
    Command* command, SourceThreadAgent* agent, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::AccessibleIsland || !agent) return true;
    if (AbortIfCancelled(command)) return false;
    const int index = command->action.itemIndex;
    if (index < 0 || static_cast<size_t>(index) >= node.islandItems.size()) return true;
    // The provider's default action may open a menu of its own, so it is queued to run
    // after this command returns rather than inside its deadline.  The published name
    // and action travel with it so the deferred handler can refuse an element that
    // moved.
    command->deferredActionKind = kDeferredIslandAction;
    command->success = agent->PostIslandAction(node, index, command->refused, &command->deferredActionToken);
    if (!command->success) command->error = command->refused
        ? L"an island action is already pending"
        : L"the island action could not be queued";
    return true;
}

bool ApplyActivateItem(
    Command* command, SourceThreadAgent* agent, HWND, const ControlNode& node) {
    if (node.kind != ControlKind::ListView || !agent) return true;
    if (AbortIfCancelled(command)) return false;
    command->deferredActionKind = kDeferredListViewActivation;
    command->success = agent->PostListViewActivation(node, command->action.itemIndex,
        command->refused, command->error, &command->deferredActionToken);
    return true;
}

bool ApplySetColumnOrder(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::ListView) return true;
    if (AbortIfCancelled(command)) return false;
    const auto& order = command->action.integerValues;
    if (order.empty() || order.size() != node.columns.size()) return true;
    // The control's own header owns the display order, so the projection sets it
    // through the documented order array and then lets the capture read back what
    // the control settled on.
    const bool applied = SetListViewColumnOrder(
        target, std::vector<int>(order.begin(), order.end()));
    if (AbortIfCancelled(command)) return false;
    command->success = applied;
    command->refused = !applied;
    if (!applied) command->error = L"the list refused the requested column order";
    return true;
}

bool ApplySetSplit(
    Command* command, SourceThreadAgent*, HWND target, const ControlNode& node) {
    if (node.kind != ControlKind::PaneContainer) return true;
    if (AbortIfCancelled(command)) return false;
    // The split is resolved against the container's live geometry rather than the
    // snapshot it was requested from: a drag arrives while the panes may already
    // have moved, and the two panes it divides are the only windows that change.
    const bool moved = SetPaneSplit(
        target, command->action.itemIndex, command->action.integerValue);
    if (AbortIfCancelled(command)) return false;
    command->success = moved;
    // The application's own layout is what refuses a split it will not accept, so
    // a refusal keeps the projection and reports the canonical geometry back.
    command->refused = !moved;
    if (!moved) command->error = L"the container refused the requested split";
    return true;
}

bool ApplyMdiCommand(
    Command* command, SourceThreadAgent* agent, HWND, const ControlNode& node) {
    if (node.kind != ControlKind::MdiChild) return true;
    return QueueNativeAction(command, agent, &node);
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
    NodeActionEntry{ L"setFocusedIndex", &ApplySetFocusedIndex },
    NodeActionEntry{ L"scrollBy", &ApplyScrollBy },
    NodeActionEntry{ L"setItemCheck", &ApplySetItemCheck },
    NodeActionEntry{ L"setItemText", &ApplySetItemText },
    NodeActionEntry{ L"setValue", &ApplySetValue },
    NodeActionEntry{ L"setExpand", &ApplySetExpand },
    NodeActionEntry{ L"toolbarCommand", &ApplyToolbarCommand },
    NodeActionEntry{ L"mdiCommand", &ApplyMdiCommand },
    NodeActionEntry{ L"setSplit", &ApplySetSplit },
    NodeActionEntry{ L"setColumnOrder", &ApplySetColumnOrder },
    NodeActionEntry{ L"islandInvoke", &ApplyIslandInvoke },
    NodeActionEntry{ L"activateItem", &ApplyActivateItem },
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
    if (action.action == L"menuCommand" && !agent->MenuBarCommandsCurrentOnSourceThread()) {
        command->refused = true;
        command->error = L"native menu changed and is awaiting refresh";
        return true;
    }
    if (action.action == L"menuCommand" &&
        action.expectedMenuBindingGeneration != before.menuBindingGeneration) {
        command->refused = true;
        command->error = L"native menu binding changed before selection";
        return true;
    }
    const bool indexedOwnerData = IsOwnerDataListViewIndexedAction(action, before);
    const bool witnessedAction = action.action == L"activateItem" || indexedOwnerData ||
        action.expectedNativeFingerprint != 0;
    if (witnessedAction && (action.expectedNativeFingerprint == 0 ||
            SnapshotFingerprint(before) != action.expectedNativeFingerprint)) {
        if (AbortIfCancelled(command)) return false;
        command->refused = true;
        command->error = L"native ListView revision changed before the indexed action";
        if (action.action != L"activateItem") {
            // The application can reorder virtual rows without changing their
            // count. Publish what was actually read, but never retarget or replay
            // the user's old index against that new ordering.
            before.revision = action.expectedRevision + 1;
            command->outcome.revision = before.revision;
            command->outcome.snapshot = std::move(before);
        }
        return true;
    }
    if (!ValidateActionForSnapshot(action, before, command->error)) {
        // A menu can disappear or disable a command between the IPC snapshot and
        // this source-thread capture. That stale click does not break the surface.
        command->refused = action.action == L"menuCommand" || action.action == L"popupCommand" ||
            action.action == L"activateItem" ||
            before.popupMenu.has_value();
        return true;
    }
    if (AbortIfCancelled(command)) return false;

    if (const WindowAction apply = FindWindowAction(action.action)) {
        if (!apply(command, agent)) return false;
    } else if (action.nodeId && !InvokeOnNode(command, agent, before)) {
        return false;
    }

    if (command->success && (agent->IsDestroyed() || !IsWindow(agent->Root()))) {
        command->outcome.destroyed = true;
    }
    if (command->success && (action.action == L"menuCommand" ||
            action.action == L"toolbarCommand" || action.action == L"mdiCommand" ||
            action.action == L"invoke" || action.action == L"islandInvoke" ||
            action.action == L"activateItem" ||
            action.action == L"select" || action.action == L"setSelection" ||
            action.action == L"setExpand" || action.action == L"setCheck" ||
            action.action == L"setItemCheck" || action.action == L"setItemText")) {
        agent->RequestMenuBarRefresh();
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
    if (!cloaked) command->agent->CancelPopupOnSourceThread();
    BOOL value = cloaked ? TRUE : FALSE;
    const HRESULT applied = DwmSetWindowAttribute(root, DWMWA_CLOAK, &value, sizeof(value));
    if (command->cancelled.load(std::memory_order_acquire)) {
        if (cloaked) {
            command->agent->CancelPopupOnSourceThread();
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

// The entire refresh, including discovery, cache publication and cleanup, runs on
// the source thread. Cancellation completes the command only after the helper has
// unwound its pump; no command field is touched after AbortIfCancelled returns true.
bool ExecuteMenuBarRefresh(Command* command) {
    if (AbortIfCancelled(command)) return false;
    command->success = command->agent->RefreshMenuBarOnSourceThread(
        command->menuBarPopupWaitMs, command->cancelled,
        command->menuBarChanged, command->error);
    if (AbortIfCancelled(command)) return false;
    return true;
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
    agent->CancelPopupOnSourceThread();
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
    command->agent->CancelPopupOnSourceThread();
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
    case kCommandMenuBarRefresh: return &ExecuteMenuBarRefresh;
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
        // Every callback owns the agent before entering application code. A
        // timed-out command can pump nested shutdown and outlive its surface.
        const auto agent = AgentForMessage(message->message);
        if (agent && message->message == agent->MessageId() &&
            message->wParam == kDeferredNativeAction) {
            const auto token = static_cast<uint64_t>(message->lParam);
            message->message = WM_NULL;
            agent->DispatchNativeActionOnSourceThread(token);
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        if (agent && message->message == agent->MessageId() &&
            message->wParam == kDeferredListViewActivation) {
            const auto token = static_cast<uint64_t>(message->lParam);
            message->message = WM_NULL;
            agent->DispatchListViewActivationOnSourceThread(token);
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        if (agent && message->message == agent->MessageId() &&
            message->wParam == kDeferredMenuAction) {
            const auto token = static_cast<uint64_t>(message->lParam);
            message->message = WM_NULL;
            agent->DispatchMenuActionOnSourceThread(token);
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        if (agent && message->message == agent->MessageId() &&
            message->wParam == kDeferredIslandAction) {
            const auto token = static_cast<uint64_t>(message->lParam);
            message->message = WM_NULL;
            agent->DispatchIslandActionOnSourceThread(token);
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        if (agent && message->message == agent->MessageId()) {
            auto* command = reinterpret_cast<Command*>(message->lParam);
            if (IsTrackedCommand(command, agent.get())) {
                // While a command is pumping the source thread's own messages -- which is
                // how a menu bar's popup is waited for -- another command must not be
                // dispatched re-entrantly.  It is put back on the queue instead, so its
                // caller keeps waiting rather than being answered out of order.
                if (MenuBarReadInProgress()) {
                    message->message = WM_NULL;
                    if (!PostThreadMessageW(agent->ThreadId(), agent->MessageId(),
                            message->wParam, message->lParam)) {
                        UntrackCommand(command);
                        SetEvent(command->started);
                        command->error = L"source-thread command could not be requeued";
                        Complete(command);
                        Release(command);
                    }
                    return CallNextHookEx(nullptr, code, wParam, lParam);
                }
                UntrackCommand(command);
                message->message = WM_NULL;
                SetEvent(command->started);
                {
                    const BoundedSourceCommandScope boundedCommand;
                    if (!command->cancelled.load()) ExecuteCommand(command);
                    else Complete(command);
                }
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
        const bool menuChanged = MenuMutationMessage(message->message, message->lParam);
        if (RelevantMessage(message->message) || message->message == WM_NCDESTROY ||
            message->message == WM_CANCELMODE || menuChanged) {
            MarkCurrentThreadAgentsDirty(message->hwnd, message->message, menuChanged);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace

bool IsOwnerDataListViewIndexedAction(
    const ActionRequest& action, const WindowSnapshot& snapshot) noexcept {
    if (!action.nodeId || (action.action != L"setSelection" &&
            action.action != L"setFocusedIndex" && action.action != L"setItemText" &&
            action.action != L"setItemCheck")) return false;
    return std::any_of(snapshot.nodes.begin(), snapshot.nodes.end(), [&](const ControlNode& node) {
        return node.nodeId == *action.nodeId && node.kind == ControlKind::ListView &&
            (node.style & LVS_OWNERDATA) != 0;
    });
}

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
        if (!popupCaptureError_.empty()) {
            error = popupCaptureError_;
            return false;
        }
        captureContext_.surfaceId = surfaceId;
        captureContext_.generation = generation_;
        captureContext_.revision = revision;
        if (!CaptureWindow(root_, captureContext_, snapshot, error)) return false;
        snapshot.popupMenu = popupMenu_.Current();
        if (snapshot.popupMenu) {
            const auto& popup = *snapshot.popupMenu;
            const auto anchor = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                [&](const ControlNode& node) { return node.nodeId == popup.nodeId; });
            if (anchor == snapshot.nodes.end() || anchor->kind != ControlKind::AccessibleIsland ||
                popup.itemIndex < 0 || static_cast<size_t>(popup.itemIndex) >= anchor->islandItems.size() ||
                !anchor->visible || !anchor->islandItems[popup.itemIndex].dropDown ||
                !trackedIslandMenu_ || anchor->generation != trackedIslandMenu_->generation ||
                anchor->islandItems[popup.itemIndex].name != trackedIslandMenu_->name ||
                anchor->islandItems[popup.itemIndex].actionName != trackedIslandMenu_->action) {
                CancelPopupOnSourceThread();
                snapshot.popupMenu.reset();
            }
        }
        return true;
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
    outcome.refused = command->refused;
    const bool success = command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::PostIslandAction(
    const ControlNode& node, int index, bool& refused, uint64_t* queuedToken) noexcept {
    refused = false;
    if (queuedToken) *queuedToken = 0;
    try {
        if (GetCurrentThreadId() != threadId_ || shuttingDown_.load() ||
            index < 0 || static_cast<size_t>(index) >= node.islandItems.size() ||
            nextIslandActionToken_ == 0) return false;
        if (queuedIslandAction_ || queuedNativeAction_ || nativeActionRunning_ || trackedPopup_ ||
            (pendingIslandMenu_ && GetTickCount64() <= pendingIslandDeadline_)) {
            refused = true;
            return false;
        }
        const auto& item = node.islandItems[index];
        IslandActionRequest deferred;
        deferred.island = node.hwnd;
        deferred.nodeId = node.nodeId;
        deferred.generation = node.generation;
        deferred.index = index;
        deferred.name = item.name;
        deferred.action = item.actionName;
        deferred.dropDown = item.dropDown;
        queuedIslandAction_ = std::move(deferred);
        queuedIslandActionToken_ = nextIslandActionToken_;
        rearmIslandAction_ = false;
        nextIslandActionToken_ = nextIslandActionToken_ == std::numeric_limits<uint64_t>::max()
            ? 0 : nextIslandActionToken_ + 1;
        if (!PostThreadMessageW(threadId_, message_, kDeferredIslandAction,
                static_cast<LPARAM>(queuedIslandActionToken_))) {
            queuedIslandAction_.reset();
            queuedIslandActionToken_ = 0;
            return false;
        }
        if (queuedToken) *queuedToken = queuedIslandActionToken_;
        return true;
    } catch (...) {
        return false;
    }
}

void SourceThreadAgent::DispatchIslandActionOnSourceThread(uint64_t token) noexcept {
    if (GetCurrentThreadId() != threadId_ || token == 0 ||
        queuedIslandActionToken_ != token || !queuedIslandAction_) return;
    if (g_boundedSourceCommandDepth != 0) {
        rearmIslandAction_ = true;
        return;
    }
    if (MenuBarReadInProgress() && !shuttingDown_.load()) {
        if (!PostThreadMessageW(threadId_, message_, kDeferredIslandAction,
                static_cast<LPARAM>(token))) {
            queuedIslandAction_.reset();
            queuedIslandActionToken_ = 0;
            try { popupCaptureError_ = L"deferred island action could not be reposted"; }
            catch (...) {}
            MarkDirty();
        }
        return;
    }
    // Remove the request before entering the provider. Nested rollback can clear
    // queued state without destroying the request currently on this stack.
    auto request = std::move(*queuedIslandAction_);
    queuedIslandAction_.reset();
    queuedIslandActionToken_ = 0;
    rearmIslandAction_ = false;
    RunIslandActionOnSourceThread(request);
}

namespace {
bool IsApplicationCloaked(HWND root) noexcept {
    DWORD cloak = 0;
    return root && IsWindow(root) &&
        SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloak, sizeof(cloak))) &&
        (cloak & DWM_CLOAKED_APP) != 0;
}

constexpr UINT kListViewActivationState = LVIS_SELECTED | LVIS_FOCUSED |
    LVIS_CUT | LVIS_DROPHILITED | LVIS_STATEIMAGEMASK;
constexpr DWORD kListViewActivationStyle = LVS_TYPEMASK | LVS_OWNERDATA | LVS_OWNERDRAWFIXED |
    LVS_SINGLESEL | LVS_EDITLABELS;

bool ListViewActivationReady(HWND root, const ListViewActivationRequest& request) noexcept {
    if (!IsApplicationCloaked(root) || !IsWindowVisible(root) ||
        !IsWindowVisible(request.listView) || !IsChild(root, request.listView) ||
        GetWindowThreadProcessId(request.listView, nullptr) != GetCurrentThreadId() ||
        reinterpret_cast<uintptr_t>(GetPropW(request.listView, kNodeGenerationProperty)) !=
            request.generation) return false;
    for (HWND current = request.listView; current; current = GetParent(current)) {
        if (!IsWindowEnabled(current)) return false;
        if (current == root) return true;
    }
    return false;
}

bool ReadListViewActivationIdentity(const ListViewActivationRequest& request,
    UINT& state, std::wstring& error) {
    const HWND list = request.listView;
    const LRESULT view = SendMessageW(list, LVM_GETVIEW, 0, 0);
    if (request.index < 0 || request.nativeId == UINT32_MAX ||
        view < LV_VIEW_ICON || view > LV_VIEW_LIST ||
        (view != LV_VIEW_ICON && static_cast<DWORD>(view) != (request.viewStyle & LVS_TYPEMASK)) ||
        static_cast<size_t>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0)) != request.itemCount ||
        (static_cast<DWORD>(GetWindowLongPtrW(list, GWL_STYLE)) & kListViewActivationStyle) !=
            request.viewStyle ||
        ResolveListViewItemByNativeId(list, request.nativeId) != request.index ||
        static_cast<uint32_t>(SendMessageW(list, LVM_MAPINDEXTOID, request.index, 0)) != request.nativeId) {
        error = L"ListView activation item moved, was replaced, or changed view";
        return false;
    }
    // One extra character detects a renamed item sharing the old name's prefix.
    std::vector<wchar_t> text(request.text.size() + 2, L'\0');
    LVITEMW item{};
    item.iSubItem = 0;
    item.pszText = text.data();
    item.cchTextMax = static_cast<int>(text.size());
    const LRESULT copied = SendMessageW(list, LVM_GETITEMTEXTW,
        request.index, reinterpret_cast<LPARAM>(&item));
    if (copied < 0 || static_cast<size_t>(copied) != request.text.size() ||
        request.text != text.data()) {
        error = L"ListView activation item text changed";
        return false;
    }
    state = static_cast<UINT>(SendMessageW(list, LVM_GETITEMSTATE,
        request.index, kListViewActivationState));
    // Text/state providers can replace an item while answering a native read.
    // Recheck the stable ID after those callbacks so the accessible child index
    // cannot silently name a replacement with identical text and selection.
    if (static_cast<size_t>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0)) != request.itemCount ||
        ResolveListViewItemByNativeId(list, request.nativeId) != request.index ||
        static_cast<uint32_t>(SendMessageW(list, LVM_MAPINDEXTOID, request.index, 0)) != request.nativeId) {
        error = L"ListView activation item was replaced while its native metadata was read";
        return false;
    }
    return true;
}

struct AccessibleListViewItem final {
    Microsoft::WRL::ComPtr<IAccessible> object;
    VARIANT child{};
};

struct ListViewAccessibleString final {
    BSTR value = nullptr;
    ~ListViewAccessibleString() { if (value) SysFreeString(value); }
};

bool ReadListViewAccessibleAction(const ListViewActivationRequest& request,
    AccessibleListViewItem& live, std::wstring& name, std::wstring& action,
    long& state, std::wstring& error, IAccessible* sharedRoot = nullptr) {
    if (!live.object) {
        Microsoft::WRL::ComPtr<IAccessible> root;
        if (sharedRoot) root = sharedRoot;
        else if (FAILED(AccessibleObjectFromWindow(request.listView, OBJID_CLIENT,
                IID_PPV_ARGS(root.GetAddressOf()))) || !root) {
            error = L"ListView exposes no accessible item action";
            return false;
        }
        live.child.vt = VT_I4;
        live.child.lVal = request.index + 1;
        Microsoft::WRL::ComPtr<IDispatch> childObject;
        root->get_accChild(live.child, childObject.GetAddressOf());
        if (childObject) {
            if (FAILED(childObject.As(&live.object)) || !live.object) {
                error = L"ListView child has no accessible item interface";
                return false;
            }
            live.child.lVal = CHILDID_SELF;
        } else {
            live.object = std::move(root);
        }
    }
    VARIANT role{};
    const HRESULT roleResult = live.object->get_accRole(live.child, &role);
    const bool itemRole = SUCCEEDED(roleResult) && role.vt == VT_I4 &&
        role.lVal == ROLE_SYSTEM_LISTITEM;
    VariantClear(&role);
    VARIANT status{};
    const HRESULT stateResult = live.object->get_accState(live.child, &status);
    const bool itemState = SUCCEEDED(stateResult) && status.vt == VT_I4;
    state = itemState ? status.lVal : 0;
    VariantClear(&status);
    if (!itemRole || !itemState || (state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0) {
        error = L"ListView accessible item is unavailable or changed role";
        return false;
    }
    ListViewAccessibleString rawName;
    const HRESULT nameResult = live.object->get_accName(live.child, &rawName.value);
    const UINT nameLength = rawName.value ? SysStringLen(rawName.value) : 0;
    if (SUCCEEDED(nameResult) && rawName.value && nameLength <= Ipc::kMaxStringChars)
        name.assign(rawName.value, nameLength);
    ListViewAccessibleString rawAction;
    const HRESULT actionResult = live.object->get_accDefaultAction(live.child, &rawAction.value);
    const UINT actionLength = rawAction.value ? SysStringLen(rawAction.value) : 0;
    if (SUCCEEDED(actionResult) && rawAction.value && actionLength <= Ipc::kMaxStringChars)
        action.assign(rawAction.value, actionLength);
    if (FAILED(nameResult) || name != request.text || nameLength > Ipc::kMaxStringChars ||
        FAILED(actionResult) || action.empty() || actionLength > Ipc::kMaxStringChars) {
        error = L"ListView accessible item name or default action is not current";
        return false;
    }
    return true;
}
}

bool ReadListViewNativeActivationPoint(HWND listView, int index, POINT& point) noexcept {
    if (!listView || index < 0 || GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId())
        return false;
    RECT client{};
    if (!GetClientRect(listView, &client)) return false;
    for (const int part : {LVIR_LABEL, LVIR_ICON}) {
        RECT rectangle{part};
        RECT visible{};
        if (!SendMessageW(listView, LVM_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rectangle)) ||
            !IntersectRect(&visible, &rectangle, &client) || IsRectEmpty(&visible)) continue;
        // LVIR_LABEL includes blank space in report view. Even a point inside
        // that rectangle needs the control's exact semantic hit-test result.
        for (const LONG x : {std::min(visible.left + 1, visible.right - 1),
                visible.left + (visible.right - visible.left) / 2, visible.right - 1}) {
            const POINT candidate{x, visible.top + (visible.bottom - visible.top) / 2};
            if (candidate.x < 0 || candidate.y < 0 ||
                candidate.x > std::numeric_limits<SHORT>::max() ||
                candidate.y > std::numeric_limits<SHORT>::max()) continue;
            LVHITTESTINFO hit{};
            hit.pt = candidate;
            const LRESULT found = SendMessageW(listView, LVM_HITTEST,
                static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&hit));
            if (found == index && hit.iItem == index && hit.iSubItem == 0 &&
                (hit.flags & (part == LVIR_LABEL ? LVHT_ONITEMLABEL : LVHT_ONITEMICON)) != 0 &&
                (hit.flags & LVHT_ONITEMSTATEICON) == 0) {
                point = candidate;
                return true;
            }
        }
    }
    return false;
}

namespace {
uint64_t NativeMenuFingerprint(const std::vector<MenuItemSnapshot>& menu) {
    WindowSnapshot witness;
    witness.menu = menu;
    return SnapshotFingerprint(witness);
}

HWND NativeActionOwner(HWND root, const ActionRequest& action, HWND source) noexcept {
    if (!source) return root;
    return action.action == L"toolbarCommand"
        ? SyntheticNotificationTarget(root, source) : GetParent(source);
}
}

bool SourceThreadAgent::PostNativeAction(const ActionRequest& action, const ControlNode* node,
    bool& refused, std::wstring& error, uint64_t* queuedToken, uint64_t* closeSequence) noexcept {
    refused = true;
    if (queuedToken) *queuedToken = 0;
    if (closeSequence) *closeSequence = 0;
    try {
        const bool nodeAction = node && action.nodeId == node->nodeId &&
            ((action.action == L"invoke" &&
                (node->kind == ControlKind::Button || node->kind == ControlKind::SysLink)) ||
             (action.action == L"toolbarCommand" && node->kind == ControlKind::Toolbar) ||
             (action.action == L"mdiCommand" && node->kind == ControlKind::MdiChild));
        const bool rootAction = !node && !action.nodeId &&
            (action.action == L"close" || action.action == L"menuCommand");
        if (GetCurrentThreadId() != threadId_ || shuttingDown_.load() || IsDestroyed() ||
            (!nodeAction && !rootAction) || nextNativeActionToken_ == 0 ||
            !IsApplicationCloaked(root_)) {
            error = L"native action has no current projected source identity";
            return false;
        }
        if (queuedNativeAction_ || nativeActionRunning_ || queuedIslandAction_ || queuedMenuAction_ ||
            queuedListViewActivation_ || trackedPopup_ ||
            (pendingIslandMenu_ && GetTickCount64() <= pendingIslandDeadline_)) {
            error = L"a native action is already pending";
            return false;
        }
        NativeActionRequest request;
        request.action = action;
        if (node) {
            request.source = node->hwnd;
            request.nodeGeneration = node->generation;
            request.kind = node->kind;
            if (!IsChild(root_, request.source) ||
                reinterpret_cast<uintptr_t>(GetPropW(request.source, kNodeGenerationProperty)) != node->generation) {
                error = L"native action source control was replaced";
                return false;
            }
        }
        request.owner = NativeActionOwner(root_, action, request.source);
        if (!request.owner || GetWindowThreadProcessId(request.owner, nullptr) != threadId_) return false;
        if (action.action == L"menuCommand") {
            std::vector<MenuItemSnapshot> menu;
            request.menu = GetMenu(root_);
            if (!request.menu || action.expectedMenuBindingGeneration != 0 ||
                !CaptureTopLevelMenu(root_, menu, error)) return false;
            request.menuFingerprint = NativeMenuFingerprint(menu);
        }
        queuedNativeAction_ = std::move(request);
        queuedNativeActionToken_ = nextNativeActionToken_;
        rearmNativeAction_ = false;
        nextNativeActionToken_ = nextNativeActionToken_ == UINT64_MAX ? 0 : nextNativeActionToken_ + 1;
        if (!PostThreadMessageW(threadId_, message_, kDeferredNativeAction,
                static_cast<LPARAM>(queuedNativeActionToken_))) {
            CancelNativeActionOnSourceThread();
            error = L"native action could not be queued";
            return false;
        }
        if (action.action == L"close") {
            queuedNativeAction_->closeSequence = RegisterCloseRequest();
            if (closeSequence) *closeSequence = queuedNativeAction_->closeSequence;
        }
        if (queuedToken) *queuedToken = queuedNativeActionToken_;
        refused = false;
        return true;
    } catch (...) {
        try { error = L"exception preparing deferred native action"; } catch (...) {}
        return false;
    }
}

void SourceThreadAgent::CancelNativeActionOnSourceThread() noexcept {
    if (GetCurrentThreadId() != threadId_) return;
    const bool cancelledClose = queuedNativeAction_ && queuedNativeAction_->closeSequence != 0;
    queuedNativeAction_.reset();
    queuedNativeActionToken_ = 0;
    rearmNativeAction_ = false;
    // A close already acknowledged by the renderer must eventually finish even
    // when geometry/rollback cancels its queued token before WM_CLOSE is sent.
    if (cancelledClose) MarkCloseRequestCompleted();
}

void SourceThreadAgent::DispatchNativeActionOnSourceThread(uint64_t token) noexcept {
    if (GetCurrentThreadId() != threadId_ || token == 0 ||
        token != queuedNativeActionToken_ || !queuedNativeAction_) return;
    if (g_boundedSourceCommandDepth != 0) {
        rearmNativeAction_ = true;
        return;
    }
    if (MenuBarReadInProgress() && !shuttingDown_.load()) {
        if (!PostThreadMessageW(threadId_, message_, kDeferredNativeAction, static_cast<LPARAM>(token)))
            CancelNativeActionOnSourceThread();
        return;
    }
    auto request = std::move(*queuedNativeAction_);
    queuedNativeAction_.reset();
    queuedNativeActionToken_ = 0;
    rearmNativeAction_ = false;
    RunNativeActionOnSourceThread(request);
}

void SourceThreadAgent::RunNativeActionOnSourceThread(const NativeActionRequest& request) noexcept {
    // This scope is deliberately outside BoundedSourceCommandScope. Sending the
    // original message now enters the real HWND proc without leaving another raw
    // message that a later bounded capture or rollback could accidentally drain.
    struct RunningScope final {
        SourceThreadAgent* agent;
        bool close;
        ~RunningScope() {
            agent->nativeActionRunning_ = false;
            if (close) agent->MarkCloseRequestCompleted();
            agent->RequestMenuBarRefresh();
            agent->MarkDirty();
        }
    } running{this, request.closeSequence != 0};
    nativeActionRunning_ = true;
    try {
        const auto sourceCurrent = [&] {
            if (!request.source) return true;
            const auto identity = captureContext_.nodeIds.find(request.source);
            return IsChild(root_, request.source) &&
                GetWindowThreadProcessId(request.source, nullptr) == threadId_ &&
                reinterpret_cast<uintptr_t>(GetPropW(request.source, kNodeGenerationProperty)) == request.nodeGeneration &&
                identity != captureContext_.nodeIds.end() &&
                identity->second.nodeId == request.action.nodeId && identity->second.generation == request.nodeGeneration &&
                NativeActionOwner(root_, request.action, request.source) == request.owner;
        };
        const auto ready = [&] {
            if (GetCurrentThreadId() != threadId_ || g_boundedSourceCommandDepth != 0 ||
                shuttingDown_.load() || IsDestroyed() || trackedPopup_ ||
                !IsApplicationCloaked(root_) || !IsWindowVisible(root_) || !sourceCurrent()) return false;
            for (HWND current = request.source ? request.source : root_; current; current = GetParent(current)) {
                if (!IsWindowVisible(current) || !IsWindowEnabled(current)) return false;
                if (current == root_) return true;
            }
            return false;
        };
        if (!ready()) return;
        WindowSnapshot current;
        std::wstring error;
        if (!CaptureOnSourceThread(request.action.surfaceId, request.action.expectedRevision, current, error) ||
            !ValidateActionForSnapshot(request.action, current, error) || !ready()) return;
        const auto node = std::find_if(current.nodes.begin(), current.nodes.end(), [&](const ControlNode& value) {
            return request.action.nodeId == value.nodeId;
        });
        if (request.source && (node == current.nodes.end() || node->hwnd != request.source ||
                node->generation != request.nodeGeneration || node->kind != request.kind)) return;
        const auto& action = request.action;
        if (action.action == L"close") {
            SendMessageW(root_, WM_CLOSE, 0, 0);
        } else if (action.action == L"menuCommand") {
            if (current.menuBindingGeneration != action.expectedMenuBindingGeneration ||
                GetMenu(root_) != request.menu || !IsMenu(request.menu) ||
                NativeMenuFingerprint(current.menu) != request.menuFingerprint) return;
            SendMessageW(root_, WM_COMMAND, MAKEWPARAM(action.menuCommandId, 0), 0);
        } else if (action.action == L"toolbarCommand") {
            if (!ApplyToolbarCheckState(request.source, *node, action.menuCommandId, error) || !ready()) return;
            SendMessageW(request.owner, WM_COMMAND, MAKEWPARAM(action.menuCommandId, 0),
                reinterpret_cast<LPARAM>(request.source));
        } else if (action.action == L"mdiCommand") {
            if (action.text == L"activate") {
                SendMessageW(request.owner, WM_MDIACTIVATE, reinterpret_cast<WPARAM>(request.source), 0);
            } else {
                const WPARAM code = action.text == L"close" ? SC_CLOSE :
                    action.text == L"minimize" ? SC_MINIMIZE :
                    action.text == L"maximize" ? SC_MAXIMIZE : SC_RESTORE;
                SendMessageW(request.source, WM_SYSCOMMAND, code, 0);
            }
        } else if (request.kind == ControlKind::Button) {
            SendMessageW(request.source, BM_CLICK, 0, 0);
        } else if (request.kind == ControlKind::SysLink) {
            LITEM item{};
            item.mask = LIF_ITEMINDEX | LIF_STATE;
            item.iLink = 0;
            item.stateMask = LIS_FOCUSED;
            item.state = LIS_FOCUSED;
            if (!SendMessageW(request.source, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item)) || !ready()) return;
            SendMessageW(request.source, WM_SETFOCUS, 0, 0);
            if (ready()) SendMessageW(request.source, WM_KEYDOWN, VK_RETURN, 1);
            if (ready()) SendMessageW(request.source, WM_KEYUP, VK_RETURN, 1 | (1ll << 30) | (1ll << 31));
            // Complete only the synthetic focus lifetime after a nested rollback;
            // no further activation message is sent to a restored native window.
            if (sourceCurrent()) SendMessageW(request.source, WM_KILLFOCUS, 0, 0);
        }
    } catch (...) {
        // The request has already been consumed. Reconcile owns any fallback.
    }
}

bool ListViewItemsHaveNativeDefaultActions(HWND listView, const ControlNode& node) noexcept {
    try {
        if (!listView || GetWindowThreadProcessId(listView, nullptr) != GetCurrentThreadId() ||
            node.kind != ControlKind::ListView || ListViewItemCount(node) == 0 ||
            node.itemNativeIds.size() != ListViewItemCount(node)) return false;
        Microsoft::WRL::ComPtr<IAccessible> root;
        if (FAILED(AccessibleObjectFromWindow(listView, OBJID_CLIENT,
                IID_PPV_ARGS(root.GetAddressOf()))) || !root) return false;
        for (size_t index = 0; index < ListViewItemCount(node); ++index) {
            ListViewActivationRequest request;
            request.listView = listView;
            request.index = static_cast<int>(index);
            request.nativeId = node.itemNativeIds[index];
            request.itemCount = ListViewItemCount(node);
            request.viewStyle = static_cast<DWORD>(node.style) & kListViewActivationStyle;
            request.text = node.items[index];
            AccessibleListViewItem live;
            std::wstring name;
            std::wstring action;
            std::wstring error;
            UINT nativeState = 0;
            long accessibleState = 0;
            if (!ReadListViewActivationIdentity(request, nativeState, error) ||
                !ReadListViewAccessibleAction(request, live, name, action, accessibleState, error, root.Get())) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SourceThreadAgent::PostListViewActivation(const ControlNode& node, int index,
    bool& refused, std::wstring& error, uint64_t* queuedToken) noexcept {
    refused = true;
    if (queuedToken) *queuedToken = 0;
    try {
        if (GetCurrentThreadId() != threadId_ || shuttingDown_.load() ||
            node.kind != ControlKind::ListView || !node.itemActivationSupported || index < 0 ||
            static_cast<size_t>(index) >= ListViewItemCount(node) ||
            node.itemNativeIds.size() != ListViewItemCount(node) ||
            nextListViewActivationToken_ == 0) {
            error = L"ListView activation has no current item identity";
            return false;
        }
        if (queuedListViewActivation_ || queuedIslandAction_ || queuedMenuAction_ ||
            queuedNativeAction_ || nativeActionRunning_ || trackedPopup_ ||
            (pendingIslandMenu_ && GetTickCount64() <= pendingIslandDeadline_)) {
            error = L"a native item action is already pending";
            return false;
        }
        // Accessible metadata can pump the source queue before a deferred
        // request exists. Reserve the action slot throughout preparation, and
        // never publish work from a lifetime that a nested command cancelled.
        struct PreparingScope final {
            SourceThreadAgent* agent;
            ~PreparingScope() { agent->nativeActionRunning_ = false; }
        } preparing{this};
        nativeActionRunning_ = true;
        const auto cancellation = listViewActivationCancellation_;
        ListViewActivationRequest request;
        request.listView = node.hwnd;
        request.nodeId = node.nodeId;
        request.generation = node.generation;
        request.index = index;
        request.nativeId = node.itemNativeIds[index];
        request.itemCount = ListViewItemCount(node);
        request.viewStyle = static_cast<DWORD>(node.style) & kListViewActivationStyle;
        request.text = node.items[index];
        const auto current = [&] {
            return !shuttingDown_.load() && cancellation == listViewActivationCancellation_ &&
                ListViewActivationReady(root_, request);
        };
        if (!current() || !ReadListViewActivationIdentity(request, request.nativeState, error) ||
            !current()) return false;
        const bool selected = std::find(node.selectedIndices.begin(), node.selectedIndices.end(), index) !=
            node.selectedIndices.end();
        const bool checked = std::find(node.checkedIndices.begin(), node.checkedIndices.end(), index) !=
            node.checkedIndices.end();
        if (((request.nativeState & LVIS_SELECTED) != 0) != selected ||
            ((request.nativeState & LVIS_FOCUSED) != 0) != (node.focusedIndex == index) ||
            (node.checkBoxes && (request.nativeState & LVIS_STATEIMAGEMASK) !=
                INDEXTOSTATEIMAGEMASK(checked ? 2 : 1))) {
            error = L"ListView activation selection, focus or check state changed";
            return false;
        }
        {
            AccessibleListViewItem live;
            if (!ReadListViewAccessibleAction(request, live, request.accessibleName,
                    request.defaultAction, request.accessibleState, error)) return false;
            // A provider's Release can pump too. Finish its lifetime before
            // final validation and publication of the deferred message.
        }
        UINT state = 0;
        if (!current() || !ReadListViewActivationIdentity(request, state, error) ||
            state != request.nativeState || !current()) {
            error = L"ListView item or activation lifetime changed while its default action was read";
            return false;
        }
        queuedListViewActivation_ = std::move(request);
        queuedListViewActivationToken_ = nextListViewActivationToken_;
        rearmListViewActivation_ = false;
        nextListViewActivationToken_ = nextListViewActivationToken_ == UINT64_MAX
            ? 0 : nextListViewActivationToken_ + 1;
        if (!PostThreadMessageW(threadId_, message_, kDeferredListViewActivation,
                static_cast<LPARAM>(queuedListViewActivationToken_))) {
            queuedListViewActivation_.reset();
            queuedListViewActivationToken_ = 0;
            error = L"ListView default action could not be queued";
            return false;
        }
        if (queuedToken) *queuedToken = queuedListViewActivationToken_;
        refused = false;
        return true;
    } catch (...) {
        try { error = L"exception preparing ListView default action"; } catch (...) {}
        return false;
    }
}

void SourceThreadAgent::DispatchListViewActivationOnSourceThread(uint64_t token) noexcept {
    if (GetCurrentThreadId() != threadId_ || token == 0 ||
        token != queuedListViewActivationToken_ || !queuedListViewActivation_) return;
    if (g_boundedSourceCommandDepth != 0) {
        rearmListViewActivation_ = true;
        return;
    }
    if (MenuBarReadInProgress() && !shuttingDown_.load()) {
        if (!PostThreadMessageW(threadId_, message_, kDeferredListViewActivation,
                static_cast<LPARAM>(token))) {
            queuedListViewActivation_.reset();
            queuedListViewActivationToken_ = 0;
        }
        return;
    }
    // Consume before entering COM. A modal handler can pump cancellation or a
    // replayed message without reusing this action or invalidating its stack data.
    auto request = std::move(*queuedListViewActivation_);
    queuedListViewActivation_.reset();
    queuedListViewActivationToken_ = 0;
    rearmListViewActivation_ = false;
    RunListViewActivationOnSourceThread(request);
}

void SourceThreadAgent::RearmDeferredActionsOnSourceThread() noexcept {
    if (GetCurrentThreadId() != threadId_) return;
    const auto rearm = [&](WPARAM kind, bool& consumed, auto& request, uint64_t& token) {
        if (!consumed) return;
        consumed = false;
        if (!request || token == 0) return;
        if (shuttingDown_.load() || !PostThreadMessageW(threadId_, message_, kind, static_cast<LPARAM>(token))) {
            request.reset();
            token = 0;
            MarkDirty();
        }
    };
    rearm(kDeferredIslandAction, rearmIslandAction_, queuedIslandAction_, queuedIslandActionToken_);
    rearm(kDeferredMenuAction, rearmMenuAction_, queuedMenuAction_, queuedMenuActionToken_);
    rearm(kDeferredListViewActivation, rearmListViewActivation_, queuedListViewActivation_, queuedListViewActivationToken_);
    if (rearmNativeAction_) {
        rearmNativeAction_ = false;
        if (queuedNativeAction_ && queuedNativeActionToken_ != 0 &&
            (shuttingDown_.load() || !PostThreadMessageW(threadId_, message_,
                kDeferredNativeAction, static_cast<LPARAM>(queuedNativeActionToken_))))
            CancelNativeActionOnSourceThread();
    }
}

void SourceThreadAgent::CancelDeferredActionOnSourceThread(WPARAM kind, uint64_t token) noexcept {
    if (GetCurrentThreadId() != threadId_ || token == 0) return;
    const auto cancel = [&](auto& request, uint64_t& queuedToken, bool& consumed) {
        if (token != queuedToken) return;
        request.reset();
        queuedToken = 0;
        consumed = false;
    };
    if (kind == kDeferredIslandAction) cancel(queuedIslandAction_, queuedIslandActionToken_, rearmIslandAction_);
    else if (kind == kDeferredMenuAction) cancel(queuedMenuAction_, queuedMenuActionToken_, rearmMenuAction_);
    else if (kind == kDeferredListViewActivation)
        cancel(queuedListViewActivation_, queuedListViewActivationToken_, rearmListViewActivation_);
    else if (kind == kDeferredNativeAction && token == queuedNativeActionToken_)
        CancelNativeActionOnSourceThread();
}

void SourceThreadAgent::RunListViewActivationOnSourceThread(const ListViewActivationRequest& request) noexcept {
    if (GetCurrentThreadId() != threadId_ || nativeActionRunning_) return;
    struct RunningScope final {
        SourceThreadAgent* agent;
        ~RunningScope() {
            agent->nativeActionRunning_ = false;
            agent->RequestMenuBarRefresh();
            agent->MarkDirty();
        }
    } running{this};
    nativeActionRunning_ = true;
    const auto cancellation = listViewActivationCancellation_;
    try {
        const auto ready = [&] {
            const auto identity = captureContext_.nodeIds.find(request.listView);
            return !shuttingDown_.load() && !trackedPopup_ &&
                cancellation == listViewActivationCancellation_ &&
                identity != captureContext_.nodeIds.end() && identity->second.nodeId == request.nodeId &&
                identity->second.generation == request.generation && ListViewActivationReady(root_, request);
        };
        if (!ready()) return;
        UINT nativeState = 0;
        std::wstring error;
        AccessibleListViewItem live;
        std::wstring name;
        std::wstring action;
        long state = 0;
        if (!ReadListViewActivationIdentity(request, nativeState, error) ||
            nativeState != request.nativeState ||
            !ready() ||
            !ReadListViewAccessibleAction(request, live, name, action, state, error) ||
            name != request.accessibleName || action != request.defaultAction || state != request.accessibleState ||
            !ready() ||
            !ReadListViewActivationIdentity(request, nativeState, error) || nativeState != request.nativeState ||
            !ready()) {
            FluentShell::Log(L"ListView default action was stale or unavailable: " + error);
            MarkDirty();
            return;
        }
        // Re-read on the same live object that receives the action, after all native
        // item-identity checks. The accessible provider always gets the first attempt.
        name.clear();
        action.clear();
        if (!ReadListViewAccessibleAction(request, live, name, action, state, error) ||
            name != request.accessibleName || action != request.defaultAction || state != request.accessibleState ||
            !ready() ||
            !ReadListViewActivationIdentity(request, nativeState, error) || nativeState != request.nativeState ||
            !ready()) {
            MarkDirty();
            return;
        }
        const HRESULT invoked = live.object->accDoDefaultAction(live.child);
        bool nativeDispatched = false;
        if (invoked == DISP_E_MEMBERNOTFOUND && ready()) {
            // Stock ListView MSAA can advertise a double-click while leaving its
            // default-action method unimplemented. Admit the control's own
            // double-click path only for the exact standard accessible contract;
            // a refusal or an application-specific default action is not replaced.
            Microsoft::WRL::ComPtr<IAccessible> standardRoot;
            AccessibleListViewItem standard;
            std::wstring standardName;
            std::wstring standardAction;
            long standardState = 0;
            name.clear();
            action.clear();
            const bool standardContract = ReadListViewAccessibleAction(request, live, name, action, state, error) &&
                name == request.accessibleName && action == request.defaultAction && state == request.accessibleState &&
                ready() &&
                SUCCEEDED(CreateStdAccessibleObject(request.listView,
                OBJID_CLIENT, IID_PPV_ARGS(standardRoot.GetAddressOf()))) && standardRoot &&
                ReadListViewAccessibleAction(request, standard, standardName, standardAction,
                    standardState, error, standardRoot.Get()) &&
                standardName == request.accessibleName && standardAction == request.defaultAction &&
                standardState == request.accessibleState;
            const auto currentItem = [&] {
                UINT currentState = 0;
                constexpr UINT selectedFocused = LVIS_SELECTED | LVIS_FOCUSED;
                constexpr DWORD unsupported = LVS_EX_TRACKSELECT | LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE;
                return ready() && (request.viewStyle & (LVS_OWNERDATA | LVS_OWNERDRAWFIXED)) == 0 &&
                    (request.nativeState & selectedFocused) == selectedFocused &&
                    ReadListViewActivationIdentity(request, currentState, error) &&
                    currentState == request.nativeState &&
                    SendMessageW(request.listView, LVM_GETSELECTEDCOUNT, 0, 0) == 1 &&
                    SendMessageW(request.listView, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED) == request.index &&
                    (SendMessageW(request.listView, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0) & unsupported) == 0 &&
                    SendMessageW(request.listView, LVM_ISGROUPVIEWENABLED, 0, 0) == FALSE &&
                    ReadListViewActivationIdentity(request, currentState, error) &&
                    currentState == request.nativeState && ready();
            };
            POINT point{};
            if (standardContract && currentItem() && !GetCapture() &&
                ReadListViewNativeActivationPoint(request.listView, request.index, point) && currentItem() && !GetCapture()) {
                const LPARAM position = MAKELPARAM(static_cast<SHORT>(point.x), static_cast<SHORT>(point.y));
                // Selection/focus already completed. Stock comctl32 processes
                // the second click pair directly and emits its own NM_DBLCLK and
                // LVN_ITEMACTIVATE. No foreground switch or forged WM_NOTIFY.
                SendMessageW(request.listView, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
                nativeDispatched = true;
                POINT after{};
                if (currentItem() && ReadListViewNativeActivationPoint(request.listView, request.index, after) &&
                    after.x == point.x && after.y == point.y && currentItem() && !GetCapture())
                    SendMessageW(request.listView, WM_LBUTTONUP, 0, position);
                FluentShell::Log(L"ListView default action dispatched through native double-click handler");
            }
        }
        if (invoked != S_OK && !nativeDispatched) {
            wchar_t result[16]{};
            swprintf_s(result, L"0x%08lX", static_cast<unsigned long>(invoked));
            FluentShell::Log(L"ListView default action did not complete: HRESULT=" +
                std::wstring(result) + L" hwnd=" + Ipc::HwndToString(request.listView) +
                L" node=" + std::to_wstring(request.nodeId) +
                L" index=" + std::to_wstring(request.index) +
                L" nativeId=" + std::to_wstring(request.nativeId) +
                L" childId=" + std::to_wstring(live.child.lVal) +
                L" action='" + action + L"'");
        }
    } catch (...) {
        // The consumed request is never replayed after a provider exception.
    }
}

void SourceThreadAgent::RunIslandActionOnSourceThread(const IslandActionRequest& request) noexcept {
    try {
        if (GetCurrentThreadId() != threadId_ || shuttingDown_.load() ||
            !IsApplicationCloaked(root_) || trackedPopup_ ||
            !IsChild(root_, request.island) || !IsWindowVisible(request.island) ||
            reinterpret_cast<uintptr_t>(GetPropW(request.island, kNodeGenerationProperty)) !=
                request.generation) return;
        for (HWND current = request.island; current; current = GetParent(current)) {
            if (!IsWindowEnabled(current)) return;
            if (current == root_) break;
        }
        pendingIslandMenu_.reset();
        if (request.dropDown) {
            pendingIslandMenu_ = request;
            pendingIslandDeadline_ = GetTickCount64() + 2000;
        }
        std::wstring reason;
        if (!InvokeAccessibleIslandItem(request.island, request.index,
                request.name, request.action, reason)) {
            pendingIslandMenu_.reset();
            FluentShell::Log(L"Island action did not run: " + reason);
        }
        MarkDirty();
    } catch (...) {
        pendingIslandMenu_.reset();
    }
}

void SourceThreadAgent::CancelPopupOnSourceThread(bool activeOnly) noexcept {
    // EnableWindow(FALSE) sends WM_CANCELMODE. A provider can disable its opener
    // before entering TrackPopupMenu; that does not cancel the pending open request.
    if (activeOnly && !trackedPopup_) return;
    ++listViewActivationCancellation_;
    CancelNativeActionOnSourceThread();
    queuedListViewActivation_.reset();
    queuedListViewActivationToken_ = 0;
    rearmListViewActivation_ = false;
    queuedMenuAction_.reset();
    queuedMenuActionToken_ = 0;
    rearmMenuAction_ = false;
    queuedIslandAction_.reset();
    queuedIslandActionToken_ = 0;
    rearmIslandAction_ = false;
    pendingIslandMenu_.reset();
    popupDecision_.reset();
    popupMenu_.Cancel();
    popupCaptureError_.clear();
    MarkDirty();
}

bool SourceThreadAgent::CompletePopupOnSourceThread(
    const ActionRequest& action, std::wstring& error) {
    PopupMenuDecision decision;
    if (!trackedPopup_ || !IsApplicationCloaked(root_) ||
        !popupMenu_.Resolve(action.popupId, action.popupItemId, decision)) {
        error = L"popup is no longer active or its item is disabled";
        return false;
    }
    MarkDirty();
    if (!decision.IsDismissal()) {
        std::vector<MenuItemSnapshot> live;
        if (!IsWindow(trackedPopupOwner_) ||
            !CaptureMenuHandle(trackedPopup_, L"", live, error) ||
            !PopupMenuState::MatchLiveChoice(live, decision)) {
            error = L"native popup command changed before selection";
            return false;
        }
    }
    popupDecision_ = std::move(decision);
    return true;
}

bool SourceThreadAgent::TrackIslandPopupOnSourceThread(
    HMENU menu, UINT flags, HWND owner, BOOL& result) {
    result = FALSE;
    if (GetCurrentThreadId() != threadId_ || shuttingDown_.load() || trackedPopup_ ||
        !pendingIslandMenu_ || !IsApplicationCloaked(root_)) return false;
    const auto request = *pendingIslandMenu_;
    pendingIslandMenu_.reset();
    if (GetTickCount64() > pendingIslandDeadline_ || !IsChild(root_, request.island) ||
        reinterpret_cast<uintptr_t>(GetPropW(request.island, kNodeGenerationProperty)) !=
            request.generation) return true;

    // Keep the HMENU on the application's own call stack. The supervisor and action
    // commands can run in this modal pump, including the command that restores the
    // complete native surface if any subsequent capture fails.
    trackedPopup_ = menu;
    trackedPopupOwner_ = owner;
    trackedIslandMenu_ = request;
    popupDecision_.reset();
    bool quit = false;
    int quitCode = 0;
    struct Cleanup final {
        SourceThreadAgent* agent;
        ~Cleanup() {
            agent->trackedPopup_ = nullptr;
            agent->trackedPopupOwner_ = nullptr;
            agent->trackedIslandMenu_.reset();
            agent->CancelPopupOnSourceThread();
            agent->RequestMenuBarRefresh();
        }
    } cleanup{ this };

    if ((flags & TPM_NONOTIFY) == 0) {
        SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
    }
    std::vector<MenuItemSnapshot> items;
    std::wstring reason;
    if (!CaptureMenuHandle(menu, L"", items, reason) || items.empty() ||
        popupMenu_.Begin(request.nodeId, request.index, std::move(items)) == 0) {
        popupCaptureError_ = L"island popup cannot be projected: " + reason;
        FluentShell::Log(popupCaptureError_);
    } else {
        FluentShell::Log(L"Island popup waiting for WinUI selection id=" +
            std::to_wstring(popupMenu_.Current()->popupId));
    }
    MarkDirty();
    while (!shuttingDown_.load() && !IsDestroyed() && IsApplicationCloaked(root_) &&
           IsWindow(owner) && IsMenu(menu) &&
           (popupMenu_.Current() || !popupCaptureError_.empty())) {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                quitCode = static_cast<int>(message.wParam);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
    if ((flags & TPM_NONOTIFY) == 0 && IsWindow(owner)) {
        SendMessageW(owner, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
    }
    // WM_UNINITMENUPOPUP is application code and can pump a restore, shutdown,
    // or WM_CANCELMODE. Read the terminal choice only after that callback: a
    // stack copy taken before it would resurrect the command cancellation revoked.
    const auto decision = popupDecision_;
    if (quit) PostQuitMessage(quitCode);
    if (!quit && !shuttingDown_.load() && IsApplicationCloaked(root_) &&
        IsWindow(owner) && decision && !decision->IsDismissal()) {
        if ((flags & TPM_RETURNCMD) != 0) {
            result = static_cast<BOOL>(decision->commandId);
        } else if ((flags & TPM_NONOTIFY) == 0) {
            // The actual tracking owner, not necessarily the top-level frame, owns
            // the command. Post it because it may enter another application modal loop.
            result = PostMessageW(owner, WM_COMMAND, MAKEWPARAM(decision->commandId, 0), 0);
        } else {
            result = TRUE;
        }
        FluentShell::Log(L"Island popup selected command=" + std::to_wstring(decision->commandId));
    }
    return true;
}

bool TryTrackIslandPopup(HMENU menu, UINT flags, HWND owner, BOOL& result) noexcept {
    try {
        const HWND root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
        std::shared_ptr<SourceThreadAgent> selected;
        {
            std::scoped_lock lock(g_agentsMutex);
            for (const auto& [_, agent] : g_agents) {
                if (agent && agent->Root() == root && agent->ThreadId() == GetCurrentThreadId()) {
                    selected = agent->shared_from_this();
                    break;
                }
            }
        }
        return selected && selected->TrackIslandPopupOnSourceThread(menu, flags, owner, result);
    } catch (...) {
        result = FALSE;
        return true;
    }
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

bool SourceThreadAgent::RefreshMenuBarToolbar(
    DWORD popupWaitMs,
    bool& changed,
    std::wstring& error,
    DWORD timeoutMs,
    HANDLE cancelEvent) {
    changed = false;
    Command* command = CreateCommand(kCommandMenuBarRefresh, this);
    if (!command) {
        error = L"source command allocation failed";
        return false;
    }
    command->menuBarPopupWaitMs = popupWaitMs;
    const bool posted = Post(command, timeoutMs, cancelEvent);
    // A timed-out callback can still be unwinding. Never read its mutable fields.
    if (!posted) error = L"source UI thread did not acknowledge menu-bar refresh";
    else {
        changed = command->menuBarChanged;
        error = command->error;
    }
    const bool success = posted && command->success;
    Release(command);
    return success;
}

bool SourceThreadAgent::RefreshMenuBarOnSourceThread(
    DWORD popupWaitMs,
    const std::atomic<bool>& cancelled,
    bool& changed,
    std::wstring& error) {
    changed = false;
    if (queuedMenuAction_ || queuedNativeAction_ || nativeActionRunning_ || trackedPopup_ ||
        (pendingIslandMenu_ && GetTickCount64() <= pendingIslandDeadline_))
        return true;
    if (GetCurrentThreadId() != threadId_) {
        error = L"menu-bar refresh requires its owning source thread";
        return false;
    }
    ObserveMenuBarToolbar(root_, captureContext_);
    auto& state = captureContext_;
    const uint64_t now = GetTickCount64();
    if (menuBarDirty_.exchange(false)) state.menuBarRefresh.Invalidate(now);
    if (!state.menuBarToolbar || GetMenu(root_) ||
        !state.menuBarRefresh.ShouldRead(now)) return true;
    DWORD cloak = 0;
    if (FAILED(DwmGetWindowAttribute(root_, DWMWA_CLOAKED, &cloak, sizeof(cloak))) ||
        (cloak & DWM_CLOAKED_APP) == 0) {
        error = L"menu-bar refresh requires a committed, cloaked native surface";
        return false;
    }
    const HWND toolbar = state.menuBarToolbar;
    const uint64_t generation = state.menuBarToolbarGeneration;
    const auto buttons = state.menuBarButtons;
    std::vector<MenuItemSnapshot> menu;
    std::vector<MenuBarCommandBinding> commands;
    const bool read = CaptureMenuBarToolbar(root_, toolbar, buttons, popupWaitMs,
        6500, cancelled, menu, error, &commands);
    if (cancelled.load(std::memory_order_acquire)) return false;
    // The application's message pump may replace the bar while it is being read.
    // Revalidate on the same source thread before publishing any cached commands.
    ObserveMenuBarToolbar(root_, state);
    if (toolbar != state.menuBarToolbar || generation != state.menuBarToolbarGeneration ||
        buttons != state.menuBarButtons) {
        changed = true;
        error = L"menu-bar toolbar changed before refresh publication";
        return false;
    }
    state.menuBarRefresh.Complete(GetTickCount64(), read);
    if (read) {
        if (state.menuBarBindingGeneration == UINT64_MAX) {
            state.menuBarToolbarCommands.clear();
            for (auto& item : state.menuBarToolbarMenu) item.enabled = false;
            changed = true;
            error = L"menu binding generation exhausted";
            return false;
        }
        ++state.menuBarBindingGeneration;
        state.menuBarToolbarMenu = std::move(menu);
        state.menuBarToolbarCommands = std::move(commands);
        changed = true;
        FluentShell::Log(L"Menu-bar toolbar refreshed as a real menu");
    } else {
        // Keep a previously projected bar's labels, but never dispatch its stale
        // commands. Bounded retries can restore it without a duplicate toolbar.
        for (auto& item : state.menuBarToolbarMenu) item.enabled = false;
        state.menuBarToolbarCommands.clear();
        changed = !state.menuBarToolbarMenu.empty();
        FluentShell::Log(L"Menu-bar toolbar refresh deferred: " + error);
    }
    return read;
}

bool SourceThreadAgent::InvokeMenuCommandOnSourceThread(uint32_t commandId,
    const std::atomic<bool>& cancelled, std::wstring& error, uint64_t* queuedToken) {
    if (queuedToken) *queuedToken = 0;
    if (GetCurrentThreadId() != threadId_ || cancelled.load(std::memory_order_acquire)) return false;
    if (GetMenu(root_)) {
        error = L"ordinary native menu requires its owned deferred action";
        return false;
    }
    if (!MenuBarCommandsCurrentOnSourceThread()) {
        error = L"native menu changed and is awaiting refresh";
        return false;
    }
    const auto& state = captureContext_;
    const auto found = std::find_if(state.menuBarToolbarCommands.begin(),
        state.menuBarToolbarCommands.end(), [commandId](const auto& binding) {
            return binding.commandId == commandId;
        });
    if (found == state.menuBarToolbarCommands.end()) {
        error = L"menu command has no current native toolbar binding";
        return false;
    }
    // The native message pump can mutate capture state; own the selection and
    // toolbar signature for the complete intercepted call.
    if (queuedMenuAction_ || queuedNativeAction_ || nativeActionRunning_ ||
        nextMenuActionToken_ == 0 || shuttingDown_.load()) {
        error = L"native menu already has a queued selection";
        return false;
    }
    MenuActionRequest request;
    request.toolbar = state.menuBarToolbar;
    request.toolbarGeneration = state.menuBarToolbarGeneration;
    request.bindingGeneration = state.menuBarBindingGeneration;
    request.binding = *found;
    request.buttons = state.menuBarButtons;
    queuedMenuAction_ = std::move(request);
    queuedMenuActionToken_ = nextMenuActionToken_;
    rearmMenuAction_ = false;
    nextMenuActionToken_ = nextMenuActionToken_ == UINT64_MAX ? 0 : nextMenuActionToken_ + 1;
    if (PostThreadMessageW(threadId_, message_, kDeferredMenuAction,
            static_cast<LPARAM>(queuedMenuActionToken_))) {
        if (queuedToken) *queuedToken = queuedMenuActionToken_;
        return true;
    }
    queuedMenuAction_.reset();
    queuedMenuActionToken_ = 0;
    error = L"native menu selection could not be queued";
    return false;
}

void SourceThreadAgent::DispatchMenuActionOnSourceThread(uint64_t token) noexcept {
    if (GetCurrentThreadId() != threadId_ || token == 0 || token != queuedMenuActionToken_ ||
        !queuedMenuAction_) return;
    if (g_boundedSourceCommandDepth != 0) {
        rearmMenuAction_ = true;
        return;
    }
    if (MenuBarReadInProgress() && !shuttingDown_.load()) {
        if (!PostThreadMessageW(threadId_, message_, kDeferredMenuAction, static_cast<LPARAM>(token))) {
            queuedMenuAction_.reset();
            queuedMenuActionToken_ = 0;
        }
        return;
    }
    auto request = std::move(*queuedMenuAction_);
    queuedMenuAction_.reset();
    queuedMenuActionToken_ = 0;
    rearmMenuAction_ = false;
    try {
        if (shuttingDown_.load() || !IsApplicationCloaked(root_) || !IsWindowEnabled(root_) ||
            !IsWindowVisible(root_) || !IsWindowVisible(request.toolbar) ||
            trackedPopup_ || GetMenu(root_)) return;
        ObserveMenuBarToolbar(root_, captureContext_);
        if (request.toolbar != captureContext_.menuBarToolbar ||
            request.toolbarGeneration != captureContext_.menuBarToolbarGeneration ||
            request.bindingGeneration != captureContext_.menuBarBindingGeneration ||
            request.buttons != captureContext_.menuBarButtons) return;
        std::wstring reason;
        if (!InvokeMenuBarToolbarCommand(root_, request.toolbar, request.buttons,
                request.binding, shuttingDown_, reason)) {
            FluentShell::Log(L"Native menu selection was stale or refused: " + reason);
        }
        RequestMenuBarRefresh();
        MarkDirty();
    } catch (...) {
        RequestMenuBarRefresh();
        MarkDirty();
    }
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
