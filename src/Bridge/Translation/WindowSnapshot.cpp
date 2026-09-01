#include "WindowSnapshot.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace FluentShell::Bridge::Translation {
namespace {

using namespace winrt::Windows::Data::Json;

std::wstring Base64Encode(const std::vector<uint8_t>& bytes) {
    static constexpr wchar_t alphabet[] =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t offset = 0; offset < bytes.size(); offset += 3) {
        const uint32_t value = static_cast<uint32_t>(bytes[offset]) << 16 |
            (offset + 1 < bytes.size() ? static_cast<uint32_t>(bytes[offset + 1]) << 8 : 0) |
            (offset + 2 < bytes.size() ? bytes[offset + 2] : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(offset + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : L'=');
        result.push_back(offset + 2 < bytes.size() ? alphabet[value & 63] : L'=');
    }
    return result;
}

JsonObject RectToJson(const RECT& rect) {
    JsonObject value;
    value.Insert(L"x", JsonValue::CreateNumberValue(rect.left));
    value.Insert(L"y", JsonValue::CreateNumberValue(rect.top));
    value.Insert(L"width", JsonValue::CreateNumberValue(std::max(0L, rect.right - rect.left)));
    value.Insert(L"height", JsonValue::CreateNumberValue(std::max(0L, rect.bottom - rect.top)));
    return value;
}

bool JsonInteger(const JsonObject& value, std::wstring_view name, LONG& result) {
    if (!value.HasKey(name)) return false;
    const double number = value.GetNamedNumber(name);
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < static_cast<double>(std::numeric_limits<LONG>::min()) ||
        number > static_cast<double>(std::numeric_limits<LONG>::max())) {
        return false;
    }
    result = static_cast<LONG>(number);
    return true;
}

template <typename T>
bool JsonUnsignedInteger(const JsonObject& value, std::wstring_view name, T& result) {
    static_assert(std::is_unsigned_v<T>);
    if (!value.HasKey(name)) return false;
    const auto json = value.GetNamedValue(name);
    if (json.ValueType() != JsonValueType::Number) return false;
    const double number = json.GetNumber();
    if (!std::isfinite(number) || std::trunc(number) != number || number < 0 ||
        number > static_cast<double>(std::numeric_limits<T>::max())) {
        return false;
    }
    result = static_cast<T>(number);
    return true;
}

bool IsNonce(std::wstring_view value) noexcept {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
            (c >= L'A' && c <= L'F');
    });
}

bool HasExpectedNonce(
    const JsonObject& root,
    std::wstring_view expectedNonce,
    std::wstring_view messageType,
    std::wstring& error) {
    const auto nonce = root.GetNamedString(L"sessionNonce", L"");
    if (!IsNonce(nonce) || nonce != expectedNonce) {
        error = std::wstring(messageType) + L" nonce mismatch";
        return false;
    }
    return true;
}

bool JsonToRect(const JsonObject& value, RECT& result) {
    LONG x = 0;
    LONG y = 0;
    LONG width = 0;
    LONG height = 0;
    if (!JsonInteger(value, L"x", x) || !JsonInteger(value, L"y", y) ||
        !JsonInteger(value, L"width", width) || !JsonInteger(value, L"height", height) ||
        width < 0 || height < 0) {
        return false;
    }
    const int64_t right = static_cast<int64_t>(x) + width;
    const int64_t bottom = static_cast<int64_t>(y) + height;
    if (right > std::numeric_limits<LONG>::max() ||
        bottom > std::numeric_limits<LONG>::max()) {
        return false;
    }
    result = { x, y, static_cast<LONG>(right), static_cast<LONG>(bottom) };
    return true;
}

std::wstring Hex64(uint64_t value) {
    return Ipc::HwndToString(reinterpret_cast<HWND>(static_cast<uintptr_t>(value)));
}

