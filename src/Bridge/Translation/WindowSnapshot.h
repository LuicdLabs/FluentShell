#pragma once

#include "../Ipc/Protocol.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace FluentShell::Bridge::Translation {

enum class SurfaceKind {
    Window,
    MessageBox,
    TaskDialog,
};

enum class ControlKind {
    StaticText,
    StaticIcon,
    Separator,
    Button,
    CheckBox,
    ThreeState,
    RadioButton,
    Edit,
    Password,
    ComboBox,
    ListBox,
    GroupBox,
    ProgressBar,
    SysLink,
    ListView,
    TreeView,
    TabControl,
    Slider,
    DialogContainer,
    MdiClient,
    MdiChild,
    StatusBar,
    Toolbar,
    PaneContainer,
    AccessibleIsland,
    StaticDecoration,
    StaticBitmap,
    UpDown,
    NativeDocument,
    DiskView,
    RichText,
    PerformanceGraph,
    PerformanceLegend,
    PerformanceValueBar,
    // SysDateTimePick32.  The value travels in the range fields every node already
    // carries: a date picker's position is its day number (days since 0001-01-01), a time
    // picker's the second of the day, both stepped through setValue.
    DatePicker,
    TimePicker,
    Count,
};

enum class ToolbarItemKind {
    PushButton,
    Separator,
    // A BTNS_CHECK button keeps its pressed look between clicks, so the projection
    // draws a toggle and carries the state the control owns.
    ToggleButton,
    // A contiguous BTNS_CHECKGROUP run selects at most one native member.
    RadioButton,
};

// One gap between two panes of a container, which the projection renders as a real
// splitter.  `position` is the gap's leading edge in the container's own client
// coordinates, and `minimum`/`maximum` bound where the split may be moved before a
// pane would collapse.
struct PaneSplitSnapshot final {
    bool vertical = false;
    int position = 0;
    int thickness = 0;
    int minimum = 0;
    int maximum = 0;
};

// A standard window scroll bar of a container whose application scrolls its child
// windows (a WinForms AutoScroll panel). The rect is in the node's own window-
// relative coordinates; position is the application's, moved only through its own
// WM_VSCROLL/WM_HSCROLL handling.
struct ScrollBarSnapshot final {
    bool vertical = true;
    bool enabled = true;
    int minimum = 0;
    int maximum = 0;
    int page = 0;
    int position = 0;
    RECT rect{};
};

// One rectangle of a container's client area that no child covers and that is too
// thick to be a splitter gap: a band the container paints itself.  The pixels travel
// with it so the projection reproduces exactly what the native window drew, and they
// are part of the snapshot fingerprint so a repaint reaches the renderer as a patch.
struct ChromeRegionSnapshot final {
    RECT rect{};
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
};

// One element of an accessible island: content the host window owns without giving it
// an HWND, read through the accessibility contract the window answers.
struct AccessibleIslandItemSnapshot final {
    std::wstring kind;
    RECT rect{};
    std::wstring name;
    std::wstring description;
    std::wstring actionName;
    bool enabled = true;
    bool dropDown = false;
    bool selected = false;
    // A section header that folds the rows under it: open or folded.  Unset for any
    // other item.
    std::optional<bool> expanded;
    // Only an "image" item carries pixels: the region the host painted for an element
    // its provider describes as a graphic.
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
};

// An owned icon from a native image list. Item-bearing controls address shared
// entries by projection index; toolbar state variants carry one optional entry.
struct ImageListEntry final {
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
};

// A DOM scrollport or the standard native scrollbar of an embedded report.
// Coordinates remain physical pixels; native report ranges remain native units.
struct NativeDocumentRegion final {
    std::wstring key;
    std::wstring kind;
    RECT rect{};
    int contentWidth = 0;
    int contentHeight = 0;
    int maximumX = 0;
    int maximumY = 0;
    int offsetX = 0;
    int offsetY = 0;
};

struct NativeReportRow final {
    std::wstring path;
    double value = 0;
    bool available = false;
};

struct NativeDocumentItem final {
    std::wstring key;
    std::wstring kind;
    RECT rect{};
    int regionIndex = -1;
    int reportRegionIndex = -1;
    std::wstring name;
    std::wstring actionIdentity;
    bool enabled = true;
    std::optional<ImageListEntry> image;
    std::vector<NativeReportRow> reportRows;
};

struct NativeDocumentSnapshot final {
    std::wstring documentId;
    std::vector<NativeDocumentRegion> regions;
    std::vector<NativeDocumentItem> items;
};

