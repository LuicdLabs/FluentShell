using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// A projected right-click asks the native tree/list for that item's own context
// menu; the native TrackPopupMenu then arrives anchored to that item.
public sealed class ItemContextMenuProtocolTests
{
    [Fact]
    public void SharedFixtureCarriesTheItemIndexAndItsLabelWitness()
    {
        var action = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, Fixture("action.invoke.context-menu.json")));
        Assert.Equal("contextMenu", action.Action);
        Assert.Equal("3", action.NodeId);
        Assert.Equal(2, action.Value.GetProperty("index").GetInt32());
        Assert.Equal("Leaf", action.Value.GetProperty("text").GetString());
        Assert.Equal("contextMenu", TranslatedWindow.PropertyForNodeAction("contextMenu"));
    }

    [Fact]
    public void EmittedValueRoundTripsAndAnEmptyListLabelIsAWitness()
    {
        Validate(JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = 0, Text = "" }));
        Validate(JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = 4095, Text = "Row" }));
    }

    // A windowed virtual list names its rows absolutely, past the per-array item cap.
    [Theory]
    [InlineData(4096)]
    [InlineData(1_000_000)]
    [InlineData(16_777_215)]
    public void AnAbsoluteRowOfAWindowedListIsAdmitted(int index)
    {
        Validate(JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = index, Text = "Row" }));
    }

    [Theory]
    [InlineData("2")]
    [InlineData("{\"index\":2}")]
    [InlineData("{\"text\":\"Leaf\"}")]
    [InlineData("{\"index\":-1,\"text\":\"Leaf\"}")]
    [InlineData("{\"index\":16777216,\"text\":\"Leaf\"}")]
    [InlineData("{\"index\":2,\"text\":\"Leaf\",\"extra\":true}")]
    [InlineData("{\"index\":2,\"text\":7}")]
    public void MalformedRequestsAreRejected(string value)
    {
        Assert.Throws<ProtocolException>(() => Validate(JsonDocument.Parse(value).RootElement));
    }

    [Fact]
    public void RequestMustAddressANode()
    {
        Assert.Throws<ProtocolException>(() => Validate(
            JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = 0, Text = "Row" }), nodeId: null));
    }

    [Fact]
    public void NativePopupMayAnchorToTheRequestedListItem()
    {
        var json = ListWithPopup(itemIndex: 1);
        var open = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, JsonSerializer.SerializeToUtf8Bytes(json)));
        Assert.Equal("1", open.Window.PopupMenu!.NodeId);
        Assert.Equal(1, open.Window.PopupMenu.ItemIndex);
    }

    [Fact]
    public void NativePopupCannotAnchorPastTheListItems()
    {
        var json = ListWithPopup(itemIndex: 2);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, JsonSerializer.SerializeToUtf8Bytes(json)));
    }

    private static JsonNode ListWithPopup(int itemIndex)
    {
        var json = JsonNode.Parse(Fixture("window.open.list-view-identity.json"))!;
        json["window"]!["popupMenu"] = new JsonObject
        {
            ["popupId"] = "5",
            ["nodeId"] = "1",
            ["itemIndex"] = itemIndex,
            ["items"] = new JsonArray(new JsonObject
            {
                ["itemId"] = "0", ["kind"] = "command", ["text"] = "&Open", ["commandId"] = 700,
                ["enabled"] = true, ["checked"] = false, ["radio"] = false, ["isDefault"] = true,
                ["items"] = new JsonArray(),
            }),
        };
        return json;
    }

    private static void Validate(JsonElement value, string? nodeId = "3")
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "contextMenu", NodeId = nodeId, Value = value,
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
    }

    private static byte[] Fixture(string name) =>
        File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", name));
}
