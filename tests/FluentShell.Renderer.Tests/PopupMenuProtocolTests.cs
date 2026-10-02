using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class PopupMenuProtocolTests
{
    private const string PopupId = "18446744073709551615";

    [Fact]
    public void SharedPopupFixturePreservesTokenAndUsesAnIndependentMenuScope()
    {
        var open = OpenFixture();
        var popup = Assert.IsType<PopupMenuSnapshot>(open.Window.PopupMenu);
        Assert.Equal(PopupId, popup.PopupId);
        Assert.Equal("10", popup.NodeId);
        Assert.Equal(0, popup.ItemIndex);
        Assert.Equal(["command", "separator", "popup"], popup.Items.Select(item => item.Kind));
        Assert.Equal(open.Window.Menu[0].Items[0].CommandId, popup.Items[0].CommandId);
        Assert.False(open.Window.Nodes[0].Enabled);
        Assert.False(open.Window.Nodes[0].IslandItems![0].Enabled);
        Assert.Equal(PopupId, RoundTrip(open.Window).PopupMenu!.PopupId);
    }

    [Theory]
    [InlineData("action.invoke.popup-command.json", "2.0")]
    [InlineData("action.invoke.popup-dismiss.json", null)]
    public void SharedPopupActionsPreserveChoiceAndExplicitDismiss(string fixture, string? itemId)
    {
        var action = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, Fixture(fixture)));
        Assert.Null(action.NodeId);
        Assert.Equal("popupCommand", action.Action);
        Assert.Equal(PopupId, action.Value.GetProperty("popupId").GetString());
        Assert.Equal(itemId, action.Value.GetProperty("itemId").GetString());
        var value = JsonSerializer.SerializeToElement(new PopupCommandActionValue
        {
            PopupId = PopupId,
            ItemId = itemId,
        });
        Assert.True(value.TryGetProperty("itemId", out var selected));
        Assert.Equal(itemId, selected.GetString());
        ValidateAction(value);
    }

    [Fact]
    public void IdleSnapshotsAcceptMissingAndNullPopup()
    {
        var open = OpenFixture() with { Window = OpenFixture().Window with { PopupMenu = null } };
        var bytes = ProtocolSerializer.Serialize(open);
        var json = JsonNode.Parse(bytes)!;
        Assert.Null(json["window"]!["popupMenu"]);
        Assert.Null(Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, bytes)).Window.PopupMenu);
        json["window"]!["popupMenu"] = null;
        Assert.Null(Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, JsonSerializer.SerializeToUtf8Bytes(json))).Window.PopupMenu);
    }

    [Theory]
    [InlineData("popupId")]
    [InlineData("nodeId")]
    [InlineData("itemIndex")]
    [InlineData("items")]
    public void PopupFieldsCannotBeSilentlyDefaulted(string field)
    {
        var json = JsonNode.Parse(Fixture("window.open.popup-menu.json"))!;
        json["window"]!["popupMenu"]!.AsObject().Remove(field);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, JsonSerializer.SerializeToUtf8Bytes(json)));
    }

    [Fact]
    public void PopupDescendantsRequireTheSameRawMenuFieldsAsTheBar()
    {
        var json = JsonNode.Parse(Fixture("window.open.popup-menu.json"))!;
        json["window"]!["popupMenu"]!["items"]![2]!["items"]![0]!.AsObject().Remove("enabled");
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, JsonSerializer.SerializeToUtf8Bytes(json)));
    }

    [Theory]
    [InlineData("0")]
    [InlineData("01")]
    [InlineData("+1")]
    [InlineData("")]
    [InlineData("18446744073709551616")]
    public void PopupTokenMustBeAPositiveCanonicalUInt64(string token)
    {
        var snapshot = OpenFixture().Window;
        Assert.Throws<ProtocolException>(() => RoundTrip(snapshot with
        {
            PopupMenu = snapshot.PopupMenu! with { PopupId = token },
        }));
    }

    [Theory]
    [InlineData("0", 0)]
    [InlineData("010", 0)]
    [InlineData("999", 0)]
    [InlineData("10", -1)]
    [InlineData("10", 1)]
    [InlineData("10", 32)]
    public void PopupAnchorMustResolveToItsExistingIslandItem(string nodeId, int index)
    {
        var snapshot = OpenFixture().Window;
        Assert.Throws<ProtocolException>(() => RoundTrip(snapshot with
        {
            PopupMenu = snapshot.PopupMenu! with { NodeId = nodeId, ItemIndex = index },
        }));
    }

    [Fact]
    public void PopupRequiresAWindowSurfaceAndAnIslandDropdown()
    {
        var snapshot = OpenFixture().Window;
        Assert.Throws<ProtocolException>(() => RoundTrip(TestData.DialogSnapshot() with
        {
            PopupMenu = snapshot.PopupMenu,
        }));
        Assert.Throws<ProtocolException>(() => RoundTrip(TestData.Snapshot() with
        {
            PopupMenu = snapshot.PopupMenu,
        }));
        var island = snapshot.Nodes[0];
        Assert.Throws<ProtocolException>(() => RoundTrip(snapshot with
        {
            Nodes = [island with { IslandItems = [island.IslandItems![0] with { DropDown = false }] }],
        }));
    }

    [Fact]
    public void PopupRejectsAmbiguousPathsCommandsAndExcessiveCensus()
    {
        var snapshot = OpenFixture().Window;
        var popup = snapshot.PopupMenu!;
        var command = popup.Items[0];
        foreach (var items in new List<MenuItemSnapshot>[]
        {
            [],
            [command with { ItemId = "00" }],
            [command, command with { ItemId = "1" }],
            Enumerable.Range(0, ProtocolConstants.MaxMenuItems + 1)
                .Select(index => command with { ItemId = index.ToString(), CommandId = index + 1 }).ToList(),
        })
        {
            Assert.Throws<ProtocolException>(() => RoundTrip(snapshot with
            {
                PopupMenu = popup with { Items = items },
            }));
        }
    }

    [Fact]
    public void PopupDepthIsBoundedIndependentlyFromTheMainMenu()
    {
        var snapshot = OpenFixture().Window;
        var popup = snapshot.PopupMenu!;
        RoundTrip(snapshot with { PopupMenu = popup with { Items = [NestedMenu(8)] } });
        Assert.Throws<ProtocolException>(() => RoundTrip(snapshot with
        {
            PopupMenu = popup with { Items = [NestedMenu(9)] },
        }));
    }

    [Theory]
    [InlineData("{\"popupId\":\"1\"}")]
    [InlineData("{\"itemId\":null}")]
    [InlineData("{\"popupId\":1,\"itemId\":null}")]
    [InlineData("{\"popupId\":\"0\",\"itemId\":null}")]
    [InlineData("{\"popupId\":\"01\",\"itemId\":null}")]
    [InlineData("{\"popupId\":\"18446744073709551616\",\"itemId\":null}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":0}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":\"\"}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":\"01\"}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":\"0.\"}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":\"-1\"}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":\"256\"}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":\"0.0.0.0.0.0.0.0.0\"}")]
    [InlineData("{\"popupId\":\"1\",\"itemId\":null,\"commandId\":77}")]
    public void PopupActionRejectsMalformedOrUnboundedIdentity(string value)
    {
        using var json = JsonDocument.Parse(value);
        Assert.Throws<ProtocolException>(() => ValidateAction(json.RootElement));
    }

    [Fact]
    public void PopupActionCannotAddressANodeOrLoseItsDismissField()
    {
        var value = JsonSerializer.SerializeToElement(new PopupCommandActionValue { PopupId = "1" });
        Assert.Throws<ProtocolException>(() => ValidateAction(value, "10"));
        ValidateAction(value);
        ValidateAction(JsonSerializer.SerializeToElement(new PopupCommandActionValue
        {
            PopupId = PopupId,
            ItemId = "255.255.255.255.255.255.255.255",
        }));
    }

    private static MenuItemSnapshot NestedMenu(int depth, string path = "0") => depth == 1
        ? new MenuItemSnapshot { ItemId = path, Kind = "command", Text = "Command", CommandId = 77, Enabled = true }
        : new MenuItemSnapshot
        {
            ItemId = path, Kind = "popup", Text = "Submenu", Enabled = true,
            Items = [NestedMenu(depth - 1, path + ".0")],
        };

    private static WindowOpenMessage OpenFixture() => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Fixture("window.open.popup-menu.json")));

    private static byte[] Fixture(string name) =>
        File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", name));

    private static WindowSnapshot RoundTrip(WindowSnapshot snapshot) => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, ProtocolSerializer.Serialize(
            new WindowOpenMessage { SessionNonce = TestData.Nonce, Window = snapshot }))).Window;

    private static void ValidateAction(JsonElement value, string? nodeId = null)
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "popupCommand", NodeId = nodeId, Value = value,
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
    }
}
