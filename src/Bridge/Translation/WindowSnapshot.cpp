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
    result.Insert(L"nativeHwnd", JsonValue::CreateStringValue(Ipc::HwndToString(node.hwnd)));
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
    result.Insert(L"selectionStart", JsonValue::CreateNumberValue(node.selectionStart));
    result.Insert(L"selectionLength", JsonValue::CreateNumberValue(node.selectionLength));
    result.Insert(L"readOnly", JsonValue::CreateBooleanValue(node.readOnly));
    result.Insert(L"multiline", JsonValue::CreateBooleanValue(node.multiline));
    result.Insert(L"isDefault", JsonValue::CreateBooleanValue(node.isDefault));
    result.Insert(L"groupStart", JsonValue::CreateBooleanValue(node.groupStart));
    JsonArray items;
    for (const auto& item : node.items) {
        items.Append(JsonValue::CreateStringValue(item));
    }
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
    if (action.action == L"setCheck" || action.action == L"select") {
        if (value.ValueType() != JsonValueType::Number) {
            error = L"setCheck/select requires an integer value";
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
    case ControlKind::Separator: return L"separator";
    case ControlKind::Button: return L"button";
    case ControlKind::CheckBox: return L"checkBox";
    case ControlKind::ThreeState: return L"threeState";
    case ControlKind::RadioButton: return L"radioButton";
    case ControlKind::Edit: return L"edit";
    case ControlKind::Password: return L"password";
    case ControlKind::ComboBox: return L"comboBox";
    case ControlKind::ListBox: return L"listBox";
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
    bool show) {
    JsonObject root;
    root.Insert(L"messageType", JsonValue::CreateStringValue(L"surface.commit"));
    root.Insert(L"sessionNonce", JsonValue::CreateStringValue(nonce));
    root.Insert(L"surfaceId", JsonValue::CreateStringValue(surfaceId));
    root.Insert(L"revision", JsonValue::CreateStringValue(Ipc::UInt64ToString(revision)));
    root.Insert(L"show", JsonValue::CreateBooleanValue(show));
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
            L"move", L"resize", L"minimize", L"maximize", L"restore", L"close"
        };
        if (std::find(std::begin(kActions), std::end(kActions), action.action) == std::end(kActions)) {
            error = L"unknown action.invoke action";
            return false;
        }
        const bool requiresNode = action.action == L"invoke" || action.action == L"setText" ||
            action.action == L"setCheck" || action.action == L"select";
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
    std::wstring& error) noexcept {
    try {
        if (!ValidateJsonLimits(payload, error)) return false;
        const auto root = ParseJson(payload);
        if (!RequiredMessage(root, L"error", error) ||
            !HasExpectedNonce(root, nonce, L"error", error)) {
            return false;
        }
        const auto code = root.GetNamedString(L"code");
        const auto detail = root.GetNamedString(L"detail");
        (void)root.GetNamedBoolean(L"fatal");
        if (code.empty() || code.size() > 64 || detail.size() > Ipc::kMaxStringChars) {
            error = L"invalid error payload";
            return false;
        }
        if (root.HasKey(L"surfaceId")) {
            const auto surface = root.GetNamedValue(L"surfaceId");
            if (surface.ValueType() != JsonValueType::Null &&
                surface.ValueType() != JsonValueType::String) {
                error = L"invalid error surfaceId";
                return false;
            }
        }
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
    if (action.action == L"invoke") {
        if (node.kind == ControlKind::Button) return true;
        error = L"invoke requires a button node";
        return false;
    }
    if (action.action == L"setText") {
        if ((node.kind == ControlKind::Edit || node.kind == ControlKind::Password) &&
            !node.readOnly && action.text.size() <= Ipc::kMaxStringChars) {
            return true;
        }
        error = L"setText requires a writable edit node";
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
        const bool selectable = node.kind == ControlKind::ComboBox ||
            node.kind == ControlKind::ListBox;
        const bool inRange = action.integerValue >= -1 &&
            (action.integerValue == -1 ||
                static_cast<size_t>(action.integerValue) < node.items.size());
        if (selectable && inRange) return true;
        error = L"select value is invalid for the node";
        return false;
    }
    error = L"nodeId is not valid for this action";
    return false;
}

bool IsRequestSemanticAction(std::wstring_view action) noexcept {
    return action == L"invoke" || action == L"close";
}

} // namespace FluentShell::Bridge::Translation
