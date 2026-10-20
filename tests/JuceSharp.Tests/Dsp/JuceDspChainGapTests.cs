using JuceSharp.Dsp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     The three gaps DotCharter found the moment it adopted
///     <see cref="JuceDspChain" /> in place of its own P/Invokes (0.9):
///     id → node lookup, a PLANAR offline render, and a meter read that can say
///     "no reading" instead of four zeros.
///     <para>
///         Offline, with the device stopped, exactly like
///         <see cref="JuceDspChainWrapperTests" />: the offline render path
///         refuses to run while a device is open.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class JuceDspChainGapTests : IDisposable
{
    private const int SampleRate = 48000;
    private const int Frames = 1024;

    private readonly JuceAudioEngine? _engine;

    public JuceDspChainGapTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        _engine.Stop();
    }

    public void Dispose() => _engine?.Dispose();

    private JuceAudioEngine Engine => _engine ?? throw new InvalidOperationException("native DLL unavailable");

    // ---------------------------------------------------------------- TryGetNode

    [NativeAvailableFact]
    public void TryGetNode_HandsBackTheSameInstanceForANodeThisChainCreated()
    {
        var chain = Engine.GetDspChain();
        var node = chain.AddNode("gain");

        Assert.True(chain.TryGetNode(node.Id, out var found));
        Assert.Same(node, found);
    }

    /// <summary>
    /// The FALLBACK probe: a node this chain never created — here added straight
    /// through the ABI, which is what another binding in the same process does —
    /// is still found, and is cached so the second lookup is the O(1) path.
    /// </summary>
    [NativeAvailableFact]
    public void TryGetNode_FindsANodeItDidNotCreate_AndRemembersIt()
    {
        var chain = Engine.GetDspChain();
        var id = NativeMethods.EngineDspAddNodeForPlayer(Engine.NativeHandle, 0, "gain");
        Assert.True(id >= 0);

        Assert.True(chain.TryGetNode(id, out var found));
        Assert.Equal(id, found.Id);
        // ABI v33 makes a foreign node first-class: the TYPE comes from the engine.
        Assert.Equal("gain", found.TypeId);

        Assert.True(chain.TryGetNode(id, out var again));
        Assert.Same(found, again);
    }

    [NativeAvailableFact]
    public void TryGetNode_IsFalseForAnIdTheChainDoesNotHave()
    {
        var chain = Engine.GetDspChain();

        Assert.False(chain.TryGetNode(123456, out var missing));
        Assert.Null(missing);
        Assert.False(chain.TryGetNode(-1, out _));

        // …and a node that WAS here is gone once removed, rather than being
        // served out of the identity map forever.
        var node = chain.AddNode("gain");
        Assert.True(chain.TryGetNode(node.Id, out _));
        node.Remove();
        Assert.False(chain.TryGetNode(node.Id, out _));
    }

    // ------------------------------------------------------------ planar render

    /// <summary>
    /// THE POINT of the planar overload: it must be the same render as the
    /// interleaved one, sample for sample — it is the same export, reached without
    /// the two copies.
    /// </summary>
    [NativeAvailableFact]
    public void PlanarProcessOffline_IsSampleForSampleTheInterleavedRender()
    {
        var chain = Engine.GetDspChain();
        chain.Clear();
        var node = chain.AddNode("gain");
        node.SetParam("gainDb", 12f);

        var (left, right) = Planes();
        var interleaved = new float[Frames * 2];
        for (var i = 0; i < Frames; i++)
        {
            interleaved[i * 2] = left[i];
            interleaved[i * 2 + 1] = right[i];
        }

        var interleavedOut = new float[interleaved.Length];
        chain.ProcessOffline(interleaved, 2, SampleRate, interleavedOut);

        var outL = new float[Frames];
        var outR = new float[Frames];
        chain.ProcessOffline(left, right, SampleRate, outL, outR);

        for (var i = 0; i < Frames; i++)
        {
            Assert.Equal(interleavedOut[i * 2], outL[i]);
            Assert.Equal(interleavedOut[i * 2 + 1], outR[i]);
        }
        // Non-vacuous: the rig really did something, so "equal" is not "both silent".
        Assert.True(Rms(outL) > Rms(left) * 1.5f, $"gain node did nothing: {Rms(left)} -> {Rms(outL)}");
    }

    /// <summary>The zero-copy case: the output planes ARE the input planes.</summary>
    [NativeAvailableFact]
    public void PlanarProcessOffline_RendersInPlaceWhenTheSpansAlias()
    {
        var chain = Engine.GetDspChain();
        chain.Clear();
        chain.AddNode("gain").SetParam("gainDb", 12f);

        var (left, right) = Planes();
        var copyL = (float[])left.Clone();
        var copyR = (float[])right.Clone();

        var outL = new float[Frames];
        var outR = new float[Frames];
        chain.ProcessOffline(copyL, copyR, SampleRate, copyL, copyR);
        chain.ProcessOffline(left, right, SampleRate, outL, outR);

        Assert.Equal(outL, copyL);
        Assert.Equal(outR, copyR);
    }

    [NativeAvailableFact]
    public void PlanarProcessOffline_ValidatesEveryPlaneLength()
    {
        var chain = Engine.GetDspChain();
        var n = new float[64];

        Assert.Throws<ArgumentException>(() => chain.ProcessOffline(n, new float[32], SampleRate, n, n));
        Assert.Throws<ArgumentException>(() => chain.ProcessOffline(n, n, SampleRate, new float[32], n));
        Assert.Throws<ArgumentException>(() => chain.ProcessOffline(n, n, SampleRate, n, new float[32]));
        // Empty is a no-op, not an error.
        chain.ProcessOffline(Array.Empty<float>(), Array.Empty<float>(), SampleRate,
                             Array.Empty<float>(), Array.Empty<float>());
    }

    // ----------------------------------------------------------------- TryReadMeter

    /// <summary>
    /// THE POINT: four zeros with metering OFF is not a reading, and a caller must
    /// be able to tell that from a node that really was silent. That distinction is
    /// the only reason DotCharter still binds the raw-handle
    /// <see cref="JuceDspMeters" /> façade beside the chain.
    /// </summary>
    [NativeAvailableFact]
    public void TryReadMeter_SaysNoReadingWhileMeteringIsOff_AndAReadingWhenItIsOn()
    {
        var chain = Engine.GetDspChain();
        chain.Clear();
        var node = chain.AddNode("gain");

        chain.MeteringEnabled = false;
        Assert.False(chain.TryReadMeter(node.Id, out var off));
        Assert.Equal(default, off);

        chain.MeteringEnabled = true;
        try
        {
            var (left, right) = Planes();
            chain.ProcessOffline(left, right, SampleRate, new float[Frames], new float[Frames]);

            Assert.True(chain.TryReadMeter(node.Id, out var on));
            Assert.True(float.IsFinite(on.InputPeak) && on.InputPeak >= 0f);
            Assert.True(float.IsFinite(on.OutputPeak) && on.OutputPeak >= 0f);

            // An id the engine does not have is "no reading" too, never a silent
            // node that does not exist.
            Assert.False(chain.TryReadMeter(987654, out _));
        }
        finally { chain.MeteringEnabled = false; }
    }

    // ---------------------------------------------------------------- helpers

    private static (float[] Left, float[] Right) Planes()
    {
        var l = new float[Frames];
        var r = new float[Frames];
        for (var i = 0; i < Frames; i++)
        {
            l[i] = 0.25f * MathF.Sin(2f * MathF.PI * 220f * i / SampleRate);
            r[i] = 0.20f * MathF.Sin(2f * MathF.PI * 330f * i / SampleRate);
        }
        return (l, r);
    }

    private static float Rms(float[] buf)
    {
        double sum = 0;
        foreach (var s in buf) sum += (double)s * s;
        return (float)Math.Sqrt(sum / Math.Max(1, buf.Length));
    }
}
