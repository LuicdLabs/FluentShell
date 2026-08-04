using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

internal static class TestData
{
    public const string Nonce = "00112233445566778899AABBCCDDEEFF";

    public static WindowSnapshot Snapshot(string revision = "7", string text = "Original") => new()
    {
        SurfaceId = Guid.Parse("11111111-2222-3333-4444-555555555555"),
        SurfaceKind = "messageBox",
        Modal = true,
        CanCancel = true,
        Icon = "warning",
        Generation = "3",
        Revision = revision,
        NativeHwnd = "0x1234",
        Title = "Legacy title",
        Dpi = 144,
        Bounds = new PixelRect { X = 100, Y = 200, Width = 600, Height = 400 },
        ClientBounds = new PixelRect { X = 0, Y = 0, Width = 580, Height = 350 },
        WindowStyle = "0x10CF0000",
        WindowExStyle = "0x00000100",
        Visible = true,
        Enabled = true,
        State = "normal",
        ShowInTaskbar = true,
        Rtl = false,
        Nodes =
        [
            new ControlNode
            {
                NodeId = "10",
                Generation = "1",
                NativeHwnd = "0x5678",
                Kind = "edit",
                ControlId = 100,
                ZIndex = 0,
                TabIndex = 0,
                Rect = new PixelRect { X = 20, Y = 30, Width = 200, Height = 30 },
                Visible = true,
                Enabled = true,
                TabStop = true,
                DialogCode = 0,
                Text = text,
                Items = [],
            }
        ],
    };
}
