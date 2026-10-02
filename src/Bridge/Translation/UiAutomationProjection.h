#pragma once

#include <Windows.h>
// WIN32_LEAN_AND_MEAN omits COM declarations from Windows.h. UIAutomation's
// generated provider/client headers use `interface` before including COM, so
// this header must establish that declaration itself, independent of its caller.
#include <combaseapi.h>
#include <UIAutomation.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace FluentShell::Bridge::Translation {

inline constexpr std::wstring_view kContentViewportAutomationId =
    L"FluentShell.ContentViewport";

// The input is the entire validated proxy subtree. A second viewport remains
// an error even when it belongs to a different XAML branch of the same host.
template <typename Elements>
std::optional<size_t> FindUniqueContentViewportIndex(const Elements& elements) noexcept {
    std::optional<size_t> found;
    for (size_t index = 0; index < elements.size(); ++index) {
        if (elements[index].automationId != kContentViewportAutomationId) continue;
        if (found) return std::nullopt;
        found = index;
    }
    return found;
}

// Ancestors are ordered from the viewport's parent towards the proxy HWND.
// Names are intentionally irrelevant: an MMC MDI child can have the same title
// and Window role as its top-level frame, but it does not own the viewport.
template <typename Elements>
std::optional<size_t> FindOutermostXamlProjectionAncestorIndex(
    const Elements& ancestors) noexcept {
    std::optional<size_t> found;
    for (size_t index = 0; index < ancestors.size(); ++index) {
        const auto& ancestor = ancestors[index];
        if (ancestor.framework == L"XAML" && ancestor.automationId.empty() &&
            (ancestor.controlType == UIA_WindowControlTypeId ||
             ancestor.controlType == UIA_PaneControlTypeId ||
             ancestor.controlType == UIA_GroupControlTypeId)) {
            found = index;
        }
    }
    return found;
}

} // namespace FluentShell::Bridge::Translation
