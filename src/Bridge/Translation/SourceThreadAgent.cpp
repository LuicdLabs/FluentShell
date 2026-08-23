#include "SourceThreadAgent.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
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
constexpr wchar_t kNodeGenerationProperty[] = L"FluentShell.Bridge.NodeGeneration";

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

void MarkCurrentThreadAgentsDirty() noexcept {
    const DWORD threadId = GetCurrentThreadId();
    try {
        std::scoped_lock lock(g_agentsMutex);
        for (const auto& [_, agent] : g_agents) {
            if (agent && agent->ThreadId() == threadId) agent->MarkDirty();
        }
    } catch (...) {}
}

bool RelevantMessage(UINT message) noexcept {
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
    case SB_SIMPLE:
    case SB_SETTEXTW:
    case SB_SETMINHEIGHT:
    // ProgressBar state is driven entirely by these messages.  Without them a
    // dirty-gated reconcile would never notice a native progress update.
    case PBM_SETPOS:
    case PBM_DELTAPOS:
    case PBM_STEPIT:
    case PBM_SETRANGE:
    case PBM_SETRANGE32:
    case PBM_SETSTEP:
    case PBM_SETSTATE:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK ControlSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData) {
    auto* owner = reinterpret_cast<SourceThreadAgent*>(refData);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (owner && RelevantMessage(message)) owner->MarkDirty();
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
    if (owner && RelevantMessage(message)) owner->MarkDirty();
    if (message == WM_NCDESTROY && owner) {
        owner->MarkDestroyed();
        RemoveWindowSubclass(window, RootSubclassProc, subclassId);
    }
    return result;
}

void InstallChildSubclass(SourceThreadAgent* agent, HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        SetWindowSubclass(hwnd, ControlSubclassProc, kControlSubclassId,
            reinterpret_cast<DWORD_PTR>(agent));
    }
}

void InstallRootSubclass(SourceThreadAgent* agent) {
    SetWindowSubclass(agent->Root(), RootSubclassProc, kRootSubclassId,
        reinterpret_cast<DWORD_PTR>(agent));
}

