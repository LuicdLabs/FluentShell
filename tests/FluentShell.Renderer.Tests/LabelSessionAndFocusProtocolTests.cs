using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 40: the user putting the focus in a projected list or tree moves the thread's native
// focus there (menu commands act on the pane that holds it), and the projected editor answers
// a label session the application opened itself.
public sealed class LabelSessionAndFocusProtocolTests
{
    [Fact]
    public void SharedFocusFixtureAddressesANodeAndCarriesNoValue()
    {
        var action = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, Fixture("action.invoke.focus.json")));
        Assert.Equal("focus", action.Action);
        Assert.Equal("2", action.NodeId);
        Assert.Equal(JsonValueKind.Null, action.Value.ValueKind);
        Assert.Equal(40, ProtocolConstants.Minor);
    }

    [Fact]
    public void EmittedFocusRoundTrips()
    {
        Validate(JsonSerializer.SerializeToElement<object?>(null));
    }

    [Theory]
    [InlineData("1")]
    [InlineData("true")]
    [InlineData("{\"index\":0}")]
    public void FocusWithAValueIsRejected(string value)
    {
        Assert.Throws<ProtocolException>(() => Validate(JsonDocument.Parse(value).RootElement));
    }

    [Fact]
    public void FocusMustAddressANode()
    {
        Assert.Throws<ProtocolException>(() => Validate(JsonSerializer.SerializeToElement<object?>(null), nodeId: null));
    }

    [Theory]
    // An editor the user opened (F2) sends only a real rename.
    [InlineData("ValueB", "ValueA", true, false, "ValueB")]
    [InlineData("ValueA", "ValueA", true, false, null)]
    [InlineData("", "ValueA", true, false, null)]
    [InlineData("ValueB", "ValueA", false, false, null)]
    // An editor answering the application's own session always answers it.
    [InlineData("ValueB", "ValueA", true, true, "ValueB")]
    [InlineData("ValueA", "ValueA", true, true, "ValueA")]
    [InlineData("ValueB", "ValueA", false, true, "ValueA")]
    [InlineData("", "ValueA", true, true, "ValueA")]
    public void ClosingEditorSendsTheRenameOrEndsTheApplicationsSession(
        string requested, string current, bool commit, bool nativeSession, string? expected)
    {
        Assert.Equal(expected, ItemLabelEdit.Answer(requested, current, commit, nativeSession));
    }

    private static void Validate(JsonElement value, string? nodeId = "2")
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "focus", NodeId = nodeId, Value = value,
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
    }

    private static byte[] Fixture(string name) =>
        File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", name));
}