struct OwnerDrawComboItemSnapshot final {
    ImageListEntry normal;
    ImageListEntry selected;
};

// The Windows Disk Management graphical view's own MSAA table. IDs are native
// positive child IDs, never disk numbers or indexes into an independent query.
struct DiskViewItemSnapshot final {
    uint32_t itemId = 0;
    uint32_t diskId = 0;
    std::wstring kind;
    RECT rect{};
    std::wstring name;
    std::wstring value;
    bool enabled = true;
    bool selected = false;
    bool focused = false;
};

struct DiskViewSnapshot final {
    std::wstring name;
    RECT clientRect{};
    RECT scrollRect{};
    int scrollMinimum = 0;
    int scrollMaximum = 0;
    int scrollPosition = 0;
    int scrollPage = 0;
    std::vector<DiskViewItemSnapshot> items;
};

// One run of a read-only rich edit control's text: a constant character format. Sizes are in
// points; the colour is 0xRRGGBB, or -1 for the control's automatic text colour.
struct RichRunSnapshot final {
    std::wstring text;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    float size = 0;
    int64_t color = -1;
};

// A paragraph of runs. Indents and spacing are in points; the alignment names its edge.
struct RichParagraphSnapshot final {
    std::wstring alignment = L"left";
    bool bullet = false;
    float leftIndent = 0;
    float firstLineIndent = 0;
    float spaceBefore = 0;
    float spaceAfter = 0;
    std::vector<RichRunSnapshot> runs;
};

// One counter's line on Performance Monitor's graph. `values` has one entry per graph slot, in
// drawing order, and none where the counter had no sample for that slot.
struct PerformanceSeriesSnapshot final {
    std::wstring name;
    // 0xRRGGBB.
    uint32_t color = 0;
    int width = 1;
    std::wstring style = L"solid";
    // The factor the control multiplies a value by before it plots it.
    double scale = 1;
    bool visible = true;
    bool highlighted = false;
    std::vector<std::optional<double>> values;
};

// The System Monitor control's chart as data: its scale, grid and counters, and the samples the
// Bridge was told about. The slots lay the samples out as the control does: wrapping (a time bar
// sweeps across and old samples remain to its right until overwritten) or scrolling.
struct PerformanceGraphSnapshot final {
    std::wstring displayType = L"line";
    double minimum = 0;
    double maximum = 100;
    int sampleCount = 100;
    int intervalMs = 1000;
    bool scroll = false;
    // Wrapping charts only: the slot of the newest sample, where the time bar stands; -1 otherwise.
    int barSlot = -1;
    // A wrapping chart that has written past its last slot: the slots after the bar hold the
    // previous pass's samples, so they are older by one pass than the slots before it.
    bool wrapped = false;
    // The time of slot 0's sample (in the pass in progress) and of the last slot's, in
    // milliseconds since the Unix epoch. The axis labels the time of the sample each slot holds.
    int64_t startTimeMs = 0;
    int64_t endTimeMs = 0;
    bool showVerticalGrid = false;
    bool showHorizontalGrid = false;
    bool showScaleLabels = true;
    bool showTimeLabels = true;
    std::wstring title;
    std::wstring yAxisLabel;
    // The plot's own background, and the control's behind the labels around it.
    uint32_t backColor = 0xFFFFFF;
    uint32_t controlColor = 0xF0F0F0;
    uint32_t gridColor = 0x808080;
    uint32_t textColor = 0;
    uint32_t barColor = 0xFF0000;
    std::vector<PerformanceSeriesSnapshot> series;
};

struct PerformanceLegendColumnSnapshot final {
    std::wstring title;
    int width = 0;
};

struct PerformanceLegendRowSnapshot final {
    bool visible = true;
    uint32_t color = 0;
    int width = 1;
    std::wstring style = L"solid";
    bool selected = false;
    // The cells after Show and Color: one per remaining column.
    std::vector<std::wstring> cells;
};

// The legend under the graph: one row per counter, in the columns the native legend has. Its
// swatches follow the chart's type: a line for a line graph, a solid bar for a histogram.
struct PerformanceLegendSnapshot final {
    std::wstring displayType = L"line";
    std::vector<PerformanceLegendColumnSnapshot> columns;
    std::vector<PerformanceLegendRowSnapshot> rows;
};

struct PerformanceValueFieldSnapshot final {
    std::wstring label;
    std::wstring value;
};

// The strip under the graph: Last, Average, Minimum, Maximum and Duration of the selected counter.
struct PerformanceValueBarSnapshot final {
    std::vector<PerformanceValueFieldSnapshot> fields;
};

