using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

internal static class TestData
{
    public const string Nonce = "00112233445566778899AABBCCDDEEFF";

    // An HWND-backed window surface: one real native edit control, which is the
    // shape almost every rule in the protocol is about.
    public static WindowSnapshot Snapshot(string revision = "7", string text = "Original") => new()
    {
        SurfaceId = Guid.Parse("11111111-2222-3333-4444-555555555555"),
        SurfaceKind = "window",
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

    // A translated MessageBox: the Bridge answers the API call itself, so the
    // surface has no native dialog HWND tree and every node is virtual.
    public static WindowSnapshot DialogSnapshot(string revision = "1") => Snapshot(revision) with
    {
        SurfaceKind = "messageBox",
        Nodes =
        [
            new ControlNode
            {
                NodeId = "1",
                Generation = "3",
                NativeHwnd = null,
                Kind = "static",
                ControlId = 0,
                ZIndex = 0,
                TabIndex = -1,
                Rect = new PixelRect { X = 24, Y = 24, Width = 480, Height = 120 },
                Visible = true,
                Enabled = true,
                TabStop = false,
                DialogCode = 0,
                Text = "The native call was answered by the projection.",
                Items = [],
            },
            new ControlNode
            {
                NodeId = "2",
                Generation = "3",
                NativeHwnd = null,
                Kind = "button",
                ControlId = 1,
                ZIndex = 1,
                TabIndex = 0,
                Rect = new PixelRect { X = 380, Y = 200, Width = 120, Height = 32 },
                Visible = true,
                Enabled = true,
                TabStop = true,
                DialogCode = 0,
                Text = "OK",
                IsDefault = true,
                Items = [],
            }
        ],
    };
}
