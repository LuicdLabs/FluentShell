using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

namespace FluentShell.Renderer.Protocol;

public static class ProtocolSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        MaxDepth = ProtocolConstants.MaxDepth,
        TypeInfoResolver = ProtocolJsonContext.Default,
    };

    public static byte[] Serialize(IProtocolMessage message)
    {
        ValidateCommon(message);
        var typeInfo = ResolveTypeInfo(message.GetType());
        var payload = JsonSerializer.SerializeToUtf8Bytes(message, typeInfo);
        if (payload.Length > ProtocolConstants.MaxPayloadBytes)
        {
            throw new ProtocolException("Serialized payload exceeds the protocol cap.");
        }
        return payload;
    }

    public static IProtocolMessage Deserialize(FrameMessageType frameType, ReadOnlySpan<byte> payload)
    {
        if (payload.Length > ProtocolConstants.MaxPayloadBytes)
        {
            throw new ProtocolException("Payload exceeds the protocol cap.");
        }

        try
        {
            using var document = JsonDocument.Parse(payload.ToArray(), new JsonDocumentOptions
            {
                MaxDepth = ProtocolConstants.MaxDepth,
                CommentHandling = JsonCommentHandling.Disallow,
                AllowTrailingCommas = false,
            });
            ValidateJsonTree(document.RootElement);
            if (!document.RootElement.TryGetProperty("messageType", out var messageTypeElement) ||
                messageTypeElement.ValueKind != JsonValueKind.String)
            {
                throw new ProtocolException("Payload is missing messageType.");
            }
            var messageType = messageTypeElement.GetString() ?? string.Empty;
            if (!string.Equals(messageType, MessageTypeNames.FromFrameType(frameType), StringComparison.Ordinal))
            {
                throw new ProtocolException("Frame type does not match payload messageType.");
            }
            ValidateRequiredFields(frameType, document.RootElement);

            var raw = document.RootElement.GetRawText();
            IProtocolMessage? message = frameType switch
            {
                FrameMessageType.Hello => (IProtocolMessage?)JsonSerializer.Deserialize<HelloMessage>(raw, Options),
                FrameMessageType.WindowOpen => JsonSerializer.Deserialize<WindowOpenMessage>(raw, Options),
                FrameMessageType.WindowPatch => JsonSerializer.Deserialize<WindowPatchMessage>(raw, Options),
                FrameMessageType.ActionInvoke => JsonSerializer.Deserialize<ActionInvokeMessage>(raw, Options),
                FrameMessageType.ActionResult => JsonSerializer.Deserialize<ActionResultMessage>(raw, Options),
                FrameMessageType.SurfaceReady => JsonSerializer.Deserialize<SurfaceReadyMessage>(raw, Options),
                FrameMessageType.SurfaceCommit => JsonSerializer.Deserialize<SurfaceCommitMessage>(raw, Options),
                FrameMessageType.WindowClose => JsonSerializer.Deserialize<WindowCloseMessage>(raw, Options),
                FrameMessageType.Heartbeat => JsonSerializer.Deserialize<HeartbeatMessage>(raw, Options),
                FrameMessageType.Error => JsonSerializer.Deserialize<ErrorMessage>(raw, Options),
                FrameMessageType.Shutdown => JsonSerializer.Deserialize<ShutdownMessage>(raw, Options),
                _ => null,
            } ?? throw new ProtocolException("Payload deserialized to null.");
            ValidateCommon(message);
            ValidateSemanticCaps(message);
            return message;
        }
        catch (JsonException exception)
        {
            throw new ProtocolException($"Invalid JSON payload: {exception.Message}");
        }
    }

    private static JsonTypeInfo ResolveTypeInfo(Type type) =>
        Options.GetTypeInfo(type) ?? throw new ProtocolException($"No JSON contract for {type.Name}.");

    private static void ValidateCommon(IProtocolMessage message)
    {
        if (MessageTypeNames.ToFrameType(message.MessageType) is var type &&
            !string.Equals(message.MessageType, MessageTypeNames.FromFrameType(type), StringComparison.Ordinal))
        {
            throw new ProtocolException("Non-canonical messageType.");
        }
        if (string.IsNullOrEmpty(message.SessionNonce) ||
            message.SessionNonce.Length != 32 || !message.SessionNonce.All(Uri.IsHexDigit))
        {
            throw new ProtocolException("sessionNonce must contain exactly 32 hexadecimal characters.");
        }
    }

    private static void ValidateSemanticCaps(IProtocolMessage message)
    {
        switch (message)
        {
            case HelloMessage hello:
                if (hello.ProcessId == 0 || hello.ProtocolMajor != ProtocolConstants.Major || hello.Role is not ("bridge" or "renderer"))
                    throw new ProtocolException("Invalid hello identity or protocol fields.");
                ParseCanonicalUInt64(hello.ProcessCreated, "processCreated");
                break;
            case WindowOpenMessage open:
                ValidateSnapshot(open.Window);
                break;
            case WindowPatchMessage patch:
                if (patch.SurfaceId == Guid.Empty) throw new ProtocolException("window.patch surfaceId cannot be empty.");
                var patchBase = ParseCanonicalUInt64(patch.BaseRevision, "baseRevision");
                var patchRevision = ParseCanonicalUInt64(patch.Revision, "revision");
                if (patchRevision < patchBase)
                    throw new ProtocolException("window.patch revision precedes baseRevision.");
                if (patch.EventId is not null) ParseCanonicalUInt64(patch.EventId, "eventId");
                if (patch.Operations.Count > ProtocolConstants.MaxPatchOperations)
                {
                    throw new ProtocolException("Patch operation count exceeds the protocol cap.");
                }
                if (patch.Snapshot is not null && patch.Operations.Count != 0)
                    throw new ProtocolException("A full-resync patch must have an empty operations array.");
                if (patch.Snapshot is not null)
                {
                    ValidateSnapshot(patch.Snapshot);
                    if (patch.Snapshot.SurfaceId != patch.SurfaceId ||
                        ParseCanonicalUInt64(patch.Snapshot.Revision, "snapshot.revision") != patchRevision)
                        throw new ProtocolException("Full-resync snapshot identity or revision differs from window.patch.");
                }
                foreach (var operation in patch.Operations)
                {
                    if (operation.Op is not ("replace" or "add" or "remove") || string.IsNullOrWhiteSpace(operation.Property))
                        throw new ProtocolException("Invalid patch operation.");
                    if (operation.NodeId is not null) ParseCanonicalUInt64(operation.NodeId, "nodeId");
                    if (operation.EventId is not null) ParseCanonicalUInt64(operation.EventId, "eventId");
                }
                break;
            case ActionInvokeMessage action:
                if (action.SurfaceId == Guid.Empty) throw new ProtocolException("action.invoke surfaceId cannot be empty.");
                ParseCanonicalUInt64(action.EventId, "eventId");
                ParseCanonicalUInt64(action.ExpectedRevision, "expectedRevision");
                if (action.NodeId is not null) ParseCanonicalUInt64(action.NodeId, "nodeId");
                var requiresNode = action.Action is "invoke" or "setText" or "setCheck" or "select";
                if (requiresNode != (action.NodeId is not null))
                    throw new ProtocolException("action.invoke nodeId does not match action semantics.");
                switch (action.Action)
                {
                    case "setText" when action.Value.ValueKind != JsonValueKind.String:
                        throw new ProtocolException("setText requires a string value.");
                    case "setCheck" or "select" when action.Value.ValueKind != JsonValueKind.Number || !action.Value.TryGetInt32(out _):
                        throw new ProtocolException("setCheck/select requires an integer value.");
                    case "move" or "resize":
                        if (action.Value.ValueKind != JsonValueKind.Object ||
                            !action.Value.TryGetProperty("x", out var x) || !x.TryGetInt32(out _) ||
                            !action.Value.TryGetProperty("y", out var y) || !y.TryGetInt32(out _) ||
                            !action.Value.TryGetProperty("width", out var width) || !width.TryGetInt32(out var widthValue) || widthValue < 0 ||
                            !action.Value.TryGetProperty("height", out var height) || !height.TryGetInt32(out var heightValue) || heightValue < 0)
                            throw new ProtocolException("move/resize requires complete integer bounds.");
                        break;
                    case "activate" or "invoke" or "minimize" or "maximize" or "restore" or "close"
                        when action.Value.ValueKind != JsonValueKind.Null:
                        throw new ProtocolException("Request action requires a null value.");
                    case "activate" or "invoke" or "setText" or "setCheck" or "select" or
                         "move" or "resize" or "minimize" or "maximize" or "restore" or "close":
                        break;
                    default:
                        throw new ProtocolException("Unknown action.invoke action.");
                }
                break;
            case ActionResultMessage result:
                if (result.SurfaceId == Guid.Empty) throw new ProtocolException("action.result surfaceId cannot be empty.");
                ParseCanonicalUInt64(result.EventId, "eventId");
                var resultRevision = ParseCanonicalUInt64(result.Revision, "revision");
                if (result.Status is not ("accepted" or "rejected" or "stale" or "closeRejected"))
                    throw new ProtocolException("Unknown action.result status.");
                if (result.Snapshot is not null)
                {
                    ValidateSnapshot(result.Snapshot);
                    if (result.Snapshot.SurfaceId != result.SurfaceId ||
                        ParseCanonicalUInt64(result.Snapshot.Revision, "snapshot.revision") != resultRevision)
                        throw new ProtocolException("action.result snapshot identity or revision differs from its result.");
                }
                break;
            case SurfaceReadyMessage ready:
                if (ready.SurfaceId == Guid.Empty) throw new ProtocolException("surface.ready surfaceId cannot be empty.");
                ParseCanonicalUInt64(ready.Revision, "revision");
                ParseCanonicalHex64(ready.ProxyHwnd, "proxyHwnd");
                ValidateRect(ready.Bounds, "bounds");
                if (ready.NodeCount is < 0 or > ProtocolConstants.MaxNodes)
                    throw new ProtocolException("surface.ready nodeCount is outside the protocol cap.");
                break;
            case SurfaceCommitMessage commit:
                if (commit.SurfaceId == Guid.Empty) throw new ProtocolException("surface.commit surfaceId cannot be empty.");
                ParseCanonicalUInt64(commit.Revision, "revision");
                break;
            case WindowCloseMessage close:
                if (close.SurfaceId == Guid.Empty) throw new ProtocolException("window.close surfaceId cannot be empty.");
                if (close.Reason is not ("nativeDestroyed" or "unsupported" or "restore" or "shutdown"))
                    throw new ProtocolException("Unknown window close reason.");
                break;
            case ShutdownMessage shutdown:
                if (string.IsNullOrWhiteSpace(shutdown.Reason)) throw new ProtocolException("Shutdown reason is required.");
                break;
            case HeartbeatMessage heartbeat:
                ParseCanonicalUInt64(heartbeat.SentAt, "sentAt");
                break;
            case ErrorMessage error:
                if (string.IsNullOrWhiteSpace(error.Code) || error.Code.Length > 64)
                    throw new ProtocolException("error code is required and must fit the protocol cap.");
                if (error.Detail is null)
                    throw new ProtocolException("error detail is required.");
                if (error.SurfaceId == Guid.Empty)
                    throw new ProtocolException("error surfaceId cannot be empty when supplied.");
                break;
        }
    }

    private static void ValidateSnapshot(WindowSnapshot snapshot)
    {
        if (snapshot.SurfaceId == Guid.Empty) throw new ProtocolException("surfaceId cannot be empty.");
        if (snapshot.Dpi is < 48 or > 960) throw new ProtocolException("Snapshot DPI is outside the protocol range.");
        if (snapshot.SurfaceKind is not ("window" or "messageBox" or "taskDialog")) throw new ProtocolException("Unknown surface kind.");
        if (snapshot.Icon is not ("none" or "warning" or "error" or "info" or "question" or "shield")) throw new ProtocolException("Unknown surface icon.");
        if (snapshot.State is not ("normal" or "minimized" or "maximized")) throw new ProtocolException("Unknown window state.");
        ValidateRect(snapshot.Bounds, "bounds");
        ValidateRect(snapshot.ClientBounds, "clientBounds");
        if (snapshot.Nodes is null || snapshot.Nodes.Count > ProtocolConstants.MaxNodes)
            throw new ProtocolException("Node count exceeds the protocol cap or nodes is null.");
        if (ParseCanonicalUInt64(snapshot.Generation, "generation") == 0 ||
            ParseCanonicalUInt64(snapshot.Revision, "revision") == 0)
            throw new ProtocolException("Snapshot generation and revision must be nonzero.");
        ParseCanonicalHex64(snapshot.NativeHwnd, "nativeHwnd");
        if (snapshot.OwnerHwnd is not null) ParseCanonicalHex64(snapshot.OwnerHwnd, "ownerHwnd");
        ParseCanonicalHex64(snapshot.WindowStyle, "windowStyle");
        ParseCanonicalHex64(snapshot.WindowExStyle, "windowExStyle");
        var nodeIds = new HashSet<string>(StringComparer.Ordinal);
        var tabIndexes = new HashSet<int>();
        foreach (var node in snapshot.Nodes)
        {
            if (ParseCanonicalUInt64(node.NodeId, "nodeId") == 0 ||
                ParseCanonicalUInt64(node.Generation, "generation") == 0 ||
                !nodeIds.Add(node.NodeId))
                throw new ProtocolException("Control node IDs and generations must be unique and nonzero.");
            ParseCanonicalHex64(node.NativeHwnd, "node.nativeHwnd");
            if (node.ParentNodeId is not null) ParseCanonicalUInt64(node.ParentNodeId, "parentNodeId");
            if (node.Style is not null) ParseCanonicalHex64(node.Style, "node.style");
            if (node.ExStyle is not null) ParseCanonicalHex64(node.ExStyle, "node.exStyle");
            if (node.Items is null || node.Items.Count > ProtocolConstants.MaxItems)
                throw new ProtocolException("Item count exceeds the protocol cap or items is null.");
            ValidateRect(node.Rect, "node.rect");
            if (node.Kind is not ("static" or "separator" or "button" or "checkBox" or "threeState" or "radioButton" or "edit" or "password" or "comboBox" or "listBox"))
                throw new ProtocolException("Unknown control node kind.");
            if (node.ZIndex is < 0 or >= ProtocolConstants.MaxNodes ||
                node.TabIndex is < -1 or >= ProtocolConstants.MaxNodes ||
                node.Checked is < 0 or > 2 || node.SelectedIndex is < -1 or > 4095 ||
                node.SelectionStart is < 0 || node.SelectionLength is < 0)
                throw new ProtocolException("Control node state is outside the protocol range.");
            if (node.TabIndex is { } tabIndex)
            {
                if (node.TabStop && node.Enabled)
                {
                    if (tabIndex < 0)
                        throw new ProtocolException($"Focusable control '{node.NodeId}' has negative tabIndex {tabIndex}.");
                    if (!tabIndexes.Add(tabIndex))
                        throw new ProtocolException($"Focusable control '{node.NodeId}' duplicates tabIndex {tabIndex}.");
                }
                else if (!node.TabStop && tabIndex != -1)
                {
                    throw new ProtocolException("Non-tab-stop control must use tabIndex -1.");
                }
            }
        }
    }

    private static void ValidateRect(PixelRect rect, string field)
    {
        if (rect.Width < 0 || rect.Height < 0) throw new ProtocolException($"{field} dimensions cannot be negative.");
    }

    public static ulong ParseCanonicalUInt64(string value, string field)
    {
        if (string.IsNullOrEmpty(value) || (value.Length > 1 && value[0] == '0') ||
            !ulong.TryParse(value, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var parsed))
        {
            throw new ProtocolException($"{field} is not a canonical unsigned 64-bit decimal string.");
        }
        return parsed;
    }

    private static ulong ParseCanonicalHex64(string value, string field)
    {
        if (string.IsNullOrEmpty(value) || value.Length is < 3 or > 18 ||
            !value.StartsWith("0x", StringComparison.Ordinal) ||
            !ulong.TryParse(value.AsSpan(2), System.Globalization.NumberStyles.AllowHexSpecifier,
                System.Globalization.CultureInfo.InvariantCulture, out var parsed))
        {
            throw new ProtocolException($"{field} is not a canonical hexadecimal value.");
        }
        return parsed;
    }

    private static void ValidateRequiredFields(FrameMessageType frameType, JsonElement root)
    {
        RequireProperties(root, "message", "messageType", "sessionNonce");
        switch (frameType)
        {
            case FrameMessageType.Hello:
                RequireProperties(root, "hello", "role", "processId", "processCreated", "protocolMajor", "protocolMinor");
                break;
            case FrameMessageType.WindowOpen:
                RequireProperties(root, "window.open", "window");
                ValidateRequiredSnapshotFields(root.GetProperty("window"), "window.open.window");
                break;
            case FrameMessageType.WindowPatch:
                RequireProperties(root, "window.patch", "surfaceId", "baseRevision", "revision", "operations");
                var operations = RequireKind(root.GetProperty("operations"), JsonValueKind.Array, "window.patch.operations");
                foreach (var operation in operations.EnumerateArray())
                    RequireProperties(operation, "window.patch.operation", "op", "property", "value");
                if (root.TryGetProperty("snapshot", out var patchSnapshot) && patchSnapshot.ValueKind != JsonValueKind.Null)
                    ValidateRequiredSnapshotFields(patchSnapshot, "window.patch.snapshot");
                break;
            case FrameMessageType.ActionInvoke:
                RequireProperties(root, "action.invoke", "surfaceId", "eventId", "expectedRevision", "action", "value");
                break;
            case FrameMessageType.ActionResult:
                RequireProperties(root, "action.result", "surfaceId", "eventId", "status", "revision");
                if (root.TryGetProperty("snapshot", out var resultSnapshot) && resultSnapshot.ValueKind != JsonValueKind.Null)
                    ValidateRequiredSnapshotFields(resultSnapshot, "action.result.snapshot");
                break;
            case FrameMessageType.SurfaceReady:
                RequireProperties(root, "surface.ready", "surfaceId", "revision", "proxyHwnd", "bounds", "nodeCount", "uiaReady");
                ValidateRequiredRectFields(root.GetProperty("bounds"), "surface.ready.bounds");
                break;
            case FrameMessageType.SurfaceCommit:
                RequireProperties(root, "surface.commit", "surfaceId", "revision", "show");
                break;
            case FrameMessageType.WindowClose:
                RequireProperties(root, "window.close", "surfaceId", "reason");
                break;
            case FrameMessageType.Heartbeat:
                RequireProperties(root, "heartbeat", "sentAt");
                break;
            case FrameMessageType.Error:
                RequireProperties(root, "error", "code", "detail", "fatal");
                break;
            case FrameMessageType.Shutdown:
                RequireProperties(root, "shutdown", "reason");
                break;
            default:
                throw new ProtocolException("Unknown frame message type.");
        }
    }

    private static void ValidateRequiredSnapshotFields(JsonElement snapshot, string context)
    {
        RequireProperties(snapshot, context,
            "surfaceId", "surfaceKind", "modal", "canCancel", "icon", "generation", "revision",
            "nativeHwnd", "title", "dpi", "bounds", "clientBounds", "windowStyle", "windowExStyle",
            "visible", "enabled", "state", "showInTaskbar", "rtl", "nodes");
        ValidateRequiredRectFields(snapshot.GetProperty("bounds"), $"{context}.bounds");
        ValidateRequiredRectFields(snapshot.GetProperty("clientBounds"), $"{context}.clientBounds");
        var nodes = RequireKind(snapshot.GetProperty("nodes"), JsonValueKind.Array, $"{context}.nodes");
        foreach (var node in nodes.EnumerateArray())
        {
            RequireProperties(node, $"{context}.node", "nodeId", "generation", "nativeHwnd", "kind",
                "controlId", "zIndex", "rect", "visible", "enabled", "tabStop", "dialogCode", "text", "items");
            ValidateRequiredRectFields(node.GetProperty("rect"), $"{context}.node.rect");
            RequireKind(node.GetProperty("items"), JsonValueKind.Array, $"{context}.node.items");
        }
    }

    private static void ValidateRequiredRectFields(JsonElement rect, string context)
    {
        RequireKind(rect, JsonValueKind.Object, context);
        RequireProperties(rect, context, "x", "y", "width", "height");
    }

    private static JsonElement RequireKind(JsonElement element, JsonValueKind kind, string context)
    {
        if (element.ValueKind != kind)
            throw new ProtocolException($"{context} must be a JSON {kind}.");
        return element;
    }

    private static void RequireProperties(JsonElement element, string context, params string[] names)
    {
        RequireKind(element, JsonValueKind.Object, context);
        foreach (var name in names)
        {
            if (!element.TryGetProperty(name, out _))
                throw new ProtocolException($"{context} is missing required property '{name}'.");
        }
    }

    private static void ValidateJsonTree(JsonElement element)
    {
        switch (element.ValueKind)
        {
            case JsonValueKind.String:
                if ((element.GetString()?.Length ?? 0) > ProtocolConstants.MaxStringChars)
                {
                    throw new ProtocolException("JSON string exceeds the protocol cap.");
                }
                break;
            case JsonValueKind.Array:
                foreach (var child in element.EnumerateArray()) ValidateJsonTree(child);
                break;
            case JsonValueKind.Object:
                foreach (var property in element.EnumerateObject())
                {
                    if (Encoding.UTF8.GetByteCount(property.Name) > 256)
                    {
                        throw new ProtocolException("JSON property name is unreasonably long.");
                    }
                    ValidateJsonTree(property.Value);
                }
                break;
        }
    }
}
