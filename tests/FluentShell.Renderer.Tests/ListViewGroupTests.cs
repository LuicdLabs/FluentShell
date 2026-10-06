using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;
using Windows.System;

namespace FluentShell.Renderer.Tests;

public sealed class ListViewGroupTests
{
    private static ControlNode Grouped(string mode = "report") => ListViewModeProtocolTests.IconList(mode) with
    {
        GroupViewEnabled = true,
        ListViewGroups = [
            new() { GroupId = "17", Header = "First group", Footer = "First footer", Collapsible = true,
                HeaderRect = new() { Y = 22, Width = 240, Height = 24 }, FooterRect = new() { Y = 74, Width = 240, Height = 20 } },
            new() { GroupId = "-7", Header = "Second group", Collapsible = true, Collapsed = true,
                HeaderRect = new() { Y = 94, Width = 240, Height = 24 } },
        ],
        ItemGroupIds = ["-7", "17"],
        ItemRects = [new(), new() { Y = 46, Width = 240, Height = 28 }],
        Columns = mode == "report" ? ["Name"] : [],
        ColumnWidths = mode == "report" ? [240] : [],
        ColumnOrder = mode == "report" ? [0] : [],
        ColumnHeadersVisible = mode == "report",
        Rows = mode == "report" ? [["Computers"], ["Event Viewer"]] : [],
    };

    private static byte[] Payload(ControlNode node) => ProtocolSerializer.Serialize(new WindowOpenMessage
    { SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] } });
    private static ControlNode Read(ControlNode node) => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Payload(node))).Window.Nodes.Single();

    [Theory]
    [InlineData("report")]
    [InlineData("largeIcon")]
    [InlineData("smallIcon")]
    public void GroupsKeepNativeIdsDisplayOrderMembershipAndCollapsedState(string mode)
    {
        var actual = Read(Grouped(mode));
        Assert.Equal(["17", "-7"], actual.ListViewGroups!.Select(group => group.GroupId));
        Assert.Equal(["-7", "17"], actual.ItemGroupIds);
        Assert.Equal(["Computers", "Event Viewer"], actual.Items);
        Assert.Equal([1], actual.SelectedIndices);
        Assert.True(actual.ListViewGroups![1].Collapsed);
        var model = ControlNodeViewModel.FromSnapshot(actual);
        Assert.True(ControlFactory.HasRenderableListViewShape(model));
        Assert.Equal([1], ListViewGroupPresentation.VisibleOrder(model.ListViewGroups, model.ItemGroupIds));
        Assert.False(ListViewGroupPresentation.ItemVisible(model.ListViewGroups, model.ItemGroupIds, 0));
        Assert.Equal(1, ListViewPresentation.Navigate(model.ItemRects, 0, VirtualKey.Home));
    }

    [Fact]
    public void UngroupedRowsRemainHiddenAndGroupReorderDoesNotRenumberItems()
    {
        var node = Grouped() with { ItemGroupIds = ["-2", "17"] };
        var actual = Read(node);
        var model = ControlNodeViewModel.FromSnapshot(actual);
        var changed = node with { ListViewGroups = node.ListViewGroups!.AsEnumerable().Reverse().ToList() };
        model.ApplySnapshot(Read(changed), preserveTransient: true);
        Assert.Equal(["0", "7"], model.ItemNativeIds);
        Assert.Equal(["-2", "17"], model.ItemGroupIds);
        Assert.Equal("-7", model.ListViewGroups[0].GroupId);
    }

    [Fact]
    public void InvalidGroupIdentityMembershipBoundsAndFacetPlacementAreRejected()
    {
        var node = Grouped();
        foreach (var changed in new[]
        {
            node with { GroupViewEnabled = false }, node with { GroupViewEnabled = null },
            node with { Kind = "listBox" }, node with { ItemGroupIds = ["17"] },
            node with { Items = ["Wrong label", "Event Viewer"] },
            node with { ItemGroupIds = ["22", "17"] }, node with { ItemGroupIds = ["-1", "17"] },
            node with { ListViewGroups = [node.ListViewGroups![0], node.ListViewGroups[0]] },
            node with { ItemRects = [new() { Width = 20, Height = 20 }, node.ItemRects![1]] },
            node with { ItemRects = [new(), new()] },
            node with { ListViewGroups = [node.ListViewGroups![0] with { GroupId = "+17" }] },
            node with { ListViewGroups = [node.ListViewGroups![0] with { HeaderAlignment = "diagonal" }] },
        }) Assert.Throws<ProtocolException>(() => Read(changed));
        foreach (var id in new[] { "01", "+1", "-0", "-1", "-2", "2147483648", "-2147483649", " 1" })
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ParseListViewGroupId(id, false));
        Assert.Equal(int.MinValue, ProtocolValidator.ParseListViewGroupId("-2147483648", false));
    }

    [Theory]
    [InlineData("groupId")]
    [InlineData("collapsed")]
    [InlineData("headerRect")]
    public void RawGroupFieldsCannotBeDefaulted(string field)
    {
        var payload = JsonNode.Parse(Payload(Grouped()))!;
        payload["window"]!["nodes"]![0]!["listViewGroups"]![0]!.AsObject().Remove(field);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            JsonSerializer.SerializeToUtf8Bytes(payload)));
    }

    [Fact]
    public void DirectUiGroupedListAdvertisesSelectionChecksAndGroupExpansionTogether()
    {
        var fixture = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", "window.open.directui-semantic.json"));
        var window = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, fixture)).Window;
        var backing = window.Nodes[1];
        var grouped = Grouped() with
        {
            NodeId = backing.NodeId, Generation = backing.Generation, NativeHwnd = backing.NativeHwnd,
            AdapterId = backing.AdapterId, PageId = backing.PageId, SemanticKey = backing.SemanticKey,
            SourceKind = "nativeBacking", PresentationVariant = "standard", HelpText = "", AccessKey = "",
            CheckBoxes = true, CheckedIndices = [1], SupportedActions = ["setSelection", "setItemCheck", "setGroupExpanded"],
        };
        ProtocolValidator.ValidateSnapshot(window with { Nodes = [grouped] });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(window with
        { Nodes = [grouped with { SupportedActions = ["setSelection", "setItemCheck"] }] }));
    }

    [Fact]
    public void SharedGroupedFixtureIsAdmitted()
    {
        var bytes = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", "window.open.list-view-groups.json"));
        var node = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes)).Window.Nodes.Single();
        Assert.Equal(["7", "17"], node.ItemGroupIds);
        Assert.True(node.ListViewGroups![1].Collapsed);
    }

    [Fact]
    public void ExpandActionCarriesNativeGroupIdAndAbsoluteState()
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId, NodeId = "1", EventId = "2", ExpectedRevision = "1",
            Action = "setGroupExpanded", Value = JsonSerializer.SerializeToElement(new ListViewGroupExpandActionValue { GroupId = "-7", Expanded = true }),
        };
        var parsed = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action)));
        Assert.Equal("-7", parsed.Value.GetProperty("groupId").GetString());
        Assert.True(parsed.Value.GetProperty("expanded").GetBoolean());
        Assert.False(NodeActionReplayPolicy.IsReplayableAfterStale("listViewGroups"));
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke,
            ProtocolSerializer.Serialize(action with { Value = JsonSerializer.SerializeToElement(new { groupId = "7", expanded = true, extra = 1 }) })));
    }
}
