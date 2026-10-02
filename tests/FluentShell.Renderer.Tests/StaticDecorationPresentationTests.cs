using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class StaticDecorationPresentationTests
{
    [Fact]
    public void AddRemoveColumnsFrameRetainsTransparentInteriorAndBothNativeEdges()
    {
        var presentation = StaticDecorationPresentation.FromStyles(0x50001007, 0x00020004);

        Assert.Null(presentation.FillTone);
        Assert.Equal([0, 1, 2], presentation.Strokes.Select(stroke => stroke.Inset).Distinct().ToArray());
        Assert.Equal(StaticDecorationEdges.All, presentation.Strokes[^1].Edges);
        Assert.Equal(StaticDecorationTone.Dark, presentation.Strokes[^1].Tone);
        Assert.All(presentation.Strokes.Where(stroke => stroke.Edges == StaticDecorationEdges.TopLeft),
            stroke => Assert.Equal(StaticDecorationTone.Shadow, stroke.Tone));
    }

    [Theory]
    [InlineData(7)]
    [InlineData(8)]
    [InlineData(9)]
    [InlineData(18)]
    public void AllFrameTypesLeaveTheInteriorUnfilled(int drawStyle)
    {
        var presentation = StaticDecorationPresentation.FromStyles(0x50000000UL | (ulong)drawStyle, 0);
        Assert.Null(presentation.FillTone);
        Assert.NotEmpty(presentation.Strokes);
    }

    [Theory]
    [InlineData(4, 0)]
    [InlineData(5, 1)]
    [InlineData(6, 2)]
    public void RectangleTypesFillTheirOwnBounds(int drawStyle, int tone)
    {
        var presentation = StaticDecorationPresentation.FromStyles(0x50000000UL | (ulong)drawStyle, 0);
        Assert.Equal((StaticDecorationTone)tone, presentation.FillTone);
        Assert.Equal(0, presentation.FillInset);
        Assert.Empty(presentation.Strokes);
    }

    [Fact]
    public void EtchedFrameUsesOpposingRingsAndNoFill()
    {
        var presentation = StaticDecorationPresentation.FromStyles(0x50000012, 0);
        Assert.Equal(4, presentation.Strokes.Count);
        Assert.Equal(StaticDecorationTone.Shadow, presentation.Strokes[0].Tone);
        Assert.Equal(StaticDecorationTone.Highlight, presentation.Strokes[2].Tone);
        Assert.Equal(0, presentation.Strokes[0].Inset);
        Assert.Equal(1, presentation.Strokes[2].Inset);
        Assert.Null(presentation.FillTone);
    }

    [Fact]
    public void FrameAndEdgeChangesReachTheExistingViewModel()
    {
        var node = StaticDecorationProtocolTests.Node();
        var model = ControlNodeViewModel.FromSnapshot(node);
        var changed = new List<string?>();
        model.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        model.ApplySnapshot(node with { Style = "0x50000006", ExStyle = "0x00000000" }, preserveTransient: true);

        Assert.Contains(nameof(model.Style), changed);
        Assert.Contains(nameof(model.ExStyle), changed);
        var presentation = StaticDecorationPresentation.FromStyles(model.Style, model.ExStyle);
        Assert.Equal(StaticDecorationTone.Light, presentation.FillTone);
        Assert.Empty(presentation.Strokes);
        Assert.False(ControlFactory.AllowsAction(model, "invoke"));
        Assert.False(ControlFactory.AllowsAction(model, "setText"));
    }
}
