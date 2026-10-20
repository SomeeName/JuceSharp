using System.Runtime.InteropServices;
using JuceSharp.Interop;

namespace JuceSharp.Dsp;

/// <summary>
///     One parameter change at one sample position of an offline render
///     (ABI v34). A by-value POD whose size and field offsets are
///     <c>static_assert</c>ed native side and pinned managed side by
///     <c>JuceDspAutomationLayoutTests</c> — do not reorder it.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct JuceDspAutomationEvent
{
    /// <summary>Absolute sample index into the render, <c>0 &lt;= Frame &lt; frames</c>.</summary>
    public long Frame;

    /// <summary>The DSP node id. Seat-scoped, the usual per-player stride rule.</summary>
    public int NodeId;

    /// <summary>
    ///     The parameter id, UTF-8, NUL-padded to 32 bytes.
    ///     <para>
    ///         <b><c>"bypass"</c> is RESERVED</b> and routes to the chain's
    ///         bypass (a non-zero <see cref="Value" /> means bypassed). No
    ///         module declares a parameter by that id, because the chain owns
    ///         bypass and not the module — so automation gets the footswitch
    ///         for free.
    ///     </para>
    /// </summary>
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
    public byte[] Param;

    /// <summary>The new value. A non-finite value is REJECTED by index.</summary>
    public float Value;

    /// <summary>Build an event from a managed parameter id.</summary>
    /// <param name="frame">Absolute sample index into the render,
    /// <c>0 &lt;= frame &lt; frames</c>. Out of range is rejected BY INDEX by the
    /// render, not here — this factory does not know the buffer.</param>
    /// <param name="nodeId">Engine-global DSP node id. It must name the SAME seat
    /// the render is for: node ids carry the seat in
    /// <c>player * PlayerNodeIdStride + local</c>, so an id from another seat
    /// would otherwise be applied to that seat's chain.</param>
    /// <param name="paramId">The parameter id, or the reserved <c>"bypass"</c>.
    /// 1..32 UTF-8 bytes.</param>
    /// <param name="value">The new value; non-finite is rejected by the render.</param>
    /// <returns>An event whose <see cref="Param" /> field is the id, UTF-8,
    /// NUL-padded to 32 bytes.</returns>
    /// <exception cref="ArgumentException">
    ///     The id is empty, or does not fit in 32 UTF-8 bytes. The field is NOT
    ///     required to be NUL-terminated — a caller may legally fill all 32 bytes
    ///     and the engine bounds the read — and this factory used to refuse 32,
    ///     which is a boundary the header goes out of its way to specify. It is
    ///     rejected rather than truncated: a silently cut id resolves to nothing,
    ///     and "nothing happened" is the hardest automation bug there is to see.
    /// </exception>
    public static JuceDspAutomationEvent Create(long frame, int nodeId, string paramId, float value)
    {
        if (string.IsNullOrEmpty(paramId))
            throw new ArgumentException("A parameter id is required.", nameof(paramId));

        var bytes = System.Text.Encoding.UTF8.GetBytes(paramId);
        if (bytes.Length > 32)
            throw new ArgumentException(
                $"'{paramId}' is {bytes.Length} UTF-8 bytes; the ABI field holds 32 (no terminator required).",
                nameof(paramId));

        var field = new byte[32];
        bytes.CopyTo(field, 0);
        return new JuceDspAutomationEvent
        {
            Frame  = frame,
            NodeId = nodeId,
            Param  = field,
            Value  = value,
        };
    }
}

public sealed partial class JuceDspChain
{
    /// <summary>
    ///     The most automation events one
    ///     <see cref="ProcessOffline(Span{float},int,int,ReadOnlySpan{JuceDspAutomationEvent})" />
    ///     call accepts (262,144), mirroring the cap the ABI documents on
    ///     <c>dca_engine_dsp_process_buffer_automated_for_player</c>. Past it the
    ///     call throws <see cref="ArgumentOutOfRangeException" /> before anything
    ///     is marshalled — the engine enforces the same bound, but only after the
    ///     whole list has been copied into unmanaged memory. Pinned against the
    ///     header by <c>NativeMethodsTests</c>.
    /// </summary>
    public static int MaxAutomationEvents => NativeMethods.MaxAutomationEvents;

