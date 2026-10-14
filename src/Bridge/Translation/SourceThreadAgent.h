#pragma once

#include "ControlAdapters.h"
#include "DateTimePickerValue.h"
#include "WindowCapture.h"
#include "PopupMenuState.h"

#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FluentShell::Bridge::Translation {

struct DirectUiNativeEvidence;
struct DirectUiActionBinding;
struct DirectUiWindowProfile;
struct DirectUiOwnedProfile;
struct DirectUiBootstrapEvidence;

struct IslandActionRequest final {
    HWND island = nullptr;
    uint64_t nodeId = 0;
    uint64_t generation = 0;
    int index = -1;
    std::wstring name;
    std::wstring action;
    bool dropDown = false;
    ControlKind kind = ControlKind::AccessibleIsland;
    std::optional<DiskViewSnapshot> diskView;
    // A toolbar drop-down only: the command id of the button whose arrow was pressed. `index` is
    // that button's position in the toolbar and `name` its label.
    uint32_t commandId = 0;
};

struct ListViewActivationRequest final {
    HWND listView = nullptr;
    uint64_t nodeId = 0;
    uint64_t generation = 0;
    int index = -1;
    uint32_t nativeId = 0;
    size_t itemCount = 0;
    DWORD viewStyle = 0;
    UINT nativeState = 0;
    long accessibleState = 0;
    std::wstring text;
    std::wstring accessibleName;
    std::wstring defaultAction;
};

struct ActionOutcome final {
    bool accepted = false;
    bool destroyed = false;
    // The application declined the operation, or the indexed target changed
    // before it could run. Canonical state is intact, so the surface stays
    // projected and the renderer is told the action was rejected.
    bool refused = false;
    uint64_t closeSequence = 0;
    uint64_t revision = 0;
    WindowSnapshot snapshot;
    std::wstring error;
};

// Prefix of the capture error raised when a surface spanning several GUI threads
// kept changing across both confirmation passes (a snap-in still loading).
inline constexpr wchar_t kCaptureBarrierUnstable[] =
    L"native controls changed across their GUI-thread capture barrier";

// Whether a message delivered to `window` can change what capture observes. The
// WM_USER-range control codes are judged by the receiving window's class, because
// the families share numbers (a toolbar read can alias a trackbar mutator).
// A WM_NOTIFY (lParam is its NMHDR) that only queries display data or reports
// painting/hovering is not a mutation.
bool IsNativeMutationMessage(HWND window, UINT message, LPARAM lParam = 0, WPARAM wParam = 0) noexcept;

// An event the thread hooks set when a top-level window on a hooked GUI thread is shown or
// activated: a dialog the application opens over a projection is invisible (its cloaked
// owner's cloak is inherited) until discovery projects it, so discovery is woken for it
// rather than finding it on its next periodic pass.  Null stops the signal.  The event
// must outlive every hook that could still see it.
void SetTopLevelShownEvent(HANDLE event) noexcept;
// The same signal for windows shown by any thread of this process (an in-context
// EVENT_OBJECT_SHOW hook): threads the agents never hook open dialogs too.
bool InstallTopLevelShownHook(HMODULE module) noexcept;
void RemoveTopLevelShownHook() noexcept;

// Virtual item indices identify only the published ordering. These actions must
// not run, or automatically replay, after that native snapshot has changed.
bool IsOwnerDataListViewIndexedAction(
    const ActionRequest& action, const WindowSnapshot& snapshot) noexcept;

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

