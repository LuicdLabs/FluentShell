#pragma once

#include "../../Common/FluentShell.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace FluentShell::Bridge::Translation {

// FLUENTSHELL_DIAG_CAPTURE=1 makes capture log the stages it is about to run (each child's
// classification, identity, facets and detail, and the heavy steps inside an adapter), so a control
// whose capture never returns -- the GUI thread of the target spins or blocks inside it -- is named by
// the last line the log holds. Off by default: a pass logs about a hundred lines.
inline bool CaptureTraceEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"FLUENTSHELL_DIAG_CAPTURE", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}

// FLUENTSHELL_DIAG_RECONCILE=1 names what changed each time reconcile republishes a surface: a console
// that republishes every few hundred milliseconds while idle loses the clicks that land between a
// capture and its publication to stale-revision refusals, and the log says which field is moving.
inline bool ReconcileTraceEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"FLUENTSHELL_DIAG_RECONCILE", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}

// A view, so a literal costs no allocation when tracing is off; a message built from parts is built only
// under `if (CaptureTraceEnabled())`.
inline void CaptureTrace(std::wstring_view text) noexcept {
    if (!CaptureTraceEnabled()) return;
    try {
        FluentShell::Log(L"capture " + std::wstring(text));
    } catch (...) {
    }
}

} // namespace FluentShell::Bridge::Translation
