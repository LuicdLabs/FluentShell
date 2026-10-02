namespace FluentShell.Renderer.Windows;

internal enum StaticDecorationTone { Dark, Gray, Light, Shadow, Highlight }

[Flags]
internal enum StaticDecorationEdges { TopLeft = 1, BottomRight = 2, All = 3 }

internal readonly record struct StaticDecorationStroke(
    int Inset, StaticDecorationTone Tone, StaticDecorationEdges Edges);

// Physical-pixel rings preserve a native Static's frame/fill distinction without
// giving an inert decoration a control role or covering a frame's interior.
internal sealed record StaticDecorationPresentation(
    StaticDecorationTone? FillTone,
    int FillInset,
    IReadOnlyList<StaticDecorationStroke> Strokes)
{
    internal static StaticDecorationPresentation FromStyles(ulong style, ulong exStyle)
    {
        var type = style & 0x1F;
        var tone = type switch
        {
            4 or 7 => StaticDecorationTone.Dark,
            5 or 8 or 18 => StaticDecorationTone.Gray,
            6 or 9 => StaticDecorationTone.Light,
            _ => throw new ArgumentOutOfRangeException(nameof(style), "Unsupported Static decoration draw style."),
        };
        var strokes = new List<StaticDecorationStroke>();
        var inset = 0;
        void AddBevel(bool raised)
        {
            strokes.Add(new(inset, raised ? StaticDecorationTone.Highlight : StaticDecorationTone.Shadow,
                StaticDecorationEdges.TopLeft));
            strokes.Add(new(inset, raised ? StaticDecorationTone.Shadow : StaticDecorationTone.Highlight,
                StaticDecorationEdges.BottomRight));
            ++inset;
        }

        // The extended style belongs to the nonclient edge; SS_SUNKEN belongs
        // to the Static's client drawing. Both may be present on the same HWND.
        if ((exStyle & 0x00020000) != 0) AddBevel(raised: false);
        if ((style & 0x00001000) != 0) AddBevel(raised: false);
        if (type == 18)
        {
            AddBevel(raised: false);
            AddBevel(raised: true);
        }
        else if (type is 7 or 8 or 9)
        {
            strokes.Add(new(inset, tone, StaticDecorationEdges.All));
        }
        return new(type is 4 or 5 or 6 ? tone : null, inset, strokes);
    }
}