JsonObject NodeToJson(const ControlNode& node) {
    JsonObject result;
    result.Insert(L"nodeId", JsonValue::CreateStringValue(Ipc::UInt64ToString(node.nodeId)));
    result.Insert(L"generation", JsonValue::CreateStringValue(Ipc::UInt64ToString(node.generation)));
    result.Insert(L"nativeHwnd", node.hwnd
        ? JsonValue::CreateStringValue(Ipc::HwndToString(node.hwnd))
        : JsonValue::CreateNullValue());
    if (node.parentNodeId) {
        result.Insert(L"parentNodeId", JsonValue::CreateStringValue(Ipc::UInt64ToString(*node.parentNodeId)));
    } else {
        result.Insert(L"parentNodeId", JsonValue::CreateNullValue());
    }
    result.Insert(L"kind", JsonValue::CreateStringValue(ControlKindName(node.kind)));
    result.Insert(L"controlId", JsonValue::CreateNumberValue(node.controlId));
    result.Insert(L"zIndex", JsonValue::CreateNumberValue(node.zIndex));
    result.Insert(L"tabIndex", JsonValue::CreateNumberValue(node.tabIndex));
    result.Insert(L"rect", RectToJson(node.rect));
    result.Insert(L"style", JsonValue::CreateStringValue(Hex64(node.style)));
    result.Insert(L"exStyle", JsonValue::CreateStringValue(Hex64(node.exStyle)));
    result.Insert(L"visible", JsonValue::CreateBooleanValue(node.visible));
    result.Insert(L"enabled", JsonValue::CreateBooleanValue(node.enabled));
    result.Insert(L"tabStop", JsonValue::CreateBooleanValue(node.tabStop));
    result.Insert(L"dialogCode", JsonValue::CreateNumberValue(node.dialogCode));
    result.Insert(L"text", JsonValue::CreateStringValue(node.text));
    result.Insert(L"automationName", JsonValue::CreateStringValue(node.automationName));
    result.Insert(L"checked", JsonValue::CreateNumberValue(node.checked));
    result.Insert(L"selectedIndex", JsonValue::CreateNumberValue(node.selectedIndex));
    JsonArray selectedIndices;
    for (const int index : node.selectedIndices) {
        selectedIndices.Append(JsonValue::CreateNumberValue(index));
    }
    result.Insert(L"selectedIndices", selectedIndices);
    result.Insert(L"focusedIndex", JsonValue::CreateNumberValue(node.focusedIndex));
    result.Insert(L"multiSelect", JsonValue::CreateBooleanValue(node.multiSelect));
    result.Insert(L"selectionStart", JsonValue::CreateNumberValue(node.selectionStart));
    result.Insert(L"selectionLength", JsonValue::CreateNumberValue(node.selectionLength));
    result.Insert(L"readOnly", JsonValue::CreateBooleanValue(node.readOnly));
    result.Insert(L"multiline", JsonValue::CreateBooleanValue(node.multiline));
    result.Insert(L"editable", JsonValue::CreateBooleanValue(node.editable));
    result.Insert(L"isDefault", JsonValue::CreateBooleanValue(node.isDefault));
    result.Insert(L"groupStart", JsonValue::CreateBooleanValue(node.groupStart));
    result.Insert(L"minimum", JsonValue::CreateNumberValue(node.minimum));
    result.Insert(L"maximum", JsonValue::CreateNumberValue(node.maximum));
    result.Insert(L"position", JsonValue::CreateNumberValue(node.position));
    if (node.kind == ControlKind::ProgressBar) {
        result.Insert(L"indeterminate", JsonValue::CreateBooleanValue(node.indeterminate));
    }
    result.Insert(L"smallChange", JsonValue::CreateNumberValue(node.smallChange));
    result.Insert(L"largeChange", JsonValue::CreateNumberValue(node.largeChange));
    result.Insert(L"vertical", JsonValue::CreateBooleanValue(node.vertical));
    result.Insert(L"reversed", JsonValue::CreateBooleanValue(node.reversed));
    JsonArray items;
    for (const auto& item : node.items) {
        items.Append(JsonValue::CreateStringValue(item));
    }
    result.Insert(L"items", items);
    if (node.kind == ControlKind::TabControl) {
        JsonArray itemRects;
        for (const auto& rect : node.itemRects) itemRects.Append(RectToJson(rect));
        result.Insert(L"itemRects", itemRects);
    }
    JsonArray columns;
    for (const auto& column : node.columns) {
        columns.Append(JsonValue::CreateStringValue(column));
    }
    result.Insert(L"columns", columns);
    JsonArray columnWidths;
    for (const int width : node.columnWidths) {
        columnWidths.Append(JsonValue::CreateNumberValue(width));
    }
    result.Insert(L"columnWidths", columnWidths);
    JsonArray rows;
    for (const auto& row : node.rows) {
        JsonArray cells;
        for (const auto& cell : row) cells.Append(JsonValue::CreateStringValue(cell));
        rows.Append(cells);
    }
    result.Insert(L"rows", rows);
    if (node.kind == ControlKind::ListView) {
        result.Insert(L"columnHeadersVisible",
            JsonValue::CreateBooleanValue(node.columnHeadersVisible));
        result.Insert(L"checkBoxes", JsonValue::CreateBooleanValue(node.checkBoxes));
        JsonArray checkedIndices;
        for (const int index : node.checkedIndices) {
            checkedIndices.Append(JsonValue::CreateNumberValue(index));
        }
        result.Insert(L"checkedIndices", checkedIndices);
    }
    JsonArray itemDepths;
    for (const int depth : node.itemDepths) {
        itemDepths.Append(JsonValue::CreateNumberValue(depth));
    }
    result.Insert(L"itemDepths", itemDepths);
    JsonArray itemExpanded;
    for (const bool expanded : node.itemExpanded) {
        itemExpanded.Append(JsonValue::CreateBooleanValue(expanded));
    }
    result.Insert(L"itemExpanded", itemExpanded);
    if (node.kind == ControlKind::StaticIcon) {
        result.Insert(L"imageWidth", JsonValue::CreateNumberValue(node.imageWidth));
        result.Insert(L"imageHeight", JsonValue::CreateNumberValue(node.imageHeight));
        result.Insert(L"imageFormat", JsonValue::CreateStringValue(node.imageFormat));
        result.Insert(L"imageData", JsonValue::CreateStringValue(Base64Encode(node.imageData)));
    }
    if (node.kind == ControlKind::Toolbar) {
        JsonArray toolbarItems;
        for (const auto& item : node.toolbarItems) {
            JsonObject value;
            value.Insert(L"kind", JsonValue::CreateStringValue(
                item.kind == ToolbarItemKind::PushButton ? L"pushButton" : L"separator"));
            value.Insert(L"commandId", JsonValue::CreateNumberValue(item.commandId));
            value.Insert(L"rect", RectToJson(item.rect));
            value.Insert(L"text", JsonValue::CreateStringValue(item.text));
            value.Insert(L"enabled", JsonValue::CreateBooleanValue(item.enabled));
            value.Insert(L"hidden", JsonValue::CreateBooleanValue(item.hidden));
            if (item.kind == ToolbarItemKind::PushButton) {
                value.Insert(L"imageWidth", JsonValue::CreateNumberValue(item.imageWidth));
                value.Insert(L"imageHeight", JsonValue::CreateNumberValue(item.imageHeight));
                value.Insert(L"imageFormat", JsonValue::CreateStringValue(item.imageFormat));
                value.Insert(L"imageData", JsonValue::CreateStringValue(Base64Encode(item.imageData)));
            }
            toolbarItems.Append(value);
        }
        result.Insert(L"toolbarItems", toolbarItems);
    }
    if (!node.adapterId.empty()) {
        result.Insert(L"adapterId", JsonValue::CreateStringValue(node.adapterId));
        result.Insert(L"pageId", JsonValue::CreateStringValue(node.pageId));
        result.Insert(L"semanticKey", JsonValue::CreateStringValue(node.semanticKey));
        result.Insert(L"sourceKind", JsonValue::CreateStringValue(node.sourceKind));
        result.Insert(L"presentationVariant", JsonValue::CreateStringValue(node.presentationVariant));
        JsonArray actions;
        for (const auto& action : node.supportedActions)
            actions.Append(JsonValue::CreateStringValue(action));
        result.Insert(L"supportedActions", actions);
        result.Insert(L"helpText", JsonValue::CreateStringValue(node.helpText));
        result.Insert(L"accessKey", JsonValue::CreateStringValue(node.accessKey));
    }
    return result;
}

