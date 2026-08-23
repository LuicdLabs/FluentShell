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
    StatusBar,
};

struct ControlNode final {
    uint64_t nodeId = 0;
    uint64_t generation = 0;
    HWND hwnd = nullptr;
    std::optional<uint64_t> parentNodeId;
    ControlKind kind = ControlKind::StaticText;
    int controlId = 0;
    int zIndex = 0;
    // Native dialog-manager order among currently focusable tab stops. A
    // value of -1 means the control is not currently keyboard-focusable.
    int tabIndex = -1;
    RECT rect{};
    uint64_t style = 0;
    uint64_t exStyle = 0;
    bool visible = true;
    bool enabled = true;
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
    int minimum = 0;
    int maximum = 100;
    int position = 0;
    int smallChange = 1;
    int largeChange = 10;
    bool vertical = false;
    bool reversed = false;
    std::vector<std::wstring> items;
    std::vector<std::wstring> columns;
    std::vector<int> columnWidths;
    std::vector<std::vector<std::wstring>> rows;
    std::vector<int> itemDepths;
    std::vector<bool> itemExpanded;
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
const wchar_t* SurfaceKindName(SurfaceKind kind) noexcept;
const wchar_t* MenuItemKindName(MenuItemKind kind) noexcept;

} // namespace FluentShell::Bridge::Translation
