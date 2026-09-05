#pragma once

#include "ControlAdapters.h"
#include "WindowCapture.h"

#include <commctrl.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace FluentShell::Bridge::Translation {

struct DirectUiNativeEvidence;
struct DirectUiActionBinding;
struct DirectUiWindowProfile;
struct DirectUiOwnedProfile;
struct DirectUiBootstrapEvidence;

struct ActionOutcome final {
    bool accepted = false;
    bool destroyed = false;
    // The application ran the requested operation and declined it.  Canonical
    // state is intact, so the surface stays projected and the renderer is told the
    // action was rejected.
    bool refused = false;
    uint64_t closeSequence = 0;
    uint64_t revision = 0;
    WindowSnapshot snapshot;
    std::wstring error;
};

inline HWND SyntheticNotificationTarget(HWND root, HWND target) noexcept {
    const HWND parent = GetParent(target);
    return parent ? parent : root;
}

// Uses the documented ListView state-image contract. The control owns
// LVN_ITEMCHANGING/LVN_ITEMCHANGED delivery and may veto the transition.
inline bool SetListViewItemCheck(HWND listView, int index, bool checked) noexcept {
    if (!listView || index < 0) return false;
    LVITEMW state{};
    state.stateMask = LVIS_STATEIMAGEMASK;
    state.state = INDEXTOSTATEIMAGEMASK(checked ? 2 : 1);
    const LRESULT applied = SendMessageW(listView, LVM_SETITEMSTATE,
        static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&state));
    return applied != FALSE &&
        (ListView_GetCheckState(listView, index) != FALSE) == checked;
}

inline bool SelectTabControl(HWND root, HWND tab, int controlId, int index, int count) noexcept {
    if (!tab || index < 0 || index >= count) return false;
    const int current = static_cast<int>(SendMessageW(tab, TCM_GETCURSEL, 0, 0));
    if (current == index) return true;
    NMHDR notification{ tab, static_cast<UINT_PTR>(controlId), TCN_SELCHANGING };
    const HWND parent = SyntheticNotificationTarget(root, tab);
    if (SendMessageW(parent, WM_NOTIFY, notification.idFrom,
            reinterpret_cast<LPARAM>(&notification)) != 0) return false;
    SendMessageW(tab, TCM_SETCURSEL, index, 0);
    if (static_cast<int>(SendMessageW(tab, TCM_GETCURSEL, 0, 0)) != index) return false;
    notification.code = TCN_SELCHANGE;
    SendMessageW(parent, WM_NOTIFY, notification.idFrom,
        reinterpret_cast<LPARAM>(&notification));
    return true;
}

// TVM_SELECTITEM is the documented selection mutator and the control raises its
// own TVN_SELCHANGING/TVN_SELCHANGED pair, so the application sees exactly the
// notification a click would have produced.
inline bool SelectTreeViewItem(HWND treeView, int index) noexcept {
    const HTREEITEM item = ResolveTreeViewItem(treeView, index);
    if (!item) return false;
    const auto caret = [treeView] {
        return reinterpret_cast<HTREEITEM>(
            SendMessageW(treeView, TVM_GETNEXTITEM, TVGN_CARET, 0));
    };
    if (caret() == item) return true;
    SendMessageW(treeView, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item));
    return caret() == item;
}

// TVM_EXPAND lets the control raise TVN_ITEMEXPANDING/TVN_ITEMEXPANDED, which is
// what gives a lazily populated tree the chance to insert the children the user
// just asked for.  The state is read back because an application handler may
// refuse the expansion.
inline bool SetTreeViewItemExpanded(HWND treeView, int index, bool expanded) noexcept {
    const HTREEITEM item = ResolveTreeViewItem(treeView, index);
    if (!item) return false;
    SendMessageW(treeView, TVM_EXPAND, expanded ? TVE_EXPAND : TVE_COLLAPSE,
        reinterpret_cast<LPARAM>(item));
    TVITEMW state{};
    state.mask = TVIF_HANDLE | TVIF_STATE;
    state.hItem = item;
    state.stateMask = TVIS_EXPANDED;
    if (!SendMessageW(treeView, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&state)))
        return false;
    return ((state.state & TVIS_EXPANDED) != 0) == expanded;
}

