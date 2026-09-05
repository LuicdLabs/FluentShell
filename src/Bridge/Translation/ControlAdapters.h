#pragma once

#include "WindowSnapshot.h"

#include <commctrl.h>

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

// The HTREEITEM at `index` in the same depth-first order the TreeView adapter
// flattens its items into, or null when the index is outside that order.  Every
// index-addressed TreeView action resolves its target through this walk, so a
// projected index can never mean one item to capture and another to an action.
HTREEITEM ResolveTreeViewItem(HWND treeView, int index) noexcept;

// Moves one of a container's splits to `position`, in the container's own client
// coordinates, by resizing exactly the two panes it divides.  The split index is
// resolved against the container's current geometry rather than a snapshot, because
// a drag arrives while the panes may already have moved.  Returns false when the
// index no longer names a split or the position is outside its range.
bool SetPaneSplit(HWND container, int index, int position) noexcept;

// Where a split measured at one container extent belongs at another, clamped to the
// range the container currently offers.  A container keeps its own proportion in
// private data that no message writes, so this is how a request the user made at one
// size is expressed again after the application has re-laid the container out.
int ProportionalSplitTarget(
    int position,
    int fromExtent,
    int toExtent,
    int minimum,
    int maximum) noexcept;

// Sets a report ListView's column display order through the control's own order
// array, which is the same state the native header writes when the user drags a
// column.  `order` must be a permutation of the column indexes.
bool SetListViewColumnOrder(HWND listView, const std::vector<int>& order) noexcept;

// Copies an HICON into bounded, owned premultiplied BGRA pixels. Application
// adapters may use this only after independently establishing a trusted source.
bool CaptureOwnedIconPixels(
    HICON icon,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason);

bool CaptureOwnedWindowPixels(
    HWND hwnd,
    uint32_t maxDimension,
    size_t maxBytes,
    uint32_t& imageWidth,
    uint32_t& imageHeight,
    std::wstring& imageFormat,
    std::vector<uint8_t>& imageData,
    std::wstring& reason);

} // namespace FluentShell::Bridge::Translation
