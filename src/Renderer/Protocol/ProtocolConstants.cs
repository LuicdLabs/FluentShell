namespace FluentShell.Renderer.Protocol;

public static class ProtocolConstants
{
    public const uint Magic = 0x48534C46; // "FLSH" in little-endian byte order.
    public const ushort Major = 1;
    // Writers emit this minor; same-major readers accept any ushort minor and ignore unknown JSON fields.
    // Minor 1 introduces the two-stage surface interaction gate; minor 2 adds
    // structural DS_CONTROL dialog containers and parent graph semantics; minor 3
    // adds bounded, owned staticIcon BGRA payloads; minor 4 adds explicit report
    // ListView column-header visibility; minor 5 adds bounded report ListView
    // checkbox state and mutation; minor 6 adds bounded textual TabControl
    // headers, native item geometry, and semantic selection; minor 7 adds bounded
    // ToolbarWindow32 push buttons, separators, owned icons, and command actions;
    // minor 8 adds exact application-adapter semantic metadata and virtual nodes;
    // minor 9 adds the second DirectUI application profile (RecoveryDrive first
    // page) with projected native checkbox toggles; minor 10 adds its canonical
    // native-backed explanatory text; minor 11 adds the fail-closed
    // capability-derived DirectUI semantic adapter contract; minor 12 adds
    // explicit determinate/marquee ProgressBar state.
    public const ushort Minor = 12;
    public const string GenericDirectUiAdapterId = "microsoft.windows.directui.semantic.v1";
    public const string GenericDirectUiPageId = "semantic-v1";
    public const int HeaderSize = 32;
    public const int MaxPayloadBytes = 4 * 1024 * 1024;
    public const int MaxDepth = 32;
    public const int MaxStringChars = 65_536;
    public const int MaxNodes = 512;
    public const int MaxItems = 4_096;
    public const int MaxTabItems = 128;
    public const int MaxToolbarItems = 64;
    public const int MaxColumns = 64;
    public const int MaxPatchOperations = 1_024;
    public const int MaxMenuItems = 256;
    public const int MaxMenuDepth = 8;
    public const int MaxImageDimension = 96;
    public const int MaxImageBytes = MaxImageDimension * MaxImageDimension * 4;
    public const int MaxImageBase64Chars = (MaxImageBytes + 2) / 3 * 4;
}

public enum FrameMessageType : ushort
{
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
}

public static class MessageTypeNames
{
    public static string FromFrameType(FrameMessageType type) => type switch
    {
        FrameMessageType.Hello => "hello",
        FrameMessageType.WindowOpen => "window.open",
        FrameMessageType.WindowPatch => "window.patch",
        FrameMessageType.ActionInvoke => "action.invoke",
        FrameMessageType.ActionResult => "action.result",
        FrameMessageType.SurfaceReady => "surface.ready",
        FrameMessageType.SurfaceCommit => "surface.commit",
        FrameMessageType.WindowClose => "window.close",
        FrameMessageType.Heartbeat => "heartbeat",
        FrameMessageType.Error => "error",
        FrameMessageType.Shutdown => "shutdown",
        _ => throw new ProtocolException($"Unknown frame message type {(ushort)type}.")
    };

    public static FrameMessageType ToFrameType(string name) => name switch
    {
        "hello" => FrameMessageType.Hello,
        "window.open" => FrameMessageType.WindowOpen,
        "window.patch" => FrameMessageType.WindowPatch,
        "action.invoke" => FrameMessageType.ActionInvoke,
        "action.result" => FrameMessageType.ActionResult,
        "surface.ready" => FrameMessageType.SurfaceReady,
        "surface.commit" => FrameMessageType.SurfaceCommit,
        "window.close" => FrameMessageType.WindowClose,
        "heartbeat" => FrameMessageType.Heartbeat,
        "error" => FrameMessageType.Error,
        "shutdown" => FrameMessageType.Shutdown,
        _ => throw new ProtocolException($"Unknown payload message type '{name}'.")
    };
}

public sealed class ProtocolException(string message) : IOException(message);

/// <summary>
/// The Bridge closed the pipe at a frame boundary.  This is how an ordinary session
/// ends -- the target process exited, taking the pipe with it -- and is distinct from
/// a frame that was cut in half, which is a genuine truncation.
/// </summary>
public sealed class PipeClosedException() : IOException("The Bridge closed the pipe at a frame boundary.");
