using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class NativeDocumentTests
{
    private static string Fixture() => File.ReadAllText(Path.Combine(
        AppContext.BaseDirectory, "ProtocolFixtures", "window.open.native-document.json"));

    private static WindowSnapshot Read(string json) => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(json))).Window;

    private static NativeDocumentSnapshot Document() => Read(Fixture()).Nodes[0].Document!;

    [Theory]
    [InlineData("link", "htmlInvoke", 1)]
    [InlineData("scroll", "htmlScroll", 0)]
    public void SharedActionsKeepTheirDocumentTarget(string fixture, string action, int index)
    {
        var payload = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory,
            "ProtocolFixtures", $"action.invoke.native-document-{fixture}.json"));
        var message = Assert.IsType<ActionInvokeMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, payload));
        Assert.Equal(action, message.Action);
        Assert.Equal(index, message.Value.GetProperty("index").GetInt32());
        Assert.Equal("document-1", message.Value.GetProperty("documentId").GetString());
        if (action == "htmlInvoke")
            Assert.Equal("9007199254740993", message.Value.GetProperty("actionIdentity").GetString());
        else Assert.Equal(80, message.Value.GetProperty("y").GetInt32());
    }

    [Fact]
    public void SharedFixtureRetainsDocumentIdentityScrollportsActionsAndSamples()
    {
        var snapshot = Read(Fixture());
        var node = Assert.Single(snapshot.Nodes);
        Assert.Equal("nativeDocument", node.Kind);
        Assert.Equal("0x31", node.NativeHwnd);
        Assert.False(node.TabStop);
        var document = node.Document!;
        Assert.Equal("9007199254740993", document.Items[1].ActionIdentity);
        Assert.Equal(20, document.Regions[0].OffsetY);
        Assert.Equal(3, document.Regions[1].OffsetX);
        Assert.Equal(8192.25, document.Items[3].ReportRows[0].Value);
        Assert.False(document.Items[3].ReportRows[1].Available);
        var model = ControlNodeViewModel.FromSnapshot(node);
        var changed = document with { Items = [.. document.Items] };
        changed.Items[3] = changed.Items[3] with
        {
            ReportRows = [changed.Items[3].ReportRows[0] with { Value = 4096 }, changed.Items[3].ReportRows[1]],
        };
        model.ApplySnapshot(node with { Document = changed }, preserveTransient: true);
        Assert.Equal(4096, model.Document!.Items[3].ReportRows[0].Value);
    }

    [Theory]
    [InlineData("documentId")]
    [InlineData("regions")]
    [InlineData("items")]
    public void CompleteDocumentEnvelopeIsRequired(string member)
    {
        var payload = JsonNode.Parse(Fixture())!;
        payload["window"]!["nodes"]![0]!["document"]!.AsObject().Remove(member);
        Assert.Throws<ProtocolException>(() => Read(payload.ToJsonString()));
    }

    [Theory]
    [InlineData("key")]
    [InlineData("actionIdentity")]
    [InlineData("enabled")]
    [InlineData("reportRows")]
    public void CompleteItemFactsAreRequired(string member)
    {
        var payload = JsonNode.Parse(Fixture())!;
        payload["window"]!["nodes"]![0]!["document"]!["items"]![1]!.AsObject().Remove(member);
        Assert.Throws<ProtocolException>(() => Read(payload.ToJsonString()));
    }

    [Fact]
    public void RectangleCoordinatesCannotBeDefaultedAcrossTheWire()
    {
        var payload = JsonNode.Parse(Fixture())!;
        payload["window"]!["nodes"]![0]!["document"]!["regions"]![0]!["rect"]!.AsObject().Remove("x");
        Assert.Throws<ProtocolException>(() => Read(payload.ToJsonString()));
    }

    [Fact]
    public void ScrollExtentMustDescribeItsCanonicalRange()
    {
        var snapshot = Read(Fixture());
        var document = snapshot.Nodes[0].Document!;
        document.Regions[0] = document.Regions[0] with { MaximumY = 159 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void ReportCannotBorrowAnHtmlScrollportOrShareItsIdentity()
    {
        var snapshot = Read(Fixture());
        var document = snapshot.Nodes[0].Document!;
        document.Items[3] = document.Items[3] with { ReportRegionIndex = 0 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot = Read(Fixture());
        document = snapshot.Nodes[0].Document!;
        document.Items.Add(document.Items[3] with { Key = "second-report" });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void CanonicalNativeHostCannotLoseItsKeyboardContract()
    {
        var snapshot = Read(Fixture());
        snapshot.Nodes[0] = snapshot.Nodes[0] with { TabStop = true, TabIndex = 0 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Theory]
    [InlineData(-1, true)]
    [InlineData(12, false)]
    public void ReportSamplesDistinguishUnavailableValuesFromActualSamples(double value, bool available)
    {
        var snapshot = Read(Fixture());
        snapshot.Nodes[0].Document!.Items[3].ReportRows[0] = new NativeReportRow
        {
            Path = "Counter", Value = value, Available = available,
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Theory]
    [InlineData("0")]
    [InlineData("01")]
    [InlineData("1.0")]
    [InlineData("18446744073709551616")]
    public void LinkActionRequiresCanonicalNonzeroUint64Identity(string identity)
    {
        var snapshot = Read(Fixture());
        var document = snapshot.Nodes[0].Document!;
        document.Items[1] = document.Items[1] with { ActionIdentity = identity };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        var action = JsonSerializer.SerializeToElement(new
        {
            index = 1, documentId = document.DocumentId, key = "link", actionIdentity = identity,
        });
        Assert.Throws<ProtocolException>(() => NativeDocumentProtocol.ValidateAction("htmlInvoke", action));
    }

    [Fact]
    public void NativeScrollActionRejectsMissingIdentityOrOutOfRangeOffsets()
    {
        NativeDocumentProtocol.ValidateAction("htmlScroll", JsonSerializer.SerializeToElement(new
        {
            index = 0, documentId = "document-1", key = "details", x = 0, y = 160,
        }));
        Assert.Throws<ProtocolException>(() => NativeDocumentProtocol.ValidateAction("htmlScroll",
            JsonSerializer.SerializeToElement(new { index = 0, documentId = "", key = "details", x = 0, y = 1 })));
        Assert.Throws<ProtocolException>(() => NativeDocumentProtocol.ValidateAction("htmlScroll",
            JsonSerializer.SerializeToElement(new { index = 0, documentId = "document-1", key = "details", x = 0, y = 65536 })));
    }

    [Fact]
    public void LiveSamplesAndCanonicalScrollPatchesReuseFocusedControls()
    {
        var before = Document();
        var after = before with { Items = [.. before.Items], Regions = [.. before.Regions] };
        after.Items[1] = after.Items[1] with { Name = "Open current monitor", ActionIdentity = "3" };
        after.Items[3] = after.Items[3] with
        {
            ReportRows = [after.Items[3].ReportRows[0] with { Value = 100 }, after.Items[3].ReportRows[1]],
        };
        after.Regions[0] = after.Regions[0] with { OffsetY = 80 };
        Assert.True(NativeDocumentPresentation.SameStructure(before, after));
        Assert.False(NativeDocumentPresentation.SameStructure(before, after with { DocumentId = "new-page" }));
        after.Items[1] = after.Items[1] with { Key = "replacement-link" };
        Assert.False(NativeDocumentPresentation.SameStructure(before, after));
    }

    [Fact]
    public void ReportsWithNoNativeScrollRangeKeepAllRowsReachableLocally()
    {
        var report = Document().Regions[1] with { MaximumX = 0, OffsetX = 0 };
        Assert.True(NativeDocumentPresentation.AllowsLocalOverflow(report));
        Assert.Equal(0, NativeDocumentPresentation.NativeOffset(200, 400, report.MaximumX));
        Assert.Equal(0, NativeDocumentPresentation.NativeOffset(600, 800, report.MaximumY));
        Assert.False(NativeDocumentPresentation.AllowsLocalOverflow(Document().Regions[0]));
        Assert.Equal(80, NativeDocumentPresentation.NativeOffset(160, 320, 160));
    }
}
