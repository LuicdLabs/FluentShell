using System.Text;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class DirectUiProtocolTests
{
    private static string Fixture() => File.ReadAllText(Path.Combine(
        AppContext.BaseDirectory, "ProtocolFixtures", "window.open.directui-semantic.json"));

    private static WindowOpenMessage Read(string payload) => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(payload)));

    [Fact]
    public void SemanticFixturePreservesProviderTextNativeCheckBoxAndDisabledBackButton()
    {
        var snapshot = Read(Fixture()).Window;
        Assert.Equal(ProtocolConstants.GenericDirectUiAdapterId, snapshot.AdapterId);
        Assert.Equal(ProtocolConstants.GenericDirectUiPageId, snapshot.PageId);
        Assert.Equal(3, snapshot.Nodes.Count);
        Assert.Null(snapshot.Nodes[0].NativeHwnd);
        Assert.Equal("uiaVirtual", snapshot.Nodes[0].SourceKind);
        Assert.Equal("nativeBacking", snapshot.Nodes[1].SourceKind);
        Assert.NotNull(snapshot.Nodes[1].NativeHwnd);
        Assert.Equal(["setCheck"], snapshot.Nodes[1].SupportedActions);
        Assert.Null(snapshot.Nodes[2].NativeHwnd);
        Assert.False(snapshot.Nodes[2].Enabled);
        Assert.Empty(snapshot.Nodes[2].SupportedActions!);
    }

    [Theory]
    [InlineData("microsoft.mdsched.directui", "initial")]
    [InlineData("microsoft.recoverydrive.directui", "first")]
    [InlineData("microsoft.unknown.directui", "initial")]
    public void RetiredAndUnknownApplicationAdaptersFaultOnlyTheirSurface(string adapter, string page)
    {
        var payload = Fixture()
            .Replace(ProtocolConstants.GenericDirectUiAdapterId, adapter, StringComparison.Ordinal)
            .Replace(ProtocolConstants.GenericDirectUiPageId, page, StringComparison.Ordinal);
        var exception = Assert.Throws<ProtocolException>(() => Read(payload));
        Assert.Contains("Unknown application adapter", exception.Message, StringComparison.Ordinal);
        Assert.Equal(Read(Fixture()).Window.SurfaceId, exception.SurfaceScope);
    }

    [Theory]
    [InlineData("mainIcon")]
    [InlineData("instruction")]
    [InlineData("content")]
    [InlineData("wizardTitle")]
    [InlineData("header")]
    public void RetiredPresentationVariantsCannotEnterTheSemanticLane(string variant)
    {
        var snapshot = Read(Fixture()).Window;
        snapshot.Nodes[0] = snapshot.Nodes[0] with { PresentationVariant = variant };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Theory]
    [InlineData("adapterId")]
    [InlineData("pageId")]
    [InlineData("semanticKey")]
    [InlineData("sourceKind")]
    [InlineData("presentationVariant")]
    [InlineData("supportedActions")]
    [InlineData("helpText")]
    [InlineData("accessKey")]
    public void SemanticNodesRequireAllMetadata(string field)
    {
        var payload = JsonNode.Parse(Fixture())!;
        payload["window"]!["nodes"]![1]!.AsObject().Remove(field);
        Assert.Throws<ProtocolException>(() => Read(payload.ToJsonString()));
    }

    [Fact]
    public void DisabledVirtualButtonOffersNoActionUntilTheCanonicalStateEnablesIt()
    {
        var snapshot = Read(Fixture()).Window;
        var back = snapshot.Nodes[2];
        snapshot.Nodes[2] = back with { SupportedActions = ["invoke"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));

        // The capability contract follows canonical state, with no fixed page
        // rule that keeps a back button disabled after the application enables it.
        snapshot.Nodes[2] = back with
        {
            Enabled = true, TabStop = true, TabIndex = 1, SupportedActions = ["invoke"],
        };
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Fact]
    public void NativeCheckBoxCannotClaimVirtualBacking()
    {
        var snapshot = Read(Fixture()).Window;
        snapshot.Nodes[1] = snapshot.Nodes[1] with { NativeHwnd = null, SourceKind = "uiaVirtual" };
        var exception = Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        Assert.Contains("requires a native backing control", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ProviderTextCannotClaimAnHwndWhileRemainingVirtual()
    {
        var snapshot = Read(Fixture()).Window;
        snapshot.Nodes[0] = snapshot.Nodes[0] with { NativeHwnd = "0x1234" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Theory]
    [InlineData("messageBox")]
    [InlineData("taskDialog")]
    public void SemanticMetadataCannotTurnATranslatedDialogIntoAnAdapterPage(string kind)
    {
        var snapshot = Read(Fixture()).Window with { SurfaceKind = kind };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }
}
