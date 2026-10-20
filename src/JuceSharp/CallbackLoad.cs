namespace JuceSharp;

/// <summary>
///     The audio callback's load (ABI v46), as FRACTIONS of the buffer period:
///     0.25 means the callback spent a quarter of the block's wall-clock budget
///     rendering it, and above 1.0 it overran its deadline. Measured inside the
///     callback, so it is the cost of the whole render (song, stems, every
///     seat's rig and VST3 chain, the master bus), not a process CPU figure.
/// </summary>
/// <param name="Average">Exponentially weighted average, 0.5 s time constant.</param>
/// <param name="Peak">The largest single block, decaying over 1.5 s; reading
/// does not reset it.</param>
public readonly record struct CallbackLoad(float Average, float Peak);