struct OwnerDrawComboSnapshot final {
    std::wstring mode;
    std::vector<OwnerDrawComboItemSnapshot> items;
    // A dropdown-list selection field gets ODS_COMBOBOXEDIT, including itemID
    // UINT(-1) when empty. Editable combos keep the native text editor instead.
    std::optional<ImageListEntry> selectionFace;
};

struct ToolbarItemSnapshot final {
    ToolbarItemKind kind = ToolbarItemKind::PushButton;
    uint32_t commandId = 0;
    RECT rect{};
    std::wstring text;
    bool enabled = true;
    bool hidden = false;
    bool checked = false;
    // One-based contiguous native CHECKGROUP run, including hidden members.
    // Zero belongs to every non-radio item and never joins a radio group.
    int radioGroup = 0;
    // BTNS_DROPDOWN: the button carries an arrow that asks its owner for a menu
    // through TBN_DROPDOWN.  With BTNS_WHOLEDROPDOWN the whole button does that and
    // there is no separate command click at all.
    bool dropDown = false;
    bool wholeDropDown = false;
    // The control draws this button's face itself, so the projection carries the pixels
    // it drew rather than an image-list icon it does not own.
    bool paintedFace = false;
    // A TBSTYLE_LIST toolbar draws this button's own label beside its icon (mstsc's
    // "Show Options").  Without it the text is only the button's name: an icon-only button
    // names itself from its tooltip, which the control never draws on the face.
    bool labelBeside = false;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
    std::optional<ImageListEntry> hotImage;
    std::optional<ImageListEntry> disabledImage;
    std::optional<ImageListEntry> pressedImage;
};

struct ListViewGroupSnapshot final {
    int groupId = 0;
    std::wstring header;
    std::wstring footer;
    std::wstring headerAlignment = L"left";
    std::wstring footerAlignment = L"left";
    bool collapsible = false;
    bool collapsed = false;
    bool hidden = false;
    RECT headerRect{};
    RECT footerRect{};
};