    /// <summary>
    ///     Render a buffer through this seat's rig with PARAMETER AUTOMATION
    ///     (ABI v34).
    ///     <para>
    ///         <see cref="ProcessOffline(ReadOnlySpan{float},int,int,Span{float})" />
    ///         renders a whole take at ONE static setting, which is the one
    ///         thing a re-amp is never asked to do: a volume swell, a wah pass
    ///         and a footswitch mid-solo are all ordinary. The render is chunked
    ///         at each event boundary, so an event at frame <c>F</c> is APPLIED at
    ///         sample <c>F</c> — not necessarily HEARD there: a <c>"bypass"</c>
    ///         event rides the chain's 10 ms crossfade and every user-facing gain
    ///         inside a core is ramped, so the change BEGINS at <c>F</c>.
    ///     </para>
    ///     <para>
    ///         Every event goes through the same <c>DspChain::setParam</c>
    ///         mailbox the LIVE audio callback's setters use. That is what makes
    ///         an automated render and a live performance of the same gesture
    ///         unable to diverge, and what makes a NaN rejected here exactly as
    ///         it would be rejected live.
    ///     </para>
    /// </summary>
    /// <param name="buffer">
    ///     Interleaved samples, <paramref name="channels" /> per frame,
    ///     processed IN PLACE. Left untouched if an event is rejected.
    /// </param>
    /// <param name="channels">1 (mono, duplicated to both rig channels) or 2.</param>
    /// <param name="sampleRate">8000..192000. The rig is prepared lazily at this rate.</param>
    /// <param name="events">
    ///     Sorted by non-decreasing <see cref="JuceDspAutomationEvent.Frame" />.
    ///     An UNSORTED array is rejected rather than silently reordered: a
    ///     caller that thinks it is automating a sweep and is actually
    ///     automating a scramble gets a render that is plausible and wrong.
    /// </param>
    /// <exception cref="JuceAudioException">
    ///     <para>
    ///         <b>An audio device is OPEN.</b> Offline means offline, and this is
    ///         the single most likely failure a consumer meets: calling a re-amp
    ///         while the engine is started fails with
    ///         <see cref="JuceAudioStatus.Device" /> and no index.
    ///     </para>
    ///     <para>
    ///         Or an event was rejected (bad frame, non-finite value, empty id, an
    ///         id naming another seat, unsorted, or a node/parameter that does not
    ///         resolve). The message names its INDEX.
    ///     </para>
    /// </exception>
    /// <exception cref="ArgumentException"><paramref name="buffer" /> is empty, or
    /// its length is not a whole number of frames, or an event's <c>Param</c> field
    /// is not exactly 32 bytes.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="channels" /> is
    /// neither 1 nor 2.</exception>
    /// <remarks>
    ///     <b>State on rejection: NOTHING is touched — not the buffer, and not the
    ///     rig.</b> The whole event list is validated AND RESOLVED (seat, node,
    ///     parameter id) before a single event is applied or a single sample is
    ///     rendered. The buffer half was always true; the rig half was not, and a
    ///     render that failed part-way used to leave this seat's chain at an
    ///     arbitrary mid-sweep state — a wah parked at 0.37, a pedal bypassed —
    ///     with no way for the caller to know or undo it.
    ///     <para>
    ///         <b>Parameter VALUES a successful render sweeps are not restored
    ///         either.</b> Automation is a performance, not a transaction: the rig
    ///         is left where the last event put it.
    ///     </para>
    /// </remarks>
    public void ProcessOffline(Span<float> buffer, int channels, int sampleRate,
                               ReadOnlySpan<JuceDspAutomationEvent> events)
    {
        if (channels is not (1 or 2))
            throw new ArgumentOutOfRangeException(nameof(channels), channels, "The rig renders mono or stereo.");
        // An empty buffer plus a populated event list used to be a silent no-op,
        // ahead of every other validation in this method — the one shape where a
        // caller that got the arguments wrong was told nothing at all.
        if (buffer.IsEmpty)
            throw new ArgumentException("There is nothing to render.", nameof(buffer));
        if (buffer.Length % channels != 0)
            throw new ArgumentException("buffer length is not a whole number of frames.", nameof(buffer));

        var frames = buffer.Length / channels;

        // The event struct carries a fixed 32-byte array field, which is not
        // blittable, so it cannot simply be pinned — marshal the block once.
        var count = events.Length;

        // The cap is checked NATIVE-side, i.e. after the whole list has been
        // copied into an unmanaged block this method used to size in 32-bit
        // arithmetic. Two things were wrong with that. A caller 10x past the cap
        // paid a full O(n) StructureToPtr marshal and a multi-hundred-megabyte
        // AllocHGlobal before being told "no" with no index; and `element *
        // count` WRAPPED — at count == 89,478,486 the product is 32, giving a
        // 32-byte allocation the loop then writes 4.2 GB into. Reaching that
        // needs a 4.2 GB span, so it is not a likely accident, but it is an
        // unbounded write in a public API whose guard exists one layer down.
        if (count > MaxAutomationEvents)
            throw new ArgumentOutOfRangeException(
                nameof(events), count,
                $"The engine accepts at most {MaxAutomationEvents} automation events per render.");

        var element = Marshal.SizeOf<JuceDspAutomationEvent>();
        // checked, and long, regardless of the cap above: the arithmetic must
        // not be the thing keeping this safe.
        var block = count > 0 ? Marshal.AllocHGlobal(checked((nint)((long)element * count))) : nint.Zero;
        try
        {
            for (var i = 0; i < count; i++)
            {
                var ev = events[i];
                // A default-constructed event has a null Param array; give it
                // the empty id the ABI already rejects rather than throwing a
                // NullReferenceException out of the marshaller.
                ev.Param ??= new byte[32];
                if (ev.Param.Length != 32)
                    throw new ArgumentException($"events[{i}].Param must be exactly 32 bytes.", nameof(events));
                Marshal.StructureToPtr(ev, block + checked((nint)((long)i * element)), false);
            }

            var h = _engine.Acquire();
            try
            {
                int status;
                int badIndex;
                unsafe
                {
                    fixed (float* p = buffer)
                    {
                        status = NativeMethods.EngineDspProcessBufferAutomatedForPlayer(
                            h, Player, p, channels, frames, sampleRate, block, count, out badIndex);
                    }
                }

                if (status != (int)NativeMethods.DcaStatus.Ok)
                {
                    if (badIndex >= 0)
                        throw new JuceAudioException(
                            $"automation event {badIndex} was rejected",
                            (JuceAudioStatus)status,
                            "dca_engine_dsp_process_buffer_automated_for_player");
                    JuceAudioEngine.Check((NativeMethods.DcaStatus)status,
                        "dca_engine_dsp_process_buffer_automated_for_player");
                }
            }
            finally { _engine.Release(); }
        }
        finally
        {
            if (block != nint.Zero) Marshal.FreeHGlobal(block);
        }
    }
}
