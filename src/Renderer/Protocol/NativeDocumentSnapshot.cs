using System.Globalization;
using System.Text.Json;

namespace FluentShell.Renderer.Protocol;

public sealed record NativeDocumentRegion
{
    public string Key { get; init; } = string.Empty;
    public string Kind { get; init; } = string.Empty;
    public PixelRect Rect { get; init; } = new();
    public int ContentWidth { get; init; }
    public int ContentHeight { get; init; }
    public int MaximumX { get; init; }
    public int MaximumY { get; init; }
    public int OffsetX { get; init; }
    public int OffsetY { get; init; }
}

public sealed record NativeReportRow
{
    public string Path { get; init; } = string.Empty;
    public double Value { get; init; }
    public bool Available { get; init; }
}

public sealed record NativeDocumentItem
{
    public string Key { get; init; } = string.Empty;
    public string Kind { get; init; } = string.Empty;
    public PixelRect Rect { get; init; } = new();
    public int RegionIndex { get; init; } = -1;
    public int ReportRegionIndex { get; init; } = -1;
    public string Name { get; init; } = string.Empty;
    public string ActionIdentity { get; init; } = string.Empty;
    public bool Enabled { get; init; }
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public ImageListEntry? Image { get; init; }
    public List<NativeReportRow> ReportRows { get; init; } = [];
}

public sealed record NativeDocumentSnapshot
{
    public string DocumentId { get; init; } = string.Empty;
    public List<NativeDocumentRegion> Regions { get; init; } = [];
    public List<NativeDocumentItem> Items { get; init; } = [];
}

public sealed record NativeDocumentScrollActionValue
{
    public int Index { get; init; }
    public string DocumentId { get; init; } = string.Empty;
    public string Key { get; init; } = string.Empty;
    public int X { get; init; }
    public int Y { get; init; }
}

public sealed record NativeDocumentInvokeActionValue
{
    public int Index { get; init; }
    public string DocumentId { get; init; } = string.Empty;
    public string Key { get; init; } = string.Empty;
    public string ActionIdentity { get; init; } = string.Empty;
}

internal static class NativeDocumentProtocol
{
    private static void Require(bool value, string message)
    {
        if (!value) throw new ProtocolException($"Native document {message}.");
    }

    private static void Text(string value, bool empty = false) => Require(
        value is not null && (empty || value.Length > 0) &&
        value.Length <= ProtocolConstants.MaxStringChars && !value.Contains('\0'), "text exceeds its bound");

    private static void Bounds(PixelRect rect, int width, int height) => Require(
        rect.X >= 0 && rect.Y >= 0 && rect.Width > 0 && rect.Height > 0 &&
        (long)rect.X + rect.Width <= width && (long)rect.Y + rect.Height <= height,
        "element escapes its containing viewport");

    public static void ValidateAction(string action, JsonElement value)
    {
        Require(value.ValueKind == JsonValueKind.Object, "action requires an object");
        var invoke = action == "htmlInvoke";
        Require(value.EnumerateObject().Count() == (invoke ? 4 : 5) &&
            value.TryGetProperty("index", out var index) && index.TryGetInt32(out var parsed) &&
            parsed >= 0 && parsed < (invoke ? 256 : 16), "action index is invalid");
        foreach (var key in new[] { "documentId", "key" })
        {
            Require(value.TryGetProperty(key, out var member) && member.ValueKind == JsonValueKind.String,
                "action target identity is missing");
            Text(value.GetProperty(key).GetString()!);
        }
        if (invoke)
            Require(value.TryGetProperty("actionIdentity", out var identity) && identity.ValueKind == JsonValueKind.String &&
                ulong.TryParse(identity.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out var token) &&
                token != 0 && token.ToString(CultureInfo.InvariantCulture) == identity.GetString(), "action token is invalid");
        else
            foreach (var axis in new[] { "x", "y" })
                Require(value.TryGetProperty(axis, out var offset) && offset.TryGetInt32(out var number) &&
                    number is >= 0 and <= ProtocolConstants.MaxCoordinate, "action scroll offset is invalid");
    }

