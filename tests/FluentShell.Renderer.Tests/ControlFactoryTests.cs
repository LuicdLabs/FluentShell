using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class ControlFactoryTests
{
    [Fact]
    public void RadioGroupsFollowNativeWsGroupBoundaries()
    {
        var nodes = new[]
        {
            Node("1", "button", 0, groupStart: true),
            Node("2", "radioButton", 1),
            Node("3", "radioButton", 2),
            Node("4", "static", 3, groupStart: true),
            Node("5", "radioButton", 4),
        };

        var groups = ControlFactory.BuildRadioGroups(nodes);

        Assert.Equal(groups["2"], groups["3"]);
        Assert.NotEqual(groups["2"], groups["5"]);
    }

    [Fact]
    public void PasswordAutomationNameNeverExposesCanonicalText()
    {
        var password = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "1",
            Generation = "1",
            NativeHwnd = "0x1",
            Kind = "password",
            Rect = new PixelRect(),
            Visible = true,
            Enabled = true,
            Text = "native secret",
            AutomationName = "native secret",
        });

        Assert.Equal("Password edit", ControlFactory.AutomationNameFor(password));
        Assert.DoesNotContain("native secret", ControlFactory.AutomationNameFor(password));
    }

    private static ControlNodeViewModel Node(
        string nodeId,
        string kind,
        int zIndex,
        bool groupStart = false) =>
        ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = nodeId,
            Generation = "1",
            NativeHwnd = "0x1",
            Kind = kind,
            ZIndex = zIndex,
            Rect = new PixelRect(),
            Visible = true,
            Enabled = true,
            Text = nodeId,
            GroupStart = groupStart,
        });
}
