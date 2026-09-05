#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FluentShell::Bridge::Ipc {

inline constexpr uint32_t kFrameMagic = 0x48534C46u; // "FLSH" in little endian.
inline constexpr uint16_t kProtocolMajor = 1;
// Writers emit this minor; same-major readers accept any uint16 minor and ignore unknown JSON fields.
// Minor 1 requires the renderer-side provisional interaction gate. Minor 2
// adds structural DS_CONTROL dialogContainer nodes and parent graph semantics.
// Minor 3 adds bounded, owned staticIcon BGRA payloads. Minor 4 adds explicit
// report ListView column-header visibility. Minor 5 adds bounded report
// ListView checkbox state and mutation. Minor 6 adds bounded textual TabControl
// headers, native item geometry, and semantic selection. Minor 7 adds bounded
// ToolbarWindow32 push buttons, separators, owned icons, and command actions.
// Minor 8 adds exact application-adapter metadata, nullable virtual node HWNDs,
// semantic presentation, and supported-action declarations for the initial
// MdSched page. Minor 9 adds the second DirectUI application profile
// (RecoveryDrive first page) with projected native checkbox toggles. Minor 10
// adds its native-backed explanatory text. Minor 11 adds the fail-closed
// capability-derived DirectUI semantic adapter contract. Minor 12 adds
// explicit determinate/marquee ProgressBar state. Minor 13 adds bounded owned
// DirectUI bitmap-display and bitmap-switch pixels. Minor 14 admits every UIA
// control type the Win32 adapter registry can back on a capability-derived
// DirectUI surface, and lets one projected node advertise two in-place routes.
// Minor 15 adds the bounded textual SysTreeView32 and msctls_trackbar32
// adapters: treeView/slider kinds, per-item depth/expansion/child evidence, and
// the setValue and setExpand routes. Minor 16 adds projected MDI frames: the
// mdiClient and mdiChild container kinds, per-child caption state and client
// geometry, and the mdiCommand route. Minor 17 adds per-item icons through a
// bounded shared image list, and in-place item renaming through the native
// control's own label-edit session.
// A Bridge
// Must not pair with a minor-0 renderer because that peer would treat the
// pre-UIA provisional commit as interactive.
inline constexpr uint16_t kProtocolMinor = 19;
inline constexpr uint32_t kMaxPayloadBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxJsonDepth = 32;
inline constexpr size_t kMaxStringChars = 65536;
inline constexpr size_t kMaxNodes = 512;
inline constexpr size_t kMaxListItems = 4096;
inline constexpr size_t kMaxTabItems = 128;
// itemDepths carries the nesting level of a projected TreeView item, so the
// deepest admissible level is one less than the level count.
inline constexpr int kMaxTreeDepth = 31;
inline constexpr size_t kMaxToolbarItems = 64;
// A report ListView carries at most this many columns, which also bounds the
// display-order permutation a projected header reorder can request.
inline constexpr size_t kMaxColumns = 64;
// A container pane reproduces the bands it paints itself as bounded BGRA pixels.
// The caps keep a caption strip or separator rule reproducible while refusing a
// window that is really a custom-drawn control wearing a container's shape.
inline constexpr size_t kMaxChromeRegions = 4;
inline constexpr uint32_t kMaxChromeRegionDimension = 1024;
inline constexpr size_t kMaxChromeRegionBytes = 1024 * 1024;
// An accessible island publishes its HWND-less elements as typed items on its own
// node, the way a Toolbar publishes its buttons.  A window with more elements than
// this is not a bounded island the projection can describe.
inline constexpr size_t kMaxIslandItems = 32;
// A container pane carries one entry per splitter between its child panes.  A
// window with more than a handful of independently sized panes is not a frame the
// projection can describe, so the cap is deliberately small.
inline constexpr size_t kMaxPaneSplits = 8;
// Split positions are client coordinates, bounded by the same limit the protocol
// already applies to window and control geometry.
inline constexpr int kMaxCoordinate = 65535;
// A control's own image list travels once per node and every item indexes into
// it.  Both caps are deliberate: sixty-four icons at 64x64 is a megabyte of
// pixels, which leaves the rest of the 4 MiB frame to the items themselves.
inline constexpr size_t kMaxImageListImages = 64;
inline constexpr uint32_t kMaxImageListDimension = 64;
inline constexpr uint32_t kMaxImageDimension = 96;
inline constexpr size_t kMaxImageBytes =
    static_cast<size_t>(kMaxImageDimension) * kMaxImageDimension * 4;
inline constexpr uint32_t kMaxDirectUiBitmapDimension = 1024;
inline constexpr size_t kMaxDirectUiBitmapBytes = 2u * 1024u * 1024u;
inline constexpr size_t kMaxDirectUiBitmapBase64Chars =
    ((kMaxDirectUiBitmapBytes + 2) / 3) * 4;
inline constexpr size_t kMaxJsonStringChars = kMaxDirectUiBitmapBase64Chars;

enum class MessageType : uint16_t {
    Hello = 1,
    WindowOpen = 2,
    WindowPatch = 3,
    ActionInvoke = 4,
    ActionResult = 5,
    SurfaceReady = 6,
    SurfaceCommit = 7,
    WindowClose = 8,
    Heartbeat = 9,
    Error = 10,
    Shutdown = 11,
};

#pragma pack(push, 1)
struct FrameHeader final {
    uint32_t magic = kFrameMagic;
    uint16_t major = kProtocolMajor;
    uint16_t minor = kProtocolMinor;
    uint16_t type = 0;
    uint16_t flags = 0;
    uint32_t payloadLength = 0;
    uint64_t sequence = 0;
    uint64_t revision = 0;
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 32, "FLSH frame header must remain 32 bytes");

struct Frame final {
    FrameHeader header{};
    std::string payload;
};

bool IsKnownMessageType(uint16_t value) noexcept;
bool ValidateHeader(const FrameHeader& header, uint64_t previousSequence, std::wstring& error) noexcept;
std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
std::wstring HwndToString(HWND hwnd);
bool TryParseHwnd(std::wstring_view value, HWND& hwnd) noexcept;
std::wstring UInt64ToString(uint64_t value);
bool TryParseUInt64(std::wstring_view value, uint64_t& result) noexcept;
std::wstring NewGuidString();
std::wstring NewNonceHex();
uint64_t ProcessCreationTime(HANDLE process) noexcept;

} // namespace FluentShell::Bridge::Ipc
