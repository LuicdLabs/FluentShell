using System.Text.Json;
using System.Text.Json.Serialization;

namespace FluentShell.Renderer.Protocol;

public interface IProtocolMessage
{
    string MessageType { get; }
    string SessionNonce { get; }
}

public sealed record PixelRect
{
    public int X { get; init; }
    public int Y { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
}

public sealed record ToolbarItemSnapshot
{
    public string Kind { get; init; } = string.Empty;
    public int CommandId { get; init; }
    public PixelRect Rect { get; init; } = new();
    public string Text { get; init; } = string.Empty;
    public bool Enabled { get; init; }
    public bool Hidden { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? Checked { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? DropDown { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? WholeDropDown { get; init; }
    public int? ImageWidth { get; init; }
    public int? ImageHeight { get; init; }
    public string? ImageFormat { get; init; }
    public string? ImageData { get; init; }
}

public sealed record ImageListEntry
{
    public int ImageWidth { get; init; }
    public int ImageHeight { get; init; }
    public string ImageFormat { get; init; } = string.Empty;
    public string ImageData { get; init; } = string.Empty;
}

// One splitter between two panes of a container.  `position` is the gap's leading
// edge in the container's own client coordinates.
public sealed record PaneSplit
{
    public bool Vertical { get; init; }
    public int Position { get; init; }
    public int Thickness { get; init; }
    public int Minimum { get; init; }
    public int Maximum { get; init; }
}

// One band a container paints itself, reproduced from the pixels the native window
// drew.  `Rect` is in the container's own client coordinates.
public sealed record ChromeRegion
{
    public PixelRect Rect { get; init; } = new();
    public int ImageWidth { get; init; }
    public int ImageHeight { get; init; }
    public string ImageFormat { get; init; } = string.Empty;
    public string ImageData { get; init; } = string.Empty;
}

// One element of an accessible island: content its host window owns without giving it
// an HWND.  `ActionName` is the provider's own accDefaultAction string, which is both
// the label of the action and the contract the projection drives it by.
public sealed record AccessibleIslandItem
{
    public string Kind { get; init; } = string.Empty;
    public PixelRect Rect { get; init; } = new();
    public string Name { get; init; } = string.Empty;
    public string Description { get; init; } = string.Empty;
    public string ActionName { get; init; } = string.Empty;
    public bool Enabled { get; init; }
    public bool DropDown { get; init; }
}

public sealed record ControlNode
{
    public string NodeId { get; init; } = "0";
    public string Generation { get; init; } = "0";
    public string? NativeHwnd { get; init; } = "0x0";
    public string? ParentNodeId { get; init; }
    public string Kind { get; init; } = string.Empty;
    public int ControlId { get; init; }
    public int ZIndex { get; init; }
    // Added additively within protocol v1. Older same-major peers omit it and
    // the renderer falls back to ZIndex.
    public int? TabIndex { get; init; }
    public PixelRect Rect { get; init; } = new();
    public string? Style { get; init; }
    public string? ExStyle { get; init; }
    public bool Visible { get; init; }
    public bool Enabled { get; init; }
    public bool TabStop { get; init; }
    public uint DialogCode { get; init; }
    public string Text { get; init; } = string.Empty;
    public string? AutomationName { get; init; }
    public int? Checked { get; init; }
    public int? SelectedIndex { get; init; }
    public List<int> SelectedIndices { get; init; } = [];
    public int? FocusedIndex { get; init; }
    public bool MultiSelect { get; init; }
    public int? SelectionStart { get; init; }
    public int? SelectionLength { get; init; }
    public bool? ReadOnly { get; init; }
    public bool? Multiline { get; init; }
    public bool Editable { get; init; }
    public bool? IsDefault { get; init; }
    public bool? GroupStart { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? Active { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? WindowState { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public PixelRect? ClientRect { get; init; }
    public int? Minimum { get; init; }
    public int? Maximum { get; init; }
    public int? Position { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? Indeterminate { get; init; }
    public int? SmallChange { get; init; }
    public int? LargeChange { get; init; }
    public bool Vertical { get; init; }
    public bool Reversed { get; init; }
    public List<string> Items { get; init; } = [];
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<PixelRect>? ItemRects { get; init; }
    public List<string> Columns { get; init; } = [];
    public List<int> ColumnWidths { get; init; } = [];
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<int>? ColumnOrder { get; init; }
    public List<List<string>> Rows { get; init; } = [];
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? ColumnHeadersVisible { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? CheckBoxes { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<int>? CheckedIndices { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<int>? ItemDepths { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<bool>? ItemExpanded { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<bool>? ItemHasChildren { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<ImageListEntry>? ImageList { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<int>? ItemImages { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<int>? ItemSelectedImages { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? EditableLabels { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? EditingIndex { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? ImageWidth { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? ImageHeight { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? ImageFormat { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? ImageData { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<ToolbarItemSnapshot>? ToolbarItems { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<PaneSplit>? Splits { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<ChromeRegion>? ChromeRegions { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<AccessibleIslandItem>? IslandItems { get; init; }
    public string? AdapterId { get; init; }
    public string? PageId { get; init; }
    public string? SemanticKey { get; init; }
    public string? SourceKind { get; init; }
    public string? PresentationVariant { get; init; }
    public List<string>? SupportedActions { get; init; }
    public string? HelpText { get; init; }
    public string? AccessKey { get; init; }
}

public sealed record ListViewCheckActionValue
{
    [JsonPropertyName("index")]
    public int Index { get; init; }
    [JsonPropertyName("checked")]
    public bool Checked { get; init; }
}

public sealed record TreeExpandActionValue
{
    [JsonPropertyName("index")]
    public int Index { get; init; }
    [JsonPropertyName("expanded")]
    public bool Expanded { get; init; }
}

public sealed record ItemTextActionValue
{
    [JsonPropertyName("index")]
    public int Index { get; init; }
    [JsonPropertyName("text")]
    public string Text { get; init; } = string.Empty;
}

// A projected splitter drag: which split of the container moved, and where its
// leading edge now sits in the container's own client coordinates.
public sealed record SplitActionValue
{
    [JsonPropertyName("index")]
    public int Index { get; init; }
    [JsonPropertyName("position")]
    public int Position { get; init; }
}

// The five caption verbs a projected MDI child can route back to its native
// frame.  They travel as names rather than codes so the wire stays readable.
public static class MdiCommands
{
    public const string Activate = "activate";
    public const string Close = "close";
    public const string Minimize = "minimize";
    public const string Maximize = "maximize";
    public const string Restore = "restore";

    public static readonly string[] All = [Activate, Close, Minimize, Maximize, Restore];
}

public sealed record MenuItemSnapshot
{
    public string ItemId { get; init; } = string.Empty;
    public string Kind { get; init; } = string.Empty;
    public string Text { get; init; } = string.Empty;
    public int CommandId { get; init; }
    public bool Enabled { get; init; }
    public bool Checked { get; init; }
    public bool Radio { get; init; }
    public bool IsDefault { get; init; }
    public List<MenuItemSnapshot> Items { get; init; } = [];
}

public sealed record WindowSnapshot
{
    public Guid SurfaceId { get; init; }
    public string SurfaceKind { get; init; } = "window";
    public bool Modal { get; init; }
    public bool CanCancel { get; init; }
    public string Icon { get; init; } = "none";
    public string Generation { get; init; } = "0";
    public string Revision { get; init; } = "0";
    public string NativeHwnd { get; init; } = "0x0";
    public string? OwnerHwnd { get; init; }
    public string Title { get; init; } = string.Empty;
    public int Dpi { get; init; } = 96;
    public PixelRect Bounds { get; init; } = new();
    public PixelRect ClientBounds { get; init; } = new();
    public string WindowStyle { get; init; } = "0x0";
    public string WindowExStyle { get; init; } = "0x0";
    public bool Visible { get; init; }
    public bool Enabled { get; init; }
    public string State { get; init; } = "normal";
    public bool ShowInTaskbar { get; init; }
    public bool Rtl { get; init; }
    public List<MenuItemSnapshot> Menu { get; init; } = [];
    public List<ControlNode> Nodes { get; init; } = [];
    public string? AdapterId { get; init; }
    public string? PageId { get; init; }
}

public sealed record HelloMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "hello";
    public string SessionNonce { get; init; } = string.Empty;
    public string Role { get; init; } = string.Empty;
    public uint ProcessId { get; init; }
    public string ProcessCreated { get; init; } = "0";
    public ushort ProtocolMajor { get; init; }
    public ushort ProtocolMinor { get; init; }
}

public sealed record WindowOpenMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "window.open";
    public string SessionNonce { get; init; } = string.Empty;
    public WindowSnapshot Window { get; init; } = new();
}

public sealed record PatchOperation
{
    public string Op { get; init; } = string.Empty;
    public string? NodeId { get; init; }
    public string Property { get; init; } = string.Empty;
    public JsonElement Value { get; init; }
    public string? EventId { get; init; }
}

public sealed record WindowPatchMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "window.patch";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid SurfaceId { get; init; }
    public string BaseRevision { get; init; } = "0";
    public string Revision { get; init; } = "0";
    public string? EventId { get; init; }
    public List<PatchOperation> Operations { get; init; } = [];
    public WindowSnapshot? Snapshot { get; init; }
}

public sealed record ActionInvokeMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "action.invoke";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid SurfaceId { get; init; }
    public string? NodeId { get; init; }
    public string EventId { get; init; } = "0";
    public string ExpectedRevision { get; init; } = "0";
    public string Action { get; init; } = string.Empty;
    public JsonElement Value { get; init; }
}

public sealed record ActionResultMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "action.result";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid SurfaceId { get; init; }
    public string EventId { get; init; } = "0";
    public string Status { get; init; } = string.Empty;
    public string Revision { get; init; } = "0";
    public string? Reason { get; init; }
    public WindowSnapshot? Snapshot { get; init; }
}

public sealed record SurfaceReadyMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "surface.ready";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid SurfaceId { get; init; }
    public string Revision { get; init; } = "0";
    public string ProxyHwnd { get; init; } = "0x0";
    public PixelRect Bounds { get; init; } = new();
    public int NodeCount { get; init; }
    public bool UiaReady { get; init; }
}

public sealed record SurfaceCommitMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "surface.commit";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid SurfaceId { get; init; }
    public string Revision { get; init; } = "0";
    public bool Show { get; init; }
    // The Bridge first shows a surface while its committed UIA isolation gate
    // is running, then sends a second commit with Interactive=true.  Keeping
    // this optional preserves compatibility with older peers.
    public bool Interactive { get; init; } = true;
}

public sealed record WindowCloseMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "window.close";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid SurfaceId { get; init; }
    public string Reason { get; init; } = string.Empty;
}

public sealed record HeartbeatMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "heartbeat";
    public string SessionNonce { get; init; } = string.Empty;
    public string SentAt { get; init; } = "0";
}

public sealed record ErrorMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "error";
    public string SessionNonce { get; init; } = string.Empty;
    public Guid? SurfaceId { get; init; }
    public string Code { get; init; } = string.Empty;
    public string Detail { get; init; } = string.Empty;
    public bool Fatal { get; init; }
}

public sealed record ShutdownMessage : IProtocolMessage
{
    public string MessageType { get; init; } = "shutdown";
    public string SessionNonce { get; init; } = string.Empty;
    public string Reason { get; init; } = string.Empty;
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(HelloMessage))]
[JsonSerializable(typeof(WindowOpenMessage))]
[JsonSerializable(typeof(WindowPatchMessage))]
[JsonSerializable(typeof(ActionInvokeMessage))]
[JsonSerializable(typeof(ActionResultMessage))]
[JsonSerializable(typeof(SurfaceReadyMessage))]
[JsonSerializable(typeof(SurfaceCommitMessage))]
[JsonSerializable(typeof(WindowCloseMessage))]
[JsonSerializable(typeof(HeartbeatMessage))]
[JsonSerializable(typeof(ErrorMessage))]
[JsonSerializable(typeof(ShutdownMessage))]
internal partial class ProtocolJsonContext : JsonSerializerContext;
