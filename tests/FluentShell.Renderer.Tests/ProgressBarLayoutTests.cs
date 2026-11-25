using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class ProgressBarLayoutTests
{
    [Theory]
    [InlineData(14, 160, false)]
    [InlineData(14, 160, true)]
    [InlineData(22.4, 320, true)]
    [InlineData(160, 14, false)]
    [InlineData(0, 150, true)]
    [InlineData(16, 0, true)]
    public void RotatedVisualStaysExactlyInsideItsNativeSlot(double width, double height, bool vertical)
    {
        var layout = ProgressBarLayout.For(width, height, vertical);
        var corners = new[]
        {
            Transform(layout, 0, 0),
            Transform(layout, layout.VisualWidth, 0),
            Transform(layout, 0, layout.VisualHeight),
            Transform(layout, layout.VisualWidth, layout.VisualHeight),
        };
        Assert.Equal(0, corners.Min(point => point.X), 8);
        Assert.Equal(0, corners.Min(point => point.Y), 8);
        Assert.Equal(width, corners.Max(point => point.X), 8);
        Assert.Equal(height, corners.Max(point => point.Y), 8);
    }

    [Fact]
    public void VerticalProgressStartsAtTheBottomAndGrowsUpward()
    {
        var layout = ProgressBarLayout.For(20, 200, vertical: true);
        var minimum = Transform(layout, 0, layout.VisualHeight / 2);
        var quarter = Transform(layout, layout.VisualWidth / 4, layout.VisualHeight / 2);
        var maximum = Transform(layout, layout.VisualWidth, layout.VisualHeight / 2);
        Assert.Equal((10d, 200d), minimum);
        Assert.Equal((10d, 150d), quarter);
        Assert.Equal((10d, 0d), maximum);
    }

    [Fact]
    public void HorizontalProgressStartsAtTheLeftAndGrowsRightward()
    {
        var layout = ProgressBarLayout.For(200, 20, vertical: false);
        Assert.Equal((0d, 10d), Transform(layout, 0, layout.VisualHeight / 2));
        Assert.Equal((200d, 10d), Transform(layout, layout.VisualWidth, layout.VisualHeight / 2));
    }

    [Theory]
    [InlineData(0x50000004UL, true)]
    [InlineData(0x5000000CUL, true)] // vertical marquee
    [InlineData(0x50000008UL, false)] // horizontal marquee
    [InlineData(0x50000001UL, false)] // horizontal smooth
    public void BothNativeAnimationModesKeepTheirStyleDerivedOrientation(ulong style, bool expected)
    {
        Assert.Equal(expected, ProgressBarLayout.IsVerticalStyle(style));
    }

    [Theory]
    [InlineData(double.NaN, double.PositiveInfinity)]
    [InlineData(double.NegativeInfinity, -10)]
    public void UnmeasuredOrCollapsedControlsNeverProduceAnInvalidTransform(double width, double height)
    {
        var layout = ProgressBarLayout.For(width, height, vertical: true);
        Assert.Equal(0, layout.VisualWidth);
        Assert.Equal(0, layout.VisualHeight);
        Assert.Equal((0d, 0d), Transform(layout, 0, 0));
    }

    private static (double X, double Y) Transform(ProgressBarLayout layout, double x, double y) =>
        (x * layout.M11 + y * layout.M21 + layout.OffsetX,
         x * layout.M12 + y * layout.M22 + layout.OffsetY);
}