// A trackbar reports user movement to its parent as WM_HSCROLL/WM_VSCROLL.
// TBM_SETPOSNOTIFY makes the control itself raise that notification, which is
// the path a drag takes; comctl32 v5 does not implement it, so the documented
// set-then-notify pair stands in for it.  Either way the native position is the
// verdict, and SB_THUMBPOSITION/SB_ENDSCROLL carry the same 16-bit position Win32
// gives an application that drags the thumb itself.
inline bool SetTrackbarPosition(
    HWND root, HWND trackbar, bool vertical, int position) noexcept {
    if (!trackbar) return false;
    const auto current = [trackbar] {
        return static_cast<int>(SendMessageW(trackbar, TBM_GETPOS, 0, 0));
    };
    if (current() == position) return true;
    SendMessageW(trackbar, TBM_SETPOSNOTIFY, 0, position);
    if (current() != position) {
        SendMessageW(trackbar, TBM_SETPOS, TRUE, position);
        if (current() != position) return false;
        const UINT scroll = vertical ? WM_VSCROLL : WM_HSCROLL;
        const HWND parent = SyntheticNotificationTarget(root, trackbar);
        SendMessageW(parent, scroll,
            MAKEWPARAM(SB_THUMBPOSITION, static_cast<WORD>(position)),
            reinterpret_cast<LPARAM>(trackbar));
        SendMessageW(parent, scroll, MAKEWPARAM(SB_ENDSCROLL, 0),
            reinterpret_cast<LPARAM>(trackbar));
    }
    return current() == position;
}

// Both label-edit messages are documented to require the control to have the
// focus.  The native window is cloaked and the renderer proxy owns the desktop
// focus, so this moves only the *thread's* focus -- which is what the messages
// check -- and puts it back afterwards.
class ThreadFocusScope final {
public:
    explicit ThreadFocusScope(HWND target) noexcept
        : previous_(GetFocus()) {
        if (target && previous_ != target) SetFocus(target);
    }
    ~ThreadFocusScope() {
        if (previous_ && IsWindow(previous_) && GetFocus() != previous_) SetFocus(previous_);
    }
    ThreadFocusScope(const ThreadFocusScope&) = delete;
    ThreadFocusScope& operator=(const ThreadFocusScope&) = delete;

private:
    HWND previous_ = nullptr;
};

// A projected rename runs the native control's own in-place label session: the
// control creates its edit box, the application decides through
// TVN_BEGINLABELEDIT/LVN_BEGINLABELEDIT whether the session may start and
// through the matching ENDLABELEDIT whether the new text is accepted, and the
// item's own text afterwards is the verdict.  The session is opened and closed
// inside one source-thread command, so the projection never depends on the native
// control keeping keyboard focus while a user types in the proxy.
inline bool RenameTreeViewItem(HWND treeView, int index, const std::wstring& text) noexcept {
    const HTREEITEM item = ResolveTreeViewItem(treeView, index);
    if (!item) return false;
    ThreadFocusScope focus(treeView);
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(
        treeView, TVM_EDITLABELW, 0, reinterpret_cast<LPARAM>(item)));
    if (!edit || !IsWindow(edit)) return false;
    if (!SetWindowTextW(edit, text.c_str())) {
        SendMessageW(treeView, TVM_ENDEDITLABELNOW, TRUE, 0);
        return false;
    }
    SendMessageW(treeView, TVM_ENDEDITLABELNOW, FALSE, 0);
    std::vector<wchar_t> buffer(text.size() + 2, L'\0');
    TVITEMW read{};
    read.mask = TVIF_HANDLE | TVIF_TEXT;
    read.hItem = item;
    read.pszText = buffer.data();
    read.cchTextMax = static_cast<int>(buffer.size());
    if (!SendMessageW(treeView, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&read)))
        return false;
    return text == buffer.data();
}

inline bool RenameListViewItem(HWND listView, int index, const std::wstring& text) noexcept {
    if (index < 0) return false;
    // The item has to be the focused one before the control will open its editor,
    // exactly as it is when a user renames it.
    ThreadFocusScope focus(listView);
    LVITEMW state{};
    state.stateMask = LVIS_FOCUSED;
    state.state = LVIS_FOCUSED;
    SendMessageW(listView, LVM_SETITEMSTATE,
        static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&state));
    SendMessageW(listView, LVM_ENSUREVISIBLE, static_cast<WPARAM>(index), FALSE);
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(
        listView, LVM_EDITLABELW, static_cast<WPARAM>(index), 0));
    if (!edit || !IsWindow(edit)) return false;
    if (!SetWindowTextW(edit, text.c_str())) {
        SendMessageW(listView, LVM_CANCELEDITLABEL, 0, 0);
        return false;
    }
    // A list view has no "end edit now" message; its own edit control commits on
    // Return, which is the path a user takes.
    SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
    SendMessageW(edit, WM_KEYUP, VK_RETURN, 1 | (1ll << 30) | (1ll << 31));
    if (reinterpret_cast<HWND>(SendMessageW(listView, LVM_GETEDITCONTROL, 0, 0)) != nullptr) {
        SendMessageW(listView, LVM_CANCELEDITLABEL, 0, 0);
        return false;
    }
    std::vector<wchar_t> buffer(text.size() + 2, L'\0');
    LVITEMW read{};
    read.iSubItem = 0;
    read.pszText = buffer.data();
    read.cchTextMax = static_cast<int>(buffer.size());
    if (SendMessageW(listView, LVM_GETITEMTEXTW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&read)) < 0) return false;
    return text == buffer.data();
}