struct ControlNode final {
    uint64_t nodeId = 0;
    uint64_t generation = 0;
    HWND hwnd = nullptr;
    std::optional<uint64_t> parentNodeId;
    ControlKind kind = ControlKind::StaticText;
    int controlId = 0;
    int zIndex = 0;
    // Native dialog-manager order among currently effective tab stops. A
    // value of -1 means the control is not currently keyboard-focusable.
    int tabIndex = -1;
    RECT rect{};
    uint64_t style = 0;
    uint64_t exStyle = 0;
    bool visible = true;
    bool enabled = true;
    // Effective dialog traversal state. The raw WS_TABSTOP bit remains in
    // style even when the native dialog manager does not expose this control.
    bool tabStop = false;
    uint32_t dialogCode = 0;
    std::wstring text;
    std::wstring automationName;
    int checked = 0;
    int selectedIndex = -1;
    std::vector<int> selectedIndices;
    int focusedIndex = -1;
    bool multiSelect = false;
    int selectionStart = 0;
    int selectionLength = 0;
    bool readOnly = false;
    bool multiline = false;
    bool editable = false;
    // The grey hint an empty edit box shows (EM_GETCUEBANNER, CB_GETCUEBANNER): mstsc's
    // "Example: computer.fabrikam.com".  Edit, password, and editable combo boxes only.
    std::wstring cueBanner;
    bool isDefault = false;
    bool groupStart = false;
    // MDI child frames only: whether this is the MDI client's active child and
    // which caption state it is in.  A projected MDI child is a window inside a
    // window, so it carries the same three states a top-level surface does.
    bool active = false;
    std::wstring windowState;
    // MDI child frames only: the client area in the child's own window-relative
    // coordinates, so a projected caption can occupy exactly the band the native
    // frame occupies and the child's controls keep their native offsets.
    RECT clientRect{};
    int minimum = 0;
    int maximum = 100;
    int position = 0;
    bool indeterminate = false;
    int smallChange = 1;
    int largeChange = 10;
    bool vertical = false;
    bool reversed = false;
    std::vector<std::wstring> items;
    std::optional<OwnerDrawComboSnapshot> ownerDrawCombo;
    // ListView presentation mode. Report remains the default for old snapshots;
    // icon/list modes carry item labels and geometry without synthetic columns.
    std::wstring listViewMode = L"report";
    bool groupViewEnabled = false;
    // Native display order; row membership keeps the canonical item sequence.
    std::vector<ListViewGroupSnapshot> listViewGroups;
    std::vector<int> itemGroupIds;
    // TCM_GETITEMRECT / non-report LVM_GETITEMRECT results in client-local physical
    // pixels. List items may lie outside the current viewport after scrolling.
    std::vector<RECT> itemRects;
    // Stable ListView identities, sent as canonical decimal strings. An empty
    // vector means the control has no stable identity contract (e.g. owner data).
    // Delayed selection/activation must never be redirected to a replacement row.
    std::vector<uint32_t> itemNativeIds;
    // True only when stable item IDs and the native accessible default action
    // both exist. Older peers and controls without that capability expose false.
    bool itemActivationSupported = false;
    // The projection may open an item (double-click, Enter) with the openItem action: the
    // item is selected and its control is sent Enter. Offered for MMC result lists that have
    // no stable-identity activation contract (virtual lists) and for MMC result trees
    // (Device Manager's devices); never for the MMC scope tree, where a double-click only expands.
    bool itemDefaultActivation = false;
    std::vector<std::wstring> columns;
    std::vector<int> columnWidths;
    // Report ListView only: the display order of the columns as a permutation of
    // their logical indexes.  Columns, widths, and cells all travel in logical
    // order, so nothing on the wire changes meaning when the user reorders them.
    std::vector<int> columnOrder;
    std::vector<std::vector<std::wstring>> rows;
    // A large virtual report list carries a window of its rows: `items` and `rows`
    // hold the rows from itemWindowStart on, and itemCount is the list's full length.
    // Zero means the node carries every row.  Every index on the wire (selection,
    // focus, actions) stays absolute.
    size_t itemCount = 0;
    size_t itemWindowStart = 0;
    bool columnHeadersVisible = false;
    bool checkBoxes = false;
    std::vector<int> checkedIndices;
    std::vector<int> itemDepths;
    std::vector<bool> itemExpanded;
    // Whether a TreeView item owns children at all.  A lazily populated tree
    // reports this before its children exist, which is what lets a projected
    // expander reach a subtree the application has not inserted yet.
    std::vector<bool> itemHasChildren;
    // Referenced native icons and the per-item indexes into this compact snapshot
    // list, rebuilt on each capture. -1 means the item draws no icon.
    // itemSelectedImages is the tree's separate selected-state index, which is
    // how a folder opens and closes natively.
    std::vector<ImageListEntry> imageList;
    std::vector<int> itemImages;
    std::vector<int> itemSelectedImages;
    // TreeView only. State images are a second list drawn beside the item icon.
    // -1 means the item draws none. Overlay images are not stored here: capture
    // composites them into the normal and selected icons they decorate.
    std::vector<ImageListEntry> stateImageList;
    std::vector<int> itemStateImages;
    // The control advertises in-place label editing, so the projection offers a
    // rename that runs through the native control's own edit session.
    bool editableLabels = false;
    // The item whose native label-edit session is open, or -1.
    int editingIndex = -1;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
    std::vector<ToolbarItemSnapshot> toolbarItems;
    // Container panes only: the splitters between its child panes, and the bands the
    // container paints itself.
    std::vector<PaneSplitSnapshot> splits;
    std::vector<ChromeRegionSnapshot> chromeRegions;
    // Scrolling containers only, with clientRect set to the scrolled viewport.
    std::vector<ScrollBarSnapshot> scrollBars;
    // Accessible islands only: the HWND-less elements the host window exposes.
    std::vector<AccessibleIslandItemSnapshot> islandItems;
    std::optional<NativeDocumentSnapshot> document;
    std::optional<DiskViewSnapshot> diskView;
    // Rich text only: the paragraphs a read-only rich edit control shows, in their formats.
    std::vector<RichParagraphSnapshot> richParagraphs;
    // Performance Monitor's chart, legend and value strip (each on its own kind).
    std::optional<PerformanceGraphSnapshot> performanceGraph;
    std::optional<PerformanceLegendSnapshot> performanceLegend;
    std::optional<PerformanceValueBarSnapshot> performanceValueBar;
    std::wstring adapterId;
    std::wstring pageId;
    std::wstring semanticKey;
    std::wstring sourceKind;
    std::wstring presentationVariant;
    std::vector<std::wstring> supportedActions;
    std::wstring helpText;
    std::wstring accessKey;
};

// Report capture mirrors each first cell into items; every ListView view uses
// this one canonical item sequence for selection, checks, editing and actions.
// A windowed virtual list counts every row, carried or not.
inline size_t ListViewItemCount(const ControlNode& node) noexcept {
    return node.itemCount != 0 ? node.itemCount : node.items.size();
}

