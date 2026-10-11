using System.Text.Json;

namespace FluentShell.Renderer.Protocol;

/// <summary>One counter's line of a System Monitor graph (minor 36).</summary>
public sealed record PerformanceSeriesSnapshot
{
    /// <summary>The counter's path, which is also its accessible name.</summary>
    public string Name { get; init; } = string.Empty;
    /// <summary>0xRRGGBB.</summary>
    public long Color { get; init; }
    /// <summary>Pixels (1..8).</summary>
    public int Width { get; init; } = 1;
    /// <summary>solid, dash, dot, dashdot or dashdotdot.</summary>
    public string Style { get; init; } = "solid";
    /// <summary>The factor the control multiplies the counter's value by before it plots it.</summary>
    public double Scale { get; init; } = 1;
    public bool Visible { get; init; } = true;
    /// <summary>The counter the legend has selected, which the graph draws emphasised.</summary>
    public bool Highlighted { get; init; }
    /// <summary>One entry per graph slot in drawing order; null where the counter had no sample.</summary>
    public List<double?> Values { get; init; } = [];
}

/// <summary>
/// Performance Monitor's chart as data (minor 36): the control's scale, grid and counters, and the
/// samples the Bridge was told about, laid out in the slots the control draws them in.
/// </summary>
public sealed record PerformanceGraphSnapshot
{
    /// <summary>line or histogram.</summary>
    public string DisplayType { get; init; } = "line";
    public double Minimum { get; init; }
    public double Maximum { get; init; } = 100;
    /// <summary>How many slots the graph's width stands for.</summary>
    public int SampleCount { get; init; } = 100;
    public int IntervalMs { get; init; } = 1000;
    /// <summary>True when the chart scrolls (newest sample in the last slot); false when it wraps.</summary>
    public bool Scroll { get; init; }
    /// <summary>A wrapping chart's slot of the newest sample, where its time bar stands; -1 otherwise.</summary>
    public int BarSlot { get; init; } = -1;
    /// <summary>
    /// A wrapping chart that has written past its last slot: the slots after the bar hold the previous
    /// pass's samples, which are one pass older than the slots before it.
    /// </summary>
    public bool Wrapped { get; init; }
    /// <summary>
    /// The time of slot 0's sample (in the pass in progress) and of the last slot's, in milliseconds
    /// since the Unix epoch. The axis labels the time of the sample each slot holds.
    /// </summary>
    public double StartTimeMs { get; init; }
    public double EndTimeMs { get; init; }
    public bool ShowVerticalGrid { get; init; }
    public bool ShowHorizontalGrid { get; init; }
    public bool ShowScaleLabels { get; init; } = true;
    public bool ShowTimeLabels { get; init; } = true;
    public string Title { get; init; } = string.Empty;
    public string YAxisLabel { get; init; } = string.Empty;
    /// <summary>The plot's own background.</summary>
    public long BackColor { get; init; } = 0xFFFFFF;
    /// <summary>The control's background around the plot, behind the labels.</summary>
    public long ControlColor { get; init; } = 0xF0F0F0;
    public long GridColor { get; init; } = 0xC0C0C0;
    public long TextColor { get; init; }
    public long BarColor { get; init; } = 0xFF0000;
    public List<PerformanceSeriesSnapshot> Series { get; init; } = [];
}

public sealed record PerformanceLegendColumnSnapshot
{
    public string Title { get; init; } = string.Empty;
    /// <summary>Native pixels.</summary>
    public int Width { get; init; }
}

public sealed record PerformanceLegendRowSnapshot
{
    public bool Visible { get; init; } = true;
    public long Color { get; init; }
    public int Width { get; init; } = 1;
    public string Style { get; init; } = "solid";
    public bool Selected { get; init; }
    /// <summary>Scale, Counter, Instance, Parent, Object, Computer (the cells after Show and Color).</summary>
    public List<string> Cells { get; init; } = [];
}

/// <summary>The legend under the graph: one row per counter (minor 36).</summary>
public sealed record PerformanceLegendSnapshot
{
    /// <summary>line (swatches are lines) or histogram (swatches are solid bars).</summary>
    public string DisplayType { get; init; } = "line";
    public List<PerformanceLegendColumnSnapshot> Columns { get; init; } = [];
    public List<PerformanceLegendRowSnapshot> Rows { get; init; } = [];
}

public sealed record PerformanceValueFieldSnapshot
{
    public string Label { get; init; } = string.Empty;
    public string Value { get; init; } = string.Empty;
}

/// <summary>The strip under the graph: Last, Average, Minimum, Maximum and Duration (minor 36).</summary>
public sealed record PerformanceValueBarSnapshot
{
    public List<PerformanceValueFieldSnapshot> Fields { get; init; } = [];
}

internal static class PerformanceProtocol
{
    internal const int MaxSeries = 64;
    internal const int MaxSamples = 2000;
    internal const int MaxLegendColumns = 16;
    internal const int MaxLegendRows = 256;
    internal const int MaxValueFields = 8;
    internal const int MaxText = 1024;

    private static void Require(bool valid, string reason)
    {
        if (!valid) throw new ProtocolException($"Performance chart {reason}.");
    }

