using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

// Minor 36: Performance Monitor's chart is three kinds read from the System Monitor control: the
// graph (scale, grid, counters and their samples in the slots the control draws them), the legend
// (one row per counter) and the value strip (Last, Average, Minimum, Maximum, Duration).
public sealed class PerformanceChartProtocolTests
{
    private static PerformanceGraphSnapshot Graph() => new()
    {
        DisplayType = "line", Minimum = 0, Maximum = 100, SampleCount = 100, IntervalMs = 1000,
        Scroll = false, BarSlot = 2, Wrapped = true, StartTimeMs = 1_760_000_000_000, EndTimeMs = 1_760_000_099_000,
        ShowScaleLabels = true, ShowTimeLabels = true,
        BackColor = 0xFFFFFF, ControlColor = 0xF0F0F0, GridColor = 0x808080, TextColor = 0, BarColor = 0xFF0000,
        Series =
        [
            new PerformanceSeriesSnapshot
            {
                Name = @"\Processor Information(_Total)\% Processor Time", Color = 0xFF0000, Width = 2, Style = "dash",
                Scale = 1, Highlighted = true, Values = [3.5, 4.25, 5, null, 7.125],
            },
            new PerformanceSeriesSnapshot { Name = @"\Memory\Available MBytes", Color = 0x0000FF, Scale = 0.01, Visible = false },
        ],
    };

    private static PerformanceLegendSnapshot Legend() => new()
    {
        Columns =
        [
            new PerformanceLegendColumnSnapshot { Title = "Show", Width = 61 },
            new PerformanceLegendColumnSnapshot { Title = "Color", Width = 61 },
            new PerformanceLegendColumnSnapshot { Title = "Scale", Width = 61 },
            new PerformanceLegendColumnSnapshot { Title = "Counter", Width = 153 },
            new PerformanceLegendColumnSnapshot { Title = "Instance", Width = 76 },
        ],
        Rows =
        [
            new PerformanceLegendRowSnapshot
            {
                Color = 0xFF0000, Width = 1, Selected = true, Cells = ["1.0", "% Processor Time", "_Total"],
            },
            new PerformanceLegendRowSnapshot
            {
                Visible = false, Color = 0x00AA00, Width = 3, Style = "dot", Cells = ["0.01", "Available MBytes", "---"],
            },
        ],
    };

    private static PerformanceValueBarSnapshot ValueBar() => new()
    {
        Fields =
        [
            new PerformanceValueFieldSnapshot { Label = "Last", Value = "2.532" },
            new PerformanceValueFieldSnapshot { Label = "Average", Value = "3.473" },
            new PerformanceValueFieldSnapshot { Label = "Duration", Value = "1:40" },
        ],
    };

    private static ControlNode GraphNode(PerformanceGraphSnapshot? graph, string kind = "performanceGraph") =>
        TestData.Snapshot().Nodes[0] with { Kind = kind, PerformanceGraph = graph };

    private static ControlNode LegendNode(PerformanceLegendSnapshot? legend, string kind = "performanceLegend") =>
        TestData.Snapshot().Nodes[0] with { Kind = kind, PerformanceLegend = legend };

    private static ControlNode ValueBarNode(PerformanceValueBarSnapshot? bar, string kind = "performanceValueBar") =>
        TestData.Snapshot().Nodes[0] with { Kind = kind, PerformanceValueBar = bar };

    private static WindowSnapshot Snapshot(ControlNode node) => TestData.Snapshot() with { Nodes = [node] };

