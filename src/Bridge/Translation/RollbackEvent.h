#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace FluentShell::Bridge::Translation {

// The event a Bridge creates for each projected root.  Signaling it asks that Bridge to roll the
// root back to native (RendererSession::ServiceRollbackRequests).  The event exists only while the
// root is projected, so opening it is how the GUI finds out which windows this process projects.
// Forcing a window native needs no authentication: a stranger who signals it can only take a
// projection away, never give one.  Re-projecting a rolled-back root stays a restart.
inline std::wstring RollbackEventName(DWORD processId, HWND root) {
    wchar_t name[80] = {};
    swprintf_s(name, L"Local\\FluentShell.Rollback.%lu.%llx",
        static_cast<unsigned long>(processId),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(root)));
    return name;
}

} // namespace FluentShell::Bridge::Translation
