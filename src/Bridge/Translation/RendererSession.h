#pragma once

#include "SourceThreadAgent.h"

#include <commctrl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <thread>
#include <unordered_map>

namespace FluentShell::Bridge::Translation {

struct TaskDialogResult final {
    int button = 0;
    bool verificationChecked = false;
};

class RendererSession final : public std::enable_shared_from_this<RendererSession> {
public:
    explicit RendererSession(HMODULE bridgeModule) noexcept;
    ~RendererSession();

    RendererSession(const RendererSession&) = delete;
    RendererSession& operator=(const RendererSession&) = delete;

    bool Start();
    void RunSupervisor();
    void Stop() noexcept;
    bool IsReady() const noexcept { return ready_.load() && !failed_.load(); }

    std::optional<int> ShowMessageBox(
        HWND owner,
        std::wstring_view text,
        std::wstring_view caption,
        UINT type,
        WORD languageId = 0);

    std::optional<TaskDialogResult> ShowTaskDialog(
        const TASKDIALOGCONFIG& config,
        const std::vector<std::pair<int, std::wstring>>& buttons,
        std::wstring_view title,
        std::wstring_view instruction,
        std::wstring_view content,
        std::wstring_view footer,
        std::wstring_view verification);

private:
    struct Surface;
    // One split the user moved through the projection.  A container's own stored
    // proportion lives in the application's private data, and there is no message
    // that writes it, so a later re-layout would silently undo the request.  The
    // position is remembered against the extent it was measured on so it can be
    // re-asserted proportionally, and only ever when the extent itself changed --
    // a split the application or the user moved at an unchanged size is a real
    // decision the projection does not fight.
    struct PaneSplitIntent final {
        uint64_t nodeId = 0;
        uint64_t generation = 0;
        int index = 0;
        bool vertical = true;
        // The split position and the container extent it was measured against, both
        // in the container's own client pixels.
        int position = 0;
        int extent = 0;
        // Consecutive re-assertions that did not take effect.  A container that
        // refuses the proportion twice owns its layout and the intent is dropped.
        unsigned failures = 0;
    };
    // One projection attempt.  Groups the values every gate stage reads so the
    // gate can be a sequence of named stages instead of one long function.
    struct ProjectionAttempt;

    // What discovery decided about one visible top-level window.  Classification
    // runs under the surface barrier; acting on the decision must not, because
    // rollback and projection both take that barrier themselves.
    enum class DiscoveryAction {
        Skip,
        RestoreOwnerGraph,
        DeferOwnerGraph,
        Project,
        ProjectAfterDeferral,
    };
    struct DiscoveryDecision final {
        DiscoveryAction action = DiscoveryAction::Skip;
        // Set for RestoreOwnerGraph: the projected ancestor to roll back.
        std::shared_ptr<Surface> projectedOwner;
        // Set for DeferOwnerGraph: true the first time this root is deferred, so
        // a steady state does not repeat the log line every pass.
        bool firstDeferral = false;
    };