JsonObject MenuItemToJson(const MenuItemSnapshot& item) {
    JsonObject result;
    result.Insert(L"itemId", JsonValue::CreateStringValue(item.itemId));
    result.Insert(L"kind", JsonValue::CreateStringValue(MenuItemKindName(item.kind)));
    result.Insert(L"text", JsonValue::CreateStringValue(item.text));
    result.Insert(L"commandId", JsonValue::CreateNumberValue(item.commandId));
    result.Insert(L"enabled", JsonValue::CreateBooleanValue(item.enabled));
    result.Insert(L"checked", JsonValue::CreateBooleanValue(item.checked));
    result.Insert(L"radio", JsonValue::CreateBooleanValue(item.radio));
    result.Insert(L"isDefault", JsonValue::CreateBooleanValue(item.isDefault));
    JsonArray items;
    for (const auto& child : item.items) items.Append(MenuItemToJson(child));
    result.Insert(L"items", items);
    return result;
}

JsonObject SnapshotToJson(const WindowSnapshot& snapshot) {
    JsonObject result;
    result.Insert(L"surfaceId", JsonValue::CreateStringValue(snapshot.surfaceId));
    result.Insert(L"surfaceKind", JsonValue::CreateStringValue(SurfaceKindName(snapshot.surfaceKind)));
    result.Insert(L"modal", JsonValue::CreateBooleanValue(snapshot.modal));
    result.Insert(L"canCancel", JsonValue::CreateBooleanValue(snapshot.canCancel));
    result.Insert(L"icon", JsonValue::CreateStringValue(snapshot.icon));
    result.Insert(L"generation", JsonValue::CreateStringValue(Ipc::UInt64ToString(snapshot.generation)));
    result.Insert(L"revision", JsonValue::CreateStringValue(Ipc::UInt64ToString(snapshot.revision)));
    result.Insert(L"nativeHwnd", JsonValue::CreateStringValue(Ipc::HwndToString(snapshot.nativeHwnd)));
    if (snapshot.ownerHwnd) {
        result.Insert(L"ownerHwnd", JsonValue::CreateStringValue(Ipc::HwndToString(snapshot.ownerHwnd)));
    } else {
        result.Insert(L"ownerHwnd", JsonValue::CreateNullValue());
    }
    result.Insert(L"title", JsonValue::CreateStringValue(snapshot.title));
    result.Insert(L"dpi", JsonValue::CreateNumberValue(snapshot.dpi));
    result.Insert(L"bounds", RectToJson(snapshot.bounds));
    result.Insert(L"clientBounds", RectToJson(snapshot.clientBounds));
    result.Insert(L"windowStyle", JsonValue::CreateStringValue(Hex64(snapshot.windowStyle)));
    result.Insert(L"windowExStyle", JsonValue::CreateStringValue(Hex64(snapshot.windowExStyle)));
    result.Insert(L"visible", JsonValue::CreateBooleanValue(snapshot.visible));
    result.Insert(L"enabled", JsonValue::CreateBooleanValue(snapshot.enabled));
    result.Insert(L"state", JsonValue::CreateStringValue(snapshot.state));
    result.Insert(L"showInTaskbar", JsonValue::CreateBooleanValue(snapshot.showInTaskbar));
    result.Insert(L"rtl", JsonValue::CreateBooleanValue(snapshot.rtl));
    if (!snapshot.adapterId.empty()) {
        result.Insert(L"adapterId", JsonValue::CreateStringValue(snapshot.adapterId));
        result.Insert(L"pageId", JsonValue::CreateStringValue(snapshot.pageId));
    }
    JsonArray menu;
    for (const auto& item : snapshot.menu) menu.Append(MenuItemToJson(item));
    result.Insert(L"menu", menu);
    JsonArray nodes;
    for (const auto& node : snapshot.nodes) nodes.Append(NodeToJson(node));
    result.Insert(L"nodes", nodes);
    return result;
}

