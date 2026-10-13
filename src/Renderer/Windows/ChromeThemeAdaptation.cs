namespace FluentShell.Renderer.Windows;

/// <summary>
/// A container's painted band (MMC's "Actions" title strip, a caption band) arrives as the
/// pixels the native window drew, which is the light system theme.  Over a dark proxy such
/// a band reads as a hole in the window.  In dark mode a band that is mostly light is
/// redrawn on the dark side of the scale: every near-grey pixel -- the band's face, its
/// text, its rules -- has its lightness mirrored into the dark theme's range, and every
/// coloured pixel -- an icon, an accent -- is left exactly as the application drew it, its
/// antialiased edge mixed with the dark face instead of the white one.
/// A band that is already dark is left alone, so an application with its own dark theme
/// is never turned light.
/// </summary>
internal static class ChromeThemeAdaptation
{
    // The dark theme's range: a band face lands near the window's own layer colour and
    // black text near the theme's primary text colour.
    private const double DarkFloor = 0x20 / 255.0;
    private const double LightCeiling = 0xE8 / 255.0;
    // Below this saturation a pixel is part of the band's grey scale, not a colour.  It is
    // high on purpose: ClearType draws grey text with coloured fringes (about 0.5), which
    // kept their colour on a dark face and outlined every glyph in orange and blue.
    private const double GreySaturation = 0.6;
    // The band is adapted only when its opaque pixels are this light on average.
    private const double LightBand = 0.6;

    // A pixel next to a coloured one that reads as that colour mixed with the face around it
    // is the colour's own antialiased edge (mstsc's blue title on its light banner): it is
    // mixed again with the dark face instead of joining the grey scale, which drew a grey
    // outline around every coloured glyph.  ClearType covers each channel separately, so each
    // channel has its own coverage; a pixel some channel cannot be explained in by mixing
    // the two (a ClearType fringe of grey text beside a colour) is no such edge.
    private const double CoverageSlack = 0.2;
    // A channel the colour and the face nearly share says nothing about the coverage.
    private const double TellingChannel = 24;
    // The offsets an edge looks at for its colour, nearest first, out to two pixels.
    private static readonly (int Dx, int Dy)[] EdgeNeighbourhood =
        (from dy in Enumerable.Range(-2, 5)
         from dx in Enumerable.Range(-2, 5)
         where dx != 0 || dy != 0
         orderby dx * dx + dy * dy
         select (dx, dy)).ToArray();

    /// <summary>
    /// Adapts premultiplied BGRA pixels in place; returns whether it did.  With the row
    /// width, the antialiased edge of coloured content is kept coloured too.
    /// </summary>
    public static bool AdaptToDark(byte[] pixels, int width = 0)
    {
        var colours = width > 0 ? ColouredPixels(pixels) : null;
        var original = colours is null ? null : pixels.ToArray();
        var faces = colours is null ? null : RowFaces(pixels, colours, width);
        double total = 0;
        var opaque = 0;
        for (var offset = 0; offset + 3 < pixels.Length; offset += 4)
        {
            var alpha = pixels[offset + 3];
            if (alpha == 0) continue;
            total += Luminance(pixels[offset + 2], pixels[offset + 1], pixels[offset], alpha);
            ++opaque;
        }
        if (opaque == 0 || total / opaque < LightBand) return false;
        for (var offset = 0; offset + 3 < pixels.Length; offset += 4)
        {
            var alpha = pixels[offset + 3];
            if (alpha == 0) continue;
            var scale = 255.0 / alpha;
            var blue = Math.Min(255, pixels[offset] * scale);
            var green = Math.Min(255, pixels[offset + 1] * scale);
            var red = Math.Min(255, pixels[offset + 2] * scale);
            var max = Math.Max(red, Math.Max(green, blue));
            var min = Math.Min(red, Math.Min(green, blue));
            if (max > 0 && (max - min) / max >= GreySaturation) continue;
            if (colours is not null && original is not null && faces is not null &&
                ColouredEdge(original, colours, faces, offset / 4, width, red, green, blue) is { } edge)
            {
                pixels[offset] = (byte)Math.Round(edge.Blue * alpha / 255.0);
                pixels[offset + 1] = (byte)Math.Round(edge.Green * alpha / 255.0);
                pixels[offset + 2] = (byte)Math.Round(edge.Red * alpha / 255.0);
                continue;
            }
            var lightness = (0.299 * red + 0.587 * green + 0.114 * blue) / 255.0;
            var mirrored = DarkFloor + (1 - lightness) * (LightCeiling - DarkFloor);
            var grey = (byte)Math.Round(mirrored * alpha);
            pixels[offset] = grey;
            pixels[offset + 1] = grey;
            pixels[offset + 2] = grey;
        }
        return true;
    }

