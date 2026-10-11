using System.Globalization;
using System.Text.Json;

namespace FluentShell.Renderer.Protocol;

public sealed record DiskViewItemSnapshot
{
    public string ItemId { get; init; } = string.Empty;
    public string DiskId { get; init; } = string.Empty;
    public string Kind { get; init; } = string.Empty;
    public PixelRect Rect { get; init; } = new();
    public string Name { get; init; } = string.Empty;
    public string Value { get; init; } = string.Empty;
    public bool Enabled { get; init; }
    public bool Selected { get; init; }
    public bool Focused { get; init; }
}

public sealed record DiskViewSnapshot
{
    public string Name { get; init; } = string.Empty;
    public PixelRect ClientRect { get; init; } = new();
    public PixelRect ScrollRect { get; init; } = new();
    public int ScrollMinimum { get; init; }
    public int ScrollMaximum { get; init; }
    public int ScrollPosition { get; init; }
    public int ScrollPage { get; init; }
    public List<DiskViewItemSnapshot> Items { get; init; } = [];
}
public sealed record DiskViewItemActionValue
{
    public string ItemId { get; init; } = string.Empty;
}
public sealed record DiskViewScrollActionValue
{
    public int Position { get; init; }
}

internal static class DiskViewProtocol
{
    private static void Require(bool valid, string reason)
    {
        if (!valid) throw new ProtocolException($"Disk view {reason}.");
    }
    internal static uint ItemId(string? id)
    {
        Require(uint.TryParse(id, NumberStyles.None, CultureInfo.InvariantCulture, out var number) &&
            number is > 0 and <= int.MaxValue && number.ToString(CultureInfo.InvariantCulture) == id,
            "item ID is not a positive canonical MSAA child ID");
        return number;
    }
    private static void Text(string? value) => Require(value is { Length: > 0 and <= 65536 } &&
        !value.Contains('\0'), "text is empty or exceeds its bound");
    private static void Rect(PixelRect? rect, bool empty = false)
    {
        Require(rect is not null && Math.Abs((long)rect.X) <= ProtocolConstants.MaxCoordinate &&
            Math.Abs((long)rect.Y) <= ProtocolConstants.MaxCoordinate &&
            rect.Width >= (empty ? 0 : 1) && rect.Height >= (empty ? 0 : 1) &&
            rect.Width <= ProtocolConstants.MaxCoordinate && rect.Height <= ProtocolConstants.MaxCoordinate,
            "rectangle exceeds the coordinate bound");
    }
    private static void Contained(PixelRect rect, PixelRect parent) => Require(
        rect.X >= 0 && rect.Y >= 0 && (long)rect.X + rect.Width <= parent.Width &&
        (long)rect.Y + rect.Height <= parent.Height, "viewport escapes the native host");

    public static void Validate(ControlNode node)
    {
        Require(node.DiskView is not null && node.NativeHwnd is not null && !node.TabStop &&
            node.TabIndex is null or -1, "requires an HWND-backed composite host");
        var view = node.DiskView!;
        Text(view.Name);
        Rect(view.ClientRect); Contained(view.ClientRect, node.Rect);
        Rect(view.ScrollRect, empty: true); Contained(view.ScrollRect, node.Rect);
        Require(view.ScrollMinimum >= 0 && view.ScrollMaximum >= view.ScrollMinimum &&
            view.ScrollMaximum <= 65535 && view.ScrollPosition >= view.ScrollMinimum &&
            view.ScrollPosition <= view.ScrollMaximum && view.ScrollPage is >= 0 and <= 65535,
            "scroll range exceeds the native thumb-position contract");
        Require(view.ScrollMaximum == view.ScrollMinimum ||
            view.ScrollRect.Width > 0 && view.ScrollRect.Height > 0, "scrollable view has no native scrollbar");
        Require(view.Items is { Count: > 0 and <= 256 }, "table item count exceeds its bound");
        var ids = new HashSet<uint>();
        uint currentDisk = 0;
        var selected = 0;
        long textSize = view.Name.Length;
        foreach (var item in view.Items!)
        {
            Require(item is not null, "item is null");
            var id = ItemId(item!.ItemId);
            var disk = ItemId(item.DiskId);
            Require(ids.Add(id) && item.Kind is "disk" or "partition", "item identity or role is invalid");
            if (item.Kind == "disk") currentDisk = id;
            Require(currentDisk != 0 && disk == currentDisk, "partition does not follow its native disk header");
            Rect(item.Rect); Text(item.Name); Text(item.Value);
            if (item.Selected) Require(++selected <= 1, "single-selection table has multiple selected items");
            textSize += item.Name.Length + item.Value.Length;
            Require(textSize <= 256 * 1024, "table text exceeds its aggregate bound");
        }
    }
    public static void ValidateAction(string action, JsonElement value)
    {
        Require(value.ValueKind == JsonValueKind.Object && value.EnumerateObject().Count() == 1,
            "action requires exactly one target field");
        if (action == "diskScroll")
            Require(value.TryGetProperty("position", out var position) && position.TryGetInt32(out var number) &&
                number is >= 0 and <= 65535, "scroll action position is invalid");
        else
        {
            Require(value.TryGetProperty("itemId", out var id) && id.ValueKind == JsonValueKind.String,
                "action item ID is missing");
            ItemId(value.GetProperty("itemId").GetString());
        }
    }
    public static void RequireFields(JsonElement node)
    {
        Require(node.TryGetProperty("diskView", out var view) && view.ValueKind == JsonValueKind.Object,
            "state is missing");
        void Fields(JsonElement item, params string[] fields)
        {
            Require(item.ValueKind == JsonValueKind.Object && fields.All(field => item.TryGetProperty(field, out _)),
                "required raw field is missing");
        }
        Fields(view, "name", "clientRect", "scrollRect", "scrollMinimum", "scrollMaximum", "scrollPosition", "scrollPage", "items");
        foreach (var rect in new[] { "clientRect", "scrollRect" }) Fields(view.GetProperty(rect), "x", "y", "width", "height");
        var items = view.GetProperty("items");
        Require(items.ValueKind == JsonValueKind.Array, "items must be an array");
        foreach (var item in items.EnumerateArray())
        {
            Fields(item, "itemId", "diskId", "kind", "rect", "name", "value", "enabled", "selected", "focused");
            Fields(item.GetProperty("rect"), "x", "y", "width", "height");
        }
    }
}
