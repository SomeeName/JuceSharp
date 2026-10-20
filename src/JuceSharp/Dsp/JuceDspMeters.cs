using JuceSharp.Interop;

namespace JuceSharp.Dsp;

/// <summary>
///     Linear sample amplitudes for one native DSP node — that node's own input
///     and output, not the song or master bus.
///     <para>
///         LINEAR, never dB: the ABI reports what the samples did and the caller
///         picks the scale. Values above 1 are legitimate for a node driving into
///         a clipper. Peaks decay ~34 dB/s so a 30 Hz poll sees every transient;
///         the RMS integrates over ~300 ms. All four read 0 while
///         <see cref="JuceDspChain.MeteringEnabled" /> is false.
///     </para>
/// </summary>
public readonly record struct JuceDspNodeMeter(float InputPeak, float InputRms, float OutputPeak, float OutputRms);

/// <summary>
///     RAW-HANDLE façade over the ABI v30 per-node meters, kept for callers that
///     already bind the rig against <see cref="JuceAudioEngine.NativeHandle" />.
///     <para>
///         <b>Prefer <see cref="JuceAudioEngine.GetDspChain" />:</b>
///         <see cref="JuceDspChain.MeteringEnabled" />,
///         <see cref="JuceDspChain.TryReadMeters(Span{JuceDspNodeMeter}, out int)" /> and
///         <see cref="JuceDspNode.Meter" /> do the same thing with the engine
///         handle PINNED for the duration of each call, so a poll racing an engine
///         dispose is a caught <see cref="ObjectDisposedException" /> instead of a
///         read through freed native memory. Nothing on this type can offer that:
///         it is handed a bare <c>nint</c>, and the caller has to guarantee the
///         engine outlives every call itself.
///     </para>
///     <para>
///         Not <c>[Obsolete]</c> only because DotCharter still calls it and builds
///         with warnings as errors; it is superseded, and the P/Invokes it used to
///         declare privately now live in <c>NativeMethods</c> with every other one
///         (they were invisible to the marshalling and callback lints while they
///         sat here).
///     </para>
/// </summary>
public static class JuceDspMeters
{
    /// <summary>Switch metering on or off for one seat's chain. Off also ZEROES
    /// the meters, so a UI that stops polling leaves no frozen picture.</summary>
    public static bool SetEnabled(nint engine, int player, bool enabled)
        => engine != 0
           && player is >= 0 and < NativeMethods.MaxPlayers
           && NativeMethods.EngineDspSetMeteringForPlayer(engine, player, enabled ? 1 : 0)
              == NativeMethods.DcaStatus.Ok;

    /// <summary>Whether metering is on for that seat — which is how a caller
    /// tells "everything is silent" from "nobody switched the meters on".</summary>
    public static bool IsEnabled(nint engine, int player)
        => engine != 0
           && player is >= 0 and < NativeMethods.MaxPlayers
           && NativeMethods.EngineDspGetMetering(engine, player) != 0;

    /// <summary>Levels for ONE node, by engine-global node id.</summary>
    public static bool TryRead(nint engine, int nodeId, out JuceDspNodeMeter meter)
    {
        meter = default;
        if (engine == 0) return false;
        if (NativeMethods.EngineDspGetNodeMeter(engine, nodeId, out var ip, out var ir, out var op, out var orms)
            != NativeMethods.DcaStatus.Ok) return false;
        meter = new JuceDspNodeMeter(ip, ir, op, orms);
        return true;
    }

    /// <summary>Four floats per node in chain order. Returns the total node count;
    /// an empty array sizes the result. The native call writes only complete
    /// entries.</summary>
    /// <remarks>
    ///     <c>0</c> is returned both for "this seat has no nodes" and for a
    ///     fault. The native export answers <c>-1</c> for a dead engine or a
    ///     seat out of range — the ABI's sentinel for every value-returning
    ///     export since the v35 wave — and the clamp below folds it to <c>0</c>
    ///     so a caller sizing an array never sees a negative length. This façade
    ///     takes a RAW handle it cannot validate, so it has no better answer to
    ///     give; use <see cref="JuceDspChain" /> where the engine's lifetime is
    ///     tracked.
    /// </remarks>
    public static int Read(nint engine, int player, float[] values)
    {
        ArgumentNullException.ThrowIfNull(values);
        if (engine == 0 || player < 0 || player >= NativeMethods.MaxPlayers) return 0;
        unsafe
        {
            fixed (float* p = values)
            {
                return Math.Max(0, NativeMethods.EngineDspReadNodeMeters(engine, player, p, values.Length));
            }
        }
    }
}
