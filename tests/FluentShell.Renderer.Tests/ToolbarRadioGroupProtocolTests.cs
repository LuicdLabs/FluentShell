using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public class ToolbarRadioGroupProtocolTests
{
    private static ToolbarItemSnapshot Radio(int command, int group, bool selected = false) => new()
    {
        Kind = "radioButton", RadioGroup = group, CommandId = command, Text = $"View {command}",
        Enabled = true, Checked = selected,
        Rect = new PixelRect { X = (command - 1) * 25, Width = 25, Height = 25 },
    };
    private static void Validate(params ToolbarItemSnapshot[] items)
    {
        var node = TestData.Snapshot().Nodes[0] with
        {
            Kind = "toolbar", Style = "0x50000000", ExStyle = "0x0",
            Rect = new PixelRect { Width = 200, Height = 25 }, ToolbarItems = items.ToList(),
        };
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] },
            }));
    }

    [Fact]
    public void IndependentContiguousRadioGroupsPreserveTheirCheckedMembers()
    {
        Validate(Radio(1, 1, true), Radio(2, 1),
            Radio(3, 0) with { Kind = "pushButton", RadioGroup = null, Checked = false },
            Radio(4, 2), Radio(5, 2, true));
        Validate(Radio(1, 1), Radio(2, 1)); // Native groups may start without selection.
    }

    [Fact]
    public void InvalidOrAmbiguousRadioStateCannotCrossTheProtocol()
    {
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 0)));
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 65)));
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 1) with { Checked = null }));
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 1, true), Radio(2, 1, true)));
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 1), Radio(2, 2), Radio(3, 1)));
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 1) with { Kind = "toggleButton" }));
        Assert.Throws<ProtocolException>(() => Validate(Radio(1, 1) with { DropDown = true }));
    }
}