    public static void Validate(ControlNode node)
    {
        Require(node.Document is not null && node.NativeHwnd is not null &&
            !node.TabStop && node.TabIndex is null or -1, "requires an inert HWND-backed host");
        var document = node.Document!;
        Text(document.DocumentId);
        Require(document.Items.Count is > 0 and <= 256 && document.Regions.Count <= 16,
            "collection count exceeds its bound");
        var regionKeys = new HashSet<string>(StringComparer.Ordinal);
        for (var index = 0; index < document.Regions.Count; ++index)
        {
            var region = document.Regions[index];
            Text(region.Key);
            Require(regionKeys.Add(region.Key) && region.Kind is "html" or "report", "scrollport identity is invalid");
            Bounds(region.Rect, node.Rect.Width, node.Rect.Height);
            Require(region.MaximumX is >= 0 and <= ProtocolConstants.MaxCoordinate &&
                region.MaximumY is >= 0 and <= ProtocolConstants.MaxCoordinate &&
                region.OffsetX >= 0 && region.OffsetX <= region.MaximumX &&
                region.OffsetY >= 0 && region.OffsetY <= region.MaximumY,
                "scroll offsets exceed the canonical range");
            if (region.Kind == "html")
                Require(region.ContentWidth >= region.Rect.Width && region.ContentWidth <= ProtocolConstants.MaxCoordinate &&
                    region.ContentHeight >= region.Rect.Height && region.ContentHeight <= ProtocolConstants.MaxCoordinate &&
                    region.MaximumX == region.ContentWidth - region.Rect.Width &&
                    region.MaximumY == region.ContentHeight - region.Rect.Height,
                    "HTML content extent disagrees with its scroll range");
            else
                Require(region.ContentWidth == 0 && region.ContentHeight == 0 &&
                    region.MaximumX <= ushort.MaxValue && region.MaximumY <= ushort.MaxValue,
                    "report scroll range exceeds the standard native contract");
        }

        var itemKeys = new HashSet<string>(StringComparer.Ordinal);
        var reportRegions = new HashSet<int>();
        var textLength = 0L;
        var pixelBytes = 0L;
        var reportRows = 0;
        foreach (var item in document.Items)
        {
            Text(item.Key);
            Text(item.Name, empty: true);
            Text(item.ActionIdentity, empty: true);
            textLength += item.Name.Length;
            Require(itemKeys.Add(item.Key) && item.Kind is "text" or "heading" or "link" or "image" or "report",
                "item identity or kind is invalid");
            Require(item.RegionIndex >= -1 && item.RegionIndex < document.Regions.Count,
                "item names an absent scrollport");
            if (item.RegionIndex < 0)
                Bounds(item.Rect, node.Rect.Width, node.Rect.Height);
            else
            {
                var region = document.Regions[item.RegionIndex];
                Require(region.Kind == "html", "ordinary item names a report scrollport");
                Bounds(item.Rect, region.ContentWidth, region.ContentHeight);
            }
            if (item.Kind == "link")
                Require(ulong.TryParse(item.ActionIdentity, NumberStyles.None, CultureInfo.InvariantCulture, out var action) &&
                    action != 0 && action.ToString(CultureInfo.InvariantCulture) == item.ActionIdentity,
                    "link has no canonical action identity");
            else
                Require(item.ActionIdentity.Length == 0, "inert item carries an action");
            if (item.Kind is "text" or "heading") Text(item.Name);
            if (item.Image is { } image)
            {
                Require(item.Kind is "image" or "link", "non-image item carries pixels");
                Require(image.ImageWidth is > 0 and <= ProtocolConstants.MaxImageDimension &&
                    image.ImageHeight is > 0 and <= ProtocolConstants.MaxImageDimension &&
                    image.ImageFormat == "bgra8-premultiplied", "image shape is invalid");
                byte[] pixels;
                try { pixels = Convert.FromBase64String(image.ImageData); }
                catch (FormatException) { throw new ProtocolException("Native document image base64 is invalid."); }
                Require(pixels.Length == image.ImageWidth * image.ImageHeight * 4, "image byte count is invalid");
                for (var pixel = 0; pixel < pixels.Length; pixel += 4)
                    Require(pixels[pixel] <= pixels[pixel + 3] && pixels[pixel + 1] <= pixels[pixel + 3] &&
                        pixels[pixel + 2] <= pixels[pixel + 3], "image is not premultiplied");
                pixelBytes += pixels.Length;
            }
            else
                Require(item.Kind != "image" && (item.Kind != "link" || !string.IsNullOrWhiteSpace(item.Name)),
                    "unnamed graphical item has no pixels");
            if (item.Kind == "report")
            {
                Require(item.RegionIndex == -1 && item.ReportRegionIndex >= 0 &&
                    item.ReportRegionIndex < document.Regions.Count && reportRegions.Add(item.ReportRegionIndex),
                    "report names an invalid or shared scrollport");
                var region = document.Regions[item.ReportRegionIndex];
                Require(region.Kind == "report" && region.Key == item.Key && region.Rect == item.Rect,
                    "report viewport differs from its scrollport");
                reportRows += item.ReportRows.Count;
                foreach (var row in item.ReportRows)
                {
                    Text(row.Path);
                    textLength += row.Path.Length;
                    Require(double.IsFinite(row.Value) && row.Value >= 0 && (row.Available || row.Value == 0),
                        "report sample is not a finite canonical value");
                }
            }
            else
                Require(item.ReportRegionIndex == -1 && item.ReportRows.Count == 0,
                    "ordinary item carries report data");
        }
        Require(reportRows <= 256 && textLength <= 65536 && pixelBytes <= 1024 * 1024,
            "aggregate data exceeds its bound");
        Require(reportRegions.Count == document.Regions.Count(region => region.Kind == "report"),
            "contains an unrepresented report scrollport");
    }

    public static void RequireFields(JsonElement node)
    {
        static JsonElement Member(JsonElement value, string name, JsonValueKind kind)
        {
            if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty(name, out var member) || member.ValueKind != kind)
                throw new ProtocolException($"Native document requires {name} ({kind}).");
            return member;
        }
        static void Fields(JsonElement value, params string[] names)
        {
            foreach (var name in names)
                if (value.ValueKind != JsonValueKind.Object || !value.TryGetProperty(name, out _))
                    throw new ProtocolException($"Native document requires {name}.");
        }
        var document = Member(node, "document", JsonValueKind.Object);
        Member(document, "documentId", JsonValueKind.String);
        foreach (var region in Member(document, "regions", JsonValueKind.Array).EnumerateArray())
        {
            Fields(region, "key", "kind", "rect", "contentWidth", "contentHeight", "maximumX", "maximumY", "offsetX", "offsetY");
            Fields(Member(region, "rect", JsonValueKind.Object), "x", "y", "width", "height");
        }
        foreach (var item in Member(document, "items", JsonValueKind.Array).EnumerateArray())
        {
            Fields(item, "key", "kind", "rect", "regionIndex", "reportRegionIndex", "name", "actionIdentity", "enabled", "reportRows");
            Fields(Member(item, "rect", JsonValueKind.Object), "x", "y", "width", "height");
            if (item.TryGetProperty("image", out var image))
                Fields(image, "imageWidth", "imageHeight", "imageFormat", "imageData");
            foreach (var row in Member(item, "reportRows", JsonValueKind.Array).EnumerateArray())
                Fields(row, "path", "value", "available");
        }
    }
}
