using System;
using JuceSharp;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     Wave-2 cleanup: <c>TryReadMeters</c> is named <c>Try*</c> and returns an
///     <c>int</c>, so a failed <c>TryAcquire</c> comes back as <c>0</c> —
///     indistinguishable from "this seat has no nodes". Its sibling
///     <see cref="JuceDspChain.TryReadMeter" /> is a proper <c>bool</c> <c>Try</c>
///     whose documented purpose is exactly that distinction, and a UI that cannot
///     make it either holds a stale board or blanks a live one.
/// </summary>
[Collection("NativeEngine")]
public sealed class Wave2ChainMeterTests
{
    [NativeAvailableFact]
    public void TryReadMeters_TellsADisposedEngineApartFromASeatWithNoNodes()
    {
        var engine = new JuceAudioEngine();
        JuceDspChain chain;
        try
        {
            // dca_engine_create opens a device; nothing here needs it running.
            engine.Stop();
            chain = engine.GetDspChain();

            Span<JuceDspNodeMeter> dest = new JuceDspNodeMeter[8];

            Assert.True(chain.TryReadMeters(dest, out var empty),
                "a live engine with an empty chain is an ANSWER, not a failure");
            Assert.Equal(0, empty);

            chain.AddNode("gain");
            Assert.True(chain.TryReadMeters(dest, out var one));
            Assert.Equal(1, one);

            chain.Clear();
        }
        finally { engine.Dispose(); }

        Span<JuceDspNodeMeter> after = new JuceDspNodeMeter[8];
        Assert.False(chain.TryReadMeters(after, out var gone),
            "a poll racing an engine dispose must report FAILURE, not an empty board");
        Assert.Equal(0, gone);

        // …and this is the finding: the int-returning overload answers 0 to both
        // of the cases above, so no caller of it can tell them apart. It is
        // [Obsolete] for that reason and this is the one site that still calls
        // it deliberately — the assertion IS that it cannot distinguish them.
#pragma warning disable CS0618 // deliberately exercising the obsolete overload
        Assert.Equal(0, chain.TryReadMeters(after));
#pragma warning restore CS0618
    }
}
