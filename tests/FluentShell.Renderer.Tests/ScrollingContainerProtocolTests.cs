using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

// A container that scrolls its own child windows (a WinForms AutoScroll panel)
// carries its standard scroll bars and the viewport its children are clipped to.
public sealed class ScrollingContainerProtocolTests
{
    private static ControlNode Pane() => new()
    {
        NodeId = "10",
        Generation = "1",
        NativeHwnd = "0x5678",
        Kind = "paneContainer",
        ControlId = 100,
        TabIndex = -1,
        Rect = new PixelRect { X = 0, Y = 0, Width = 240, Height = 200 },
        Visible = true,
        Enabled = true,
        Text = string.Empty,
        AutomationName = string.Empty,
        Items = [],
        Splits = [],
        ChromeRegions = [],
        ClientRect = new PixelRect { X = 0, Y = 0, Width = 223, Height = 200 },
        ScrollBars =
        [
            new ScrollBarSnapshot
            {
                Vertical = true, Enabled = true, Minimum = 0, Maximum = 599, Page = 200, Position = 120,
                Rect = new PixelRect { X = 223, Y = 0, Width = 17, Height = 200 },
            },
        ],
    };

    private static void Validate(ControlNode pane)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = pane;
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Fact]
    public void ScrollingContainerCarriesItsBarAndViewport()
    {
        Validate(Pane());
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = Pane();
        var json = JsonNode.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        }))!;
        var node = json["window"]!["nodes"]![0]!;
        Assert.Equal(120, (int)node["scrollBars"]![0]!["position"]!);
        Assert.Equal(223, (int)node["clientRect"]!["width"]!);
        var reopened = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, JsonSerializer.SerializeToUtf8Bytes(json)));
        Assert.Equal(Pane().ScrollBars![0], reopened.Window.Nodes[0].ScrollBars![0]);
    }

    [Fact]
    public void BrokenScrollStateIsRejected()
    {
        var pane = Pane();
        var bar = pane.ScrollBars![0];
        foreach (var broken in new[]
        {
            pane with { ClientRect = null },
            pane with { ScrollBars = [] },
            pane with { ScrollBars = [bar, bar] },
            pane with { ScrollBars = [bar with { Position = 600 }] },
            pane with { ScrollBars = [bar with { Maximum = -1 }] },
            pane with { ScrollBars = [bar with { Rect = bar.Rect with { X = 230 } }] },
            pane with { ClientRect = new PixelRect { X = 0, Y = 0, Width = 241, Height = 200 } },
        })
        {
            Assert.Throws<ProtocolException>(() => Validate(broken));
        }
        var notAPane = TestData.Snapshot();
        notAPane.Nodes[0] = notAPane.Nodes[0] with { ScrollBars = [bar] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(notAPane));
    }

    [Theory]
    [InlineData("{\"vertical\":true,\"position\":300}")]
    [InlineData("{\"vertical\":false,\"lines\":-3}")]
    [InlineData("{\"lines\":100,\"vertical\":true}")]
    public void SetScrollAcceptsOnePositionOrLineCount(string value) => ValidateAction(value);

    [Theory]
    [InlineData("{\"vertical\":true}")]
    [InlineData("{\"vertical\":true,\"position\":1,\"lines\":1}")]
    [InlineData("{\"vertical\":true,\"lines\":0}")]
    [InlineData("{\"vertical\":true,\"lines\":101}")]
    [InlineData("{\"vertical\":\"yes\",\"position\":1}")]
    [InlineData("{\"vertical\":true,\"position\":70000}")]
    [InlineData("3")]
    public void SetScrollRejectsAmbiguousOrUnboundedRequests(string value) =>
        Assert.Throws<ProtocolException>(() => ValidateAction(value));

    [Fact]
    public void EmittedValueOmitsTheUnusedForm()
    {
        var value = JsonSerializer.SerializeToElement(new SetScrollActionValue { Vertical = true, Lines = 3 });
        Assert.False(value.TryGetProperty("position", out _));
        ValidateAction(value.GetRawText());
    }

    private static void ValidateAction(string value)
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "setScroll", NodeId = "10",
            Value = JsonDocument.Parse(value).RootElement.Clone(),
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
    }
}
