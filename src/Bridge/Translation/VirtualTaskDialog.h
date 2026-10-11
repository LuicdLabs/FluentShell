// A TaskDialogIndirect call answered by the Bridge.
//
// The dialog is a virtual surface (DialogSnapshots.h builds it from a
// TaskDialogModel), but the application still talks to a task dialog: its callback
// receives TDN_* notifications with an HWND it sends TDM_* messages to.  That HWND
// is a hidden host window on the calling thread, so every notification runs on the
// thread that called TaskDialogIndirect -- the thread the native dialog would have
// run its modal loop on -- and every TDM_* message, from the callback or from any
// other thread, is applied there.  Renderer actions arrive on the session's action
// worker and are posted to the host, so the callback never runs on a Bridge thread.
//
// A dialog the application owns from the host HWND (a MessageBox raised from a
// callback) is owned by this dialog's projection: RendererSession maps the host
// back to the surface.
#pragma once

#include "DialogSnapshots.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace FluentShell::Bridge::Translation {

struct TaskDialogResult final {
    int button = 0;
    int radioButton = 0;
    bool verificationChecked = false;
};

class VirtualTaskDialog final {
public:
    struct Sink final {
        // Publishes a rebuilt dialog.  `eventId` answers the renderer action that
        // caused it, or is 0.
        std::function<void(WindowSnapshot, uint64_t)> publish;
        // The dialog has its result.
        std::function<void()> complete;
    };

    // Reads the configuration on the calling thread.  Returns null, with a reason,
    // for a configuration the projection cannot honour; the caller then shows the
    // native dialog.
    static std::unique_ptr<VirtualTaskDialog> Create(
        const TASKDIALOGCONFIG& config, HWND owner, std::wstring& reason);
    ~VirtualTaskDialog();

    VirtualTaskDialog(const VirtualTaskDialog&) = delete;
    VirtualTaskDialog& operator=(const VirtualTaskDialog&) = delete;

    // The dialog as first shown.  The snapshot's identity (surface ID, generation)
    // is kept by every later rebuild.
    WindowSnapshot InitialSnapshot();
    // Creates the host window on the calling thread.  Call on the thread that called
    // TaskDialogIndirect, after the projection is shown.
    bool Start(Sink sink);
    // Sends TDN_DIALOG_CONSTRUCTED and TDN_CREATED, once the host is registered as
    // an owner, so a dialog the callback opens at once is owned by this one.
    void Announce();
    HWND Host() const noexcept { return host_.load(); }
    // Any thread: queues a renderer action for the calling thread.
    void PostAction(const ActionRequest& action);
    bool Completed() const noexcept { return completed_; }
    // Sends TDN_DESTROYED and destroys the host.  Call on the calling thread.
    void Finish();
    TaskDialogResult Result() const noexcept;
    const TaskDialogModel& Model() const noexcept { return model_; }

    // Applies a TDM_* message.  Public for the host window procedure and tests.
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    VirtualTaskDialog() = default;
    bool Load(const TASKDIALOGCONFIG& config, std::wstring& reason);
    HRESULT Notify(UINT notification, WPARAM wParam = 0, LPARAM lParam = 0);
    void Rebuild(uint64_t eventId = 0);
    void ApplyAction(const ActionRequest& action);
    void ClickButton(int id, uint64_t eventId = 0);
    void ClickRadio(int id, uint64_t eventId = 0);
    void ClickVerification(bool checked, uint64_t eventId = 0);
    bool UpdateIcon(bool footer, LPARAM icon);
    bool SetElementText(WPARAM element, LPARAM text);

    TaskDialogModel model_;
    TaskDialogElements elements_;
    PFTASKDIALOGCALLBACK callback_ = nullptr;
    LONG_PTR callbackData_ = 0;
    HINSTANCE instance_ = nullptr;
    TASKDIALOG_FLAGS flags_ = 0;
    HWND owner_ = nullptr;
    UINT dpi_ = 96;
    std::wstring surfaceId_;
    uint64_t generation_ = 0;
    // Read by PostAction on the session's action worker.
    std::atomic<HWND> host_{ nullptr };
    Sink sink_;
    bool completed_ = false;
    bool announced_ = false;
    bool destroyed_ = false;
    int button_ = 0;
    ULONGLONG timerBase_ = 0;
};

} // namespace FluentShell::Bridge::Translation