std::string Stringify(const JsonObject& object) {
    return Ipc::WideToUtf8(object.Stringify().c_str());
}

JsonObject ParseJson(std::string_view payload) {
    const auto wide = Ipc::Utf8ToWide(payload);
    return JsonObject::Parse(winrt::hstring(
        wide.data(), static_cast<winrt::hstring::size_type>(wide.size())));
}

bool ValidateJsonLimits(std::string_view payload, std::wstring& error) noexcept {
    if (payload.empty() || payload.size() > Ipc::kMaxPayloadBytes) {
        error = L"invalid JSON payload size";
        return false;
    }
    size_t depth = 0;
    size_t stringBytes = 0;
    bool inString = false;
    bool escaped = false;
    for (const unsigned char byte : payload) {
        if (byte == 0) {
            error = L"JSON contains NUL";
            return false;
        }
        if (inString) {
            if (!escaped && byte == '"') {
                inString = false;
                continue;
            }
            if (++stringBytes > Ipc::kMaxStringChars) {
                error = L"JSON string exceeds limit";
                return false;
            }
            if (escaped) {
                escaped = false;
                continue;
            }
            if (byte == '\\') {
                escaped = true;
                continue;
            }
            continue;
        }
        if (byte == '"') {
            inString = true;
            stringBytes = 0;
        } else if (byte == '{' || byte == '[') {
            if (++depth > Ipc::kMaxJsonDepth) {
                error = L"JSON depth exceeds limit";
                return false;
            }
        } else if (byte == '}' || byte == ']') {
            if (depth == 0) {
                error = L"invalid JSON nesting";
                return false;
            }
            --depth;
        }
    }
    if (inString || depth != 0) {
        error = L"unterminated JSON payload";
        return false;
    }
    return true;
}

bool RequiredMessage(
    const JsonObject& object,
    std::wstring_view expected,
    std::wstring& error) {
    const auto type = object.GetNamedString(L"messageType", L"");
    if (type != expected) {
        error = L"unexpected payload messageType";
        return false;
    }
    return true;
}

bool ParseActionValue(
    const JsonObject& root,
    ActionRequest& action,
    std::wstring& error) {
    if (!root.HasKey(L"value")) {
        error = L"action.invoke requires value";
        return false;
    }
    const auto value = root.GetNamedValue(L"value");
    if (action.action == L"setText") {
        if (value.ValueType() != JsonValueType::String) {
            error = L"setText requires a string value";
            return false;
        }
        action.text = value.GetString();
        return true;
    }
    if (action.action == L"setCheck" || action.action == L"select" ||
        action.action == L"menuCommand" || action.action == L"toolbarCommand") {
        if (value.ValueType() != JsonValueType::Number) {
            error = L"setCheck/select/menuCommand/toolbarCommand requires an integer value";
            return false;
        }
        const double number = value.GetNumber();
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < std::numeric_limits<int>::min() ||
            number > std::numeric_limits<int>::max()) {
            error = L"action integer is outside range";
            return false;
        }
        action.integerValue = static_cast<int>(number);
        if (action.action == L"menuCommand" || action.action == L"toolbarCommand") {
            if (action.integerValue <= 0 || action.integerValue > 0xffff) {
                error = L"command ID is outside WM_COMMAND range";
                return false;
            }
            action.menuCommandId = static_cast<uint32_t>(action.integerValue);
        }
        return true;
    }
    if (action.action == L"setSelection") {
        if (value.ValueType() != JsonValueType::Array) {
            error = L"setSelection requires an integer array";
            return false;
        }
        const auto values = value.GetArray();
        if (values.Size() > Ipc::kMaxListItems) {
            error = L"setSelection exceeds the item cap";
            return false;
        }
        int previous = -1;
        for (const auto& entry : values) {
            if (entry.ValueType() != JsonValueType::Number) {
                error = L"setSelection requires integer indexes";
                return false;
            }
            const double number = entry.GetNumber();
            if (!std::isfinite(number) || std::trunc(number) != number ||
                number < 0 || number > std::numeric_limits<int>::max()) {
                error = L"setSelection index is outside range";
                return false;
            }
            const int index = static_cast<int>(number);
            if (index <= previous) {
                error = L"setSelection indexes must be unique and sorted";
                return false;
            }
            action.integerValues.push_back(index);
            previous = index;
        }
        return true;
    }
    if (action.action == L"setItemCheck") {
        if (value.ValueType() != JsonValueType::Object) {
            error = L"setItemCheck requires an object value";
            return false;
        }
        const auto object = value.GetObject();
        if (object.Size() != 2 || !object.HasKey(L"index") || !object.HasKey(L"checked") ||
            object.GetNamedValue(L"checked").ValueType() != JsonValueType::Boolean) {
            error = L"setItemCheck requires exactly integer index and boolean checked";
            return false;
        }
        LONG index = -1;
        if (!JsonInteger(object, L"index", index) || index < 0 ||
            static_cast<size_t>(index) >= Ipc::kMaxListItems) {
            error = L"setItemCheck index is outside range";
            return false;
        }
        action.itemIndex = index;
        action.booleanValue = object.GetNamedBoolean(L"checked");
        return true;
    }
    if (action.action == L"move" || action.action == L"resize") {
        if (value.ValueType() != JsonValueType::Object) {
            error = L"move/resize requires a bounds object";
            return false;
        }
        const auto object = value.GetObject();
        if (!JsonToRect(object, action.rect)) {
            error = L"move/resize bounds are invalid";
            return false;
        }
        action.hasRect = true;
        return true;
    }
    if (value.ValueType() != JsonValueType::Null) {
        error = L"request action requires a null value";
        return false;
    }
    return true;
}

} // namespace

