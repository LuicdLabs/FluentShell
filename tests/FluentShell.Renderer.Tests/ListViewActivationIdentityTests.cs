using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class ListViewActivationIdentityTests
{
    private static byte[] Serialize(ControlNode node) => ProtocolSerializer.Serialize(new WindowOpenMessage
    {
        SessionNonce = TestData.Nonce,
        Window = TestData.Snapshot() with { Nodes = [node] },
    });

    private static ControlNode Decode(byte[] bytes) => Assert.Single(
        Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes))
            .Window.Nodes);

    [Fact]
    public void SharedFixtureRetainsZeroAndMaximumNativeIdentityAsStrings()
    {
        var bytes = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory,
            "ProtocolFixtures", "window.open.list-view-identity.json"));
        var node = Decode(bytes);
        Assert.Equal(["0", "4294967294"], node.ItemNativeIds);
        Assert.True(node.ItemActivationSupported);
        var model = ControlNodeViewModel.FromSnapshot(node);
        Assert.Equal(node.ItemNativeIds, model.ItemNativeIds);
        using var json = JsonDocument.Parse(Serialize(node));
        var ids = json.RootElement.GetProperty("window").GetProperty("nodes")[0].GetProperty("itemNativeIds");
        Assert.All(ids.EnumerateArray(), id => Assert.Equal(JsonValueKind.String, id.ValueKind));
    }

    [Fact]
    public void ActivationWaitsForBothCanonicalSelectionAndFocusOfTheSameNativeItem()
    {
        var intent = new ListViewActivationIntent(1, "7");
        Assert.Equal(ListViewActivationDecision.Wait,
            ListViewActivationIntentPolicy.Decide(intent, ["0", "7"], [], 1));
        Assert.Equal(ListViewActivationDecision.Wait,
            ListViewActivationIntentPolicy.Decide(intent, ["0", "7"], [1], 0));
        Assert.Equal(ListViewActivationDecision.Activate,
            ListViewActivationIntentPolicy.Decide(intent, ["0", "7"], [1], 1));
    }

    [Fact]
    public void EqualLabelReplacementCannotRetargetAnActivationWaitingForSelection()
    {
        var node = ListViewModeProtocolTests.IconList() with
        {
            Items = ["Same label", "Same label"], SelectedIndices = [], FocusedIndex = 0,
        };
        var model = ControlNodeViewModel.FromSnapshot(Decode(Serialize(node)));
        var intent = new ListViewActivationIntent(1, model.ItemNativeIds[1]);
        Assert.Equal(ListViewActivationDecision.Wait, ListViewActivationIntentPolicy.Decide(
            intent, model.ItemNativeIds, model.SelectedIndices, model.FocusedIndex));

        // The selected row was replaced by another item at the same position.
        // A label-based intention would invoke the replacement after this echo.
        model.ApplySnapshot(Decode(Serialize(node with
        {
            ItemNativeIds = ["0", "8"], SelectedIndices = [1], FocusedIndex = 1,
        })), preserveTransient: true);
        Assert.Equal(node.Items, model.Items);
        Assert.Equal(ListViewActivationDecision.Drop, ListViewActivationIntentPolicy.Decide(
            intent, model.ItemNativeIds, model.SelectedIndices, model.FocusedIndex));
    }

    [Fact]
    public void RemovedMovedOrUnidentifiedItemsDropPendingActivation()
    {
        var intent = new ListViewActivationIntent(1, "7");
        foreach (var ids in new IReadOnlyList<string>[] { [], ["0"], ["7", "0"], ["0", "8"] })
            Assert.Equal(ListViewActivationDecision.Drop,
                ListViewActivationIntentPolicy.Decide(intent, ids, [1], 1));
        Assert.Equal(ListViewActivationDecision.Activate,
            ListViewActivationIntentPolicy.Decide(new(0, "0"), ["0"], [0], 0));
    }

    [Fact]
    public void OwnerDataAndLegacyReadOnlyActivationMetadataRemainAdmissible()
    {
        var node = ListViewModeProtocolTests.IconList() with { ItemActivationSupported = false, ItemNativeIds = [] };
        Assert.Empty(Decode(Serialize(node)).ItemNativeIds!);
        Assert.Null(Decode(Serialize(node with { ItemNativeIds = null, ItemActivationSupported = null })).ItemNativeIds);
    }

    [Fact]
    public void IncompleteDuplicateNonCanonicalAndSentinelIdentitiesAreRejected()
    {
        var node = ListViewModeProtocolTests.IconList();
        foreach (var ids in new List<string>?[]
        {
            null, [], ["0"], ["0", "0"], ["0", "01"], ["0", "-1"], ["0", "+1"],
            ["0", "1e3"], ["0", " 1"], ["0", "4294967295"], ["0", "4294967296"],
            ["0", "18446744073709551615"], ["0", null!],
        }) Assert.Throws<ProtocolException>(() => Decode(Serialize(node with { ItemNativeIds = ids })));
        Assert.Throws<ProtocolException>(() => Decode(Serialize(node with
        {
            ItemNativeIds = Enumerable.Range(0, ProtocolConstants.MaxItems + 1).Select(index => index.ToString()).ToList(),
        })));
        Assert.Throws<ProtocolException>(() => Decode(Serialize(TestData.Snapshot().Nodes[0] with { ItemNativeIds = [] })));
    }

    [Theory]
    [InlineData("null")]
    [InlineData("[0,7]")]
    [InlineData("\"0\"")]
    public void NativeIdentityWireValuesRequireAnArrayOfStrings(string replacement)
    {
        var json = JsonNode.Parse(Serialize(ListViewModeProtocolTests.IconList()))!;
        json["window"]!["nodes"]![0]!["itemNativeIds"] = JsonNode.Parse(replacement);
        Assert.Throws<ProtocolException>(() => Decode(Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void IdentityCannotBePatchedSeparatelyFromItsCanonicalRows()
    {
        var model = ControlNodeViewModel.FromSnapshot(ListViewModeProtocolTests.IconList());
        Assert.Throws<ProtocolException>(() => model.ApplyCanonical("itemNativeIds",
            JsonSerializer.SerializeToElement(new[] { "0", "8" }), null));
        Assert.Equal(["0", "7"], model.ItemNativeIds);
    }
}