void ExecuteCommandImpl(Command* command) {
    auto* agent = command->agent;
    if (!agent || !IsWindow(agent->Root())) {
        command->error = L"source window no longer exists";
        command->outcome.destroyed = true;
        Complete(command);
        return;
    }
    PhysicalCoordinateScope dpiScope;
    if (!dpiScope.IsValid()) {
        command->error = L"cannot establish physical-coordinate DPI context";
        Complete(command);
        return;
    }
    if (AbortIfCancelled(command)) return;

    switch (command->kind) {
    case kCommandCapture: {
        command->success = agent->CaptureOnSourceThread(
            command->capture.surfaceId,
            command->capture.revision,
            command->snapshot,
            command->error);
        if (AbortIfCancelled(command)) return;
        if (command->success) {
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
                return;
            }
            agent->ClearDirty();
        }
        break;
    }
    case kCommandInvoke: {
        const auto& action = command->action;
        HWND target = nullptr;
        if (action.nodeId) {
            // The node map is reconstructed from the latest capture by matching
            // native control IDs; this also invalidates recreated HWND generations.
            struct FindContext { uint64_t id; HWND result; };
            (void)target;
        }
        // Resolve the node by a fresh capture.  The caller has already checked
        // the expected revision, so a full scan is the safest source-thread read.
        WindowSnapshot before;
        std::wstring captureError;
        if (AbortIfCancelled(command)) return;
        if (!agent->CaptureOnSourceThread(
                action.surfaceId, action.expectedRevision, before, captureError)) {
            command->error = captureError;
            break;
        }
        if (!ValidateActionForSnapshot(action, before, command->error)) break;
        if (AbortIfCancelled(command)) return;
        if (action.action == L"activate") {
            if (AbortIfCancelled(command)) return;
            command->success = SetForegroundWindow(agent->Root()) != FALSE;
        } else if (action.action == L"move" || action.action == L"resize") {
            if (AbortIfCancelled(command)) return;
            const RECT r = action.rect;
            command->success = SetWindowPos(agent->Root(), nullptr, r.left, r.top,
                r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        } else if (action.action == L"minimize") {
            if (AbortIfCancelled(command)) return;
            ShowWindow(agent->Root(), SW_MINIMIZE);
            command->success = IsIconic(agent->Root()) != FALSE;
        } else if (action.action == L"maximize") {
            if (AbortIfCancelled(command)) return;
            ShowWindow(agent->Root(), SW_MAXIMIZE);
            command->success = IsZoomed(agent->Root()) != FALSE;
        } else if (action.action == L"restore") {
            if (AbortIfCancelled(command)) return;
            ShowWindow(agent->Root(), SW_RESTORE);
            command->success = !IsIconic(agent->Root()) && !IsZoomed(agent->Root());
        } else if (action.action == L"close") {
            if (AbortIfCancelled(command)) return;
            SendMessageW(agent->Root(), WM_CLOSE, 0, 0);
            command->outcome.destroyed = !IsWindow(agent->Root());
            command->outcome.closeRejected = !command->outcome.destroyed;
            command->success = true;
        } else if (action.action == L"menuCommand") {
            if (AbortIfCancelled(command)) return;
            // Menu handlers may enter a modal loop. Queue the validated
            // WM_COMMAND so this bounded source-thread command can return;
            // periodic reconciliation captures the resulting native state.
            command->success = PostMessageW(agent->Root(), WM_COMMAND,
                MAKEWPARAM(action.menuCommandId, 0), 0) != FALSE;
        } else if (action.nodeId) {
            for (const auto& node : before.nodes) {
                if (node.nodeId != *action.nodeId) continue;
                if (AbortIfCancelled(command)) return;
                target = node.hwnd;
                const bool interactive = node.visible && node.enabled;
                if (action.action == L"invoke" &&
                    node.kind == ControlKind::Button && interactive) {
                    // A button handler is allowed to enter a synchronous
                    // MessageBox/TaskDialog.  Queue BM_CLICK so the source
                    // command can acknowledge within the bounded dispatcher
                    // deadline; the native message loop then runs the modal
                    // API normally after this hook returns.
                    if (!AbortIfCancelled(command))
                        command->success = PostMessageW(target, BM_CLICK, 0, 0) != FALSE;
                } else if (action.action == L"invoke" &&
                           node.kind == ControlKind::SysLink && interactive) {
                    // The bounded SysLink adapter accepts exactly one link.
                    // Let the native control generate its normal NM_RETURN
                    // notification instead of fabricating a parent callback.
                    // Queueing avoids holding this dispatcher command across a
                    // handler that may open another window.
                    if (!AbortIfCancelled(command)) {
                        LITEM item{};
                        item.mask = LIF_ITEMINDEX | LIF_STATE;
                        item.iLink = 0;
                        item.stateMask = LIS_FOCUSED;
                        item.state = LIS_FOCUSED;
                        const bool focused = SendMessageW(
                            target, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item)) != FALSE;
                        // The native HWND is cloaked and the renderer owns the
                        // real keyboard focus. Queue a bounded synthetic focus
                        // lifetime to the SysLink itself so its standard
                        // WM_KEYDOWN handler emits NM_RETURN without stealing
                        // the desktop focus from the proxy.
                        const bool focusEntered = focused && PostMessageW(
                            target, WM_SETFOCUS, 0, 0) != FALSE;
                        const bool down = focusEntered && PostMessageW(
                            target, WM_KEYDOWN, VK_RETURN, 1) != FALSE;
                        const bool up = down && PostMessageW(
                            target, WM_KEYUP, VK_RETURN, 1 | (1ll << 30) | (1ll << 31)) != FALSE;
                        const bool focusLeft = up && PostMessageW(
                            target, WM_KILLFOCUS, 0, 0) != FALSE;
                        command->success = focused && focusEntered && down && up && focusLeft;
                    }
                } else if (action.action == L"setText" &&
                            (node.kind == ControlKind::Edit || node.kind == ControlKind::Password ||
                             (node.kind == ControlKind::ComboBox && node.editable)) &&
                            !node.readOnly && interactive) {
                    if (AbortIfCancelled(command)) return;
                    command->success = SetWindowTextW(target, action.text.c_str()) != FALSE;
                    if (command->success && !command->cancelled.load(std::memory_order_acquire)) {
                        const HWND notificationTarget = node.kind == ControlKind::ComboBox
                            ? GetParent(target) : agent->Root();
                        SendMessageW(notificationTarget ? notificationTarget : agent->Root(), WM_COMMAND,
                            MAKEWPARAM(node.controlId,
                                node.kind == ControlKind::ComboBox ? CBN_EDITCHANGE : EN_CHANGE),
                            reinterpret_cast<LPARAM>(target));
                    }
                } else if (action.action == L"setCheck" &&
                           (node.kind == ControlKind::CheckBox || node.kind == ControlKind::ThreeState ||
                             node.kind == ControlKind::RadioButton) && interactive) {
                    const int maximum = node.kind == ControlKind::ThreeState ? 2 : 1;
                    const bool validValue = action.integerValue >= 0 &&
                        action.integerValue <= maximum &&
                        (node.kind != ControlKind::RadioButton || action.integerValue == 1);
                    if (validValue) {
                        if (AbortIfCancelled(command)) return;
                        int current = static_cast<int>(SendMessageW(target, BM_GETCHECK, 0, 0));
                        for (int attempt = 0;
                             current != action.integerValue && attempt <= maximum;
                             ++attempt) {
                            if (AbortIfCancelled(command)) return;
                            SendMessageW(target, BM_CLICK, 0, 0);
                            const int next = static_cast<int>(
                                SendMessageW(target, BM_GETCHECK, 0, 0));
                            if (next == current) break;
                            current = next;
                        }
                        command->success = static_cast<int>(
                            SendMessageW(target, BM_GETCHECK, 0, 0)) == action.integerValue;
                    }
                } else if (action.action == L"setSelection" &&
                           node.kind == ControlKind::ListView && interactive) {
                    if (AbortIfCancelled(command)) return;
                    LVITEMW state{};
                    state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                    state.state = 0;
                    SendMessageW(target, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                        reinterpret_cast<LPARAM>(&state));
                    for (size_t index = 0; index < action.integerValues.size(); ++index) {
                        if (AbortIfCancelled(command)) return;
                        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                        state.state = LVIS_SELECTED |
                            (index == 0 ? LVIS_FOCUSED : 0);
                        SendMessageW(target, LVM_SETITEMSTATE,
                            static_cast<WPARAM>(action.integerValues[index]),
                            reinterpret_cast<LPARAM>(&state));
                    }
                    std::vector<int> actual;
                    int previousSelected = -1;
                    bool validEnumeration = true;
                    while (true) {
                        const int selected = static_cast<int>(SendMessageW(
                            target, LVM_GETNEXTITEM, previousSelected, LVNI_SELECTED));
                        if (selected < 0) break;
                        if (selected <= previousSelected ||
                            static_cast<size_t>(selected) >= node.rows.size() ||
                            actual.size() >= node.rows.size()) {
                            validEnumeration = false;
                            break;
                        }
                        actual.push_back(selected);
                        previousSelected = selected;
                    }
                    command->success = validEnumeration && actual == action.integerValues;
                } else if (action.action == L"select" &&
                           (node.kind == ControlKind::ComboBox || node.kind == ControlKind::ListBox) &&
                           interactive) {
                    const bool validIndex = action.integerValue >= -1 &&
                        (action.integerValue == -1 ||
                            static_cast<size_t>(action.integerValue) < node.items.size());
                    if (validIndex) {
                        if (AbortIfCancelled(command)) return;
                        const bool combo = node.kind == ControlKind::ComboBox;
                        SendMessageW(target, combo ? CB_SETCURSEL : LB_SETCURSEL,
                            action.integerValue, 0);
                        const int selected = static_cast<int>(SendMessageW(
                            target, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0));
                        if (selected == action.integerValue && !command->cancelled.load(std::memory_order_acquire)) {
                            const HWND notificationTarget = combo ? GetParent(target) : agent->Root();
                            SendMessageW(notificationTarget ? notificationTarget : agent->Root(), WM_COMMAND,
                                MAKEWPARAM(node.controlId,
                                    combo ? CBN_SELCHANGE : LBN_SELCHANGE),
                                reinterpret_cast<LPARAM>(target));
                            command->success = true;
                        }
                    }
                }
                break;
            }
        }
        if (command->success &&
            (agent->IsDestroyed() || !IsWindow(agent->Root()))) {
            command->outcome.destroyed = true;
        }
        if (AbortIfCancelled(command)) return;
        if (command->success && !command->outcome.destroyed) {
            command->success = agent->CaptureOnSourceThread(
                action.surfaceId, action.expectedRevision + 1,
                command->outcome.snapshot, command->error);
            if (AbortIfCancelled(command)) return;
            command->outcome.accepted = command->success;
            command->outcome.revision = action.expectedRevision + 1;
        }
        break;
    }
    case kCommandCloak: {
        if (AbortIfCancelled(command)) return;
        BOOL value = command->cloaked ? TRUE : FALSE;
        const HRESULT hr = DwmSetWindowAttribute(
            agent->Root(), DWMWA_CLOAK, &value, sizeof(value));
        if (command->cancelled.load(std::memory_order_acquire)) {
            if (command->cloaked) {
                value = FALSE;
                DwmSetWindowAttribute(agent->Root(), DWMWA_CLOAK, &value, sizeof(value));
            }
            AbortIfCancelled(command);
            return;
        }
        DWORD reasons = 0;
        const HRESULT verify = DwmGetWindowAttribute(
            agent->Root(), DWMWA_CLOAKED, &reasons, sizeof(reasons));
        const bool appCloaked = (reasons & DWM_CLOAKED_APP) != 0;
        command->success = SUCCEEDED(hr) && SUCCEEDED(verify) &&
            appCloaked == command->cloaked;
        if (!command->success) command->error = L"DWMWA_CLOAK failed";
        break;
    }
    case kCommandCaptureAndCloak: {
        if (AbortIfCancelled(command)) return;
        command->success = agent->CaptureOnSourceThread(
            command->capture.surfaceId,
            command->capture.revision,
            command->snapshot,
            command->error);
        command->captured = command->success;
        if (command->success &&
            SnapshotFingerprint(command->snapshot) != command->expectedFingerprint) {
            command->success = false;
            command->error = L"native revision changed before cloak";
        }
        if (AbortIfCancelled(command)) return;
        if (command->success) {
            BOOL value = TRUE;
            const HRESULT hr = DwmSetWindowAttribute(
                agent->Root(), DWMWA_CLOAK, &value, sizeof(value));
            if (command->cancelled.load(std::memory_order_acquire)) {
                value = FALSE;
                DwmSetWindowAttribute(agent->Root(), DWMWA_CLOAK, &value, sizeof(value));
                AbortIfCancelled(command);
                return;
            }
            DWORD reasons = 0;
            const HRESULT verify = DwmGetWindowAttribute(
                agent->Root(), DWMWA_CLOAKED, &reasons, sizeof(reasons));
            command->success = SUCCEEDED(hr) && SUCCEEDED(verify) &&
                (reasons & DWM_CLOAKED_APP) != 0;
            if (!command->success) command->error = L"native cloak verification failed";
        }
        break;
    }
    case kCommandRestore: {
        if (AbortIfCancelled(command)) return;
        BOOL value = FALSE;
        const HRESULT hr = DwmSetWindowAttribute(
            agent->Root(), DWMWA_CLOAK, &value, sizeof(value));
        if (AbortIfCancelled(command)) return;
        DWORD reasons = DWM_CLOAKED_APP;
        const HRESULT verify = DwmGetWindowAttribute(
            agent->Root(), DWMWA_CLOAKED, &reasons, sizeof(reasons));
        command->success = SUCCEEDED(hr) && SUCCEEDED(verify) &&
            (reasons & DWM_CLOAKED_APP) == 0;
        if (command->success) {
            SetWindowPos(agent->Root(), nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            SetForegroundWindow(agent->Root());
        } else {
            command->error = L"native window remained application-cloaked";
        }
        break;
    }
    case kCommandShutdown:
        if (AbortIfCancelled(command)) return;
        EnumChildWindows(agent->Root(), [](HWND child, LPARAM) -> BOOL {
            RemoveWindowSubclass(child, ControlSubclassProc, kControlSubclassId);
            RemovePropW(child, kNodeGenerationProperty);
            return TRUE;
        }, 0);
        RemoveWindowSubclass(agent->Root(), RootSubclassProc, kRootSubclassId);
        command->success = true;
        break;
    default:
        command->error = L"unknown source-thread command";
        break;
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
        MarkCurrentThreadAgentsDirty();
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
            MarkCurrentThreadAgentsDirty();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace

SourceThreadAgent::SourceThreadAgent(
    HWND root, HMODULE module, DWORD threadId, UINT message) noexcept
    : root_(root), module_(module), threadId_(threadId), message_(message),
      generation_(g_nextGeneration.fetch_add(1)) {}

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
    const bool posted = agent->Post(command, 2000, nullptr);
    const bool success = posted && command->success;
    if (!success) {
        const std::wstring detail = posted
            ? command->error
            : L"source UI thread acknowledgement timed out";
        FluentShell::Log(L"Initial source capture failed within the 2 s deadline: " +
            detail);
    }
    if (!posted) {
        // The hook may already be inside ExecuteCommand. Keep every pointer used
        // by that callback valid even though Attach must honor its 2 s deadline;
        // cleanup waits for the callback before removing the dispatch hooks.
        ScheduleTimedOutAttachCleanup(agent, command);
    }
    Release(command);
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
