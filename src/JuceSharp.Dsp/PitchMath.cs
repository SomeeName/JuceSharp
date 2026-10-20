namespace JuceSharp.Dsp;

/// <summary>
/// The equal-tempered pitch conversions, in one place.
/// <para>The same three formulas were re-typed at seven call sites and the note
/// name table at three, which is how a "440" becomes a "441" in exactly one of
/// them. <b>Both a float and a double form are kept on purpose</b>: several
/// callers are on a per-frame path where the float rounding IS the shipped
/// behaviour, so migrating them to double would move detection results. Pick
/// the one matching the site you are replacing — never "upgrade" it.</para>
/// </summary>
internal static class PitchMath
{
    /// <summary>Sharp note names, index 0 = C.</summary>
    public static readonly string[] NoteNames =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    /// <summary>MIDI note number → frequency (double).</summary>
    public static double MidiToHz(double midi) => 440.0 * Math.Pow(2.0, (midi - 69.0) / 12.0);

    /// <summary>MIDI note number → frequency (float arithmetic).</summary>
    public static float MidiToHzF(float midi) => 440f * MathF.Pow(2f, (midi - 69f) / 12f);

    /// <summary>Frequency → fractional MIDI note number (double).</summary>
    public static double HzToMidi(double hz) => 69.0 + 12.0 * Math.Log2(hz / 440.0);

    /// <summary>Frequency → fractional MIDI note number (float arithmetic).</summary>
    public static float HzToMidiF(float hz) => 69f + 12f * MathF.Log2(hz / 440f);

    /// <summary>Cents between two frequencies (double).</summary>
    public static double Cents(double hz, double referenceHz) => 1200.0 * Math.Log2(hz / referenceHz);

    /// <summary>Cents between two frequencies (float arithmetic).</summary>
    public static float CentsF(float hz, float referenceHz) => 1200f * MathF.Log2(hz / referenceHz);

    /// <summary>Pitch class 0..11 of a fractional MIDI number, rounded to the
    /// nearest semitone.</summary>
    public static int PitchClass(float midi)
    {
        var nearest = (int)MathF.Round(midi);
        return ((nearest % 12) + 12) % 12;
    }

    /// <summary>Sharp note name of a MIDI note number.</summary>
    public static string NoteName(int midi) => NoteNames[((midi % 12) + 12) % 12];

    /// <summary>Octave number of a MIDI note in scientific pitch notation
    /// (MIDI 60 = C4, MIDI 0 = C-1).
    /// <para><b>Integer division in C# truncates toward ZERO, not toward minus
    /// infinity</b>, so the obvious <c>midi / 12 - 1</c> is off by one for every
    /// NEGATIVE midi number: −1 (B−2) came out as octave −1 rather than −2, and
    /// the whole band −11..−1 collapsed onto the same octave as 0..11. Nothing
    /// reaches it today (MIDI −1 is 8.18 Hz, an octave below the lowest
    /// frequency any detector here resolves), but this type exists precisely to
    /// be <i>the</i> correct copy of these formulas — a caller that hits it
    /// would get a silently wrong answer from the one place that is supposed to
    /// be trustworthy. Floors instead.</para></summary>
    public static int Octave(int midi) => (midi >= 0 ? midi / 12 : (midi - 11) / 12) - 1;
}
