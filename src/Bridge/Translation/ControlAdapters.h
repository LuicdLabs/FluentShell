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

// MMC's wait-cursor window: a transparent, unpainted child mmcndmgr.dll raises over
// the frame's client area while a snap-in holds the console busy.  It is absorbed
// (never a node), and while one is visible the session declines the frame's actions.
bool IsMmcBusyShield(HWND hwnd) noexcept;
bool HasVisibleMmcBusyShield(HWND root) noexcept;

// A window that a visible, opaque sibling above it covers except for edges no wider
// than a few pixels (MMC's result ListView over Print Management's HTML host).  It
// and everything inside it show only that edge, so none of it is captured.
bool ObscuredBySiblingAbove(HWND window) noexcept;
bool InsideObscuredHost(HWND root, HWND window) noexcept;

// A Windows Forms LinkLabel projects as a one-link SysLink.  It is activated through
// its link's accessible default action (which raises LinkClicked) rather than through
// SysLink messages it does not implement; the call fails closed unless the label
// still publishes exactly that one enabled link.  Runs the application's handler, so
// it belongs on the deferred native-action path, never inside a bounded command.
bool IsWindowsFormsLinkLabelWindow(HWND hwnd) noexcept;

// A ComboBoxEx (WC_COMBOBOXEX): a comboBox node whose typed text lives in an edit box of its own.
bool IsComboBoxExWindow(HWND hwnd) noexcept;

// What a dialog (#32770) paints itself in a full-width band above or below all of its
// controls, such as mstsc's banner.  A band it only fills with its background is left out;
// so is one past the chrome pixel caps.  False only when the dialog could not be rendered.
bool CaptureDialogPaintedBands(HWND root, std::vector<ChromeRegionSnapshot>& bands, std::wstring& reason);

// A window of a Windows Forms class.  Windows Forms moves focus with Tab inside its
// own containers, so a WS_TABSTOP control there is a tab stop even where the dialog
// manager's walk from the native root never reaches it (an MMC 3.0 snap-in view).
bool IsWindowsFormsControl(HWND hwnd) noexcept;
bool InvokeWindowsFormsLink(HWND linkLabel, const std::wstring& expectedName) noexcept;

// The arrows of a Windows Forms NumericUpDown (UpDownBase.UpDownButtons): an accessible spin
// box with an up and a down push button and no range of its own. The number beside it, in
// the sibling edit, is the control's value. ReadWindowsFormsSpin refuses a window that is not
// such a pair or whose number is not an integer; StepWindowsFormsSpin gives it the press a
// pointer gives (the upper half steps up, the lower half down). GUI thread of the control only.
struct WindowsFormsSpin final {
    HWND edit = nullptr;
    int value = 0;
};
bool ReadWindowsFormsSpin(HWND hwnd, WindowsFormsSpin& spin, std::wstring& reason) noexcept;
bool StepWindowsFormsSpin(HWND buttons, bool up) noexcept;

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
bool SetListViewGroupExpanded(HWND listView, int groupId, bool expanded) noexcept;

// Reads an owned, bounded cell through the native control on its GUI thread.
// Owner-data lists obtain their text through LVN_GETDISPINFO, not GETITEMTEXT.
bool ReadListViewItemText(HWND listView, int row, int column,
    std::wstring& text, std::wstring& reason);

// Moves only LVIS_FOCUSED. Selection is untouched. -1 clears focus; a nonnegative
// index also asks the control to bring that item into its own viewport.
bool SetListViewFocusedIndex(HWND listView, int index) noexcept;

// Scrolls a container that moves its own child windows, through its WM_VSCROLL/
// WM_HSCROLL line and page handling: a signed count of line steps, or toward a
// target position (page steps while far, then line steps). True when it moved.
bool ScrollPaneContainer(HWND container, bool vertical, bool byLines, int amount) noexcept;

// BM_GETCHECK for a standard button; for an owner-draw WinForms CheckBox or
// RadioButton (which keeps no native check state) the state its MSAA object
// reports. -1 when an owner-draw button offers no such evidence. GUI thread only.
int ReadButtonCheckState(HWND button) noexcept;

// How a request reaches an item's control once the item is selected. Most controls answer
// the keyboard form of WM_CONTEXTMENU. Some (Device Manager's tree, an MFC control)
// ignore a synthesized one and open their menu from the Menu key itself. EnterKey is the
// activation of the item (what a double-click does), and DoubleClick is the pointer's own
// double-click at the item for a control whose application listens to DoubleClick rather
// than to the activation notification (a Windows Forms list: Task Scheduler's tasks).
enum class ContextMenuTrigger { ContextMenuMessage, MenuKey, EnterKey, DoubleClick };

// Opens the application's own context menu for one TreeView or ListView item: the
// item (whose label must still be `expectedText`) is selected through the control's
// selection messages, then the menu is requested by `trigger`. The caller arms popup
// interception first; the application's TrackPopupMenu runs inside this.
bool RequestItemContextMenu(HWND control, ControlKind kind, int index,
    const std::wstring& expectedText, std::wstring& reason,
    ContextMenuTrigger trigger = ContextMenuTrigger::ContextMenuMessage) noexcept;

// Makes `indices` (ascending, without repeats) exactly the selection of a multi-select list box,
// through the control's own selection messages, and reads back what it settled on: false when the
// control is not a multi-select list box, an index is outside it, or it kept another selection.
// The owner is not told (that is LBN_SELCHANGE, which the caller sends as a click would).
bool SetListBoxSelection(HWND listBox, const std::vector<int>& indices) noexcept;

// Asks the owner of a toolbar button's arrow for its drop-down menu: the toolbar's own click on
// the arrow sends its parent TBN_DROPDOWN for the button, and the owner answers by tracking a
// menu. The button (position and command id) must still be an enabled drop-down. The caller
// arms popup interception first; the owner's TrackPopupMenu runs inside this.
bool RequestToolbarDropDown(HWND toolbar, int index, uint32_t commandId, std::wstring& reason) noexcept;
// A split button's arrow: its owner is sent the BCN_DROPDOWN the arrow sends, and tracks a menu.
bool RequestSplitButtonDropDown(HWND button, std::wstring& reason) noexcept;

// Activates the item the way its control answers a double-click: the item is selected and the
// Enter key is delivered to the control (MMC lists and snap-in trees open the item's default
// action; Services opens its properties). A Windows Forms list is given the double-click
// itself. The label is the witness, as for the context menu.
bool RequestItemKeyActivation(HWND control, ControlKind kind, int index,
    const std::wstring& expectedText, std::wstring& reason) noexcept;

// Scrolls a non-report ListView by a pixel delta through LVM_SCROLL. Report views
// are refused here because their projection scrolls inside the WinUI presenter.
bool ScrollListViewBy(HWND listView, int dx, int dy) noexcept;

// Scrolls a report list so the row at `index` is its top row (or as close as the
// list's own scroll range allows): how a windowed virtual list moves its window.
bool ScrollListViewToTop(HWND listView, int index) noexcept;

// Resolves a stable native ListView ID and verifies its reverse mapping. UINT_MAX
// and IDs whose item was removed are invalid; ID zero is a valid native identity.
int ResolveListViewItemByNativeId(HWND listView, uint32_t nativeId) noexcept;

// Applies the native check-state transition before a toolbar command reaches its
// owner: CHECK toggles, CHECKGROUP selects one member, and ordinary buttons keep
// application-owned latches. Rejects stale command identities or group boundaries.
bool ApplyToolbarCheckState(
    HWND toolbar, const ControlNode& node, uint32_t commandId, std::wstring& reason);

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
