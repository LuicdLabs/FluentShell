#pragma once

#include "WindowSnapshot.h"
#include <windows.h>
#include <string>

struct IHTMLDocument2;

namespace FluentShell::Bridge::Translation {

// Reads a complete resource-backed MSHTML host, including semantic DOM actions,
// scrollports and a read-only embedded report exposed through COM Automation.
// No native descendant is swallowed unless the document accounts for it.
bool ReadNativeDocument(HWND window, NativeDocumentSnapshot& snapshot,
    std::wstring& reason) noexcept;
bool InvokeNativeDocumentItem(HWND window, const NativeDocumentSnapshot& expected,
    int index, std::wstring& reason) noexcept;
bool ScrollNativeDocument(HWND window, const NativeDocumentSnapshot& expected,
    int regionIndex, int x, int y, std::wstring& reason) noexcept;
// Valid only after ReadNativeDocument succeeded in the same source-thread pass.
bool NativeDocumentOwnsDescendant(HWND document, HWND descendant) noexcept;
void ForgetNativeDocument(HWND window) noexcept;

namespace NativeDocumentDetail {
// Test seam for an in-process STA document. The production entry additionally
// verifies the HWND's thread and resource module and accounts for native children.
bool ReadContent(IHTMLDocument2* document, HWND window, const RECT& viewport,
    NativeDocumentSnapshot& snapshot, std::wstring& reason) noexcept;
bool InvokeItem(IHTMLDocument2* document, const NativeDocumentSnapshot& expected,
    int index, std::wstring& reason) noexcept;
bool Scroll(IHTMLDocument2* document, const NativeDocumentSnapshot& expected,
    int regionIndex, int x, int y, std::wstring& reason) noexcept;
}
}