// The label of the item at an absolute index, when the node carries that row.
inline const std::wstring* ListViewItemLabel(const ControlNode& node, int index) noexcept {
    if (index < 0 || static_cast<size_t>(index) < node.itemWindowStart) return nullptr;
    const size_t local = static_cast<size_t>(index) - node.itemWindowStart;
    return local < node.items.size() ? &node.items[local] : nullptr;
}

enum class MenuItemKind {
    Popup,
    Command,
    Separator,
};

struct MenuItemSnapshot final {
    std::wstring itemId;
    MenuItemKind kind = MenuItemKind::Command;
    std::wstring text;
    uint32_t commandId = 0;
    bool enabled = true;
    bool checked = false;
    bool radio = false;
    bool isDefault = false;
    std::vector<MenuItemSnapshot> items;
};

// A live native TrackPopupMenu call, anchored to the island item that opened it.
// The token scopes item paths to this one invocation; native handles stay in Bridge.
struct PopupMenuSnapshot final {
    uint64_t popupId = 0;
    uint64_t nodeId = 0;
    int itemIndex = -1;
    std::vector<MenuItemSnapshot> items;
};

struct WindowSnapshot final {
    std::wstring surfaceId;
    SurfaceKind surfaceKind = SurfaceKind::Window;
    bool modal = false;
    bool canCancel = true;
    // Whether the native title-bar close button (and Alt+F4) does anything.  Escape is
    // canCancel's: an ordinary window closes from its title bar but ignores Escape.
    bool canClose = true;
    std::wstring icon = L"none";
    uint64_t generation = 0;
    uint64_t revision = 0;
    HWND nativeHwnd = nullptr;
    HWND ownerHwnd = nullptr;
    std::wstring title;
    UINT dpi = 96;
    RECT bounds{};
    RECT clientBounds{};
    uint64_t windowStyle = 0;
    uint64_t windowExStyle = 0;
    bool visible = true;
    bool enabled = true;
    std::wstring state = L"normal";
    bool showInTaskbar = true;
    bool rtl = false;
    std::vector<MenuItemSnapshot> menu;
    // Where the menu bar is when it is a toolbar inside the client area (MMC's), in client
    // pixels: the projection draws its menu bar there instead of above the client area,
    // which is where an HMENU's bar is.  Unset for an HMENU or no menu.
    std::optional<RECT> menuBarRect;
    // What a dialog paints itself in a full-width band above or below all of its controls
    // (mstsc's banner: its logo and title), in client pixels.  A band the dialog only fills
    // with its background is not carried.
    std::vector<ChromeRegionSnapshot> paintedBands;
    // Bridge-only identity: a refreshed toolbar binding invalidates the revision
    // even when its rendered labels/command IDs happen to remain identical.
    uint64_t menuBindingGeneration = 0;
    std::optional<PopupMenuSnapshot> popupMenu;
    std::vector<ControlNode> nodes;
    std::wstring adapterId;
    std::wstring pageId;
};

struct ActionRequest final {
    std::wstring surfaceId;
    std::optional<uint64_t> nodeId;
    uint64_t eventId = 0;
    uint64_t expectedRevision = 0;
    // Bridge-only witness taken from the canonical published snapshot. Indexed
    // activation rechecks it before invoking, so an unpublished item replacement
    // cannot redirect an action even when its label happens to be unchanged.
    uint64_t expectedNativeFingerprint = 0;
    // Toolbar menu IDs can be remapped by a refresh. Keep the canonical binding
    // generation across the source-thread hop, including a failed recapture.
    uint64_t expectedMenuBindingGeneration = 0;
    std::wstring action;
    std::wstring nativeDocumentId;
    std::wstring nativeDocumentItemKey;
    std::wstring nativeDocumentActionIdentity;
    uint32_t diskItemId = 0;
    std::wstring text;
    int integerValue = 0;
    std::vector<int> integerValues;
    int itemIndex = -1;
    bool booleanValue = false;
    // setScroll: booleanValue names the vertical bar, and integerValue is either a
    // target position or (when this is set) a signed count of line steps.
    bool scrollByLines = false;
    int groupId = 0;
    int selectionStart = 0;
    int selectionLength = 0;
    uint32_t menuCommandId = 0;
    uint64_t popupId = 0;
    std::optional<std::wstring> popupItemId;
    RECT rect{};
    bool hasRect = false;
};

