using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 41: an owner-draw ListBox (the Mouse applet's pointer list) carries its rows as its owner
// painted them; only the selected rows carry a selected face.
public sealed class OwnerDrawListBoxProtocolTests
{
    private static ImageListEntry Face(byte shade) => new()
    {
        ImageWidth = 2, ImageHeight = 2, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(Enumerable.Range(0, 4).SelectMany(_ => new byte[] { shade, shade, shade, 255 }).ToArray()),
    };

    private static ControlNode List(OwnerDrawListSnapshot? painted, int selected = 1, string style = "0x50210151") =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "listBox", Style = style, Text = string.Empty,
            Items = ["Normal Select", "Help Select", "Busy"], SelectedIndex = selected,
            OwnerDrawList = painted,
        };

    private static OwnerDrawListSnapshot Painted(int selected = 1, string mode = "fixed") => new()
    {
        Mode = mode,
        Items = Enumerable.Range(0, 3).Select(index => new OwnerDrawListItem
        {
            Normal = Face(250), Selected = index == selected ? Face(40) : null,
        }).ToList(),
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    private static byte[] Wire(ControlNode node) => ProtocolSerializer.Serialize(new WindowOpenMessage
    {
        SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] },
    });

    [Fact]
    public void AnOwnerDrawListTravelsWithItsFacesAndShowsTheSelectedOne()
    {
        Validate(List(Painted()));
        Validate(List(Painted(-1), selected: -1));
        var received = Assert.Single(Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Wire(List(Painted())))).Window.Nodes);
        var painted = Assert.IsType<OwnerDrawListSnapshot>(received.OwnerDrawList);
        Assert.Null(painted.Items[0].Selected);
        Assert.Equal(Face(40), OwnerDrawListBoxControl.FaceFor(painted, 1));
        Assert.Equal(Face(250), OwnerDrawListBoxControl.FaceFor(painted, 2));
        Assert.NotNull(ControlNodeViewModel.FromSnapshot(received).OwnerDrawList);
    }

    [Fact]
    public void FacesMustMatchTheNativeShapeAndSelection()
    {
        foreach (var node in new[]
        {
            List(null),                                  // owner-draw style without faces
            List(Painted(2)),                            // selected face on an unselected row
            List(Painted(), selected: 2),                // the selected row has no selected face
            List(Painted(mode: "variable")),             // fixed style, variable faces
            List(Painted(), style: "0x50210101"),        // faces on a plain list
            List(Painted() with { Items = Painted().Items.Take(2).ToList() }),
        }) Assert.Throws<ProtocolException>(() => Validate(node));
        var combo = TestData.Snapshot().Nodes[0] with { Kind = "button", OwnerDrawList = Painted() };
        Assert.Throws<ProtocolException>(() => Validate(combo));
    }

    [Fact]
    public void TheRawWireNeedsEveryRowsNormalFace()
    {
        var json = JsonNode.Parse(Wire(List(Painted())))!;
        json["window"]!["nodes"]![0]!["ownerDrawList"]!["items"]![0]!.AsObject().Remove("normal");
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
    }
}

// Minor 41: a BS_OWNERDRAW push button's picture is its whole face (Internet Options' colour buttons
// paint their label and a swatch), up to 512 by 128; any other button picture keeps the icon cap.
public sealed class OwnerDrawButtonFaceTests
{
    private static ControlNode Button(string style, int width, int height) => TestData.Snapshot().Nodes[0] with
    {
        Kind = "button", Style = style, Text = "&Text:",
        ImageWidth = width, ImageHeight = height, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(new byte[width * height * 4]),
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void AnOwnerDrawFaceMayBeWiderThanAnIcon()
    {
        Validate(Button("0x5001000B", 130, 22));
        Validate(Button("0x50010000", 32, 22));
        Assert.Throws<ProtocolException>(() => Validate(Button("0x50010000", 130, 22)));
        Assert.Throws<ProtocolException>(() => Validate(Button("0x5001000B", 600, 22)));
        Assert.Throws<ProtocolException>(() => Validate(Button("0x5001000B", 130, 140)));
    }
}