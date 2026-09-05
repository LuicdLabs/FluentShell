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
    Count,
};

enum class ToolbarItemKind {
    PushButton,
    Separator,
    // A BTNS_CHECK button keeps its pressed look between clicks, so the projection
    // draws a toggle and carries the state the control owns.
    ToggleButton,
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
};

struct ToolbarItemSnapshot final {
    ToolbarItemKind kind = ToolbarItemKind::PushButton;
    uint32_t commandId = 0;
    RECT rect{};
    std::wstring text;
    bool enabled = true;
    bool hidden = false;
    bool checked = false;
    // BTNS_DROPDOWN: the button carries an arrow that asks its owner for a menu
    // through TBN_DROPDOWN.  With BTNS_WHOLEDROPDOWN the whole button does that and
    // there is no separate command click at all.
    bool dropDown = false;
    bool wholeDropDown = false;
    // The control draws this button's face itself, so the projection carries the pixels
    // it drew rather than an image-list icon it does not own.
    bool paintedFace = false;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
};

// One icon of a control's own image list.  Item-bearing controls share a bounded
// list and address it by index, exactly as Win32 does: a tree or list with two
// hundred rows normally draws a handful of distinct icons, so embedding pixels
// per item would multiply the same bytes across the payload.
struct ImageListEntry final {
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
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
    // TCM_GETITEMRECT results in TabControl client-local physical pixels.
    std::vector<RECT> itemRects;
    std::vector<std::wstring> columns;
    std::vector<int> columnWidths;
    // Report ListView only: the display order of the columns as a permutation of
    // their logical indexes.  Columns, widths, and cells all travel in logical
    // order, so nothing on the wire changes meaning when the user reorders them.
    std::vector<int> columnOrder;
    std::vector<std::vector<std::wstring>> rows;
    bool columnHeadersVisible = false;
    bool checkBoxes = false;
    std::vector<int> checkedIndices;
    std::vector<int> itemDepths;
    std::vector<bool> itemExpanded;
    // Whether a TreeView item owns children at all.  A lazily populated tree
    // reports this before its children exist, which is what lets a projected
    // expander reach a subtree the application has not inserted yet.
    std::vector<bool> itemHasChildren;
    // The control's own image list and the per-item indexes into it.  -1 means the
    // item draws no icon.  itemSelectedImages is the tree's separate selected-state
    // index, which is how a folder opens and closes natively.
    std::vector<ImageListEntry> imageList;
    std::vector<int> itemImages;
    std::vector<int> itemSelectedImages;
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
    // Accessible islands only: the HWND-less elements the host window exposes.
    std::vector<AccessibleIslandItemSnapshot> islandItems;
    std::wstring adapterId;
    std::wstring pageId;
    std::wstring semanticKey;
    std::wstring sourceKind;
    std::wstring presentationVariant;
    std::vector<std::wstring> supportedActions;
    std::wstring helpText;
    std::wstring accessKey;
};

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

struct WindowSnapshot final {
    std::wstring surfaceId;
    SurfaceKind surfaceKind = SurfaceKind::Window;
    bool modal = false;
    bool canCancel = true;
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
    std::vector<ControlNode> nodes;
    std::wstring adapterId;
    std::wstring pageId;
};

struct ActionRequest final {
    std::wstring surfaceId;
    std::optional<uint64_t> nodeId;
    uint64_t eventId = 0;
    uint64_t expectedRevision = 0;
    std::wstring action;
    std::wstring text;
    int integerValue = 0;
    std::vector<int> integerValues;
    int itemIndex = -1;
    bool booleanValue = false;
    int selectionStart = 0;
    int selectionLength = 0;
    uint32_t menuCommandId = 0;
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
// A kind whose projected element frames other nodes.  Only these may be named as a
// node's parent: the renderer places a child inside its parent's own element, and
// every other kind draws its content itself.
bool IsProjectedContainerKind(ControlKind kind) noexcept;
const wchar_t* SurfaceKindName(SurfaceKind kind) noexcept;
const wchar_t* MenuItemKindName(MenuItemKind kind) noexcept;

} // namespace FluentShell::Bridge::Translation