inline bool IsTreeViewItemChecked(HWND treeView, HTREEITEM item) noexcept {
    const auto state = static_cast<UINT>(SendMessageW(treeView, TVM_GETITEMSTATE,
        reinterpret_cast<WPARAM>(item), TVIS_STATEIMAGEMASK));
    return (state & TVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
}

// A TVS_CHECKBOXES tree toggles its caret item's box on the Space key, the one gesture every
// check-box tree honours: comctl32 raises TVN_KEYDOWN and then TVN_ITEMCHANGING/TVN_ITEMCHANGED
// for the new state image, and a Windows Forms tree toggles the node in its own key handler
// (AfterCheck).  A direct TVM_SETITEM would reach only the applications listening for
// TVN_ITEMCHANGED.  So the item becomes the caret, as a keyboard user's would, and the key
// arrives as the messages a key press produces.  The state is read back, because the
// application may refuse the change.
inline bool SetTreeViewItemCheck(HWND treeView, int index, bool checked) noexcept {
    const HTREEITEM item = ResolveTreeViewItem(treeView, index);
    if (!item) return false;
    if (IsTreeViewItemChecked(treeView, item) == checked) return true;
    if (!SelectTreeViewItem(treeView, index)) return false;
    const UINT scan = MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC);
    const LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
    SendMessageW(treeView, WM_KEYDOWN, VK_SPACE, down);
    SendMessageW(treeView, WM_CHAR, L' ', down);
    SendMessageW(treeView, WM_KEYUP, VK_SPACE, down | (1ll << 30) | (1ll << 31));
    return IsTreeViewItemChecked(treeView, item) == checked;
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

// A picker's new value is applied the way the control applies one the user chose: the value
// itself, then the DTN_DATETIMECHANGE notification the control raises to its parent (a Windows
// Forms DateTimePicker learns of its new Value from that notification alone; DTM_SETSYSTEMTIME
// does not send it).  `requested` is the day number of a date picker or the second of the day of
// a time picker; the other half of the value is left as the control has it.  The native value is
// the verdict.
inline bool SetDateTimePickerValue(
    HWND root, HWND picker, bool timeOfDay, int requested) noexcept {
    if (!picker) return false;
    SYSTEMTIME value{};
    if (SendMessageW(picker, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&value)) != GDT_VALID)
        return false;
    SYSTEMTIME target = value;
    if (timeOfDay) {
        if (requested < 0 || requested >= kSecondsPerDay) return false;
        target.wHour = static_cast<WORD>(requested / 3600);
        target.wMinute = static_cast<WORD>(requested % 3600 / 60);
        target.wSecond = static_cast<WORD>(requested % 60);
        target.wMilliseconds = 0;
    } else {
        int year = 0;
        int month = 0;
        int day = 0;
        CivilFromDayNumber(requested, year, month, day);
        target.wYear = static_cast<WORD>(year);
        target.wMonth = static_cast<WORD>(month);
        target.wDay = static_cast<WORD>(day);
        target.wDayOfWeek = DayOfWeekFromDayNumber(requested);
    }
    if (std::memcmp(&target, &value, sizeof(SYSTEMTIME)) == 0) return true;
    if (!SendMessageW(picker, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&target)))
        return false;
    // The control keeps a value inside its range and clamps one outside it, so what it holds
    // now is what the user chose or it is not.
    SYSTEMTIME applied{};
    if (SendMessageW(picker, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&applied)) != GDT_VALID ||
        (timeOfDay
            ? applied.wHour != target.wHour || applied.wMinute != target.wMinute ||
                applied.wSecond != target.wSecond
            : applied.wYear != target.wYear || applied.wMonth != target.wMonth ||
                applied.wDay != target.wDay)) {
        SendMessageW(picker, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&value));
        return false;
    }
    NMDATETIMECHANGE change{};
    change.nmhdr.hwndFrom = picker;
    change.nmhdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(picker));
    change.nmhdr.code = DTN_DATETIMECHANGE;
    change.dwFlags = GDT_VALID;
    change.st = applied;
    const HWND parent = SyntheticNotificationTarget(root, picker);
    SendMessageW(parent, WM_NOTIFY, change.nmhdr.idFrom, reinterpret_cast<LPARAM>(&change));
    return true;
}