const wchar_t* ControlKindName(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::StaticText: return L"static";
    case ControlKind::StaticIcon: return L"staticIcon";
    case ControlKind::Separator: return L"separator";
    case ControlKind::Button: return L"button";
    case ControlKind::CheckBox: return L"checkBox";
    case ControlKind::ThreeState: return L"threeState";
    case ControlKind::RadioButton: return L"radioButton";
    case ControlKind::Edit: return L"edit";
    case ControlKind::Password: return L"password";
    case ControlKind::ComboBox: return L"comboBox";
    case ControlKind::ListBox: return L"listBox";
    case ControlKind::GroupBox: return L"groupBox";
    case ControlKind::ProgressBar: return L"progressBar";
    case ControlKind::SysLink: return L"sysLink";
    case ControlKind::ListView: return L"listView";
    case ControlKind::TreeView: return L"treeView";
    case ControlKind::TabControl: return L"tabControl";
    case ControlKind::Slider: return L"slider";
    case ControlKind::DialogContainer: return L"dialogContainer";
    case ControlKind::StatusBar: return L"statusBar";
    case ControlKind::Toolbar: return L"toolbar";
    }
    return L"static";
}

const wchar_t* SurfaceKindName(SurfaceKind kind) noexcept {
    switch (kind) {
    case SurfaceKind::Window: return L"window";
    case SurfaceKind::MessageBox: return L"messageBox";
    case SurfaceKind::TaskDialog: return L"taskDialog";
    }
    return L"window";
}

const wchar_t* MenuItemKindName(MenuItemKind kind) noexcept {
    switch (kind) {
    case MenuItemKind::Popup: return L"popup";
    case MenuItemKind::Command: return L"command";
    case MenuItemKind::Separator: return L"separator";
    }
    return L"command";
}

std::string SerializeHello(
    std::wstring_view nonce,
    std::wstring_view role,
    DWORD processId,
    uint64_t processCreated) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"hello"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"role", JsonValue::CreateStringValue(role));
    root.Insert(L"processId", JsonValue::CreateNumberValue(processId));
    root.Insert(L"processCreated", JsonValue::CreateStringValue(Ipc::UInt64ToString(processCreated)));
    root.Insert(L"protocolMajor", JsonValue::CreateNumberValue(Ipc::kProtocolMajor));
    root.Insert(L"protocolMinor", JsonValue::CreateNumberValue(Ipc::kProtocolMinor));
    return Stringify(root);
}

std::string SerializeWindowOpen(std::wstring_view nonce, const WindowSnapshot& snapshot) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"window.open"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"window", SnapshotToJson(snapshot));
    return Stringify(root);
}

std::string SerializeWindowPatch(
    std::wstring_view nonce,
    uint64_t baseRevision,
    const WindowSnapshot& snapshot,
    std::optional<uint64_t> eventId) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"window.patch"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"surfaceId", JsonValue::CreateStringValue(snapshot.surfaceId));
    root.Insert(L"baseRevision", JsonValue::CreateStringValue(Ipc::UInt64ToString(baseRevision)));
    root.Insert(L"revision", JsonValue::CreateStringValue(Ipc::UInt64ToString(snapshot.revision)));
    if (eventId) {
        root.Insert(L"eventId", JsonValue::CreateStringValue(Ipc::UInt64ToString(*eventId)));
    }
    JsonArray operations;
    root.Insert(L"operations", operations);
    root.Insert(L"snapshot", SnapshotToJson(snapshot));
    return Stringify(root);
}

