#pragma once

#include "WindowSnapshot.h"

#include <string>
#include <vector>

namespace FluentShell::Bridge::Translation {

// What a read-only rich edit control shows when its text needs rich rendering: paragraphs
// of runs, each run one constant character format (bold, italic, underline, strike-through,
// size in points, colour). Read through the text object model without touching the
// selection. A control whose content the projection cannot draw (a link, an embedded
// object, hidden, raised or lowered text, a background colour, a numbered list) is refused
// with the reason, so the whole window stays native rather than losing what the text said.
// GUI thread of the control only.
bool ReadRichText(HWND richEdit, std::vector<RichParagraphSnapshot>& paragraphs,
    std::wstring& reason) noexcept;

} // namespace FluentShell::Bridge::Translation
