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
// Minor 20 adds a live island popup-menu snapshot and token-scoped popupCommand.
// Minor 21 carries only referenced item icons, bounded by decoded bytes as well
// as the maximum number of normal and selected-state item references.
// It also adds inert staticDecoration nodes for built-in Static frames and fills.
// Minor 22 adds MSAA page-tab islands with canonical selection state.
// Minor 23 adds positioned ListView modes, revision-gated item activation, and toolbar radio groups.
// Minor 24 separates ListView focus from selection and scrolls non-report views natively.
// Minor 25 projects TreeView state images beside the item. Overlay images are
// composited into the existing item icons, and an empty tree is a valid snapshot.
// Minor 26 carries stable native ListView item IDs so delayed activation cannot
// target a replacement item with the same label and index.
// Minor 27 adds inert heading and painted image island items (MMC message views).
// Minor 28 adds the staticBitmap kind (SS_BITMAP pictures under the large-bitmap cap)
// and the upDown kind (msctls_updown32 stepped through setValue); a BS_ICON/BS_BITMAP
// push button may carry its picture in the image fields.
// Minor 29 retires exact application pages in favor of capability-derived DirectUI
// semantics and adds owned toolbar hot, disabled, and pressed image variants.
// Minor 30 adds native documents, owner-draw ComboBox faces, and ListView groups.
// Minor 31 adds the native Disk Management MSAA table and its semantic actions.
// Minor 32 adds the contextMenu action for TreeView and ListView items.
// Minor 33 carries a large virtual report ListView as a window of rows (itemCount,
// itemWindowStart) with absolute indices, and adds setTopIndex to move that window.
// Minor 34 lets a ListView or TreeView offer itemDefaultActivation and adds the openItem
// action: a double-click selects the item and sends its control the Enter key.
// Minor 35 adds the richText kind: a read-only rich edit control carried as paragraphs of
// formatted runs (bold, italic, underline, strike-through, size in points, colour).
// Minor 36 adds Performance Monitor's chart as three kinds read from the System Monitor control:
// performanceGraph (scale, grid, counters and their recorded samples), performanceLegend (one
// row per counter) and performanceValueBar (Last, Average, Minimum, Maximum, Duration).
// Minor 37 adds the datePicker and timePicker kinds (SysDateTimePick32): the value is the
// position of a range, as for a slider -- a date's day number (days since 0001-01-01), a
// time's second of the day -- and setValue changes it.
// Minor 38 completes the translated dialogs: a task dialog's nodes may carry presentation
// variants (mainInstruction, commandLink*, elevated, expando*) and a command link's note as
// helpText, and a task dialog's text is a sysLink with one label per link, named by index
// through activateItem.
// Minor 39 adds canClose to a window (its title-bar close button does something, which an
// ordinary window's does although Escape does not) and expanded to an accessible island
// item (a section header that folds the rows under it, as in MMC's Actions pane).
// Minor 40 adds the focus action: the user put the keyboard focus in a projected list or
// tree, and the thread's native focus follows it (menu commands act on the focused pane).
// It also adds a toolbar item's labelBeside (a TBSTYLE_LIST button draws its own label
// beside its icon) and an edit box's cueBanner (the grey hint it shows while empty).
// Minor 41 adds the tile ListView mode, whose items carry itemDetails: the lines a tile
// shows under its label (the Sound applet's devices), and the splitButton variant of a
// button, whose arrow is the splitDropDown action (BCN_DROPDOWN to its owner).  A
// TVS_CHECKBOXES tree carries checkBoxes and checkedIndices, and setItemCheck toggles one;
// an owner-draw ListBox carries ownerDrawList, the rows as its owner paints them, and an owner-draw
// push button's picture is its whole face, up to 512 by 128.  A SysLink label that also occurs
// elsewhere in its text carries linkStarts.
inline constexpr uint16_t kProtocolMinor = 41;
inline constexpr uint32_t kMaxPayloadBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxJsonDepth = 32;
inline constexpr size_t kMaxStringChars = 65536;
inline constexpr size_t kMaxNodes = 512;
inline constexpr size_t kMaxListItems = 4096;
// A windowed virtual ListView may count this many rows; its window and every other
// per-row array stay under kMaxListItems.
inline constexpr size_t kMaxVirtualListItems = 16777216;
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
inline constexpr size_t kMaxMenuItems = 256;
inline constexpr size_t kMaxMenuDepth = 8;
inline constexpr size_t kMaxMenuItemPathChars = 64;
// A container pane carries one entry per splitter between its child panes.  A
// window with more than a handful of independently sized panes is not a frame the
// projection can describe, so the cap is deliberately small.
inline constexpr size_t kMaxPaneSplits = 8;
// A scrolling container has at most its two standard window scroll bars, and one
// setScroll request moves at most this many application line steps.
inline constexpr size_t kMaxScrollBars = 2;
inline constexpr int kMaxScrollLines = 100;
// Split positions are client coordinates, bounded by the same limit the protocol
// already applies to window and control geometry.
inline constexpr int kMaxCoordinate = 65535;
// Only referenced native icons travel, shared by all items in the node. A tree
// has at most two references per item. The decoded-byte budget retains the old
// worst case of sixty-four 64x64 icons while admitting more small icons; the
// complete serialized snapshot must still fit the 4 MiB frame.
inline constexpr size_t kMaxImageListImages = kMaxListItems * 2;
inline constexpr uint32_t kMaxImageListDimension = 64;
inline constexpr size_t kMaxImageListBytes = 1024u * 1024u;
inline constexpr uint32_t kMaxImageDimension = 96;
// An owner-draw push button's whole face (minor 41), under the owner-draw combo face bound.
inline constexpr uint32_t kMaxOwnerDrawFaceWidth = 512;
inline constexpr uint32_t kMaxOwnerDrawFaceHeight = 128;
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
