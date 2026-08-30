using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

namespace FluentShell.Renderer.Protocol;

/// <summary>
/// Binds protocol messages to and from their wire JSON.  Admission rules live in
/// <see cref="ProtocolValidator"/>; this type only decides how bytes become
/// typed messages, and runs the validator on both directions.
/// </summary>
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
        ProtocolValidator.ValidateCommon(message);
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
            var root = document.RootElement;
            ProtocolValidator.ValidateJsonTree(root);
            RequireDeclaredType(root, frameType);
            ProtocolValidator.ValidateRequiredFields(frameType, root);

            var message = Bind(frameType, root);
            ProtocolValidator.ValidateCommon(message);
            ProtocolValidator.ValidateSemanticCaps(message);
            return message;
        }
        catch (JsonException exception)
        {
            throw new ProtocolException($"Invalid JSON payload: {exception.Message}");
        }
    }

    // The frame header and the payload must agree, so a peer cannot smuggle one
    // message shape inside another frame type.
    private static void RequireDeclaredType(JsonElement root, FrameMessageType frameType)
    {
        if (!root.TryGetProperty("messageType", out var declared) ||
            declared.ValueKind != JsonValueKind.String)
        {
            throw new ProtocolException("Payload is missing messageType.");
        }
        if (!string.Equals(declared.GetString() ?? string.Empty,
                MessageTypeNames.FromFrameType(frameType), StringComparison.Ordinal))
        {
            throw new ProtocolException("Frame type does not match payload messageType.");
        }
    }

    private static IProtocolMessage Bind(FrameMessageType frameType, JsonElement root)
    {
        var raw = root.GetRawText();
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

        // System.Text.Json source generation initializes an omitted bool to false
        // even when the record property has a true initializer.  An older Bridge
        // has no interaction-gate field and means "show and hand off input", so
        // that wire behavior is restored explicitly.
        if (message is SurfaceCommitMessage legacyCommit &&
            !root.TryGetProperty("interactive", out _))
        {
            message = legacyCommit with { Interactive = true };
        }
        return message;
    }

    private static JsonTypeInfo ResolveTypeInfo(Type type) =>
        Options.GetTypeInfo(type) ?? throw new ProtocolException($"No JSON contract for {type.Name}.");

    /// <summary>
    /// Parses a canonical unsigned 64-bit decimal string: no sign, no leading
    /// zero, no separators.  Revisions and IDs travel as strings so a JSON reader
    /// cannot silently round them through a double.
    /// </summary>
    public static ulong ParseCanonicalUInt64(string value, string field)
    {
        if (string.IsNullOrEmpty(value) || (value.Length > 1 && value[0] == '0') ||
            !ulong.TryParse(value, System.Globalization.NumberStyles.None,
                System.Globalization.CultureInfo.InvariantCulture, out var parsed))
        {
            throw new ProtocolException($"{field} is not a canonical unsigned 64-bit decimal string.");
        }
        return parsed;
    }
}
