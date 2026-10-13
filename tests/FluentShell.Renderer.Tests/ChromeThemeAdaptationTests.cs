using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class ChromeThemeAdaptationTests
{
    private static byte[] Band(params (byte R, byte G, byte B, byte A)[] colors) =>
        colors.SelectMany(color => new[]
        {
            (byte)(color.B * color.A / 255), (byte)(color.G * color.A / 255),
            (byte)(color.R * color.A / 255), color.A,
        }).ToArray();

    [Fact]
    public void ALightBandIsRedrawnOnTheDarkSideAndItsColoursAreKept()
    {
        var pixels = Band((255, 255, 255, 255), (255, 255, 255, 255), (250, 250, 250, 255),
            (0, 0, 0, 255), (0, 120, 215, 255), (230, 170, 110, 255));
        Assert.True(ChromeThemeAdaptation.AdaptToDark(pixels));
        Assert.True(pixels[0] < 0x30, "a white face did not turn dark");
        Assert.True(pixels[12] > 0xD0, "black text did not turn light");
        Assert.Equal(new byte[] { 215, 120, 0, 255 }, pixels[16..20]);
        // A ClearType fringe joins the grey scale instead of outlining the glyph in colour.
        Assert.True(pixels[20] == pixels[21] && pixels[21] == pixels[22], "a text fringe kept its colour");
    }

    [Fact]
    public void AClearTypeFringeBesideAColourStaysGreyWhenTheRowIsKnown()
    {
        var pixels = Band((255, 255, 255, 255), (255, 255, 255, 255), (250, 250, 250, 255),
            (0, 0, 0, 255), (0, 120, 215, 255), (230, 170, 110, 255));
        Assert.True(ChromeThemeAdaptation.AdaptToDark(pixels, width: 6));
        Assert.Equal(new byte[] { 215, 120, 0, 255 }, pixels[16..20]);
        Assert.True(pixels[20] == pixels[21] && pixels[21] == pixels[22], "a text fringe kept its colour");
    }

    // mstsc's blue title on its white banner: the half-covered edge pixels are the blue mixed
    // with white, and turn into the blue mixed with the dark face, not a grey outline.
    [Fact]
    public void TheSoftEdgeOfAColouredGlyphStaysThatColourOnTheDarkFace()
    {
        var pixels = Band((255, 255, 255, 255), (128, 173, 228, 255), (0, 90, 200, 255),
            (255, 255, 255, 255), (255, 255, 255, 255), (255, 255, 255, 255));
        Assert.True(ChromeThemeAdaptation.AdaptToDark(pixels, width: 6));
        Assert.Equal(new byte[] { 200, 90, 0, 255 }, pixels[8..12]);
        var (blue, green, red) = (pixels[4], pixels[5], pixels[6]);
        Assert.True(blue > red + 40 && blue > green, $"the glyph's edge turned grey ({red},{green},{blue})");
        Assert.True(blue < 160, "the glyph's edge stayed as light as the white it was mixed with");
        Assert.True(pixels[0] < 0x30, "the white face did not turn dark");
    }

    // mstsc's pixels: a ClearType fringe of its blue title on the banner's light blue-grey face.
    // Each channel has its own coverage, so the fringe is purple, and it stays a fringe of the
    // blue on the dark face rather than a grey dot beside the glyph.
    [Fact]
    public void AClearTypeFringeOfColouredTextFollowsItsTextOntoTheDarkFace()
    {
        var pixels = Band((0xE3, 0xEB, 0xF6, 255), (0xE3, 0xEB, 0xF6, 255), (0xC3, 0x95, 0xA9, 255),
            (0x00, 0x33, 0x99, 255), (0xE3, 0xEB, 0xF6, 255), (0xE3, 0xEB, 0xF6, 255));
        Assert.True(ChromeThemeAdaptation.AdaptToDark(pixels, width: 6));
        var (blue, green, red) = (pixels[8], pixels[9], pixels[10]);
        Assert.False(red == green && green == blue, "the fringe turned grey");
        Assert.True(blue > red && blue > green && blue < 0xA0, $"the fringe is not the blue's edge ({red},{green},{blue})");
        Assert.True(pixels[4] < 0x40, "the light face did not turn dark");
    }

    [Fact]
    public void ADarkBandIsLeftAsTheApplicationDrewIt()
    {
        var pixels = Band((30, 30, 30, 255), (40, 40, 40, 255), (230, 230, 230, 255));
        var original = pixels.ToArray();
        Assert.False(ChromeThemeAdaptation.AdaptToDark(pixels));
        Assert.Equal(original, pixels);
    }

    [Fact]
    public void TransparentPixelsStayTransparent()
    {
        var pixels = Band((255, 255, 255, 255), (0, 0, 0, 0), (255, 255, 255, 128));
        Assert.True(ChromeThemeAdaptation.AdaptToDark(pixels));
        Assert.Equal(new byte[] { 0, 0, 0, 0 }, pixels[4..8]);
        Assert.Equal(128, pixels[11]);
        Assert.True(pixels[8] <= 128, "a premultiplied channel exceeded its alpha");
    }
}
