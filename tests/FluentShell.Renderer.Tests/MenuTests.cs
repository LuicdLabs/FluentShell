using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class MenuTests
{
    [Fact]
    public void MenuViewModelPreservesTypedRecursiveState()
    {
        var snapshot = MenuSnapshot();
        var viewModel = WindowViewModel.FromSnapshot(snapshot);

        Assert.Single(viewModel.Menu);
        Assert.Equal("&File", viewModel.Menu[0].Text);
        Assert.Equal(42, viewModel.Menu[0].Items[0].CommandId);
        Assert.True(viewModel.Menu[0].Items[0].Checked);
        Assert.True(viewModel.Menu[0].Items[0].Radio);
        Assert.True(viewModel.Menu[0].Items[0].IsDefault);
    }

    [Fact]
    public void ProtocolRoundTripsBoundedMenuAndMenuCommand()
    {
        var open = new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = MenuSnapshot(),
        };
        var decoded = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, ProtocolSerializer.Serialize(open)));
        Assert.Equal(42, decoded.Window.Menu[0].Items[0].CommandId);

        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = open.Window.SurfaceId,
            EventId = "9",
            ExpectedRevision = open.Window.Revision,
            Action = "menuCommand",
            Value = System.Text.Json.JsonSerializer.SerializeToElement(42),
        };
        Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action)));
    }

    [Fact]
    public void ProtocolRejectsAmbiguousCommandIds()
    {
        var snapshot = MenuSnapshot();
        snapshot.Menu[0].Items.Add(snapshot.Menu[0].Items[0] with { ItemId = "0.1" });
        var open = new WindowOpenMessage { SessionNonce = TestData.Nonce, Window = snapshot };

        var payload = ProtocolSerializer.Serialize(open);
        Assert.Throws<ProtocolException>(() =>
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));
    }

    [Theory]
    [InlineData("0", "FluentShell.Menu.0")]
    [InlineData("2.4.1", "FluentShell.Menu.2.4.1")]
    public void AutomationIdsAreDeterministic(string path, string expected) =>
        Assert.Equal(expected, MenuProjectionFactory.AutomationId(path));

    private static WindowSnapshot MenuSnapshot()
    {
        var snapshot = TestData.Snapshot() with { SurfaceKind = "window" };
        snapshot.Menu.Add(new MenuItemSnapshot
        {
            ItemId = "0",
            Kind = "popup",
            Text = "&File",
            Enabled = true,
            Items =
            [
                new MenuItemSnapshot
                {
                    ItemId = "0.0",
                    Kind = "command",
                    Text = "&Open",
                    CommandId = 42,
                    Enabled = true,
                    Checked = true,
                    Radio = true,
                    IsDefault = true,
                },
            ],
        });
        return snapshot;
    }
}