    private static WindowSnapshot RoundTrip(WindowSnapshot snapshot) =>
        Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage { SessionNonce = TestData.Nonce, Window = snapshot }))).Window;

    [Fact]
    public void TheSharedFixtureTheBridgeSideChecksValidatesHere()
    {
        var payload = File.ReadAllBytes(Path.Combine(
            AppContext.BaseDirectory, "ProtocolFixtures", "window.open.performance-chart.json"));
        var message = Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));
        ProtocolValidator.ValidateSnapshot(message.Window);
        Assert.Equal(["performanceGraph", "performanceValueBar", "performanceLegend"],
            message.Window.Nodes.Select(node => node.Kind));
        var graph = message.Window.Nodes[0].PerformanceGraph!;
        Assert.Equal(6, graph.SampleCount);
        Assert.Equal(2, graph.BarSlot);
        Assert.Equal(new double?[] { 3.5, 4.25, null, null, 7.125, null }, graph.Series[0].Values);
        Assert.Equal(5, message.Window.Nodes[1].PerformanceValueBar!.Fields.Count);
        Assert.Equal(2, message.Window.Nodes[2].PerformanceLegend!.Rows.Count);
    }

    [Fact]
    public void AGraphValidatesAndSurvivesTheWire()
    {
        ProtocolValidator.ValidateSnapshot(Snapshot(GraphNode(Graph())));
        var received = Assert.Single(RoundTrip(Snapshot(GraphNode(Graph()))).Nodes).PerformanceGraph!;
        Assert.Equal("line", received.DisplayType);
        Assert.Equal(100, received.SampleCount);
        Assert.Equal(2, received.BarSlot);
        Assert.False(received.Scroll);
        Assert.True(received.Wrapped);
        Assert.Equal(1_760_000_000_000, received.StartTimeMs);
        Assert.Equal(1_760_000_099_000, received.EndTimeMs);
        Assert.Equal(0xF0F0F0, received.ControlColor);
        Assert.Equal(0xFF0000, received.BarColor);
        Assert.Equal(2, received.Series.Count);
        var first = received.Series[0];
        Assert.Equal("dash", first.Style);
        Assert.True(first.Highlighted);
        // A slot with no sample stays a gap rather than becoming a zero.
        Assert.Equal(new double?[] { 3.5, 4.25, 5, null, 7.125 }, first.Values);
        Assert.False(received.Series[1].Visible);
        Assert.Equal(0.01, received.Series[1].Scale);
    }

    [Fact]
    public void ALegendValidatesAndSurvivesTheWire()
    {
        ProtocolValidator.ValidateSnapshot(Snapshot(LegendNode(Legend())));
        var received = Assert.Single(RoundTrip(Snapshot(LegendNode(Legend()))).Nodes).PerformanceLegend!;
        Assert.Equal(5, received.Columns.Count);
        Assert.Equal(153, received.Columns[3].Width);
        Assert.Equal(2, received.Rows.Count);
        Assert.True(received.Rows[0].Selected);
        Assert.False(received.Rows[1].Visible);
        Assert.Equal("dot", received.Rows[1].Style);
        Assert.Equal(["1.0", "% Processor Time", "_Total"], received.Rows[0].Cells);
    }

    [Fact]
    public void AValueBarValidatesAndSurvivesTheWire()
    {
        ProtocolValidator.ValidateSnapshot(Snapshot(ValueBarNode(ValueBar())));
        var received = Assert.Single(RoundTrip(Snapshot(ValueBarNode(ValueBar()))).Nodes).PerformanceValueBar!;
        Assert.Equal(3, received.Fields.Count);
        Assert.Equal("Last", received.Fields[0].Label);
        Assert.Equal("1:40", received.Fields[2].Value);
    }

    [Fact]
    public void AnEmptyChartIsValid()
    {
        // No counters: a graph with no series, a legend with no rows, a strip of empty values.
        ProtocolValidator.ValidateSnapshot(Snapshot(GraphNode(Graph() with { Series = [] })));
        ProtocolValidator.ValidateSnapshot(Snapshot(LegendNode(Legend() with { Rows = [] })));
        ProtocolValidator.ValidateSnapshot(Snapshot(ValueBarNode(new PerformanceValueBarSnapshot
        {
            Fields = [new PerformanceValueFieldSnapshot { Label = "Last", Value = string.Empty }],
        })));
    }

    [Fact]
    public void ViewModelsCarryTheStateAndAnnounceAChange()
    {
        var graph = ControlNodeViewModel.FromSnapshot(GraphNode(Graph()));
        Assert.Equal(2, graph.PerformanceGraph!.Series.Count);
        var changed = new List<string?>();
        graph.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        graph.ApplySnapshot(GraphNode(Graph() with { BarSlot = 3 }), preserveTransient: true);
        Assert.Equal(3, graph.PerformanceGraph!.BarSlot);
        Assert.Contains("PerformanceGraph", changed);

        var legend = ControlNodeViewModel.FromSnapshot(LegendNode(Legend()));
        Assert.Equal(2, legend.PerformanceLegend!.Rows.Count);
        var bar = ControlNodeViewModel.FromSnapshot(ValueBarNode(ValueBar()));
        Assert.Equal(3, bar.PerformanceValueBar!.Fields.Count);
    }

    [Fact]
    public void OnlyTheMatchingKindCarriesTheState()
    {
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(GraphNode(Graph(), kind: "edit"))));
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(LegendNode(Legend(), kind: "listView"))));
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(ValueBarNode(ValueBar(), kind: "static"))));
        // And each kind insists on its own state.
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(GraphNode(null))));
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(LegendNode(null))));
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(ValueBarNode(null))));
    }

    public static TheoryData<string> MalformedGraphs() => new()
    {
        "type", "range", "samples", "interval", "bar", "scrolling bar", "wrapped scroll", "wrapped without bar",
        "time", "colour", "style", "width", "scale", "values", "series", "nul",
    };

    [Theory]
    [MemberData(nameof(MalformedGraphs))]
    public void OutOfBoundsGraphsAreRejected(string flaw)
    {
        var graph = Graph();
        var series = graph.Series[0];
        graph = flaw switch
        {
            "type" => graph with { DisplayType = "report" },
            "range" => graph with { Minimum = 10, Maximum = 10 },
            "samples" => graph with { SampleCount = 1 },
            "interval" => graph with { IntervalMs = 99 },
            "bar" => graph with { BarSlot = graph.SampleCount },
            "scrolling bar" => graph with { Scroll = true, BarSlot = 0, Wrapped = false },
            "wrapped scroll" => graph with { Scroll = true, BarSlot = -1, Wrapped = true },
            "wrapped without bar" => graph with { BarSlot = -1, Wrapped = true },
            "time" => graph with { StartTimeMs = -1 },
            "colour" => graph with { BarColor = 0x1000000 },
            "style" => graph with { Series = [series with { Style = "wavy" }] },
            "width" => graph with { Series = [series with { Width = 9 }] },
            "scale" => graph with { Series = [series with { Scale = 1e10 }] },
            "values" => graph with { SampleCount = 4, Series = [series with { Values = [.. Enumerable.Repeat<double?>(1, 5)] }] },
            "series" => graph with
            {
                Series = [.. Enumerable.Repeat(series, PerformanceProtocol.MaxSeries + 1)],
            },
            _ => graph with { Series = [series with { Name = "a\0b" }] },
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(GraphNode(graph))));
    }

    public static TheoryData<string> MalformedLegends() => new()
    {
        "type", "no columns", "cells", "colour", "style", "two selected", "rows",
    };

    [Theory]
    [MemberData(nameof(MalformedLegends))]
    public void OutOfBoundsLegendsAreRejected(string flaw)
    {
        var legend = Legend();
        var row = legend.Rows[0];
        legend = flaw switch
        {
            "type" => legend with { DisplayType = "report" },
            "no columns" => legend with { Columns = [] },
            "cells" => legend with { Rows = [row with { Cells = ["1.0"] }] },
            "colour" => legend with { Rows = [row with { Color = 0x1000000 }] },
            "style" => legend with { Rows = [row with { Style = "wavy" }] },
            "two selected" => legend with { Rows = [row with { Selected = true }, row with { Selected = true }] },
            _ => legend with { Rows = [.. Enumerable.Repeat(row with { Selected = false }, PerformanceProtocol.MaxLegendRows + 1)] },
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(LegendNode(legend))));
    }

    [Theory]
    [InlineData("scroll")]
    [InlineData("barSlot")]
    [InlineData("wrapped")]
    [InlineData("startTimeMs")]
    [InlineData("showTimeLabels")]
    [InlineData("controlColor")]
    [InlineData("barColor")]
    public void ARawGraphFieldTheSerializerWouldDefaultIsStillRequired(string field)
    {
        var json = JsonNode.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = Snapshot(GraphNode(Graph())),
        }))!;
        json["window"]!["nodes"]![0]!["performanceGraph"]!.AsObject().Remove(field);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Theory]
    [InlineData("rows")]
    [InlineData("displayType")]
    public void ARawLegendFieldTheSerializerWouldDefaultIsStillRequired(string field)
    {
        var legend = JsonNode.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = Snapshot(LegendNode(Legend())),
        }))!;
        legend["window"]!["nodes"]![0]!["performanceLegend"]!.AsObject().Remove(field);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(legend.ToJsonString())));
    }

    [Fact]
    public void ARawValueBarFieldIsStillRequired()
    {
        var bar = JsonNode.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = Snapshot(ValueBarNode(ValueBar())),
        }))!;
        bar["window"]!["nodes"]![0]!["performanceValueBar"]!.AsObject().Remove("fields");
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(bar.ToJsonString())));
    }
}
