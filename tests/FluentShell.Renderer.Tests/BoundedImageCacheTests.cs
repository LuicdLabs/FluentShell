using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class BoundedImageCacheTests
{
    [Fact]
    public void IdenticalPixelsAndDimensionsShareOneDecodedImage()
    {
        var cache = new BoundedImageCache<object>(1024, 16);
        var first = cache.GetOrCreate(new(1, 1, "AAAAAA=="), () => new object());
        var repeat = cache.GetOrCreate(new(1, 1, new string("AAAAAA==".ToCharArray())),
            () => throw new InvalidOperationException("An identical image was decoded again."));
        Assert.Same(first, repeat);
        Assert.NotSame(first, cache.GetOrCreate(new(1, 1, "AQAAAA=="), () => new object()));
        Assert.NotSame(first, cache.GetOrCreate(new(2, 1, "AAAAAA=="), () => new object()));
    }

    [Fact]
    public void LeastRecentlyUsedEntryIsEvictedByCount()
    {
        var cache = new BoundedImageCache<object>(1024, 2);
        var first = cache.GetOrCreate(new(1, 1, "first"), () => new object());
        var second = cache.GetOrCreate(new(1, 1, "second"), () => new object());
        Assert.Same(first, cache.GetOrCreate(new(1, 1, "first"), () => new object()));
        cache.GetOrCreate(new(1, 1, "third"), () => new object());
        Assert.Equal(2, cache.Count);
        Assert.Same(first, cache.GetOrCreate(new(1, 1, "first"), () => new object()));
        Assert.NotSame(second, cache.GetOrCreate(new(1, 1, "second"), () => new object()));
    }

    [Fact]
    public void ByteCapIncludesEncodedKeysAndOversizedImagesDoNotDisplaceIcons()
    {
        var cache = new BoundedImageCache<object>(40, 256);
        var icon = cache.GetOrCreate(new(1, 1, "AAAAAA=="), () => new object());
        Assert.Equal(20, cache.RetainedBytes);
        cache.GetOrCreate(new(1, 1, "AQAAAA=="), () => new object());
        Assert.Equal(40, cache.RetainedBytes);
        cache.GetOrCreate(new(1024, 1024, "large"), () => new object());
        Assert.Equal(2, cache.Count);
        Assert.Same(icon, cache.GetOrCreate(new(1, 1, "AAAAAA=="), () => new object()));
        cache.GetOrCreate(new(1, 1, "AgAAAA=="), () => new object());
        Assert.Equal(2, cache.Count);
        Assert.InRange(cache.RetainedBytes, 0, 40);
        cache.Clear();
        Assert.Equal(0, cache.Count);
        Assert.Equal(0, cache.RetainedBytes);
    }

    [Fact]
    public void DecodeFailuresDoNotReplaceHealthyImagesOrPoisonFutureAttempts()
    {
        var cache = new BoundedImageCache<object>(20, 1);
        var healthy = cache.GetOrCreate(new(1, 1, "AAAAAA=="), () => new object());
        Assert.Throws<FormatException>(() => cache.GetOrCreate(new(1, 1, "invalid"),
            () => throw new FormatException()));
        Assert.Same(healthy, cache.GetOrCreate(new(1, 1, "AAAAAA=="), () => new object()));
        var recovered = new object();
        Assert.Same(recovered, cache.GetOrCreate(new(1, 1, "invalid"), () => recovered));
        Assert.Equal(1, cache.Count);
    }
}