std::string SerializeActionResult(
    std::wstring_view nonce,
    const ActionRequest& action,
    std::wstring_view status,
    uint64_t revision,
    std::wstring_view reason) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"action.result"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"surfaceId", JsonValue::CreateStringValue(action.surfaceId));
    root.Insert(L"eventId", JsonValue::CreateStringValue(Ipc::UInt64ToString(action.eventId)));
    root.Insert(L"status", JsonValue::CreateStringValue(status));
    root.Insert(L"revision", JsonValue::CreateStringValue(Ipc::UInt64ToString(revision)));
    if (!reason.empty()) root.Insert(L"reason", JsonValue::CreateStringValue(reason));
    return Stringify(root);
}

std::string SerializeSurfaceCommit(
    std::wstring_view nonce,
    std::wstring_view surfaceId,
    uint64_t revision,
    bool show,
    bool interactive) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"surface.commit"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"surfaceId", JsonValue::CreateStringValue(surfaceId));
    root.Insert(L"revision", JsonValue::CreateStringValue(Ipc::UInt64ToString(revision)));
    root.Insert(L"show", JsonValue::CreateBooleanValue(show));
    root.Insert(L"interactive", JsonValue::CreateBooleanValue(interactive));
    return Stringify(root);
}

std::string SerializeWindowClose(
    std::wstring_view nonce,
    std::wstring_view surfaceId,
    std::wstring_view reason) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"window.close"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"surfaceId", JsonValue::CreateStringValue(surfaceId));
    root.Insert(L"reason", JsonValue::CreateStringValue(reason));
    return Stringify(root);
}

std::string SerializeHeartbeat(std::wstring_view nonce, uint64_t sentAt) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"heartbeat"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"sentAt", JsonValue::CreateStringValue(Ipc::UInt64ToString(sentAt)));
    return Stringify(root);
}

std::string SerializeError(
    std::wstring_view nonce,
    std::wstring_view code,
    std::wstring_view detail,
    bool fatal,
    std::wstring_view surfaceId) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"error"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    if (surfaceId.empty()) root.Insert(L"surfaceId", JsonValue::CreateNullValue());
    else root.Insert(L"surfaceId", JsonValue::CreateStringValue(surfaceId));
    root.Insert(L"code", JsonValue::CreateStringValue(code));
    root.Insert(L"detail", JsonValue::CreateStringValue(detail));
    root.Insert(L"fatal", JsonValue::CreateBooleanValue(fatal));
    return Stringify(root);
}

std::string SerializeShutdown(std::wstring_view nonce, std::wstring_view reason) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"shutdown"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"reason", JsonValue::CreateStringValue(reason));
    return Stringify(root);
}

bool ParseHello(std::string_view payload, HelloMessage& message, std::wstring& error) noexcept {
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"hello", error)) return false;
        message.nonce = root.GetNamedString(L"sessionNonce");
        message.role = root.GetNamedString(L"role");
        if (!IsNonce(message.nonce) ||
            (message.role != L"bridge" && message.role != L"renderer") ||
            !JsonUnsignedInteger(root, L"processId", message.processId) ||
            message.processId == 0) {
            error = L"invalid hello identity";
            return false;
        }
        if (!Ipc::TryParseUInt64(root.GetNamedString(L"processCreated"), message.processCreated)) {
            error = L"invalid process creation time";
            return false;
        }
        if (message.processCreated == 0 ||
            !JsonUnsignedInteger(root, L"protocolMajor", message.protocolMajor) ||
            !JsonUnsignedInteger(root, L"protocolMinor", message.protocolMinor)) {
            error = L"invalid hello protocol fields";
            return false;
        }
        if (message.protocolMajor != Ipc::kProtocolMajor) {
            error = L"unsupported hello protocol version";
            return false;
        }
        return true;
    } catch (...) {
        error = L"invalid hello JSON";
        return false;
    }
}

bool ParseSurfaceReady(
    std::string_view payload,
    std::wstring_view expectedNonce,
    SurfaceReady& ready,
    std::wstring& error) noexcept {
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"surface.ready", error)) return false;
        if (!HasExpectedNonce(root, expectedNonce, L"surface.ready", error)) return false;
        ready.surfaceId = root.GetNamedString(L"surfaceId");
        if (!Ipc::TryParseUInt64(root.GetNamedString(L"revision"), ready.revision) ||
            !Ipc::TryParseHwnd(root.GetNamedString(L"proxyHwnd"), ready.proxyHwnd)) {
            error = L"invalid surface.ready identifiers";
            return false;
        }
        if (!JsonToRect(root.GetNamedObject(L"bounds"), ready.bounds)) {
            error = L"invalid surface.ready bounds";
            return false;
        }
        uint32_t nodeCount = 0;
        if (!JsonUnsignedInteger(root, L"nodeCount", nodeCount)) {
            error = L"invalid surface.ready node count";
            return false;
        }
        ready.nodeCount = nodeCount;
        ready.uiaReady = root.GetNamedBoolean(L"uiaReady");
        return ready.nodeCount <= Ipc::kMaxNodes;
    } catch (...) {
        error = L"invalid surface.ready JSON";
        return false;
    }
}

