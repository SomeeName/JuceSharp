namespace JuceSharp.Dsp;

/// <summary>Detected chord plus a 0..1 confidence score.</summary>
public readonly record struct JuceChordReading(string Name, int RootSemitone, JuceChordQuality Quality, float Confidence);

/// <summary>Triad quality of a detected chord — the four templates
/// <see cref="JuceChordDetector"/> matches against, transposed to all twelve roots.</summary>
public enum JuceChordQuality
{
    /// <summary>Major triad: root, major third, perfect fifth.</summary>
    Major,
    /// <summary>Minor triad: root, minor third, perfect fifth.</summary>
    Minor,
    /// <summary>Diminished triad: root, minor third, diminished fifth.</summary>
    Diminished,
    /// <summary>Augmented triad: root, major third, augmented fifth.</summary>
    Augmented,
}

/// <summary>
/// Chord identification from a 12-bin chroma vector using template
/// matching. Supports major, minor, diminished and augmented triads —
/// 48 templates total. Returns the highest-scoring template if its
/// score is meaningful, else <c>null</c>.
/// </summary>
public sealed class JuceChordDetector
{
    // The note-name table lives in PitchMath. This file carried a FOURTH byte-identical
    // copy of it, missed by the consolidation that retired the other three (see
    // JuceNoteStream.EmitOff's comment, which records the same oversight in that file).
    private static readonly (int[] mask, JuceChordQuality q)[] BaseTemplates =
    {
        (new[] { 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0 }, JuceChordQuality.Major),
        (new[] { 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 }, JuceChordQuality.Minor),
        (new[] { 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0 }, JuceChordQuality.Diminished),
        (new[] { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 }, JuceChordQuality.Augmented),
    };

    private readonly (float[] tpl, int root, JuceChordQuality q)[] _templates;

    /// <summary>Cosine-similarity score the best-matching template must reach before
    /// <see cref="Detect"/> returns anything at all; below it the frame is reported as
    /// "no chord" (<c>null</c>). Defaults to 0.55.</summary>
    public float MinConfidence { get; init; } = 0.55f;

    /// <summary>Build the 48 chord templates — four triad qualities across twelve roots —
    /// once, so <see cref="Detect"/> is pure comparison.</summary>
    /// <remarks>The templates are the raw 0/1 masks. They used to be divided through by
    /// their own sum, which reads as though the scale matters; the only consumer is
    /// <c>CosineSim</c>, which normalises both operands, so dividing every template by 3
    /// could not move a score or a verdict. Dropped rather than left to mislead — pinned by
    /// <c>Wave4DspAuditTests.ChordDetector_NamesEveryRoot_AndIsUnchangedByTemplateScale</c>.</remarks>
    public JuceChordDetector()
    {
        var list = new List<(float[], int, JuceChordQuality)>();
        foreach (var (mask, q) in BaseTemplates)
        {
            for (int root = 0; root < 12; root++)
            {
                var t = new float[12];
                for (int i = 0; i < 12; i++) t[(root + i) % 12] = mask[i];
                list.Add((t, root, q));
            }
        }
        _templates = list.ToArray();
    }

    /// <summary>
    /// Match a 12-bin chroma vector against all 48 templates by cosine similarity and
    /// return the best one, or <c>null</c> when its score is below
    /// <see cref="MinConfidence"/>. Stateless and allocation-free.
    /// </summary>
    /// <param name="chroma">Exactly 12 pitch-class energies, in C..B order.</param>
    /// <returns>The best chord and its 0..1 score, or <c>null</c> for no confident match.</returns>
    /// <exception cref="ArgumentException"><paramref name="chroma"/> is not 12 bins.</exception>
    public JuceChordReading? Detect(ReadOnlySpan<float> chroma)
    {
        if (chroma.Length != 12) throw new ArgumentException("Chroma must be 12 bins.", nameof(chroma));

        float bestScore = -1;
        int   bestIdx   = -1;
        for (int i = 0; i < _templates.Length; i++)
        {
            float s = CosineSim(chroma, _templates[i].tpl);
            if (s > bestScore) { bestScore = s; bestIdx = i; }
        }
        if (bestIdx < 0 || bestScore < MinConfidence) return null;
        var t = _templates[bestIdx];
        var name = PitchMath.NoteNames[t.root] + Suffix(t.q);
        return new JuceChordReading(name, t.root, t.q, bestScore);
    }

    private static string Suffix(JuceChordQuality q) => q switch
    {
        JuceChordQuality.Major      => "",
        JuceChordQuality.Minor      => "m",
        JuceChordQuality.Diminished => "dim",
        JuceChordQuality.Augmented  => "aug",
        _ => "",
    };

    private static float CosineSim(ReadOnlySpan<float> a, ReadOnlySpan<float> b)
    {
        float dot = 0, na = 0, nb = 0;
        for (int i = 0; i < 12; i++)
        {
            dot += a[i] * b[i];
            na  += a[i] * a[i];
            nb  += b[i] * b[i];
        }
        var d = MathF.Sqrt(na) * MathF.Sqrt(nb);
        return d == 0 ? 0 : dot / d;
    }
}
