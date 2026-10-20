using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
///     A snapshot of the engine's dropout counters (ABI v30) — what the audio
///     CALLBACK observed, not what a managed poll could infer.
///
///     <para>
///         The three callback counters answer different questions and must never
///         be added together. <see cref="LateCallbacks" /> is the audible event:
///         the device had to fill part of a buffer without us, whoever's fault
///         that was. <see cref="DeadlineOverruns" /> says the fault was OURS — the
///         callback took longer than the block it was rendering.
///         <see cref="NearMisses" /> is a load warning inside budget, and is
///         strictly disjoint from the overruns.
///     </para>
///     <para>
///         That separation is the whole point of reading these instead of timing
///         the render clock from managed code: "your machine stalled the audio
///         thread" and "your tone rig is too heavy for this buffer size" are
///         different problems with different fixes, and they look identical from
///         outside the callback.
///     </para>
/// </summary>
public readonly record struct XrunStats
{
    /// <summary>An audio device is open.</summary>
    public bool DeviceOpen { get; init; }

    /// <summary>That device is started and callbacks are flowing.</summary>
    public bool DeviceRunning { get; init; }

    /// <summary>
    ///     What the DRIVER itself reports since the last reset, or <c>null</c>
    ///     when it cannot report at all.
    ///
    ///     <para>
    ///         ASIO and DirectSound implement this; WASAPI and CoreAudio's
    ///         aggregate path do not. It is authoritative when present and absent
    ///         most of the time, which is exactly why the callback-side counters
    ///         exist. <b>Never render an absent count as zero</b> — "the driver
    ///         reported no dropouts" and "the driver cannot tell us" are opposite
    ///         claims.
    ///     </para>
    /// </summary>
    public int? DeviceXruns { get; init; }

    /// <summary>Audio callbacks since the last reset.</summary>
    public long Callbacks { get; init; }

    /// <summary>Real, audible dropouts: the gap before the callback exceeded 1.5x
    /// the previous block plus 0.5 ms, so the device rendered without us. This is
    /// the number to show a user.</summary>
    public long LateCallbacks { get; init; }

    /// <summary>Callbacks that overran their OWN block budget — we were the cause.
    /// Usually produces a late callback on the next block, so the two correlate.
    /// </summary>
    public long DeadlineOverruns { get; init; }

    /// <summary>Callbacks over 80% of the block budget but still inside it. A load
    /// warning, not a dropout, and never also counted as an overrun.</summary>
    public long NearMisses { get; init; }

    /// <summary>Estimated frames the device rendered without us.</summary>
    public long DroppedFrames { get; init; }

    /// <summary>Host ticks at the last late callback or overrun; 0 when there has
    /// been none since the reset. Same timebase as <c>Stopwatch</c> (QPC on
    /// Windows) and the render clock.</summary>
    public long LastXrunHostTicks { get; init; }

    /// <summary>Render-clock frame at that moment; -1 when there has been none.</summary>
    public long LastXrunRenderFrame { get; init; }

    /// <summary>Longest callback since the reset, in nanoseconds.</summary>
    public long MaxCallbackNs { get; init; }

    /// <summary>Most recent callback duration, in nanoseconds.</summary>
    public long LastCallbackNs { get; init; }

    /// <summary>The most recent block's budget, in nanoseconds.</summary>
    public long BlockBudgetNs { get; init; }

    /// <summary>
    ///     How many input-chain VST3 plugins threw out of <c>processBlock</c> and
    ///     were DISABLED for the rest of their chain snapshot, since the reset.
    ///     <para>
    ///         Counted ONCE per plugin, not once per block. A plugin that faults
    ///         deterministically — an expired trial's licence check, a layout it
    ///         was not prepared for — used to throw on every callback for ever;
    ///         the engine now latches it, skips it, and keeps the rest of the
    ///         chain. Non-zero means a pedal in the signal path is SILENTLY
    ///         ABSENT, which is worth telling the user about: nothing else says
    ///         so.
    ///     </para>
    ///     <para>
    ///         Re-armed by the next attach, detach or device rebuild, each of
    ///         which gives the plugin a fresh chance.
    ///     </para>
    /// </summary>
    public long InputPluginFaults { get; init; }

    /// <summary>
    ///     The most recent callback's share of its block budget, or <c>null</c>
    ///     when no block has been rendered yet.
    ///
    ///     <para>
    ///         Guarded rather than computed inline because the budget is 0 before
    ///         the first block, and a load of "infinity" reads as a catastrophic
    ///         overload on a device that has simply not started.
    ///     </para>
    /// </summary>
    public double? Load => BlockBudgetNs > 0 ? (double)LastCallbackNs / BlockBudgetNs : null;

    /// <summary>Whether anything AUDIBLE has happened since the reset. Deliberately
    /// not a function of the overruns: an overrun inside budget elsewhere in the
    /// window is a load warning, and the driver's own count is absent far more
    /// often than it is zero.</summary>
    public bool HasDropouts => LateCallbacks > 0 || DeviceXruns > 0;

    internal static XrunStats From(NativeMethods.DcaXrunStats n) => new()
    {
        DeviceOpen          = n.DeviceOpen != 0,
        DeviceRunning       = n.DeviceRunning != 0,
        // -1 is the native "cannot report" sentinel, and it must not survive as a
        // number anyone can sum, average or print.
        DeviceXruns         = n.DeviceXruns < 0 ? null : n.DeviceXruns,
        Callbacks           = n.Callbacks,
        LateCallbacks       = n.LateCallbacks,
        DeadlineOverruns    = n.DeadlineOverruns,
        NearMisses          = n.NearMisses,
        DroppedFrames       = n.DroppedFrames,
        LastXrunHostTicks   = n.LastXrunHostTicks,
        LastXrunRenderFrame = n.LastXrunRenderFrame,
        MaxCallbackNs       = n.MaxCallbackNs,
        LastCallbackNs      = n.LastCallbackNs,
        BlockBudgetNs       = n.BlockBudgetNs,
        InputPluginFaults   = n.InputPluginFaults
    };
}
