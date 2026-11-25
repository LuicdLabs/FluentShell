namespace FluentShell.Renderer.Windows;

// The bitmap and its base64 key both consume memory. Bound retention by their
// combined size and entry count; decoded images still used by controls remain
// alive independently when their cache entries are evicted.
internal sealed class BoundedImageCache<T> where T : class
{
    internal readonly record struct Key(int Width, int Height, string Data);
    private sealed record Entry(Key Key, T Value, long Bytes);

    private readonly long _maximumBytes;
    private readonly int _maximumEntries;
    private readonly Dictionary<Key, LinkedListNode<Entry>> _entries = [];
    private readonly LinkedList<Entry> _recency = [];

    internal BoundedImageCache(long maximumBytes, int maximumEntries)
    {
        if (maximumBytes <= 0) throw new ArgumentOutOfRangeException(nameof(maximumBytes));
        if (maximumEntries <= 0) throw new ArgumentOutOfRangeException(nameof(maximumEntries));
        _maximumBytes = maximumBytes;
        _maximumEntries = maximumEntries;
    }

    internal int Count => _entries.Count;
    internal long RetainedBytes { get; private set; }

    internal T GetOrCreate(Key key, Func<T> create)
    {
        if (key.Width <= 0 || key.Height <= 0)
            throw new InvalidOperationException("Validated pixel dimensions are unavailable.");
        var bytes = checked((long)key.Width * key.Height * 4 + (long)key.Data.Length * sizeof(char));
        if (_entries.TryGetValue(key, out var cached))
        {
            _recency.Remove(cached);
            _recency.AddFirst(cached);
            return cached.Value.Value;
        }
        // A failed decode must never poison the cache, and a large one-off
        // picture must not evict every reusable icon just to bypass the cap.
        var value = create();
        if (bytes > _maximumBytes) return value;
        while (_entries.Count >= _maximumEntries || RetainedBytes > _maximumBytes - bytes)
        {
            var oldest = _recency.Last!;
            _recency.RemoveLast();
            _entries.Remove(oldest.Value.Key);
            RetainedBytes -= oldest.Value.Bytes;
        }
        var entry = _recency.AddFirst(new Entry(key, value, bytes));
        _entries.Add(key, entry);
        RetainedBytes += bytes;
        return value;
    }

    internal void Clear()
    {
        _entries.Clear();
        _recency.Clear();
        RetainedBytes = 0;
    }
}