    // Which pixels keep their colour: opaque enough to judge and saturated.
    private static bool[] ColouredPixels(byte[] pixels)
    {
        var coloured = new bool[pixels.Length / 4];
        for (var offset = 0; offset + 3 < pixels.Length; offset += 4)
        {
            var alpha = pixels[offset + 3];
            if (alpha == 0) continue;
            var (red, green, blue) = Straight(pixels, offset);
            var max = Math.Max(red, Math.Max(green, blue));
            var min = Math.Min(red, Math.Min(green, blue));
            coloured[offset / 4] = max > 0 && (max - min) / max >= GreySaturation;
        }
        return coloured;
    }

    private static (double Red, double Green, double Blue) Straight(byte[] pixels, int offset)
    {
        var scale = 255.0 / pixels[offset + 3];
        return (Math.Min(255, pixels[offset + 2] * scale), Math.Min(255, pixels[offset + 1] * scale),
            Math.Min(255, pixels[offset] * scale));
    }

    // Each row's face: the light grey most of its pixels are, which follows a band that is a
    // vertical gradient (mstsc's banner).  Null for a row with no light grey at all.
    private static (double Red, double Green, double Blue)?[] RowFaces(byte[] pixels, bool[] colours, int width)
    {
        var height = colours.Length / width;
        var faces = new (double Red, double Green, double Blue)?[height];
        var counts = new Dictionary<uint, int>();
        for (var y = 0; y < height; ++y)
        {
            counts.Clear();
            for (var x = 0; x < width; ++x)
            {
                var index = y * width + x;
                if (colours[index] || pixels[index * 4 + 3] != 255) continue;
                var (red, green, blue) = Straight(pixels, index * 4);
                if ((0.299 * red + 0.587 * green + 0.114 * blue) / 255.0 < LightBand) continue;
                var key = BitConverter.ToUInt32(pixels, index * 4);
                counts[key] = counts.GetValueOrDefault(key) + 1;
            }
            if (counts.Count == 0) continue;
            var face = counts.MaxBy(pair => pair.Value).Key;
            faces[y] = ((face >> 16) & 0xff, (face >> 8) & 0xff, face & 0xff);
        }
        return faces;
    }

    // The pixel as its coloured neighbour's antialiased edge over the dark face, or null when it
    // is not a mix of a neighbouring colour and the face around it.
    private static (double Red, double Green, double Blue)? ColouredEdge(byte[] pixels, bool[] colours,
        (double Red, double Green, double Blue)?[] faces, int index, int width, double red, double green, double blue)
    {
        var height = colours.Length / width;
        var (x, y) = (index % width, index / width);
        if (faces[y] is not { } light) return null;
        var faceLightness = (0.299 * light.Red + 0.587 * light.Green + 0.114 * light.Blue) / 255.0;
        var dark = (DarkFloor + (1 - faceLightness) * (LightCeiling - DarkFloor)) * 255;
        // The colour this pixel is an edge of: the nearest coloured pixel that explains it (in a
        // dense glyph the core can be two pixels away, past other edge pixels).
        foreach (var (dx, dy) in EdgeNeighbourhood)
        {
            var (nx, ny) = (x + dx, y + dy);
            if (nx < 0 || ny < 0 || nx >= width || ny >= height || !colours[ny * width + nx]) continue;
            var colour = Straight(pixels, (ny * width + nx) * 4);
            var channels = new[]
            {
                (Value: red, Face: light.Red, Colour: colour.Red),
                (Value: green, Face: light.Green, Colour: colour.Green),
                (Value: blue, Face: light.Blue, Colour: colour.Blue),
            };
            var coverages = new double?[3];
            var explained = true;
            for (var channel = 0; channel < 3 && explained; ++channel)
            {
                var (value, faceValue, colourValue) = channels[channel];
                if (Math.Abs(faceValue - colourValue) < TellingChannel) continue;
                var coverage = (faceValue - value) / (faceValue - colourValue);
                explained = coverage >= -CoverageSlack && coverage <= 1 + CoverageSlack;
                coverages[channel] = Math.Clamp(coverage, 0, 1);
            }
            var known = coverages.Where(coverage => coverage is not null).Select(coverage => coverage!.Value).ToArray();
            if (!explained || known.Length == 0) continue;
            var shared = known.Average();
            double Over(int channel) =>
                (coverages[channel] ?? shared) * channels[channel].Colour + (1 - (coverages[channel] ?? shared)) * dark;
            return (Over(0), Over(1), Over(2));
        }
        return null;
    }

    private static double Luminance(byte red, byte green, byte blue, byte alpha) =>
        (0.299 * red + 0.587 * green + 0.114 * blue) / alpha;
}
