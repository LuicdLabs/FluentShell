#pragma once

#include "../Common/FluentShell.h"

namespace FluentShell::Dwm {

// L0: cross-process DWM attributes. No injection required.
bool ApplyToWindow(HWND hwnd, const DwmStyle& style);
bool ApplyDarkTitleBar(HWND hwnd, bool dark);
bool ApplyBackdrop(HWND hwnd, Backdrop backdrop);
bool ApplyRoundCorners(HWND hwnd, bool round);
bool ApplyCaptionColors(HWND hwnd, bool dark);

// Enumerate top-level windows and apply style when match rules hit.
using EnumCallback = bool (*)(HWND hwnd, const MatchRule& rule, void* ctx);
void EnumTopLevelWindows(const std::vector<MatchRule>& rules, EnumCallback cb, void* ctx);

// Convenience: apply first matching rule to a single HWND.
bool ApplyMatchingRules(HWND hwnd, const std::vector<MatchRule>& rules);

// Built-in default rules (dialogs + common system shells get L0 floor).
std::vector<MatchRule> DefaultRules();

} // namespace FluentShell::Dwm