    private static bool Finite(double value, double low, double high) =>
        double.IsFinite(value) && value >= low && value <= high;

    private static bool Text(string? value) =>
        value is not null && value.Length <= MaxText && !value.Contains('\0');

    private static bool Style(string? value) =>
        value is "solid" or "dash" or "dot" or "dashdot" or "dashdotdot";

    public static void ValidateGraph(ControlNode node)
    {
        Require(node.PerformanceGraph is not null && node.NativeHwnd is not null,
            "graph requires an HWND-backed node with its state");
        var graph = node.PerformanceGraph!;
        Require(graph.DisplayType is "line" or "histogram", "graph type is unknown");
        Require(Finite(graph.Minimum, -1e15, 1e15) && Finite(graph.Maximum, -1e15, 1e15) &&
            graph.Maximum > graph.Minimum, "scale is not a finite range");
        Require(graph.SampleCount is >= 2 and <= MaxSamples, "sample count is outside its range");
        Require(graph.IntervalMs is >= 100 and <= 3_600_000, "update interval is outside its range");
        Require(graph.BarSlot >= -1 && graph.BarSlot < graph.SampleCount &&
            (graph.Scroll ? graph.BarSlot == -1 : true), "time bar is outside the graph's slots");
        Require(!graph.Wrapped || (!graph.Scroll && graph.BarSlot >= 0), "only a wrapping chart with a time bar has wrapped");
        Require(Finite(graph.StartTimeMs, 0, 253_402_300_799_999) && Finite(graph.EndTimeMs, 0, 253_402_300_799_999),
            "axis time is outside its range");
        Require(Text(graph.Title) && Text(graph.YAxisLabel), "title or axis label is not bounded text");
        Require(graph.BackColor is >= 0 and <= 0xFFFFFF && graph.ControlColor is >= 0 and <= 0xFFFFFF &&
            graph.GridColor is >= 0 and <= 0xFFFFFF && graph.TextColor is >= 0 and <= 0xFFFFFF &&
            graph.BarColor is >= 0 and <= 0xFFFFFF, "colour is outside 0xRRGGBB");
        Require(graph.Series is { Count: <= MaxSeries }, "series count exceeds its bound");
        foreach (var series in graph.Series)
        {
            Require(series is not null && Text(series.Name) && series.Values is not null, "series is malformed");
            var source = series!;
            var values = source.Values!;
            Require(source.Color is >= 0 and <= 0xFFFFFF, "series colour is outside 0xRRGGBB");
            Require(source.Width is >= 1 and <= 8, "series width is outside its range");
            Require(Style(source.Style), "series line style is unknown");
            Require(Finite(source.Scale, -1e9, 1e9), "series scale is outside its range");
            Require(values.Count <= graph.SampleCount, "series carries more samples than the graph shows");
            foreach (var value in values)
                Require(value is null || Finite(value.Value, -1e300, 1e300), "sample is not a finite number");
        }
    }

    public static void ValidateLegend(ControlNode node)
    {
        Require(node.PerformanceLegend is not null && node.NativeHwnd is not null,
            "legend requires an HWND-backed node with its state");
        var legend = node.PerformanceLegend!;
        Require(legend.DisplayType is "line" or "histogram", "legend chart type is unknown");
        Require(legend.Columns is { Count: > 0 and <= MaxLegendColumns }, "legend column count is outside its range");
        foreach (var column in legend.Columns)
            Require(column is not null && Text(column.Title) && column.Width is >= 0 and <= 16_384,
                "legend column is malformed");
        Require(legend.Rows is { Count: <= MaxLegendRows }, "legend row count exceeds its bound");
        var selected = 0;
        foreach (var row in legend.Rows)
        {
            Require(row is not null && row.Cells is not null, "legend row is null");
            var source = row!;
            var cells = source.Cells!;
            Require(source.Color is >= 0 and <= 0xFFFFFF && source.Width is >= 1 and <= 8 && Style(source.Style),
                "legend swatch is malformed");
            // The cells that follow Show and Color are one per remaining column.
            Require(cells.Count == legend.Columns.Count - 2 && cells.All(Text),
                "legend row cells do not match its columns");
            if (source.Selected) ++selected;
        }
        Require(selected <= 1, "legend selects more than one row");
    }

    public static void ValidateValueBar(ControlNode node)
    {
        Require(node.PerformanceValueBar is not null && node.NativeHwnd is not null,
            "value bar requires an HWND-backed node with its state");
        var fields = node.PerformanceValueBar!.Fields;
        Require(fields is { Count: > 0 and <= MaxValueFields }, "value bar field count is outside its range");
        foreach (var field in fields)
            Require(field is not null && Text(field.Label) && Text(field.Value), "value bar field is malformed");
    }

    public static void RequireFields(JsonElement node, string facet, params string[] fields)
    {
        Require(node.TryGetProperty(facet, out var content) && content.ValueKind == JsonValueKind.Object,
            "state is missing");
        foreach (var field in fields)
            Require(content.TryGetProperty(field, out _), $"raw field '{field}' is missing");
    }
}