    bool CreatePipeAndRenderer(uint64_t handshakeDeadline);
    bool Handshake(DWORD timeoutMs);
    void HeartbeatMain() noexcept;
    void ReaderMain() noexcept;
    bool Send(
        Ipc::MessageType type,
        uint64_t revision,
        std::string payload,
        DWORD timeoutMs = 2000,
        bool failSessionOnError = true) noexcept;
    void HandleFrame(const Ipc::Frame& frame);
    void HandleReady(const SurfaceReady& ready);
    void HandleAction(const ActionRequest& action);
    // Rejects an action and resynchronizes the renderer with the canonical
    // revision.  Requires `lock` to own surface->mutex and releases it before
    // the frames are sent, because Send can block on pipe backpressure.
    void RejectAction(
        const std::shared_ptr<Surface>& surface,
        std::unique_lock<std::mutex>& lock,
        const ActionRequest& action,
        std::wstring_view status,
        std::wstring_view reason);
    // A virtual dialog has no native HWND, so the Bridge answers MessageBox and
    // TaskDialog semantics directly from the snapshot it synthesized.  Same lock
    // contract as RejectAction.
    void ApplyVirtualDialogAction(
        const std::shared_ptr<Surface>& surface,
        std::unique_lock<std::mutex>& lock,
        const ActionRequest& action);
    void HandleNativeAction(const ActionRequest& action, const std::shared_ptr<Surface>& surface);
    void ActionMain() noexcept;
    void DiscoverTopLevelWindows();
    void PruneDiscoveryState();
    DiscoveryDecision ClassifyTopLevel(HWND window, bool ownsVisibleTopLevel);
    bool OpenNativeWindow(HWND root);
    bool IsProjectedOwner(HWND owner);
    bool OpenSurface(const std::shared_ptr<Surface>& surface, bool cloakNative);
    // Projection gate stages, in the order OpenSurface runs them.  Each logs its
    // own rejection reason and returns false; the caller owns rollback.  All of
    // them run with the surface canonical barrier held.
    bool ResolveOwnerProxy(ProjectionAttempt& attempt);
    bool PublishAndAwaitReady(ProjectionAttempt& attempt);
    bool SynchronizeNativeRevision(ProjectionAttempt& attempt);
    // One resync round: republish the recaptured native revision and wait for
    // the renderer to re-report a matching hidden proxy.
    bool RepublishResyncRevision(
        ProjectionAttempt& attempt,
        WindowSnapshot& barrier,
        uint64_t baseRevision,
        uint64_t expectedGeneration,
        std::chrono::steady_clock::time_point deadline);
    bool CommitProvisional(ProjectionAttempt& attempt);
    bool AwaitProxyVisible(ProjectionAttempt& attempt);
    bool EstablishProxyZOrder(const ProjectionAttempt& attempt);
    bool VerifyNativeCloaked(const ProjectionAttempt& attempt);
    bool CommitInteractive(ProjectionAttempt& attempt);
    bool RunUiaGate(
        const ProjectionAttempt& attempt,
        const WindowSnapshot& snapshot,
        const wchar_t* stage);
    bool RunCommittedUiaGate(const ProjectionAttempt& attempt);
    // Rolls the native window back before rejecting.  Used only after the
    // provisional commit has already made the proxy visible.
    bool RejectAfterCommit(const ProjectionAttempt& attempt, std::wstring_view reason);
    // One reconcile pass over one surface.
    struct ReconcilePass;
    void ReconcileNativeSurfaces();
    // Both return the rollback reason the surface needs, or nullptr to keep the
    // projection.  Returning instead of rolling back inline is what keeps the
    // lock choreography out of the reconcile logic: the caller restores only
    // after every lock this pass took has been released.
    const wchar_t* ReconcileSurface(const std::shared_ptr<Surface>& surface);
    // Re-asserts the splits the user moved through the projection when the
    // application has re-laid the container out at a new size.  Runs with the
    // surface canonical barrier held, on the freshly captured snapshot, and may
    // replace it with the one the re-assertion produced.
    void ReassertRememberedSplits(ReconcilePass& pass);
    // Reads a menu bar the application draws with a toolbar, once per surface, after the
    // projection is committed and the native window is cloaked.
    void ReadMenuBarToolbarOnce(ReconcilePass& pass);
    // Records the split a completed setSplit produced, or forgets it when the
    // container no longer describes one.  Requires surface->mutex.
    static void RememberSplitIntent(
        const std::shared_ptr<Surface>& surface, const ActionRequest& action);
    const wchar_t* PublishReconciledSnapshot(ReconcilePass& pass);
    // In-place DirectUI page swap.  A handoff-declared route that only replaces
    // the page inside the same top-level window keeps its projection: the proxy
    // holds the screen behind a re-armed input gate while the bridge admits the
    // page that replaced this one and republishes it as one full-snapshot patch.
    // All five run with the surface canonical barrier held.
    uint64_t BeginDirectUiPageSwap(
        const std::shared_ptr<Surface>& surface, std::wstring_view trigger);
    void EndDirectUiPageSwap(const std::shared_ptr<Surface>& surface) noexcept;
    const wchar_t* AdvanceDirectUiPageSwap(ReconcilePass& pass);
    const wchar_t* ReadmitDirectUiPage(ReconcilePass& pass);
    const wchar_t* ReleaseDirectUiSwapGate(ReconcilePass& pass);
    // Gives a DirectUI surface back to native and permits exactly one more
    // discovery attempt.  A navigation the lane refused is not evidence that the
    // window itself is unsupportable, so the page the application settled on gets
    // a chance instead of being stranded native for the rest of the process.
    // Must be called with the surface canonical barrier released.
    void FallBackFromDirectUiHandoff(
        const std::shared_ptr<Surface>& surface,
        const std::shared_ptr<SourceThreadAgent>& agent,
        std::wstring_view reason) noexcept;
    // Everything the rollback path needs, captured once under the barrier so no
    // stage can read a surface another thread is concurrently changing.
    struct RestoreAttempt;
    void RestoreSurface(const std::shared_ptr<Surface>& surface, std::wstring_view reason) noexcept;
    // Rollback stages, in the order RestoreSurface runs them.  The first four run
    // with the surface canonical barrier held; the last two must not.
    bool BeginRestore(RestoreAttempt& attempt);
    void IsolateProxyForRestore(const RestoreAttempt& attempt) noexcept;
    bool RestoreNativeWindow(const RestoreAttempt& attempt);
    bool PublishRestoredState(const RestoreAttempt& attempt, bool nativeRestored);
    void NotifyRendererOfRollback(
        const RestoreAttempt& attempt, std::wstring_view protocolReason) noexcept;
    void ScheduleEmergencyUncloakRetry(const RestoreAttempt& attempt) noexcept;
    void RestoreAll(std::wstring_view reason) noexcept;
    bool IsRetiredSurface(std::wstring_view surfaceId) noexcept;
    void RememberRetiredSurfaceLocked(std::wstring_view surfaceId) noexcept;
    void RetireSurfaceIfClosed(const std::shared_ptr<Surface>& surface) noexcept;
    std::optional<int> WaitForDialogResult(
        const std::shared_ptr<Surface>& surface,
        HWND owner,
        bool ownerWasEnabled);
    std::wstring RendererPath() const;
    bool VerifyRendererClient() const noexcept;
    bool HasRendererExited() noexcept;
    void TerminateRenderer(DWORD exitCode) noexcept;
    void Fail(std::wstring_view reason) noexcept;

