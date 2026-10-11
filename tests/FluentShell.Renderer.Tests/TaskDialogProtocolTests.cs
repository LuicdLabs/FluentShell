using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

// A translated task dialog (minor 38) presents its main instruction, command links
// with their note, elevation shields and expando button through presentationVariant,
// and its text as a sysLink with one label per link.  Nothing else on a generic
// surface may carry those fields.
public sealed class TaskDialogProtocolTests
{
    private static ControlNode Node(string id, string kind, int z, string text) => new()
    {
        NodeId = id,
        Generation = "3",
        NativeHwnd = null,
        Kind = kind,
        ZIndex = z,
        TabIndex = -1,
        Rect = new PixelRect { X = 24, Y = 24 + z * 40, Width = 400, Height = 32 },
        Visible = true,
        Enabled = true,
        Text = text,
        Items = [],
    };

    private static WindowSnapshot TaskDialog(params ControlNode[] nodes) =>
        TestData.DialogSnapshot() with { SurfaceKind = "taskDialog", Nodes = [.. nodes] };

    [Fact]
    public void AcceptsTheTaskDialogPresentation()
    {
        var snapshot = TaskDialog(
            Node("2", "static", 0, "Delete the file?") with { PresentationVariant = "mainInstruction" },
            Node("3", "sysLink", 1, "Read the help or the support page.") with { Items = ["help", "support page"], TabStop = true, TabIndex = 0 },
            Node("300", "button", 2, "&Delete") with
            {
                PresentationVariant = "commandLink",
                HelpText = "The file is removed for good.",
                TabStop = true,
                TabIndex = 1,
            },
            Node("301", "button", 3, "&Keep") with { PresentationVariant = "commandLinkNoIcon", TabStop = true, TabIndex = 2 },
            Node("6", "button", 4, "See details") with { PresentationVariant = "expandoCollapsed", TabStop = true, TabIndex = 3 },
            Node("302", "button", 5, "Install") with { PresentationVariant = "elevated", TabStop = true, TabIndex = 4 });

        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Theory]
    [InlineData("static", "commandLink")]
    [InlineData("button", "mainInstruction")]
    [InlineData("button", "bitmapDisplay")]
    [InlineData("checkBox", "elevated")]
    public void RejectsAVariantTheKindDoesNotHave(string kind, string variant)
    {
        var snapshot = TaskDialog(Node("2", kind, 0, "Label") with { PresentationVariant = variant });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void OnlyACommandLinkCarriesANote()
    {
        var snapshot = TaskDialog(Node("300", "button", 0, "OK") with { HelpText = "A note", TabStop = true, TabIndex = 0 });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void AMessageBoxCarriesNoPresentationVariant()
    {
        var snapshot = TestData.DialogSnapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with { PresentationVariant = "mainInstruction" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void LinksMustOccurInTheirOrder()
    {
        var snapshot = TaskDialog(
            Node("3", "sysLink", 0, "Read the support page or the help.") with { Items = ["help", "support page"], TabStop = true, TabIndex = 0 });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void LocatesRepeatedLabelsLeftToRight()
    {
        Assert.True(ProtocolValidator.TryLocateSysLinkLabels("a link and a link", ["link", "link"], out var starts));
        Assert.Equal([2, 13], starts);
        // A lone label must still be unique, as a native SysLink's is.
        Assert.False(ProtocolValidator.TryLocateSysLinkLabels("a link and a link", ["link"], out _));
    }

    [Fact]
    public void ALinkIsActivatedByIndex()
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = Guid.NewGuid(),
            NodeId = "3",
            EventId = "5",
            ExpectedRevision = "1",
            Action = "activateItem",
            Value = System.Text.Json.JsonSerializer.SerializeToElement(1),
        };
        var bytes = ProtocolSerializer.Serialize(action);
        var read = (ActionInvokeMessage)ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, bytes);
        Assert.Equal(1, read.Value.GetInt32());
    }
}