// An UpDown step follows the sequence the control itself runs for an arrow click:
// UDN_DELTAPOS to its parent, which may veto the step or rewrite its delta; then the
// position, which a UDS_SETBUDDYINT control also writes into its buddy; then the
// WM_VSCROLL/WM_HSCROLL SB_THUMBPOSITION and SB_ENDSCROLL pair.  A veto leaves the
// native state untouched and is reported as `refused`.
inline bool SetUpDownPosition(HWND root, HWND upDown, int requested, bool& refused) noexcept {
    refused = false;
    if (!upDown) return false;
    BOOL failed = FALSE;
    const auto current = [&] {
        return static_cast<int>(SendMessageW(upDown, UDM_GETPOS32, 0,
            reinterpret_cast<LPARAM>(&failed)));
    };
    const int before = current();
    if (before == requested) return true;
    int low = 0;
    int high = 0;
    SendMessageW(upDown, UDM_GETRANGE32, reinterpret_cast<WPARAM>(&low),
        reinterpret_cast<LPARAM>(&high));
    NMUPDOWN change{};
    change.hdr.hwndFrom = upDown;
    change.hdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(upDown));
    change.hdr.code = UDN_DELTAPOS;
    change.iPos = before;
    change.iDelta = requested - before;
    const HWND parent = SyntheticNotificationTarget(root, upDown);
    if (SendMessageW(parent, WM_NOTIFY, change.hdr.idFrom,
            reinterpret_cast<LPARAM>(&change)) != 0) {
        refused = true;
        return false;
    }
    const long long stepped = static_cast<long long>(before) + change.iDelta;
    const int target = static_cast<int>(std::clamp<long long>(
        stepped, (std::min)(low, high), (std::max)(low, high)));
    SendMessageW(upDown, UDM_SETPOS32, 0, target);
    const UINT scroll = (GetWindowLongPtrW(upDown, GWL_STYLE) & UDS_HORZ) != 0
        ? WM_HSCROLL : WM_VSCROLL;
    SendMessageW(parent, scroll, MAKEWPARAM(SB_THUMBPOSITION, static_cast<WORD>(target)),
        reinterpret_cast<LPARAM>(upDown));
    SendMessageW(parent, scroll, MAKEWPARAM(SB_ENDSCROLL, static_cast<WORD>(target)),
        reinterpret_cast<LPARAM>(upDown));
    return current() == target;
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
// `noEditor` reports that the application declined to open the session at all (its
// TVN_BEGINLABELEDIT/LVN_BEGINLABELEDIT veto): some applications (regedit) rename only from
// their own Rename command, and the caller then takes that path instead.
inline bool RenameTreeViewItem(
    HWND treeView, int index, const std::wstring& text, bool* noEditor = nullptr) noexcept {
    if (noEditor) *noEditor = false;
    const HTREEITEM item = ResolveTreeViewItem(treeView, index);
    if (!item) return false;
    ThreadFocusScope focus(treeView);
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(
        treeView, TVM_EDITLABELW, 0, reinterpret_cast<LPARAM>(item)));
    if (!edit || !IsWindow(edit)) {
        if (noEditor) *noEditor = true;
        return false;
    }
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

inline bool RenameListViewItem(
    HWND listView, int index, const std::wstring& text, bool* noEditor = nullptr) noexcept {
    if (noEditor) *noEditor = false;
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
    if (!edit || !IsWindow(edit)) {
        if (noEditor) *noEditor = true;
        return false;
    }
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
    std::wstring actual;
    std::wstring reason;
    return ReadListViewItemText(listView, index, 0, actual, reason) && actual == text;
}

// Ends an open label session without a rename, as the user's Escape does.  A list has
// LVM_CANCELEDITLABEL only from common controls 6; before that its edit box answers the
// Escape key itself.
inline void CancelOpenLabelEdit(HWND control, bool treeView) noexcept {
    if (treeView) {
        SendMessageW(control, TVM_ENDEDITLABELNOW, TRUE, 0);
        return;
    }
    SendMessageW(control, LVM_CANCELEDITLABEL, 0, 0);
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(control, LVM_GETEDITCONTROL, 0, 0));
    if (!edit || !IsWindow(edit)) return;
    SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 1);
    SendMessageW(edit, WM_KEYUP, VK_ESCAPE, 1 | (1ll << 30) | (1ll << 31));
}

// Finishes a label session the application opened itself (from its own Rename command):
// the text goes into the control's own edit box, which then commits the way a user's Return
// does.  Returns false when no session is open.  What the application makes of the text is
// read back by the next capture: it may rename asynchronously or normalize the name.
inline bool CommitOpenLabelEdit(HWND control, bool treeView, const std::wstring& text) noexcept {
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(
        control, treeView ? TVM_GETEDITCONTROL : LVM_GETEDITCONTROL, 0, 0));
    if (!edit || !IsWindow(edit)) return false;
    if (!SetWindowTextW(edit, text.c_str())) {
        CancelOpenLabelEdit(control, treeView);
        return false;
    }
    if (treeView) {
        SendMessageW(control, TVM_ENDEDITLABELNOW, FALSE, 0);
    } else {
        SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
        SendMessageW(edit, WM_KEYUP, VK_RETURN, 1 | (1ll << 30) | (1ll << 31));
    }
    return true;
}

