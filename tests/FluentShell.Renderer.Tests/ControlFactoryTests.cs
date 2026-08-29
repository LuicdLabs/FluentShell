using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;

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

    [Fact]
    public void SplitsExactlyOneSysLinkLabelIntoInlineSegments()
    {
        var segments = ControlFactory.SplitSysLinkText(
            "Read the privacy statement before continuing.", "privacy statement");

        Assert.Equal("Read the ", segments.Prefix);
        Assert.Equal("privacy statement", segments.Label);
        Assert.Equal(" before continuing.", segments.Suffix);
        Assert.Throws<ArgumentException>(() =>
            ControlFactory.SplitSysLinkText("Help and Help", "Help"));
    }

    [Fact]
    public void ListViewSelectionIsCanonicalAndUsesNativeSelectionSemantics()
    {
        Assert.Equal([0, 1, 2], ControlFactory.CanonicalSelectionIndices([2, -1, 0, 2, 1]));
        Assert.Equal(ListViewSelectionMode.Single, ControlFactory.SelectionModeFor(false));
        Assert.Equal(ListViewSelectionMode.Extended, ControlFactory.SelectionModeFor(true));
        Assert.Equal(AutomationControlType.List, ControlFactory.AutomationControlTypeFor("listView"));
    }

    [Fact]
    public void SemanticWrapperPeersExposeBoundedNativeControlTypes()
    {
        Assert.Equal(AutomationControlType.Text, ControlFactory.AutomationControlTypeFor("static"));
        Assert.Equal(AutomationControlType.Pane, ControlFactory.AutomationControlTypeFor("sysLink"));
        Assert.Equal(AutomationControlType.StatusBar, ControlFactory.AutomationControlTypeFor("statusBar"));
    }

    [Theory]
    [InlineData(true, 0x0004u, true)]
    [InlineData(true, 0x0000u, false)]
    [InlineData(false, 0x0004u, false)]
    public void TextBoxAcceptsReturnOnlyWithMultilineDialogEvidence(
        bool multiline, uint dialogCode, bool expected)
    {
        var node = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "1",
            Generation = "1",
            NativeHwnd = "0x1",
            Kind = "edit",
            Rect = new PixelRect(),
            Visible = true,
            Enabled = true,
            Text = string.Empty,
            Multiline = multiline,
            DialogCode = dialogCode,
        });

        Assert.Equal(expected, ControlFactory.AcceptsReturnFor(node));
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
