using System.Text.Json;

namespace FluentShell.Renderer.Protocol;

/// <summary>One run of a read-only rich edit control's text: a constant character format.</summary>
public sealed record RichRunSnapshot
{
    public string Text { get; init; } = string.Empty;
    public bool Bold { get; init; }
    public bool Italic { get; init; }
    public bool Underline { get; init; }
    public bool Strike { get; init; }
    /// <summary>Points.</summary>
    public double Size { get; init; }
    /// <summary>0xRRGGBB, or -1 for the control's automatic text colour.</summary>
    public long Color { get; init; } = -1;
}

/// <summary>A paragraph of runs. Indents and spacing are in points.</summary>
public sealed record RichParagraphSnapshot
{
    public string Alignment { get; init; } = "left";
    public bool Bullet { get; init; }
    public double LeftIndent { get; init; }
    public double FirstLineIndent { get; init; }
    public double SpaceBefore { get; init; }
    public double SpaceAfter { get; init; }
    public List<RichRunSnapshot> Runs { get; init; } = [];
}

/// <summary>
/// A read-only rich edit control drawn in its formats (minor 35): paragraphs of runs, each run
/// one character format, read by the Bridge through the text object model.
/// </summary>
public sealed record RichTextSnapshot
{
    public List<RichParagraphSnapshot> Paragraphs { get; init; } = [];
}

internal static class RichTextProtocol
{
    internal const int MaxParagraphs = 512;
    internal const int MaxRunsPerParagraph = 128;
    internal const int MaxRuns = 1024;
    internal const int MaxCharacters = 65_536;

    private static void Require(bool valid, string reason)
    {
        if (!valid) throw new ProtocolException($"Rich text {reason}.");
    }

    private static bool Finite(double value, double low, double high) =>
        double.IsFinite(value) && value >= low && value <= high;

    public static void Validate(ControlNode node)
    {
        Require(node.RichText is not null && node.NativeHwnd is not null && node.ReadOnly == true,
            "requires a read-only HWND-backed text control");
        var paragraphs = node.RichText!.Paragraphs;
        Require(paragraphs is { Count: <= MaxParagraphs }, "paragraph count exceeds its bound");
        var runs = 0;
        long characters = 0;
        foreach (var paragraph in paragraphs)
        {
            Require(paragraph is not null && paragraph.Runs is not null, "paragraph is null");
            var source = paragraph!;
            Require(source.Alignment is "left" or "center" or "right" or "justify",
                "paragraph alignment is unknown");
            Require(Finite(source.LeftIndent, -2000, 2000) && Finite(source.FirstLineIndent, -2000, 2000) &&
                Finite(source.SpaceBefore, 0, 2000) && Finite(source.SpaceAfter, 0, 2000),
                "paragraph indent or spacing is outside its range");
            var sourceRuns = source.Runs!;
            Require(sourceRuns.Count <= MaxRunsPerParagraph, "run count exceeds its bound");
            foreach (var run in sourceRuns)
            {
                Require(run is not null && run.Text is { Length: > 0 and <= MaxCharacters } &&
                    !run.Text.Contains('\0'), "run text is empty or exceeds its bound");
                var value = run!;
                Require(Finite(value.Size, 1, 400), "run size is outside its range");
                Require(value.Color is >= -1 and <= 0xFFFFFF, "run colour is outside 0xRRGGBB");
                characters += value.Text.Length;
                Require(++runs <= MaxRuns && characters <= MaxCharacters, "content exceeds its aggregate bound");
            }
        }
    }

    public static void RequireFields(JsonElement node)
    {
        JsonElement paragraphs = default;
        Require(node.TryGetProperty("richText", out var content) && content.ValueKind == JsonValueKind.Object &&
            content.TryGetProperty("paragraphs", out paragraphs) && paragraphs.ValueKind == JsonValueKind.Array,
            "state is missing");
        foreach (var paragraph in paragraphs.EnumerateArray())
        {
            Require(paragraph.ValueKind == JsonValueKind.Object &&
                new[] { "alignment", "bullet", "leftIndent", "firstLineIndent", "spaceBefore", "spaceAfter", "runs" }
                    .All(field => paragraph.TryGetProperty(field, out _)), "paragraph raw field is missing");
            var runs = paragraph.GetProperty("runs");
            Require(runs.ValueKind == JsonValueKind.Array, "runs must be an array");
            foreach (var run in runs.EnumerateArray())
                Require(run.ValueKind == JsonValueKind.Object &&
                    new[] { "text", "bold", "italic", "underline", "strike", "size", "color" }
                        .All(field => run.TryGetProperty(field, out _)), "run raw field is missing");
        }
    }
}
