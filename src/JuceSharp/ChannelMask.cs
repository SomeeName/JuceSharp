namespace JuceSharp;

/// <summary>
/// Helpers to build the channel bitmasks consumed by <see cref="JuceEngineConfig"/>
/// and <see cref="JuceAudioEngine.SetDeviceType"/>. LSB = channel 0.
/// </summary>
public static class ChannelMask
{
    /// <summary>Activate one channel (zero-based). Out-of-range indices return
    /// an EMPTY mask, never a wrong one.</summary>
    /// <remarks>The upper bound is load-bearing, not defensive. C# masks a
    /// shift count to the operand width, so <c>1u &lt;&lt; 32</c> is <c>1u</c> —
    /// asking for channel 32 used to silently select channel 0, which on a
    /// large interface is a different guitar. <see cref="FromChannels"/> always
    /// carried the <c>c &lt; 32</c> guard; this did not.</remarks>
    public static uint Single(int channel) => channel is < 0 or >= 32 ? 0u : 1u << channel;

    /// <summary>Activate the first <paramref name="count"/> channels (channels 0..count-1).
    /// Counts above 32 saturate at all 32.</summary>
    /// <remarks><c>1L &lt;&lt; 64</c> is <c>1L</c> for the same reason, so an
    /// unguarded <c>count = 64</c> returned 0 — no channels at all.</remarks>
    public static uint First(int count) =>
        count <= 0 ? 0u : count >= 32 ? uint.MaxValue : (uint)((1L << count) - 1);

    /// <summary>Mono = channel 0 only.</summary>
    public static uint Mono => 1u;

    /// <summary>Stereo = channels 0 and 1.</summary>
    public static uint Stereo => 0b11u;

    /// <summary>Custom mask from individual channel indices.</summary>
    public static uint FromChannels(params int[] channels)
    {
        var m = 0u;
        if (channels is null) return 0u;
        foreach (var c in channels)
            if (c >= 0 && c < 32) m |= 1u << c;
        return m;
    }

    /// <summary>Iterate the active channel indices in a mask, lowest first.</summary>
    public static IEnumerable<int> Enumerate(uint mask)
    {
        for (var i = 0; i < 32; i++)
            if ((mask & (1u << i)) != 0)
                yield return i;
    }
}