// Whether the control has a label session open on that item: the session the application
// opened itself (its Rename command) is on the focused list row or the tree's caret item.
inline bool LabelSessionOpenOn(HWND control, bool treeView, int index) noexcept {
    if (index < 0) return false;
    const HWND edit = reinterpret_cast<HWND>(SendMessageW(
        control, treeView ? TVM_GETEDITCONTROL : LVM_GETEDITCONTROL, 0, 0));
    if (!edit || !IsWindow(edit)) return false;
    if (!treeView) {
        return static_cast<int>(SendMessageW(control, LVM_GETNEXTITEM,
            static_cast<WPARAM>(-1), LVNI_FOCUSED)) == index;
    }
    const HTREEITEM item = ResolveTreeViewItem(control, index);
    return item && reinterpret_cast<HTREEITEM>(
        SendMessageW(control, TVM_GETNEXTITEM, TVGN_CARET, 0)) == item;
}

// The item's live label (the first column of a list row).
inline bool ReadItemLabel(HWND control, bool treeView, int index, std::wstring& label) noexcept {
    label.clear();
    if (!treeView) {
        std::wstring reason;
        return ReadListViewItemText(control, index, 0, label, reason);
    }
    const HTREEITEM item = ResolveTreeViewItem(control, index);
    if (!item) return false;
    try {
        std::vector<wchar_t> buffer(1024, L'\0');
        TVITEMW read{};
        read.mask = TVIF_HANDLE | TVIF_TEXT;
        read.hItem = item;
        read.pszText = buffer.data();
        read.cchTextMax = static_cast<int>(buffer.size());
        if (!SendMessageW(control, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&read))) return false;
        buffer.back() = L'\0';
        label = buffer.data();
        return true;
    } catch (...) {
        return false;
    }
}

// A rename the application only allows from its own Rename command, waiting for the session
// that command opens (see SourceThreadAgent::PostLabelRename).
struct LabelRenameRequest final {
    HWND control = nullptr;
    bool treeView = false;
    uint64_t nodeId = 0;
    uint64_t generation = 0;
    std::wstring text;
    uint64_t deadline = 0;
};

// One Bridge-owned action waiting on the source thread's queue.  The posted message
// carries only `token`; the request stays here, so cancelling releases it even when
// the message is never dispatched, and an old token can never run again.
template <typename Request>
struct DeferredSlot final {
    uint64_t nextToken = 1;
    uint64_t token = 0;
    std::optional<Request> request;
    // A bounded command consumed the message; the request is reposted after it ends.
    bool rearm = false;

    explicit operator bool() const noexcept { return request.has_value(); }
    void Reset() noexcept {
        request.reset();
        token = 0;
        rearm = false;
    }
};