struct SurfaceReady final {
    std::wstring surfaceId;
    uint64_t revision = 0;
    HWND proxyHwnd = nullptr;
    RECT bounds{};
    size_t nodeCount = 0;
    bool uiaReady = false;
};

struct HelloMessage final {
    std::wstring role;
    DWORD processId = 0;
    uint64_t processCreated = 0;
    uint16_t protocolMajor = 0;
    uint16_t protocolMinor = 0;
    std::wstring nonce;
};

std::string SerializeHello(
    std::wstring_view nonce,
    std::wstring_view role,
    DWORD processId,
    uint64_t processCreated);
std::string SerializeWindowOpen(std::wstring_view nonce, const WindowSnapshot& snapshot);
std::string SerializeWindowPatch(
    std::wstring_view nonce,
    uint64_t baseRevision,
    const WindowSnapshot& snapshot,
    std::optional<uint64_t> eventId = std::nullopt);
std::string SerializeActionResult(
    std::wstring_view nonce,
    const ActionRequest& action,
    std::wstring_view status,
    uint64_t revision,
    std::wstring_view reason = {});
std::string SerializeSurfaceCommit(
    std::wstring_view nonce,
    std::wstring_view surfaceId,
    uint64_t revision,
    bool show,
    bool interactive = true);
std::string SerializeWindowClose(
    std::wstring_view nonce,
    std::wstring_view surfaceId,
    std::wstring_view reason);
std::string SerializeHeartbeat(std::wstring_view nonce, uint64_t sentAt);
std::string SerializeError(
    std::wstring_view nonce,
    std::wstring_view code,
    std::wstring_view detail,
    bool fatal,
    std::wstring_view surfaceId = {});
std::string SerializeShutdown(std::wstring_view nonce, std::wstring_view reason);

bool ParseHello(std::string_view payload, HelloMessage& message, std::wstring& error) noexcept;
bool ParseSurfaceReady(
    std::string_view payload,
    std::wstring_view expectedNonce,
    SurfaceReady& ready,
    std::wstring& error) noexcept;
bool ParseActionInvoke(
    std::string_view payload,
    std::wstring_view expectedNonce,
    ActionRequest& action,
    std::wstring& error) noexcept;
bool ParseHeartbeat(std::string_view payload, std::wstring_view nonce, std::wstring& error) noexcept;
// fatal and surfaceId report the renderer's own scoping for the fault.  A non-fatal
// error naming a live surface is a per-surface fault, not a session fault.
bool ParseErrorMessage(
    std::string_view payload,
    std::wstring_view nonce,
    std::wstring& error,
    bool* fatal = nullptr,
    std::wstring* surfaceId = nullptr) noexcept;
bool ParseShutdownMessage(
    std::string_view payload,
    std::wstring_view nonce,
    std::wstring& error) noexcept;
bool ValidateActionForSnapshot(
    const ActionRequest& action,
    const WindowSnapshot& snapshot,
    std::wstring& error) noexcept;
bool IsRequestSemanticAction(std::wstring_view action) noexcept;

const wchar_t* ControlKindName(ControlKind kind) noexcept;
// The nearest owner that can put pixels on the screen, or null when the whole
// owner chain is hidden.  A hidden owner -- the StubWindow32 behind a control
// panel property sheet, a message-only owner behind a tool window -- is never
// projected and never competes for z-order, so a window it owns is the
// application's own top-level rather than a node in an owner graph.  A projected
// root is cloaked rather than hidden and still reports WS_VISIBLE, so it keeps
// being seen as an owner.
HWND EffectiveTopLevelOwner(HWND window) noexcept;
// A dialog the application is running modally against its owner: the owner can hold
// the screen and the application disabled it for the dialog's lifetime.  That is the
// only owner relationship the projection reproduces, because the proxy inherits the
// real owner and blocks it -- which is exactly what the renderer's surface registry
// and the committed UIA gate already validate for a translated MessageBox.  A
// modeless owned top-level, and a dialog nested deeper than one level, stay native.
bool IsModalOwnedTopLevel(HWND window) noexcept;
// A kind whose projected element frames other nodes.  Only these may be named as a
// node's parent: the renderer places a child inside its parent's own element, and
// every other kind draws its content itself.
bool IsProjectedContainerKind(ControlKind kind) noexcept;
const wchar_t* SurfaceKindName(SurfaceKind kind) noexcept;
const wchar_t* MenuItemKindName(MenuItemKind kind) noexcept;

} // namespace FluentShell::Bridge::Translation
