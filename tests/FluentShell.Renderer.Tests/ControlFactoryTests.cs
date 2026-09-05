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

    [Fact]
    public void OnlyAbsoluteStateActionsAreReplayedAfterARevisionRace()
    {
        // A `stale` refusal says the snapshot the request named is no longer current,
        // not that the application declined it.  An action that carries the absolute
        // state the user asked for converges when it is re-sent against the newer
        // revision, which is what keeps a tree expansion from being lost to the
        // roughly one-per-second reconcile.
        foreach (var property in new[]
        {
            "itemExpanded", "checked", "selectedIndex", "selectedIndices", "checkedIndices",
        })
        {
            Assert.True(NodeActionReplayPolicy.IsReplayableAfterStale(property));
        }

        // A one-shot command would be performed twice, and text would overwrite the
        // normalization the application owns, so neither is ever re-sent.  Geometry is
        // absent for a different reason: the Bridge rebases request-semantic actions
        // and never reports them stale.
        foreach (var property in new[]
        {
            "invoke", "close", "text", "items", "position", "splits", "columnOrder",
            "islandItems", "state", "bounds", "menu:101",
        })
        {
            Assert.False(NodeActionReplayPolicy.IsReplayableAfterStale(property));
        }

        Assert.Equal(1, NodeActionReplayPolicy.MaxStaleRetries);
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
        Assert.Equal(AutomationControlType.Tree, ControlFactory.AutomationControlTypeFor("treeView"));
        Assert.Equal(AutomationControlType.Slider, ControlFactory.AutomationControlTypeFor("slider"));
        Assert.Equal(AutomationControlType.Pane, ControlFactory.AutomationControlTypeFor("mdiClient"));
        // A native MDI child is a window inside a window and keeps that UIA role.
        Assert.Equal(AutomationControlType.Window, ControlFactory.AutomationControlTypeFor("mdiChild"));
        Assert.Equal("mdiCommand", TranslatedWindow.PropertyForNodeAction("mdiCommand"));
        Assert.Equal(["activate", "close", "minimize", "maximize", "restore"], MdiCommands.All);
        Assert.Equal(AutomationControlType.TabItem, ControlFactory.TabItemAutomationControlType());
    }

    [Fact]
    public void TreeViewProjectionNeedsOneCompleteHierarchy()
    {
        var node = ControlNodeViewModel.FromSnapshot(TreeNode());
        Assert.True(ControlFactory.HasRenderableTreeShape(node));
        Assert.Equal("selectedIndex", TranslatedWindow.PropertyForNodeAction("select"));
        Assert.Equal("itemExpanded", TranslatedWindow.PropertyForNodeAction("setExpand"));
        Assert.Equal("position", TranslatedWindow.PropertyForNodeAction("setValue"));

        // A tree whose arrays disagree describes no hierarchy, so the projection
        // waits for the next canonical revision instead of rendering a partial one.
        var mismatched = ControlNodeViewModel.FromSnapshot(TreeNode() with { ItemExpanded = [true] });
        Assert.False(ControlFactory.HasRenderableTreeShape(mismatched));
        var rootless = ControlNodeViewModel.FromSnapshot(TreeNode() with { ItemDepths = [1, 1] });
        Assert.False(ControlFactory.HasRenderableTreeShape(rootless));
    }

    [Fact]
    public void ProjectedItemIconFollowsTheNativeSelectedStateIndex()
    {
        var node = ControlNodeViewModel.FromSnapshot(TreeNode() with
        {
            ImageList = [Icon(), Icon()],
            ItemImages = [0, 0],
            ItemSelectedImages = [1, 1],
            SelectedIndex = 1,
        });
        // The unselected item keeps its normal icon while the selected one swaps to
        // the selected-state icon, exactly as the native tree draws them.
        Assert.Equal(0, ControlFactory.ImageIndexForItem(node, 0, node.SelectedIndex));
        Assert.Equal(1, ControlFactory.ImageIndexForItem(node, 1, node.SelectedIndex));

        // An item that declares no icon draws none even when the control has an
        // image list, and a control without a selected-state list keeps the normal
        // icon under selection.
        var partial = ControlNodeViewModel.FromSnapshot(TreeNode() with
        {
            ImageList = [Icon()],
            ItemImages = [-1, 0],
            SelectedIndex = 1,
        });
        Assert.Equal(-1, ControlFactory.ImageIndexForItem(partial, 0, partial.SelectedIndex));
        Assert.Equal(0, ControlFactory.ImageIndexForItem(partial, 1, partial.SelectedIndex));

        // Items outside the captured imagery draw nothing rather than throwing.
        Assert.Equal(-1, ControlFactory.ImageIndexForItem(partial, 5, 5));
        Assert.Equal(-1, ControlFactory.ImageIndexForItem(
            ControlNodeViewModel.FromSnapshot(TreeNode()), 0, 0));
    }

    // A 1x1 opaque pixel: the smallest entry the validator accepts.
    private static ImageListEntry Icon() => new()
    {
        ImageWidth = 1,
        ImageHeight = 1,
        ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(new byte[] { 0x20, 0x40, 0x60, 0xFF }),
    };

    private static ControlNode TreeNode() => new()
    {
        NodeId = "1",
        Generation = "1",
        NativeHwnd = "0x1",
        Kind = "treeView",
        Rect = new PixelRect(),
        Visible = true,
        Enabled = true,
        Items = ["Console Root", "Services"],
        ItemDepths = [0, 1],
        ItemExpanded = [true, false],
        ItemHasChildren = [true, false],
        SelectedIndex = 1,
    };

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

    // The Bridge's committed UIA gate enumerates the proxy with the control-view
    // condition, so whichever element carries the node identity must remain in that
    // view. Only a variant that wraps the Image in a peer-publishing ContentControl
    // may drop the Image itself to the raw view.
    [Theory]
    [InlineData("standard", false)]
    [InlineData("mainIcon", false)]
    [InlineData("", false)]
    [InlineData("bitmapDisplay", true)]
    [InlineData("monitorPalette", true)]
    public void StaticIconHidesItsImageOnlyWhenASemanticWrapperRepublishesIt(
        string presentationVariant, bool wrapped)
    {
        Assert.Equal(wrapped, ControlFactory.StaticIconUsesSemanticWrapper(presentationVariant));
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

    [Fact]
    public void CapabilityLaneActionGatesFollowEachSlotsAdvertisedRoutes()
    {
        static ControlNodeViewModel Semantic(string kind, params string[] actions) =>
            ControlNodeViewModel.FromSnapshot(new ControlNode
            {
                NodeId = "1", Generation = "1", NativeHwnd = "0x2", Kind = kind,
                Rect = new PixelRect { Width = 200, Height = 24 },
                Visible = true, Enabled = true, Text = string.Empty,
                AutomationName = "Localized " + kind,
                AdapterId = "microsoft.windows.directui.semantic.v1", PageId = "semantic-v1",
                SemanticKey = kind + "slot", SourceKind = "nativeBacking",
                PresentationVariant = "standard", SupportedActions = [.. actions],
                HelpText = string.Empty, AccessKey = string.Empty,
            });

        // A generated slot's routes come from that revision's own typed state, so
        // the factory refuses anything the profile did not advertise even when the
        // WinUI control is perfectly capable of raising it.
        var checkList = Semantic("listView", "setSelection", "setItemCheck");
        Assert.True(ControlFactory.AllowsAction(checkList, "setSelection"));
        Assert.True(ControlFactory.AllowsAction(checkList, "setItemCheck"));
        Assert.False(ControlFactory.AllowsAction(checkList, "select"));
        Assert.False(ControlFactory.AllowsAction(Semantic("listView", "setSelection"), "setItemCheck"));

        var editableCombo = Semantic("comboBox", "select", "setText");
        Assert.True(ControlFactory.AllowsAction(editableCombo, "select"));
        Assert.True(ControlFactory.AllowsAction(editableCombo, "setText"));
        Assert.False(ControlFactory.AllowsAction(Semantic("comboBox", "select"), "setText"));

        // A read-only edit box and a disabled slot advertise nothing at all.
        Assert.False(ControlFactory.AllowsAction(Semantic("edit"), "setText"));
        Assert.False(ControlFactory.AllowsAction(Semantic("button"), "invoke"));

        Assert.True(ControlFactory.AllowsAction(Semantic("sysLink", "invoke"), "invoke"));
        Assert.True(ControlFactory.AllowsAction(Semantic("radioButton", "setCheck"), "setCheck"));
        var toolbar = Semantic("toolbar", "toolbarCommand");
        Assert.True(ControlFactory.AllowsAction(toolbar, "toolbarCommand"));
        Assert.False(ControlFactory.AllowsAction(toolbar, "invoke"));

        // Ordinary Win32 surfaces carry no adapter id and no route list, so the
        // gate stays open for them; closing it would make every dialog inert.
        Assert.True(ControlFactory.AllowsAction(Node("win32", "listView", 0), "setSelection"));
        Assert.True(ControlFactory.AllowsAction(Node("win32", "edit", 0), "setText"));
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