class SourceThreadAgent final : public std::enable_shared_from_this<SourceThreadAgent> {
public:
    // transientFailure (optional) reports whether a failed initial capture was a
    // timing condition worth retrying later: the GUI thread did not answer within the
    // deadline, or a multi-thread surface kept changing across its capture barrier.
    static std::shared_ptr<SourceThreadAgent> Attach(
        HWND root, HMODULE module, bool* transientFailure = nullptr);
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
    bool RefreshMenuBarToolbar(
        DWORD popupWaitMs,
        bool& changed,
        std::wstring& error,
        DWORD timeoutMs = 8000,
        HANDLE cancelEvent = nullptr);
    bool RefreshMenuBarOnSourceThread(DWORD popupWaitMs,
        const std::atomic<bool>& cancelled, bool& changed, std::wstring& error);
    bool InvokeMenuCommandOnSourceThread(uint32_t commandId,
        const std::atomic<bool>& cancelled, std::wstring& error, uint64_t* queuedToken = nullptr);
    void DispatchMenuActionOnSourceThread(uint64_t token) noexcept;
    bool PostListViewActivation(const ControlNode& node, int index,
        bool& refused, std::wstring& error, uint64_t* queuedToken = nullptr) noexcept;
    void DispatchListViewActivationOnSourceThread(uint64_t token) noexcept;
    // A rename the application refused as a bare label session: its own Rename command is
    // started the way a user starts it -- F2 to the control, with the control's thread focus,
    // through the application's message loop and accelerators -- and the session that opens
    // is finished afterwards.  Source thread only.
    bool PostLabelRename(const ControlNode& node, HWND control, int index,
        const std::wstring& text, uint64_t* queuedToken) noexcept;
    void DispatchLabelRenameOnSourceThread(uint64_t token) noexcept;
    // Only Bridge-issued generic actions enter this queue. Application-posted
    // HWND messages are never intercepted or reordered.
    bool PostNativeAction(const ActionRequest& action, const ControlNode* node,
        bool& refused, std::wstring& error, uint64_t* queuedToken = nullptr,
        uint64_t* closeSequence = nullptr) noexcept;
    void DispatchNativeActionOnSourceThread(uint64_t token) noexcept;
    // Bounded command dispatch consumes nested action messages and rearms them
    // after Complete; a failed command cancels only the token it created.
    void RearmDeferredActionsOnSourceThread() noexcept;
    void CancelDeferredActionOnSourceThread(WPARAM kind, uint64_t token) noexcept;
    void RequestMenuBarRefresh() noexcept { menuBarDirty_.store(true); }
    bool MenuBarCommandsCurrentOnSourceThread() const noexcept {
        return GetCurrentThreadId() == threadId_ &&
            (!captureContext_.menuBarToolbar ||
                (!menuBarDirty_.load() && !captureContext_.menuBarRefresh.pending));
    }
    bool Shutdown() noexcept;

