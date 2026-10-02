#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "AccessibleIsland.h"

struct IHTMLDocument2;

namespace FluentShell::Bridge::Translation {

// Bounded, opt-in diagnostics from the native MSHTML document contract.
std::wstring DescribeMmcHtmlDocument(HWND window) noexcept;

// MMC's resource-backed extended-view template. Unknown documents or interactive
// content outside this read-only contract remain native.
// Pending: projected text must also be checked for native font, padding, and
// wrapping parity; native overflow admission alone does not establish text fit.
bool ReadMmcHtmlDocument(HWND window, std::vector<AccessibleIslandItem>& items,
    std::wstring& reason) noexcept;

namespace MmcHtmlDocumentDetail {
// Shared content admission, exposed for deterministic in-process MSHTML tests.
// This does not establish URL trust. Production HWND capture must enter through
// ReadMmcHtmlDocument, which verifies the exact Windows resource first.
bool ReadContent(IHTMLDocument2* document, const RECT& viewport,
    std::vector<AccessibleIslandItem>& items, std::wstring& reason) noexcept;
}

}
