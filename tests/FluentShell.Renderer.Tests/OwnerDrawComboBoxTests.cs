using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Tests;

public sealed class OwnerDrawComboBoxTests
{
    private static ImageListEntry Face(int width = 2, int height = 1, byte color = 32) => new()
    {
        ImageWidth = width, ImageHeight = height, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(Enumerable.Range(0, width * height)
            .SelectMany(_ => new byte[] { color, 0, 0, 255 }).ToArray()),
    };

    private static ControlNode Combo(bool editable = false, bool variable = false) => new()
    {
        NodeId = "1", Generation = "1", NativeHwnd = "0x31", Kind = "comboBox",
        Style = variable ? editable ? "0x50010062" : "0x50010063" :
            editable ? "0x50010052" : "0x50010053",
        Rect = new PixelRect { Width = 180, Height = 28 }, Visible = true, Enabled = true,
        TabIndex = -1, SelectedIndex = 1, Editable = editable, Text = "Second",
        Items = ["First", "Second"],
        OwnerDrawCombo = new OwnerDrawComboSnapshot
        {
            Mode = variable ? "variable" : "fixed",
            Items = [new() { Normal = Face(), Selected = Face(color: 90) },
                new() { Normal = Face(height: variable ? 2 : 1), Selected = Face(height: variable ? 2 : 1, color: 90) }],
            SelectionFace = editable ? null : Face(color: 180),
        },
    };

    private static byte[] Payload(ControlNode node) => ProtocolSerializer.Serialize(new WindowOpenMessage
    {
        SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] },
    });

    private static WindowOpenMessage Read(ControlNode node) => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Payload(node)));

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void NativeOwnerFacesRoundTripWithEditorAndSelection(bool editable, bool variable)
    {
        var node = Read(Combo(editable, variable)).Window.Nodes.Single();
        Assert.Equal(["First", "Second"], node.Items);
        Assert.Equal(1, node.SelectedIndex);
        Assert.Equal(editable, node.Editable);
        Assert.Equal(variable ? "variable" : "fixed", node.OwnerDrawCombo!.Mode);
        Assert.Equal(editable, node.OwnerDrawCombo.SelectionFace is null);
        Assert.Equal(2, node.OwnerDrawCombo.Items.Count);
    }

    [Fact]
    public void InvalidStyleShapeOrFacetPlacementIsRejected()
    {
        var node = Combo();
        foreach (var changed in new[]
        {
            node with { Kind = "listBox" }, node with { NativeHwnd = null },
            node with { Style = "0x50010043" }, node with { Style = "0x50010073" },
            node with { Style = "0x50010051" }, node with { Editable = true },
            node with { OwnerDrawCombo = null },
            node with { OwnerDrawCombo = node.OwnerDrawCombo! with { Mode = "variable" } },
            node with { OwnerDrawCombo = node.OwnerDrawCombo! with { SelectionFace = null } },
            node with { Items = ["First", ""] },
            node with { OwnerDrawCombo = node.OwnerDrawCombo! with { Items = [] } },
        }) Assert.Throws<ProtocolException>(() => Read(changed));
    }

    [Fact]
    public void FacesEnforceDimensionsStateAlignmentPremultiplicationAndTotalBudget()
    {
        var node = Combo();
        ControlNode WithFaces(List<OwnerDrawComboItem> items) => node with
        {
            Items = items.Select((_, index) => $"Item {index}").ToList(), SelectedIndex = 0,
            OwnerDrawCombo = node.OwnerDrawCombo! with { Items = items },
        };
        foreach (var face in new[]
        {
            Face(513), Face(height: 129), Face() with { ImageData = "AAAA" },
            Face() with { ImageFormat = "rgba8" },
            Face(1) with { ImageData = Convert.ToBase64String([1, 0, 0, 0]) },
        }) Assert.Throws<ProtocolException>(() => Read(WithFaces([new() { Normal = face, Selected = face }])));
        Assert.Throws<ProtocolException>(() => Read(WithFaces([new() { Normal = Face(), Selected = Face(height: 2) }])));
        Assert.Throws<ProtocolException>(() => Read(WithFaces([
            new() { Normal = Face(), Selected = Face() }, new() { Normal = Face(height: 2), Selected = Face(height: 2) }])));
        var large = Face(512, 128);
        Assert.Throws<ProtocolException>(() => Read(WithFaces(Enumerable.Range(0, 3)
            .Select(_ => new OwnerDrawComboItem { Normal = large, Selected = large }).ToList())));
        Assert.Throws<ProtocolException>(() => Read(WithFaces(Enumerable.Range(0, 65)
            .Select(_ => new OwnerDrawComboItem { Normal = Face(), Selected = Face() }).ToList())));
    }

    [Theory]
    [InlineData("mode")]
    [InlineData("items")]
    [InlineData("normal")]
    [InlineData("imageData")]
    public void RawRequiredFaceFieldsCannotBeDefaulted(string field)
    {
        var payload = JsonNode.Parse(Payload(Combo()))!;
        var painted = payload["window"]!["nodes"]![0]!["ownerDrawCombo"]!.AsObject();
        if (field is "mode" or "items") painted.Remove(field);
        else if (field == "normal") painted["items"]![0]!.AsObject().Remove(field);
        else painted["items"]![0]!["normal"]!.AsObject().Remove(field);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            JsonSerializer.SerializeToUtf8Bytes(payload)));
    }

    [Fact]
    public void ProjectionUsesRealComboBoxAndNativeLabelsWithCorrectFaceState()
    {
        Assert.True(typeof(ComboBox).IsAssignableFrom(typeof(OwnerDrawComboBoxControl)));
        var node = Combo();
        var painted = node.OwnerDrawCombo!;
        Assert.Same(painted.Items[0].Normal, OwnerDrawComboBoxControl.FaceFor(painted, 0, 1, 1, true));
        Assert.Same(painted.Items[1].Selected, OwnerDrawComboBoxControl.FaceFor(painted, 1, 1, 1, true));
        Assert.Same(painted.SelectionFace, OwnerDrawComboBoxControl.FaceFor(painted, 1, 1, 1, false));
        Assert.Same(painted.Items[0].Selected, OwnerDrawComboBoxControl.FaceFor(painted, 0, 0, 1, false));
        Assert.Equal("Native label", new OwnerDrawComboItemPresentation("Native label").ToString());
        var viewModel = ControlNodeViewModel.FromSnapshot(node);
        Assert.Same(painted, viewModel.OwnerDrawCombo);
        viewModel.DraftText = "pending edit";
        viewModel.ApplySnapshot(node with { OwnerDrawCombo = painted with { SelectionFace = Face(color: 240) } },
            preserveTransient: true);
        Assert.Equal("pending edit", viewModel.DraftText);
        Assert.Equal(1, viewModel.SelectedIndex);
        Assert.Equal(Face(color: 240), viewModel.OwnerDrawCombo!.SelectionFace);
    }

    [Fact]
    public void SharedOwnerDrawFixtureIsAdmitted()
    {
        var bytes = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures",
            "window.open.owner-draw-combo.json"));
        var message = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes));
        Assert.Equal("fixed", message.Window.Nodes.Single().OwnerDrawCombo!.Mode);
    }
}
