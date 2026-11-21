namespace JuceSharp.Interop;

/// <summary>
/// One native "probe the count, then copy" call: <c>(null, 0)</c> reports how many
/// items there are, <c>(buffer, capacity)</c> fills at most <c>capacity</c> of them.
/// <c>count</c> is always the number of items the native side HAS, which can be more
/// than <c>capacity</c> when the list grew between the probe and the copy.
/// </summary>
internal delegate NativeMethods.DcaStatus CountedRead<T>(T[]? buffer, int capacity, out int count);

/// <summary>Runs a <see cref="CountedRead{T}"/> so a list that grows between the probe
/// and the copy is never indexed past its buffer.</summary>
internal static class CountedReads
{
    /// <summary>How many times a read is repeated with a larger buffer before the
    /// items that fit are returned.</summary>
    internal const int MaxAttempts = 4;

    /// <summary>
    /// Probes the count and copies the items, repeating the copy with a larger buffer while
    /// the native side reports more items than the buffer holds (at most
    /// <see cref="MaxAttempts"/> times; a list that is still growing then returns the items
    /// that fit). Returns the buffer and how many leading entries of it are valid: none
    /// when the native side fails or has nothing.
    /// </summary>
    internal static (T[] Items, int Count) Read<T>(CountedRead<T> read)
    {
        if (read(null, 0, out var count) != NativeMethods.DcaStatus.Ok || count <= 0)
            return (Array.Empty<T>(), 0);
        for (var attempt = 1; ; attempt++)
        {
            var buffer = new T[count];
            if (read(buffer, buffer.Length, out var actual) != NativeMethods.DcaStatus.Ok || actual <= 0)
                return (Array.Empty<T>(), 0);
            if (actual <= buffer.Length)
                return (buffer, actual);
            if (attempt == MaxAttempts)
                return (buffer, buffer.Length);
            count = actual;
        }
    }
}
