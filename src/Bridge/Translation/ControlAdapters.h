#pragma once

#include "WindowSnapshot.h"

#include <string>
#include <string_view>

namespace FluentShell::Bridge::Translation {

// The bounded Win32 control adapter registry.
//
// docs/goals/win32-to-winui-translation/CONTROL-ADAPTER-ROADMAP.md defines each
// adapter as owning a complete contract.  This header exposes the two stages
// WindowCapture drives on the owning UI thread:
//
//   Match(class) + Probe(styles) -> ClassifyControl
//   Capture(HWND)                -> CaptureControlDetail
//
// Adding a control therefore means adding one registry row plus its two
// functions, never editing a chain of class or kind comparisons.

// Longest class name Win32 reports plus its terminator.
inline constexpr size_t kMaxClassNameChars = 256;

// Non-allocating class-name read.  Capture resolves the class of every child
// (and of some parents) more than once per pass, so this stays off the heap.
// The returned view is valid while `buffer` is in scope.
std::wstring_view ClassNameOf(HWND hwnd, wchar_t (&buffer)[kMaxClassNameChars]) noexcept;

// Resolves the adapter for a visible child HWND, or explains in `reason` why it
// sits outside the adapter boundary.  A rejection makes the whole window fall
// back to native; adapters must reject anything they cannot prove equivalent.
bool ClassifyControl(HWND hwnd, ControlKind& kind, std::wstring& reason);

// Reads the canonical state specific to `node.kind`.  WindowCapture has already
// filled the facets every control shares (identity, geometry, styles, text, tab
// order); kinds that add nothing succeed without touching the control.
bool CaptureControlDetail(HWND hwnd, ControlNode& node, std::wstring& reason);

// True for the implementation children a composite control owns: a ComboBox's
// edit and dropdown list, a ListView's header.  These are part of their owner's
// adapter contract and are never separate projected nodes.
bool IsCompositeImplementationChild(HWND hwnd) noexcept;

// Copies an HICON into bounded, owned premultiplied BGRA pixels. Application
// adapters may use this only after independently establishing a trusted source.
bool CaptureOwnedIconPixels(
    HICON icon,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason);

} // namespace FluentShell::Bridge::Translation
