namespace JuceSharp.Dsp;

/// <summary>
///     ABI v34. What a keyed node's DETECTOR listens to — see
///     <see cref="JuceDspNode.Sidechain" />. Mirrors the
///     <c>DCA_DSP_SIDECHAIN_*</c> constants in <c>dotcharter_audio.h</c>; the
///     numeric values are part of the ABI and are persisted in presets, so they
///     may not be renumbered.
/// </summary>
public enum JuceDspSidechainSource
{
    /// <summary>Key off the module's own input. The default, and byte-for-byte
    /// the pre-v34 behaviour.</summary>
    None = 0,

    /// <summary>Key off the seat's DRY input — the chain's input block, before
    /// any pedal — so a module late in the chain follows the pick rather than
    /// what the drive ahead of it produced.</summary>
    SeatInput = 1,

    /// <summary>
    ///     Key off the song / master mix as it stands when this seat is
    ///     processed: the transport output after the master bus, plus the
    ///     monitor of any seat processed earlier in the same callback. This is
    ///     how the rig ducks under a backing track. Unavailable on the offline
    ///     render path, where the module falls back to its own input.
    /// </summary>
    SongMix = 2,
}