bool ParseActionInvoke(
    std::string_view payload,
    std::wstring_view expectedNonce,
    ActionRequest& action,
    std::wstring& error) noexcept {
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"action.invoke", error)) return false;
        if (!HasExpectedNonce(root, expectedNonce, L"action.invoke", error)) return false;
        action.surfaceId = root.GetNamedString(L"surfaceId");
        const auto node = root.GetNamedValue(L"nodeId", JsonValue::CreateNullValue());
        if (node.ValueType() == JsonValueType::String) {
            uint64_t parsed = 0;
            if (!Ipc::TryParseUInt64(node.GetString(), parsed)) {
                error = L"invalid node id";
                return false;
            }
            action.nodeId = parsed;
        }
        if (!Ipc::TryParseUInt64(root.GetNamedString(L"eventId"), action.eventId) ||
            !Ipc::TryParseUInt64(root.GetNamedString(L"expectedRevision"), action.expectedRevision)) {
            error = L"invalid action revision";
            return false;
        }
        action.action = root.GetNamedString(L"action");
        static constexpr std::wstring_view kActions[] = {
            L"activate", L"invoke", L"setText", L"setCheck", L"select",
            L"setSelection", L"setItemCheck", L"menuCommand", L"toolbarCommand",
            L"move", L"resize", L"minimize", L"maximize", L"restore", L"close"
        };
        if (std::find(std::begin(kActions), std::end(kActions), action.action) == std::end(kActions)) {
            error = L"unknown action.invoke action";
            return false;
        }
        const bool requiresNode = action.action == L"invoke" || action.action == L"setText" ||
            action.action == L"setCheck" || action.action == L"select" ||
            action.action == L"setSelection" || action.action == L"setItemCheck" ||
            action.action == L"toolbarCommand";
        if (requiresNode != action.nodeId.has_value()) {
            error = L"action.invoke nodeId does not match action semantics";
            return false;
        }
        if (action.surfaceId.empty() || action.eventId == 0 || action.expectedRevision == 0 ||
            (action.nodeId && *action.nodeId == 0)) {
            error = L"action.invoke identifiers must be nonzero";
            return false;
        }
        if (!ParseActionValue(root, action, error)) return false;
        if (action.action == L"setText" && action.text.size() > Ipc::kMaxStringChars) {
            error = L"setText value exceeds limit";
            return false;
        }
        return true;
    } catch (...) {
        error = L"invalid action.invoke JSON";
        return false;
    }
}

bool ParseHeartbeat(
    std::string_view payload,
    std::wstring_view nonce,
    std::wstring& error) noexcept {
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"heartbeat", error)) return false;
        if (!HasExpectedNonce(root, nonce, L"heartbeat", error)) return false;
        uint64_t sentAt = 0;
        if (!Ipc::TryParseUInt64(root.GetNamedString(L"sentAt"), sentAt)) {
            error = L"invalid heartbeat timestamp";
            return false;
        }
        return true;
    } catch (...) {
        error = L"invalid heartbeat JSON";
        return false;
    }
}

bool ParseErrorMessage(
    std::string_view payload,
    std::wstring_view nonce,
    std::wstring& error,
    bool* fatal,
    std::wstring* surfaceId) noexcept {
    // Default to session scope: an error we cannot fully classify must not be
    // downgraded to a recoverable one.
    if (fatal) *fatal = true;
    if (surfaceId) surfaceId->clear();
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"error", error) ||
            !HasExpectedNonce(root, nonce, L"error", error)) {
            return false;
        }
        const auto code = root.GetNamedString(L"code");
        const auto detail = root.GetNamedString(L"detail");
        const bool isFatal = root.GetNamedBoolean(L"fatal");
        if (code.empty() || code.size() > 64 || detail.size() > Ipc::kMaxStringChars) {
            error = L"invalid error payload";
            return false;
        }
        std::wstring scopedSurfaceId;
        if (root.HasKey(L"surfaceId")) {
            const auto surface = root.GetNamedValue(L"surfaceId");
            if (surface.ValueType() != JsonValueType::Null &&
                surface.ValueType() != JsonValueType::String) {
                error = L"invalid error surfaceId";
                return false;
            }
            if (surface.ValueType() == JsonValueType::String) {
                scopedSurfaceId = surface.GetString();
            }
        }
        // Publish scope only once the payload is fully validated, so a rejected
        // message can never downgrade a session fault to a recoverable one.
        if (fatal) *fatal = isFatal;
        if (surfaceId) *surfaceId = std::move(scopedSurfaceId);
        error = L"renderer error " + std::wstring(code) + L": " + std::wstring(detail);
        return true;
    } catch (...) {
        error = L"invalid error JSON";
        return false;
    }
}

bool ParseShutdownMessage(
    std::string_view payload,
    std::wstring_view nonce,
    std::wstring& error) noexcept {
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"shutdown", error) ||
            !HasExpectedNonce(root, nonce, L"shutdown", error)) {
            return false;
        }
        const auto reason = root.GetNamedString(L"reason");
        if (reason.empty() || reason.size() > 128) {
            error = L"invalid shutdown reason";
            return false;
        }
        return true;
    } catch (...) {
        error = L"invalid shutdown JSON";
        return false;
    }
}

