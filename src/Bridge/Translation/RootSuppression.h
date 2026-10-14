#pragma once

#include <Windows.h>

#include <cstdint>
#include <unordered_map>

namespace FluentShell::Bridge::Translation {

// The production store.  The stamp is a window property the Bridge attaches to a suppressed
// root.  Windows releases a window's property list when the window is destroyed, so a window
// created at a reused HWND carries no stamp and is never taken for the suppressed root.
struct WindowPropertyStamps final {
    static constexpr wchar_t kProperty[] = L"FluentShell.RollbackStamp";

    bool Attach(HWND window, uintptr_t stamp) const noexcept {
        return SetPropW(window, kProperty, reinterpret_cast<HANDLE>(stamp)) != FALSE;
    }
    uintptr_t Stamp(HWND window) const noexcept {
        return reinterpret_cast<uintptr_t>(GetPropW(window, kProperty));
    }
    void Detach(HWND window) const noexcept {
        RemovePropW(window, kProperty);
    }
    bool Alive(HWND window) const noexcept {
        return IsWindow(window) != FALSE;
    }
};

// Roots the user rolled back, which projection must not take again until the user allows them.
// A suppression applies only while its window still carries the stamp it was made with, so a
// destroyed window, or one whose HWND was reused, drops its entry the next time it is consulted.
// The caller serializes access; RendererSession guards it with surfacesMutex_.
template <typename Handle, typename Store>
class RootSuppressionLedger final {
public:
    explicit RootSuppressionLedger(Store store = {}) : store_(store) {}

    // Returns false when nothing was suppressed: the window is gone or cannot carry a stamp.
    bool Suppress(Handle root) {
        if (!root || !store_.Alive(root)) return false;
        const uintptr_t stamp = ++lastStamp_;
        if (!store_.Attach(root, stamp)) return false;
        roots_.insert_or_assign(root, stamp);
        return true;
    }

    bool IsSuppressed(Handle root) {
        const auto entry = roots_.find(root);
        if (entry == roots_.end()) return false;
        if (store_.Alive(root) && store_.Stamp(root) == entry->second) return true;
        roots_.erase(entry);
        return false;
    }

    // Lets the root be projected again.  Removing an entry that has gone stale is a no-op.
    void Allow(Handle root) {
        if (roots_.erase(root) == 0) return;
        if (store_.Alive(root)) store_.Detach(root);
    }

    size_t Count() const noexcept { return roots_.size(); }

private:
    Store store_;
    std::unordered_map<Handle, uintptr_t> roots_;
    uintptr_t lastStamp_ = 0;
};

} // namespace FluentShell::Bridge::Translation