    HMODULE bridgeModule_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE rendererProcess_ = nullptr;
    HANDLE rendererThread_ = nullptr;
    HANDLE shutdownEvent_ = nullptr;
    DWORD rendererProcessId_ = 0;
    uint64_t rendererCreated_ = 0;
    std::wstring pipeName_;
    std::wstring nonce_;
    std::atomic<bool> ready_{ false };
    std::atomic<bool> failed_{ false };
    std::atomic<bool> stopping_{ false };
    std::atomic<uint64_t> outboundSequence_{ 0 };
    std::atomic<uint64_t> inboundSequence_{ 0 };
    std::atomic<uint64_t> lastHeartbeatTick_{ 0 };
    std::mutex stopMutex_;
    std::mutex writeMutex_;
    bool pipeClosing_ = false;
    std::mutex processMutex_;
    std::mutex surfacesMutex_;
    std::unordered_map<std::wstring, std::shared_ptr<Surface>> surfaces_;
    std::unordered_set<std::wstring> retiredSurfaceIds_;
    std::deque<std::wstring> retiredSurfaceOrder_;
    std::unordered_set<HWND> discoveryAttempts_;
    // Zero means an owned top-level is still visible. A nonzero tick starts the
    // quiet period that must elapse before the native owner is projected again.
    std::unordered_map<HWND, uint64_t> ownerGraphDeferrals_;
    std::mutex actionMutex_;
    std::condition_variable actionCondition_;
    std::deque<std::pair<ActionRequest, std::shared_ptr<Surface>>> actionQueue_;
    std::thread reader_;
    std::thread actionWorker_;
    std::thread heartbeatWorker_;
};

void SetActiveRendererSession(std::shared_ptr<RendererSession> session);
std::shared_ptr<RendererSession> ActiveRendererSession();

} // namespace FluentShell::Bridge::Translation
