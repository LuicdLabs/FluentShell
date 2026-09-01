using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Tests;

public sealed class ControlFactoryTests
{
    [Fact]
    public void CommandLinkMetadataSurvivesTheViewModelBoundary()
    {
        var node = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "1", Generation = "1", NativeHwnd = "0x1", Kind = "button",
            Rect = new PixelRect { Width = 200, Height = 60 }, Visible = true, Enabled = true,
            Text = "Run now", AutomationName = "Run now", PresentationVariant = "commandLink",
            SupportedActions = ["invoke"], HelpText = "Checks memory after restart.", AccessKey = "Alt+R",
            AdapterId = "microsoft.mdsched.directui", PageId = "initial",
            SemanticKey = "CommandLink.0", SourceKind = "nativeBacking",
        });

        Assert.Equal("commandLink", node.PresentationVariant);
        Assert.Equal("Checks memory after restart.", node.HelpText);
        Assert.Equal("Alt+R", node.AccessKey);
        Assert.Equal(["invoke"], node.SupportedActions);
    }

    [Fact]
    public void RecoveryDriveCheckboxAndDisabledBackSurviveTheViewModelBoundary()
    {
        var checkbox = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "1", Generation = "1", NativeHwnd = "0x2", Kind = "checkBox",
            Rect = new PixelRect { Width = 300, Height = 24 }, Visible = true, Enabled = true,
            Checked = 0, Text = "Back up system files", AutomationName = "Back up system files",
            AdapterId = "microsoft.recoverydrive.directui", PageId = "first",
            SemanticKey = "backupSystemFiles", SourceKind = "nativeBacking",
            PresentationVariant = "standard", SupportedActions = ["setCheck"],
            HelpText = string.Empty, AccessKey = string.Empty,
        });
        var back = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "2", Generation = "1", NativeHwnd = null, Kind = "button",
            Rect = new PixelRect { Width = 80, Height = 32 }, Visible = true, Enabled = false,
            Text = "Back", AutomationName = "Back",
            AdapterId = "microsoft.recoverydrive.directui", PageId = "first",
            SemanticKey = "backbutton", SourceKind = "uiaVirtual",
            PresentationVariant = "standard", SupportedActions = [],
            HelpText = string.Empty, AccessKey = string.Empty,
        });

        // The checkbox toggles through the projected surface; the inert back
        // button carries no action and stays disabled.
        Assert.Equal(["setCheck"], checkbox.SupportedActions);
        Assert.True(back is { Enabled: false, SupportedActions.Count: 0 });
        Assert.Null(back.NativeHwnd);
        Assert.True(ControlFactory.AllowsAction(checkbox, "setCheck"));
        Assert.False(ControlFactory.AllowsAction(checkbox, "invoke"));
        Assert.False(ControlFactory.AllowsAction(back, "invoke"));
        Assert.True(ControlFactory.AllowsAction(Node("generic", "button", 0), "invoke"));
    }

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
        Assert.Equal("checkedIndices", TranslatedWindow.PropertyForNodeAction("setItemCheck"));
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void ListViewHeaderRowFollowsCanonicalVisibilityWithoutChangingShape(bool visible)
    {
        var node = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "1",
            Generation = "1",
            NativeHwnd = "0x1",
            Kind = "listView",
            Rect = new PixelRect(),
            Visible = true,
            Enabled = true,
            Columns = ["Name", "Status"],
            ColumnWidths = [160, 80],
            Rows = [["Drive C", "Ready"]],
            FocusedIndex = -1,
            ColumnHeadersVisible = visible,
            CheckBoxes = true,
            CheckedIndices = [0],
        });

        Assert.Equal(visible, ControlFactory.ShouldRenderListViewHeader(node));
        Assert.True(ControlFactory.HasRenderableListViewShape(node));
        Assert.Equal(AutomationControlType.List, ControlFactory.AutomationControlTypeFor(node.Kind));
        Assert.True(ControlFactory.IsListViewRowChecked(node, 0));
        Assert.False(ControlFactory.IsListViewRowChecked(node, 1));
    }

    [Fact]
    public void SemanticWrapperPeersExposeBoundedNativeControlTypes()
    {
        Assert.Equal("FluentShell.ContentViewport", SemanticContentViewport.AutomationId);
        Assert.False(SemanticContentViewport.AutomationId.StartsWith(
            "FluentShell.Node.", StringComparison.Ordinal));
        Assert.Equal(AutomationControlType.Pane, SemanticContentViewport.ControlType);
        Assert.Equal(AutomationControlType.Text, ControlFactory.AutomationControlTypeFor("static"));
        Assert.Equal(AutomationControlType.Image, ControlFactory.AutomationControlTypeFor("staticIcon"));
        Assert.Equal(AutomationControlType.Pane, ControlFactory.AutomationControlTypeFor("sysLink"));
        Assert.Equal(AutomationControlType.StatusBar, ControlFactory.AutomationControlTypeFor("statusBar"));
        Assert.Equal(AutomationControlType.ToolBar, ControlFactory.AutomationControlTypeFor("toolbar"));
        Assert.Equal(AutomationControlType.Pane, ControlFactory.AutomationControlTypeFor("dialogContainer"));
        Assert.Equal(AutomationControlType.Tab, ControlFactory.AutomationControlTypeFor("tabControl"));
        Assert.Equal(AutomationControlType.TabItem, ControlFactory.TabItemAutomationControlType());
    }

    [Fact]
    public void TabControlFactoryPreservesCanonicalHeaderShape()
    {
        var node = ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = "20",
            Generation = "1",
            NativeHwnd = "0x20",
            Kind = "tabControl",
            Rect = new PixelRect { Width = 300, Height = 180 },
            Visible = true,
            Enabled = true,
            Items = ["General", "Advanced"],
            ItemRects =
            [
                new PixelRect { X = 0, Y = 0, Width = 90, Height = 24 },
                new PixelRect { X = 0, Y = 24, Width = 110, Height = 24 },
            ],
            SelectedIndex = 1,
        });

        Assert.True(ControlFactory.HasRenderableTabShape(node));
        Assert.Equal(new PixelRect { X = 0, Y = 24, Width = 110, Height = 24 }, node.ItemRects[1]);
        Assert.Equal("selectedIndex", TranslatedWindow.PropertyForNodeAction("select"));
    }

    [Fact]
    public void TabControlUsesFullBoundsHostWithSemanticHeaderPeers()
    {
        Assert.True(typeof(ContentControl).IsAssignableFrom(typeof(SemanticTabControl)));
        Assert.False(typeof(Canvas).IsAssignableFrom(typeof(SemanticTabControl)));
        Assert.True(typeof(ListViewItem).IsAssignableFrom(typeof(TabViewItem)));
        Assert.True(typeof(ISelectionProvider).IsAssignableFrom(
            typeof(SemanticTabControlAutomationPeer)));
        Assert.Equal(AutomationControlType.TabItem, ControlFactory.TabItemAutomationControlType());

        var policy = SemanticTabControl.Policy;
        Assert.False(policy.IsAddTabButtonVisible);
        Assert.False(policy.CanDragTabs);
        Assert.False(policy.CanReorderTabs);
        Assert.False(policy.AreItemsClosable);
        Assert.Equal(TabViewWidthMode.SizeToContent, policy.TabWidthMode);
    }

    [Fact]
    public void TabControlGroupsSingleAndMultilineHeadersIntoOrderedBands()
    {
        var single = ControlFactory.GroupTabHeaderRows([
            new PixelRect { X = 2, Y = 3, Width = 80, Height = 24 },
            new PixelRect { X = 82, Y = 3, Width = 100, Height = 24 },
        ]);
        var multiline = ControlFactory.GroupTabHeaderRows([
            new PixelRect { X = 90, Y = 28, Width = 110, Height = 24 },
            new PixelRect { X = 2, Y = 3, Width = 80, Height = 24 },
            new PixelRect { X = 82, Y = 3, Width = 100, Height = 24 },
            new PixelRect { X = 2, Y = 28, Width = 88, Height = 24 },
        ]);

        Assert.Single(single);
        Assert.Equal(new PixelRect { X = 2, Y = 3, Width = 180, Height = 24 }, single[0].Bounds);
        Assert.Equal(2, multiline.Count);
        Assert.Equal([1, 2], multiline[0].Items.Select(item => item.Index));
        Assert.Equal([3, 0], multiline[1].Items.Select(item => item.Index));
        Assert.Equal(new PixelRect { X = 2, Y = 28, Width = 198, Height = 24 }, multiline[1].Bounds);
        Assert.Equal(-1, ControlFactory.LocalTabSelectionIndex(multiline[0], 0));
        Assert.Equal(1, ControlFactory.LocalTabSelectionIndex(multiline[1], 0));

        Assert.Throws<ArgumentException>(() => ControlFactory.GroupTabHeaderRows([
            new PixelRect { X = 0, Y = 0, Width = 80, Height = 24 },
            new PixelRect { X = 80, Y = 0, Width = 80, Height = 25 },
        ]));
        Assert.Throws<ArgumentException>(() => ControlFactory.GroupTabHeaderRows([
            new PixelRect { X = 0, Y = 0, Width = 80, Height = 24 },
            new PixelRect { X = 0, Y = 20, Width = 80, Height = 24 },
        ]));
    }

    [Fact]
    public void TabControlPeerExpandsHeaderUnionToOwnerBounds()
    {
        var expanded = SemanticTabControlAutomationPeer.ExpandHeaderUnionBounds(
            new global::Windows.Foundation.Rect(172, 196, 150, 18),
            new global::Windows.Foundation.Rect(-6, -7, 150, 18),
            353,
            379);

        Assert.Equal(new global::Windows.Foundation.Rect(178, 203, 353, 379), expanded);
    }

    [Fact]
    public void StaticIconPixelsAreOwnedBgraBytes()
    {
        var node = Node("1", "staticIcon", 0, imageWidth: 1, imageHeight: 1,
            imageData: Convert.ToBase64String([0x10, 0x20, 0x30, 0x40]));

        var first = ControlFactory.DecodeImagePixels(node);
        var second = ControlFactory.DecodeImagePixels(node);

        Assert.Equal([0x10, 0x20, 0x30, 0x40], first);
        Assert.NotSame(first, second);
    }

    [Fact]
    public void NestedBoundsAreParentRelativeWhileSnapshotsRemainRootRelative()
    {
        var parent = Node("1", "dialogContainer", 0, rect: new PixelRect { X = 40, Y = 30, Width = 200, Height = 100 });
        var child = Node("2", "edit", 1, parentNodeId: "1", rect: new PixelRect { X = 55, Y = 42, Width = 80, Height = 20 });
        var nodes = new Dictionary<string, ControlNodeViewModel> { ["1"] = parent, ["2"] = child };

        Assert.Equal(new PixelRect { X = 15, Y = 12, Width = 80, Height = 20 },
            ControlFactory.RelativeRectFor(child, nodes));
        Assert.Equal(55, child.Rect.X);
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
        bool groupStart = false,
        string? parentNodeId = null,
        PixelRect? rect = null,
        int imageWidth = 0,
        int imageHeight = 0,
        string imageData = "") =>
        ControlNodeViewModel.FromSnapshot(new ControlNode
        {
            NodeId = nodeId,
            Generation = "1",
            NativeHwnd = "0x1",
            Kind = kind,
            ParentNodeId = parentNodeId,
            ZIndex = zIndex,
            Rect = rect ?? new PixelRect(),
            Visible = true,
            Enabled = true,
            Text = nodeId,
            GroupStart = groupStart,
            ImageWidth = imageWidth == 0 ? null : imageWidth,
            ImageHeight = imageHeight == 0 ? null : imageHeight,
            ImageFormat = imageWidth == 0 ? null : "bgra8-premultiplied",
            ImageData = imageWidth == 0 ? null : imageData,
        });
}