    HWND Root() const noexcept { return root_; }
    DWORD ThreadId() const noexcept { return threadId_; }
    bool IsDirty() const noexcept { return dirty_.load(); }
    void ClearDirty() noexcept { dirty_.store(false); }
    bool IsDestroyed() const noexcept { return destroyed_.load(); }
    uint64_t Generation() const noexcept { return generation_; }
    UINT MessageId() const noexcept { return message_; }
    // Queues one accessible island element's own default action to run on this thread
    // after the current command returns.  It is posted rather than performed inline
    // because a provider's default action may open a menu of its own and spin a modal
    // loop, which would blow every bounded command deadline.  Identity is revalidated
    // inside the deferred handler, so an element that moved is dropped instead of
    // acted on.
    // `open` queues the item's activation (selection and the Enter key) instead of its
    // context menu: a TreeView or ListView item only.
    bool PostIslandAction(const ControlNode& node, int index, bool& refused,
        uint64_t* queuedToken = nullptr, bool open = false) noexcept;
    void DispatchIslandActionOnSourceThread(uint64_t token) noexcept;
    bool TrackIslandPopupOnSourceThread(HMENU menu, UINT flags, HWND owner, BOOL& result);
    bool CompletePopupOnSourceThread(const ActionRequest& action, std::wstring& error);
    void CancelPopupOnSourceThread(bool activeOnly = false) noexcept;
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
    void AdoptDirectUiProfile(std::shared_ptr<DirectUiOwnedProfile> profile) noexcept;

private:
    SourceThreadAgent(HWND root, HMODULE module, DWORD threadId, UINT message) noexcept;
    bool Post(void* command, DWORD timeoutMs, HANDLE cancelEvent = nullptr) noexcept;
    void UnhookAll() noexcept;
    void RunIslandActionOnSourceThread(const IslandActionRequest& request) noexcept;
    void RunListViewActivationOnSourceThread(const ListViewActivationRequest& request) noexcept;
    struct RemotePopupState final {
        std::mutex mutex;
        PopupMenuState menu;
        std::optional<PopupMenuDecision> decision;
        IslandActionRequest request;
        std::wstring error;
        // Set once the control's own thread tracked the popup: the root thread must then not
        // claim a second one for the same request.
        std::atomic<bool> claimed{false};
    };
    bool TrackRemoteIslandPopup(const IslandActionRequest& request,
        const std::shared_ptr<RemotePopupState>& state,
        const std::shared_ptr<std::atomic<bool>>& cancelled,
        HMENU menu, UINT flags, HWND owner, BOOL& result);
    void RunListViewActivationOnOwner(const ListViewActivationRequest& request,
        uint64_t cancellation, const std::shared_ptr<std::atomic<bool>>& cancelled) noexcept;
    bool PostRemoteAction(HWND target, std::function<void(const std::shared_ptr<std::atomic<bool>>&)> work,
        std::wstring& error);

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
    struct NativeActionRequest final {
        ActionRequest action;
        HWND source = nullptr;
        HWND owner = nullptr;
        uint64_t nodeGeneration = 0;
        ControlKind kind = ControlKind::StaticText;
        HMENU menu = nullptr;
        uint64_t menuFingerprint = 0;
        uint64_t closeSequence = 0;
        std::optional<NativeDocumentSnapshot> document;
    };
    void RunNativeActionOnSourceThread(const NativeActionRequest& request) noexcept;
    void RunNativeActionOnOwner(const NativeActionRequest& request, const ControlNode& node,
        const std::shared_ptr<std::atomic<bool>>& cancelled) noexcept;
    void CancelNativeActionOnSourceThread() noexcept;
    struct MenuActionRequest final {
        HWND toolbar = nullptr;
        uint64_t toolbarGeneration = 0;
        uint64_t bindingGeneration = 0;
        std::vector<MenuBarButton> buttons;
        MenuBarCommandBinding binding;
    };
    // Publishes `request` under a fresh token and posts `kind` for it. Returns the token,
    // or 0 when the post failed and the slot was cleared.
    template <typename Request>
    uint64_t QueueDeferred(DeferredSlot<Request>& slot, WPARAM kind, Request request) noexcept;
    // Claims the request `token` names -- or defers it: a bounded command re-arms it and
    // a menu-bar read in progress reposts it. `dropped` sees a request whose repost failed.
    template <typename Request, typename Dropped>
    std::optional<Request> TakeDeferred(
        DeferredSlot<Request>& slot, WPARAM kind, uint64_t token, Dropped dropped) noexcept;
    // All of this state belongs to the source GUI thread. The native tracking call
    // keeps its HMENU alive while the renderer owns the visible flyout.
    DeferredSlot<NativeActionRequest> nativeAction_;
    DeferredSlot<MenuActionRequest> menuAction_;
    DeferredSlot<ListViewActivationRequest> listViewActivation_;
    DeferredSlot<IslandActionRequest> islandAction_;
    DeferredSlot<LabelRenameRequest> labelRename_;
    bool nativeActionRunning_ = false;
    std::atomic<uint64_t> listViewActivationCancellation_{0};
    std::atomic<unsigned> remoteActions_{0};
    std::mutex remoteSubmissionMutex_;
    std::atomic<bool> restoring_{false};
    std::shared_ptr<std::atomic<bool>> remoteCancellation_;
    std::shared_ptr<RemotePopupState> remotePopup_;
    std::atomic<uint64_t> nextPopupId_{1};
    uint64_t AllocatePopupId() noexcept;
    std::optional<IslandActionRequest> pendingIslandMenu_;
    std::optional<IslandActionRequest> trackedIslandMenu_;
    ULONGLONG pendingIslandDeadline_ = 0;
    PopupMenuState popupMenu_;
    std::optional<PopupMenuDecision> popupDecision_;
    HMENU trackedPopup_ = nullptr;
    HWND trackedPopupOwner_ = nullptr;
    std::wstring popupCaptureError_;
    std::atomic<bool> menuBarDirty_{ false };
    uint64_t nextDirectUiWindowGeneration_ = 1;
    std::atomic<bool> directUiUiaPoisoned_{ false };
    const DirectUiWindowProfile* directUiProfile_ = nullptr;
    bool genericDirectUiCandidate_ = false;
    std::shared_ptr<DirectUiOwnedProfile> ownedDirectUiProfile_;
};

void RetainSourceThreadAgent(std::shared_ptr<SourceThreadAgent> agent) noexcept;
bool TryTrackIslandPopup(HMENU menu, UINT flags, HWND owner, BOOL& result) noexcept;

} // namespace FluentShell::Bridge::Translation
