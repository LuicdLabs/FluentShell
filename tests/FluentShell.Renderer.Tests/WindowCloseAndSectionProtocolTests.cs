using System.Text;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

// Protocol minor 39: a window's title-bar close button (canClose) is its own contract,
// separate from Escape (canCancel); an accessible island's section header carries its
// fold state (expanded); and a native command link travels with its presentation.
public sealed class WindowCloseAndSectionProtocolTests
{
    [Fact]
    public void CloseButtonIsSeparateFromEscape()
    {
        var ordinary = TestData.Snapshot() with { CanCancel = false, CanClose = true };
        ProtocolValidator.ValidateSnapshot(ordinary);
        var model = WindowViewModel.FromSnapshot(ordinary);
        Assert.True(model.CanClose);
        Assert.False(model.CanCancel);
    }

    [Fact]
    public void AnOlderBridgeLeavesTheCloseButtonToCancel()
    {
        Assert.False(WindowViewModel.FromSnapshot(TestData.Snapshot() with { CanCancel = false }).CanClose);
        Assert.True(WindowViewModel.FromSnapshot(TestData.Snapshot() with { CanCancel = true }).CanClose);
    }

    [Fact]
    public void CanCloseAndFoldStateTravelOnTheWire()
    {
        var bytes = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = TestData.Snapshot() with { CanClose = false, Nodes = [Island(Header(false))] },
        });
        Assert.Contains("\"canClose\":false", Encoding.UTF8.GetString(bytes));
        var received = Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes)).Window;
        Assert.False(received.CanClose);
        Assert.False(Assert.Single(Assert.Single(received.Nodes).IslandItems!).Expanded);
    }

    private static AccessibleIslandItem Header(bool? expanded) => new()
    {
        Kind = "button", Name = "Services (Local)", ActionName = "Services (Local)", Enabled = true,
        Rect = new PixelRect { X = 0, Y = 0, Width = 186, Height = 25 }, Expanded = expanded,
    };

    private static ControlNode Island(params AccessibleIslandItem[] items) => new()
    {
        NodeId = "11", Generation = "1", NativeHwnd = "0x6789", Kind = "accessibleIsland",
        Style = "0x56000000", ExStyle = "0x0", Rect = new PixelRect { Width = 186, Height = 576 },
        Visible = true, Enabled = true, TabIndex = -1, IslandItems = [.. items],
    };

    [Fact]
    public void ASectionHeaderCarriesItsFoldState()
    {
        foreach (var expanded in new bool?[] { true, false, null })
        {
            var node = Island(Header(expanded));
            ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });
            Assert.Equal(expanded, ControlNodeViewModel.FromSnapshot(node).IslandItems[0].Expanded);
        }
    }

    [Fact]
    public void OnlyAButtonThatOpensNoMenuFoldsASection()
    {
        foreach (var item in new[]
        {
            Header(true) with { DropDown = true },
            Header(false) with { Kind = "link" },
            new AccessibleIslandItem
            {
                Kind = "text", Name = "Body", Enabled = true, Expanded = true,
                Rect = new PixelRect { Width = 10, Height = 10 },
            },
        })
            Assert.Throws<ProtocolException>(() =>
                ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [Island(item)] }));
    }

    private static ControlNode Button(string? variant, string? helpText) => new()
    {
        NodeId = "12", Generation = "1", NativeHwnd = "0x6790", Kind = "button", ControlId = 1,
        Rect = new PixelRect { X = 10, Y = 10, Width = 300, Height = 58 },
        Visible = true, Enabled = true, TabStop = true, TabIndex = 0, Text = "Restart now",
        Items = [], PresentationVariant = variant, HelpText = helpText,
    };

    [Fact]
    public void ANativeCommandLinkKeepsItsPresentationAndNote()
    {
        foreach (var node in new[] { Button("commandLink", "Save your work first."), Button("commandLink", null) })
            ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });
    }

    [Fact]
    public void AWizardBackButtonIsDrawnAsItsArrowAndNothingElseIs()
    {
        var payload = File.ReadAllText(Path.Combine(
            AppContext.BaseDirectory, "ProtocolFixtures", "window.open.directui-semantic.json"));
        var snapshot = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(payload))).Window;
        // The fixture's third node is the wizard's virtual (disabled) Back button.
        Assert.Equal("button", snapshot.Nodes[2].Kind);
        snapshot.Nodes[2] = snapshot.Nodes[2] with { PresentationVariant = "navigationBack" };
        ProtocolValidator.ValidateSnapshot(snapshot);
        snapshot.Nodes[2] = snapshot.Nodes[2] with { PresentationVariant = "standard" };
        snapshot.Nodes[0] = snapshot.Nodes[0] with { PresentationVariant = "navigationBack" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(
            TestData.Snapshot() with { Nodes = [Button("navigationBack", null)] }));
    }

    [Fact]
    public void AnOrdinaryWindowStillRefusesOtherPresentations()
    {
        foreach (var node in new[]
        {
            Button("commandLinkElevated", null),
            Button("expandoExpanded", null),
            Button(null, "a note without a command link"),
            Button("commandLink", null) with { Kind = "checkBox" },
        })
            Assert.Throws<ProtocolException>(() =>
                ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] }));
    }
}