class SourceThreadAgent final : public std::enable_shared_from_this<SourceThreadAgent> {
public:
    static std::shared_ptr<SourceThreadAgent> Attach(HWND root, HMODULE module);
    ~SourceThreadAgent();

    SourceThreadAgent(const SourceThreadAgent&) = delete;
    SourceThreadAgent& operator=(const SourceThreadAgent&) = delete;

    // timedOut distinguishes a source thread that did not acknowledge within the
    // deadline -- a transient condition -- from a capture that ran and rejected the
    // window.  Only the latter means the window is unsupported.
    bool Capture(
        WindowSnapshot& snapshot,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr,
        bool* timedOut = nullptr);
    bool Invoke(
        const ActionRequest& action,
        ActionOutcome& outcome,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool SetCloaked(
        bool cloaked,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool CaptureAndCloak(
        WindowSnapshot& snapshot,
        uint64_t expectedFingerprint,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool Restore(
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool CaptureDirectUiNativeEvidence(
        const DirectUiWindowProfile& profile,
        DirectUiNativeEvidence& evidence,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr,
        bool* timedOut = nullptr);
    bool VerifyDirectUiAndCloak(
        const DirectUiWindowProfile& profile,
        const DirectUiNativeEvidence& expected,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool RestoreThenDirectUiButtonClick(
        const DirectUiWindowProfile& profile,
        const DirectUiNativeEvidence& expected,
        const DirectUiActionBinding& binding,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool PostDirectUiPropertySheetButton(
        const DirectUiWindowProfile& profile,
        const DirectUiNativeEvidence& expected,
        const DirectUiActionBinding& binding,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    // Drives one handoff-declared DirectUI navigation without giving the page up:
    // the native root keeps its application cloak and the renderer proxy keeps the
    // screen, so the session can admit the page that replaces this one in place.
    // previousActive is returned whenever the command ran, because activation
    // moved to the native dialog before its handler did.
    bool NavigateDirectUiProjected(
        const DirectUiWindowProfile& profile,
        const DirectUiNativeEvidence& expected,
        const DirectUiActionBinding& binding,
        HWND& previousActive,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool CaptureDirectUiBootstrapEvidence(
        DirectUiBootstrapEvidence& evidence,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool MoveDirectUiWindow(
        const DirectUiWindowProfile& profile,
        const DirectUiNativeEvidence& expected,
        const RECT& bounds,
        DirectUiNativeEvidence& evidence,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    // Walks a projected native checkbox to the requested state through its own
    // click state machine, so the application handler observes the exact
    // transition it would have seen from a user.
    bool InvokeDirectUiToggle(
        const DirectUiActionBinding& binding,
        int requested,
        HWND& previousActive,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    // Drives one projected DirectUI slot through the same registered adapter the
    // Win32 lane uses, so the surface stays projected and the caller accepts only
    // that control's own delta. previousActive is returned whenever the command
    // ran, because activation moved to the native dialog before the handler did.
    bool InvokeDirectUiNodeAction(
        const DirectUiWindowProfile& profile,
        const DirectUiNativeEvidence& expected,
        const DirectUiActionBinding& binding,
        const ActionRequest& request,
        HWND& previousActive,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool RestoreDirectUiActivation(
        HWND previousActive,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool PlaceBehind(HWND sibling, std::wstring& error,
        DWORD timeoutMs = 1000, HANDLE cancelEvent = nullptr);
    // Reads a menu bar the application draws with a toolbar.  Must run after the
    // projection is committed and the native window is cloaked: the read asks the
    // application to open its own menus, which needs its message loop and must not
    // compete with the proxy for the foreground.
    bool ReadMenuBarToolbar(
        HWND toolbar,
        DWORD popupWaitMs,
        std::wstring& error,
        DWORD timeoutMs = 6000,
        HANDLE cancelEvent = nullptr);
    bool HasMenuBarToolbarMenu() const noexcept;
    bool Shutdown() noexcept;

    HWND Root() const noexcept { return root_; }
    DWORD ThreadId() const noexcept { return threadId_; }
    bool IsDirty() const noexcept { return dirty_.load(); }
    void ClearDirty() noexcept { dirty_.store(false); }
    bool IsDestroyed() const noexcept { return destroyed_.load(); }
    // True while a command is pumping the source thread's own messages.  Another command
    // arriving then is requeued rather than dispatched inside the pump.
    bool DeferringCommands() const noexcept { return deferCommands_.load(); }
    void SetDeferringCommands(bool defer) noexcept { deferCommands_.store(defer); }
    uint64_t Generation() const noexcept { return generation_; }
    UINT MessageId() const noexcept { return message_; }
    // Queues one accessible island element's own default action to run on this thread
    // after the current command returns.  It is posted rather than performed inline
    // because a provider's default action may open a menu of its own and spin a modal
    // loop, which would blow every bounded command deadline.  Identity is revalidated
    // inside the deferred handler, so an element that moved is dropped instead of
    // acted on.
    bool PostIslandAction(
        HWND island,
        int index,
        const std::wstring& expectedName,
        const std::wstring& expectedAction) noexcept;
    void MarkDirty(HWND window = nullptr, UINT message = 0) noexcept {
        lastMutationHwnd_.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
        lastMutationMessage_.store(message, std::memory_order_release);
        mutationEpoch_.fetch_add(1, std::memory_order_acq_rel);
        dirty_.store(true);
    }
    void MarkDestroyed() noexcept { destroyed_.store(true); MarkDirty(); }
    uint64_t RegisterCloseRequest() noexcept;
    void MarkCloseRequestCompleted() noexcept;
    uint64_t CompletedCloseSequence() const noexcept {
        return closeCompleted_.load(std::memory_order_acquire);
    }
    bool CaptureOnSourceThread(
        std::wstring_view surfaceId,
        uint64_t revision,
        WindowSnapshot& snapshot,
        std::wstring& error) noexcept;
    uint64_t MutationEpoch() const noexcept {
        return mutationEpoch_.load(std::memory_order_acquire);
    }
    HWND LastMutationHwnd() const noexcept {
        return reinterpret_cast<HWND>(lastMutationHwnd_.load(std::memory_order_acquire));
    }
    UINT LastMutationMessage() const noexcept {
        return lastMutationMessage_.load(std::memory_order_acquire);
    }
    uint64_t DirectUiWindowGeneration(HWND window) noexcept;
    bool DirectUiUiaPoisoned() const noexcept { return directUiUiaPoisoned_.load(); }
    void PoisonDirectUiUia() noexcept { directUiUiaPoisoned_.store(true); }
    const DirectUiWindowProfile* DirectUiProfile() const noexcept { return directUiProfile_; }
    bool GenericDirectUiCandidate() const noexcept { return genericDirectUiCandidate_; }
    // Lets an exact-profile surface degrade to the capability-derived lane once the
    // application navigates past the page its declarative row describes. Returns
    // false when the process itself is not eligible for generic admission.
    bool EnableGenericDirectUiCandidate() noexcept;
    void AdoptDirectUiProfile(std::shared_ptr<DirectUiOwnedProfile> profile) noexcept;

private:
    SourceThreadAgent(HWND root, HMODULE module, DWORD threadId, UINT message) noexcept;
    bool Post(void* command, DWORD timeoutMs, HANDLE cancelEvent = nullptr) noexcept;
    void UnhookAll() noexcept;

    HWND root_ = nullptr;
    HMODULE module_ = nullptr;
    DWORD threadId_ = 0;
    UINT message_ = 0;
    HHOOK hook_ = nullptr;
    HHOOK cbtHook_ = nullptr;
    HHOOK callWndRetHook_ = nullptr;
    uint64_t generation_ = 0;
    std::atomic<bool> dirty_{ true };
    std::atomic<uint64_t> mutationEpoch_{ 1 };
    std::atomic<uintptr_t> lastMutationHwnd_{ 0 };
    std::atomic<UINT> lastMutationMessage_{ 0 };
    std::atomic<bool> destroyed_{ false };
    std::atomic<bool> shuttingDown_{ false };
    std::atomic<uint64_t> closeIssued_{ 0 };
    std::atomic<uint64_t> closeCompleted_{ 0 };
    CaptureContext captureContext_;
    std::atomic<bool> deferCommands_{ false };
    uint64_t nextDirectUiWindowGeneration_ = 1;
    std::atomic<bool> directUiUiaPoisoned_{ false };
    const DirectUiWindowProfile* directUiProfile_ = nullptr;
    bool genericDirectUiCandidate_ = false;
    std::shared_ptr<DirectUiOwnedProfile> ownedDirectUiProfile_;
};

void RetainSourceThreadAgent(std::shared_ptr<SourceThreadAgent> agent) noexcept;

} // namespace FluentShell::Bridge::Translation
