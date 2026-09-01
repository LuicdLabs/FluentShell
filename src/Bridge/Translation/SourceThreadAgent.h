#pragma once

#include "WindowCapture.h"

#include <commctrl.h>
#include <atomic>
#include <memory>
#include <string>

namespace FluentShell::Bridge::Translation {

struct DirectUiNativeEvidence;
struct DirectUiActionBinding;
struct DirectUiWindowProfile;
struct DirectUiOwnedProfile;
struct DirectUiBootstrapEvidence;

struct ActionOutcome final {
    bool accepted = false;
    bool destroyed = false;
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
        HANDLE cancelEvent = nullptr);
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
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool Shutdown() noexcept;

    HWND Root() const noexcept { return root_; }
    DWORD ThreadId() const noexcept { return threadId_; }
    bool IsDirty() const noexcept { return dirty_.load(); }
    void ClearDirty() noexcept { dirty_.store(false); }
    bool IsDestroyed() const noexcept { return destroyed_.load(); }
    uint64_t Generation() const noexcept { return generation_; }
    UINT MessageId() const noexcept { return message_; }
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
    uint64_t nextDirectUiWindowGeneration_ = 1;
    std::atomic<bool> directUiUiaPoisoned_{ false };
    const DirectUiWindowProfile* directUiProfile_ = nullptr;
    bool genericDirectUiCandidate_ = false;
    std::shared_ptr<DirectUiOwnedProfile> ownedDirectUiProfile_;
};

void RetainSourceThreadAgent(std::shared_ptr<SourceThreadAgent> agent) noexcept;

} // namespace FluentShell::Bridge::Translation
