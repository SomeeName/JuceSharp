using System.Collections.Concurrent;

namespace JuceSharp.Dsp;

/// <summary>
/// The one Hann-window implementation for this assembly — and, since it is the window
/// every analysis type here runs through, the one a CALLER needs too: an application
/// feeding <see cref="JuceFft"/> directly, or comparing its own STFT against
/// <see cref="JuceSpectrogramBuilder"/>, had to hand-roll a seventh copy of the two loops
/// this type exists to retire. Public for exactly that reason.
/// <para>Seven classes hand-rolled the same two loops, and the two differ only
/// in the divisor — which is exactly the difference that silently changes an
/// analysis result. <b>Symmetric</b> (<c>N-1</c>) is the classical analysis
/// window; <b>periodic</b> (<c>N</c>) is what <c>numpy.hanning(sym=False)</c> /
/// <c>torch.hann_window</c> produce and the only one whose squared overlap sums
/// to a constant for hop = N/4, so it is the correct choice for a
/// reconstructing STFT.</para>
/// <para><b>The returned array is CACHED and shared — treat it as read-only.</b>
/// Every caller in this assembly windows by multiplying into its own scratch;
/// a caller that wrote into the window would corrupt every later transform of
/// the same size.</para>
/// </summary>
public static class Windows
{
    private static readonly ConcurrentDictionary<(int N, bool Periodic), float[]> Cache = new();

    /// <summary>
    /// Hann window of length <paramref name="n"/>, built once per
    /// <c>(length, periodic)</c> pair and then served from a process-wide cache.
    /// <para><b>The returned array is SHARED — treat it as read-only.</b> Window by
    /// multiplying into your own scratch; writing into it corrupts every later transform
    /// of the same length, in every thread.</para>
    /// <para>Thread-safe: the cache is a <see cref="ConcurrentDictionary{TKey,TValue}"/>
    /// and the build function is pure.</para>
    /// </summary>
    /// <param name="n">Window length in samples; must be positive.</param>
    /// <param name="periodic">
    /// <c>false</c> (default) → <c>0.5·(1 - cos(2πi/(n-1)))</c>, the symmetric
    /// analysis form, which is what every analysis type in this assembly uses.
    /// <c>true</c> → <c>0.5·(1 - cos(2πi/n))</c>, the periodic / DFT-even form that
    /// <c>numpy.hanning(sym=False)</c> and <c>torch.hann_window</c> produce and the only
    /// one whose squared overlap sums to a constant at hop = n/4 — so it, not the
    /// default, is the correct choice for a RECONSTRUCTING STFT.
    /// </param>
    /// <returns>The cached, shared window of length <paramref name="n"/>.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="n"/> is not
    /// positive.</exception>
    public static float[] Hann(int n, bool periodic = false)
    {
        if (n <= 0) throw new ArgumentOutOfRangeException(nameof(n));
        return Cache.GetOrAdd((n, periodic), static key => Build(key.N, key.Periodic));
    }

    private static float[] Build(int n, bool periodic)
    {
        var w = new float[n];
        if (n == 1) { w[0] = periodic ? 0f : 1f; return w; }
        float denom = periodic ? n : n - 1;
        for (var i = 0; i < n; i++)
            w[i] = 0.5f * (1f - MathF.Cos(2f * MathF.PI * i / denom));
        return w;
    }
}
