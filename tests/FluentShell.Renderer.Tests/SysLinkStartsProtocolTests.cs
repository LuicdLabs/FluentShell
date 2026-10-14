using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

// Minor 41: a SysLink label that also occurs elsewhere in its text (User Profiles: "...user account...
// <A>User Accounts</A>") carries linkStarts, where its link starts.
public sealed class SysLinkStartsProtocolTests
{
    private static ControlNode Link(List<int>? starts, string text = "Open user accounts in user accounts.") =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "sysLink", Text = text, AutomationName = text, Items = ["user accounts"], LinkStarts = starts,
        };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void ARepeatedLabelIsPlacedByItsStart()
    {
        Assert.Throws<ProtocolException>(() => Validate(Link(null)));
        Validate(Link([22]));
        Validate(Link([5]));
        var model = ControlNodeViewModel.FromSnapshot(Link([22]));
        Assert.Equal([22], model.LinkStarts);
        Assert.Empty(ControlNodeViewModel.FromSnapshot(Link(null, "Open <x> user accounts.")).LinkStarts);
    }

    [Fact]
    public void StartsMustPlaceTheLabelInsideTheText()
    {
        foreach (var starts in new List<int>[] { [21], [30], [-1], [5, 22] })
            Assert.Throws<ProtocolException>(() => Validate(Link(starts)));
        var button = TestData.Snapshot().Nodes[0] with { Kind = "button", LinkStarts = [0] };
        Assert.Throws<ProtocolException>(() => Validate(button));
    }

    [Fact]
    public void TheRawWireNeedsAnIntegerArray()
    {
        var json = JsonNode.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [Link([22])] },
        }))!;
        json["window"]!["nodes"]![0]!["linkStarts"] = "22";
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
    }
}
