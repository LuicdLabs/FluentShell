using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 41: a native split button (the Sound applet's Set Default) is a button whose arrow is the
// splitDropDown action; the menu its owner tracks comes back as a popup anchored under it.
public sealed class SplitButtonProtocolTests
{
    private static WindowSnapshot WithSplitButton(string? variant = "splitButton")
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with { Kind = "button", Text = "Set &Default", PresentationVariant = variant };
        return snapshot;
    }

    [Fact]
    public void ANativeSplitButtonIsAdmitted()
    {
        ProtocolValidator.ValidateSnapshot(WithSplitButton());
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(WithSplitButton("splitButtonish")));
    }

    [Fact]
    public void TheArrowIsANodeActionWithoutAValue()
    {
        Validate(JsonSerializer.SerializeToElement<object?>(null));
        Assert.Throws<ProtocolException>(() => Validate(JsonDocument.Parse("1").RootElement));
        Assert.Throws<ProtocolException>(() => Validate(JsonSerializer.SerializeToElement<object?>(null), nodeId: null));
    }

    [Fact]
    public void ItsMenuOpensUnderTheWholeButton()
    {
        var node = ControlNodeViewModel.FromSnapshot(WithSplitButton().Nodes[0]);
        var popup = new PopupMenuSnapshot { PopupId = "4", NodeId = node.NodeId, ItemIndex = 0, Items = [] };
        var rect = PopupMenuAnchor.ItemRect(node, popup);
        Assert.NotNull(rect);
        Assert.Equal(node.Rect.Width, rect!.Width);
        Assert.Equal(node.Rect.Height, rect.Height);
        Assert.Null(PopupMenuAnchor.ItemRect(ControlNodeViewModel.FromSnapshot(WithSplitButton(null).Nodes[0]), popup));
    }

    private static void Validate(JsonElement value, string? nodeId = "2")
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "splitDropDown", NodeId = nodeId, Value = value,
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
    }
}
