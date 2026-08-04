#pragma once

#include "WindowSnapshot.h"

#include <string>
#include <unordered_map>

namespace FluentShell::Bridge::Translation {

struct CaptureContext final {
    struct NodeIdentity final {
        uint64_t generation = 0;
        uint64_t nodeId = 0;
    };

    std::wstring surfaceId;
    uint64_t generation = 0;
    uint64_t revision = 0;
    uint64_t nextNodeId = 1;
    uint64_t nextNodeGeneration = 1;
    std::unordered_map<HWND, NodeIdentity> nodeIds;
};

bool CaptureWindow(
    HWND root,
    CaptureContext& context,
    WindowSnapshot& snapshot,
    std::wstring& rejectionReason) noexcept;

uint64_t SnapshotFingerprint(const WindowSnapshot& snapshot) noexcept;

} // namespace FluentShell::Bridge::Translation
