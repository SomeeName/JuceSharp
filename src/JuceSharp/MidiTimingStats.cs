namespace JuceSharp;

/// <summary>
///     A snapshot of the engine's SCHEDULED-MIDI counters (ABI v34) — what the
///     audio callback did with the events a caller parked for a future block.
///     <para>
///         Shaped like <see cref="XrunStats" /> on purpose. Two poll-only native
///         counter reads with identical semantics had two different conventions
///         (a value tuple that lost the status, against
///         <see cref="JuceAudioEngine.TryGetXrunStats" />'s <c>Try</c> + record),
///         which made a consumer learn both for one concept.
///     </para>
/// </summary>
public readonly record struct MidiTimingStats
{
    /// <summary>Events PARKED for a future block since the last reset — the ones
    /// that took the sample-accurate path at all.</summary>
    public long Scheduled { get; init; }

    /// <summary>Events whose frame had already gone past when their block ran.
    /// They are placed at sample 0 (the pre-v34 behaviour) and counted here, and
    /// this is the ONLY signal that a caller's scheduling horizon is shorter than
    /// the engine's own latency.</summary>
    public long Late { get; init; }

    /// <summary>Events the queue or the audio thread's parked set could not hold,
    /// plus anything scheduled more than 5 s out. A swallowed note-off is a stuck
    /// note, so it is counted rather than ignored.</summary>
    public long Dropped { get; init; }
}