bool ValidateActionForSnapshot(
    const ActionRequest& action,
    const WindowSnapshot& snapshot,
    std::wstring& error) noexcept {
    if (action.action == L"menuCommand") {
        if (action.nodeId || action.menuCommandId == 0) {
            error = L"menuCommand identity is invalid";
            return false;
        }
        const auto contains = [&](const auto& self,
                                  const std::vector<MenuItemSnapshot>& items) -> bool {
            for (const auto& item : items) {
                if (item.kind == MenuItemKind::Command &&
                    item.commandId == action.menuCommandId)
                    return item.enabled;
                if (self(self, item.items)) return true;
            }
            return false;
        };
        if (contains(contains, snapshot.menu)) return true;
        error = L"menuCommand references an unknown or disabled command";
        return false;
    }
    if (!action.nodeId) return true;
    const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [&](const ControlNode& node) {
        return node.nodeId == *action.nodeId;
    });
    if (found == snapshot.nodes.end()) {
        error = L"action references an unknown node";
        return false;
    }
    const auto& node = *found;
    if (!node.visible || !node.enabled) {
        error = L"action references a non-interactive node";
        return false;
    }
    if (!snapshot.adapterId.empty() &&
        std::find(node.supportedActions.begin(), node.supportedActions.end(),
            action.action) == node.supportedActions.end()) {
        error = L"application-adapter node does not admit the requested action";
        return false;
    }
    if (action.action == L"invoke") {
        if (node.kind == ControlKind::Button || node.kind == ControlKind::SysLink) return true;
        error = L"invoke requires a button or SysLink node";
        return false;
    }
    if (action.action == L"setText") {
        if ((node.kind == ControlKind::Edit || node.kind == ControlKind::Password ||
             (node.kind == ControlKind::ComboBox && node.editable)) &&
            !node.readOnly && action.text.size() <= Ipc::kMaxStringChars) {
            return true;
        }
        error = L"setText requires a writable edit or editable ComboBox node";
        return false;
    }
    if (action.action == L"setCheck") {
        const bool check = node.kind == ControlKind::CheckBox &&
            action.integerValue >= 0 && action.integerValue <= 1;
        const bool threeState = node.kind == ControlKind::ThreeState &&
            action.integerValue >= 0 && action.integerValue <= 2;
        const bool radio = node.kind == ControlKind::RadioButton && action.integerValue == 1;
        if (check || threeState || radio) return true;
        error = L"setCheck value is invalid for the node kind";
        return false;
    }
    if (action.action == L"select") {
        const bool tab = node.kind == ControlKind::TabControl;
        const bool selectable = node.kind == ControlKind::ComboBox ||
            node.kind == ControlKind::ListBox || tab;
        const bool inRange = action.integerValue >= (tab ? 0 : -1) &&
            (action.integerValue == -1 ||
                static_cast<size_t>(action.integerValue) < node.items.size());
        if (selectable && inRange) return true;
        error = L"select value is invalid for the node";
        return false;
    }
    if (action.action == L"setSelection") {
        if (node.kind != ControlKind::ListView ||
            (!node.multiSelect && action.integerValues.size() > 1)) {
            error = L"setSelection requires a compatible ListView node";
            return false;
        }
        if (std::any_of(action.integerValues.begin(), action.integerValues.end(),
                [&](int index) { return index < 0 ||
                    static_cast<size_t>(index) >= node.rows.size(); })) {
            error = L"setSelection index is outside the ListView";
            return false;
        }
        return true;
    }
    if (action.action == L"setItemCheck") {
        if (node.kind != ControlKind::ListView || !node.checkBoxes) {
            error = L"setItemCheck requires a checkbox-enabled ListView node";
            return false;
        }
        if (action.itemIndex < 0 ||
            static_cast<size_t>(action.itemIndex) >= node.rows.size()) {
            error = L"setItemCheck index is outside the ListView";
            return false;
        }
        return true;
    }
    if (action.action == L"toolbarCommand") {
        if (node.kind != ControlKind::Toolbar) {
            error = L"toolbarCommand requires a ToolbarWindow32 node";
            return false;
        }
        const auto item = std::find_if(node.toolbarItems.begin(), node.toolbarItems.end(),
            [&](const ToolbarItemSnapshot& candidate) {
                return candidate.kind == ToolbarItemKind::PushButton &&
                    candidate.commandId == action.menuCommandId;
            });
        if (item == node.toolbarItems.end() || !item->enabled || item->hidden) {
            error = L"toolbarCommand references an unknown, disabled, or hidden push button";
            return false;
        }
        return true;
    }
    error = L"nodeId is not valid for this action";
    return false;
}

bool IsRequestSemanticAction(std::wstring_view action) noexcept {
    // Geometry is latest-wins: the pointer, not a snapshot revision, is the truth
    // for where the window is.  Revision-gating move/resize turns every frame of a
    // drag that raced a reconcile capture into a stale rejection plus a full
    // resync, so these carry the same request semantics as invoke/close and are
    // rebased onto the current revision by HandleNativeAction.
    return action == L"invoke" || action == L"toolbarCommand" || action == L"close" ||
        action == L"move" || action == L"resize";
}

} // namespace FluentShell::Bridge::Translation
