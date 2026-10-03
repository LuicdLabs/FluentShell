using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Windows;
using Microsoft.UI.Xaml.Automation.Peers;

namespace FluentShell.Renderer.Tests;

// A projected UpDown carries the native range ordered, its position and its arrow
// increment, and each arrow asks for the position the native step would reach.
public sealed class UpDownProtocolTests
{
    private static ControlNode Node() => new()
    {
        NodeId = "21", Generation = "1", NativeHwnd = "0x4321", Kind = "upDown",
        Style = "0x50000026", ExStyle = "0x0", Rect = new PixelRect { Width = 16, Height = 22 },
        Visible = true, Enabled = true, TabIndex = -1,
        Minimum = 0, Maximum = 99, Position = 10, SmallChange = 1, LargeChange = 0,
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void ARangePositionAndStepAreAdmittedAndProjectAsASpinner()
    {
        Validate(Node());
        Validate(Node() with { Minimum = 5, Maximum = 5, Position = 5 });
        Assert.Equal(AutomationControlType.Spinner, ControlFactory.AutomationControlTypeFor("upDown"));
    }

    [Fact]
    public void AMissingOrInconsistentRangeIsRefused()
    {
        foreach (var node in new[]
        {
            Node() with { Minimum = null },
            Node() with { SmallChange = null },
            Node() with { Minimum = 10, Maximum = 9 },
            Node() with { Position = 100 },
            Node() with { Position = -1 },
            Node() with { SmallChange = 0 },
        })
            Assert.Throws<ProtocolException>(() => Validate(node));
    }

    [Theory]
    // minimum, maximum, position, step, reversed, wrap, up -> target
    [InlineData(0, 99, 10, 1, false, false, true, 11)]
    [InlineData(0, 99, 10, 1, false, false, false, 9)]
    [InlineData(0, 99, 10, 5, false, false, true, 15)]
    [InlineData(0, 99, 99, 1, false, false, true, 99)]
    [InlineData(0, 99, 99, 1, false, true, true, 0)]
    [InlineData(0, 99, 0, 1, false, true, false, 99)]
    // A backwards native range: the up arrow decreases the value.
    [InlineData(0, 99, 10, 1, true, false, true, 9)]
    [InlineData(0, 99, 0, 1, true, false, true, 0)]
    [InlineData(int.MinValue, int.MaxValue, int.MaxValue, 1, false, false, true, int.MaxValue)]
    public void AnArrowLandsWhereTheNativeStepWould(
        int minimum, int maximum, int position, int step, bool reversed, bool wrap, bool up, int expected)
    {
        Assert.Equal(expected,
            ControlFactory.UpDownStepTarget(minimum, maximum, position, step, reversed, wrap, up));
    }
}
