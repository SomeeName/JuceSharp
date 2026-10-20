namespace JuceSharp.Tests;

/// <summary>
/// Synthetic guitar-ish tone generator — a C# port of the NoteDetection
/// testbed's <c>synth.py</c>. Renders sequences of known notes (sharp pluck
/// attack + a few decaying harmonics) with ground truth we control, so the
/// CQT / verifier / lead-tracker port can be checked end-to-end with no
/// datasets. Kept deliberately identical to the Python model so the ported
/// golden-vector tests carry over their thresholds.
/// </summary>
internal static class Synth
{
    public const int SampleRate = 44100;

    /// <summary>One ground-truth note: absolute onset (s) and MIDI pitch.</summary>
    public readonly record struct NoteEvent(double Onset, double Midi, double Offset);

    public static float MidiToHz(double midi) => 440f * MathF.Pow(2f, (float)(midi - 69.0) / 12f);

    /// <summary>One plucked note: fundamental + 4 harmonics, exponential decay, ~3 ms attack.</summary>
    public static float[] Pluck(int midi, double dur, int sr = SampleRate)
    {
        int n = (int)(dur * sr);
        var sig = new float[n];
        float f0 = MidiToHz(midi);
        ReadOnlySpan<float> weights = stackalloc float[] { 1.0f, 0.5f, 0.3f, 0.2f, 0.12f };
        for (int i = 0; i < n; i++)
        {
            float t = (float)i / sr;
            float s = 0;
            for (int k = 0; k < weights.Length; k++)
                s += weights[k] * MathF.Sin(2 * MathF.PI * f0 * (k + 1) * t);
            float decay = MathF.Exp(-t * 6.0f);
            float attack = MathF.Min(1.0f, t / 0.003f);
            sig[i] = s * decay * attack;
        }
        return Normalize(sig, 0.3f);
    }

    /// <summary>Render a monophonic sequence and return (audio, ground-truth notes).</summary>
    public static (float[] Audio, List<NoteEvent> Truth) RenderNotes(
        IReadOnlyList<int> midis, int sr = SampleRate,
        double noteDur = 0.6, double gap = 0.15, double leadIn = 0.1)
    {
        var parts = new List<float[]> { new float[(int)(leadIn * sr)] };
        var truth = new List<NoteEvent>();
        double cursor = leadIn;
        foreach (var m in midis)
        {
            parts.Add(Pluck(m, noteDur, sr));
            truth.Add(new NoteEvent(cursor, m, cursor + noteDur));
            parts.Add(new float[(int)(gap * sr)]);
            cursor += noteDur + gap;
        }
        return (Concat(parts), truth);
    }

    /// <summary>Additive tone that follows a time-varying MIDI-pitch curve (phase-accumulated).</summary>
    private static float[] ToneFromCurve(ReadOnlySpan<float> midiCurve, int sr)
    {
        int n = midiCurve.Length;
        var sig = new float[n];
        ReadOnlySpan<float> weights = stackalloc float[] { 1.0f, 0.5f, 0.3f, 0.2f, 0.12f };
        double phase = 0;
        for (int i = 0; i < n; i++)
        {
            float f0 = 440f * MathF.Pow(2f, (midiCurve[i] - 69f) / 12f);
            phase += 2.0 * Math.PI * f0 / sr;
            float s = 0;
            for (int k = 0; k < weights.Length; k++)
                s += weights[k] * MathF.Sin((k + 1) * (float)phase);
            float t = (float)i / sr;
            float env = MathF.Exp(-t * 3.0f) * MathF.Min(1.0f, t / 0.003f);
            sig[i] = s * env;
        }
        return Normalize(sig, 0.3f);
    }

    /// <summary>A note bent up <paramref name="semitones"/> over the first <paramref name="rise"/>
    /// fraction, then held. Returns (audio, target MIDI reached).</summary>
    public static (float[] Audio, double Target) Bend(
        double startMidi, double semitones, double dur = 0.9, int sr = SampleRate, double rise = 0.4)
    {
        int n = (int)(dur * sr);
        var curve = new float[n];
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / sr;
            double ramp = Math.Clamp(t / (rise * dur), 0.0, 1.0);
            curve[i] = (float)(startMidi + semitones * ramp);
        }
        return (ToneFromCurve(curve, sr), startMidi + semitones);
    }

    /// <summary>A held note with sinusoidal pitch vibrato (rate Hz, ± depth semitones).</summary>
    public static float[] Vibrato(double midi, double dur = 0.9, int sr = SampleRate,
        double rate = 5.5, double depth = 0.35)
    {
        int n = (int)(dur * sr);
        var curve = new float[n];
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / sr;
            curve[i] = (float)(midi + depth * Math.Sin(2 * Math.PI * rate * t));
        }
        return ToneFromCurve(curve, sr);
    }

    /// <summary>Render a sequence of simultaneous-note chords (for polyphony tests).</summary>
    public static (float[] Audio, List<NoteEvent> Truth) RenderChords(
        IReadOnlyList<IReadOnlyList<int>> chords, int sr = SampleRate,
        double chordDur = 0.8, double gap = 0.2, double leadIn = 0.1)
    {
        var parts = new List<float[]> { new float[(int)(leadIn * sr)] };
        var truth = new List<NoteEvent>();
        double cursor = leadIn;
        foreach (var chord in chords)
        {
            int len = (int)(chordDur * sr);
            var mix = new float[len];
            foreach (var m in chord)
            {
                var note = Pluck(m, chordDur, sr);
                int c = Math.Min(len, note.Length);
                for (int i = 0; i < c; i++) mix[i] += note[i] / Math.Max(1, chord.Count);
                truth.Add(new NoteEvent(cursor, m, cursor + chordDur));
            }
            parts.Add(mix);
            parts.Add(new float[(int)(gap * sr)]);
            cursor += chordDur + gap;
        }
        return (Concat(parts), truth);
    }

    private static float[] Normalize(float[] sig, float peak)
    {
        float max = 1e-9f;
        foreach (var v in sig) max = MathF.Max(max, MathF.Abs(v));
        for (int i = 0; i < sig.Length; i++) sig[i] = peak * sig[i] / max;
        return sig;
    }

    private static float[] Concat(List<float[]> parts)
    {
        int total = 0;
        foreach (var p in parts) total += p.Length;
        var outp = new float[total];
        int o = 0;
        foreach (var p in parts) { Array.Copy(p, 0, outp, o, p.Length); o += p.Length; }
        return outp;
    }
}
