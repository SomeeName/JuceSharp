using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The provisional onset (DotCharter #643): <see cref="JuceGuitarAnalyzer"/> publishes each
/// detector onset as a <see cref="JuceOnsetNoticeKind.Detected"/> notice the moment it fires,
/// then finalizes it into an event carrying the same <see cref="JuceGuitarEvent.DetectorSamplePosition"/>
/// or withdraws it. A game times a hit another detector confirmed from the notice, ~170 ms
/// sooner than from the event.
/// </summary>
public sealed class JuceGuitarAnalyzerProvisionalOnsetTests
{
    private const int Sr = 48000;
    private const int Block = 256;

    /// <summary>Synthetic plucked string: 8 harmonics with 1/h amplitude, per-harmonic
    /// exponential decay, 3 ms linear attack (as <c>JuceGuitarAnalyzerTests.Pluck</c>), and a
    /// 100 ms fade-out — a pluck cut off mid-ring is a transient the detector rightly fires on.</summary>
    private static float[] Pluck(float f0, double seconds, float gain = 0.2f)
    {
        var n = (int)(seconds * Sr);
        var s = new float[n];
        for (var h = 1; h <= 8; h++)
        {
            var amp = 1.0 / h;
            var decay = 2.0 + 0.7 * h;
            var w = 2 * Math.PI * f0 * h / Sr;
            for (var i = 0; i < n; i++)
            {
                var t = (double)i / Sr;
                var attack = Math.Min(1.0, t / 0.003);
                s[i] += (float)(gain * amp * attack * Math.Exp(-t * decay) * Math.Sin(w * i));
            }
        }

        var fade = Math.Min(n, Sr / 10);
        for (var i = 0; i < fade; i++) s[n - fade + i] *= 1f - (float)i / fade;
        return s;
    }

    private static void Mix(float[] dest, float[] src, int offset)
    {
        var n = Math.Min(src.Length, dest.Length - offset);
        for (var i = 0; i < n; i++) dest[offset + i] += src[i];
    }

    private sealed record Seen(ulong EmittedAt, JuceOnsetNotice? Notice, JuceGuitarEvent? Event);

    /// <summary>Pushes in small blocks, the way the live detection worker does, and records
    /// the stream position each notice and event came OUT at.</summary>
    private static List<Seen> Run(JuceGuitarAnalyzer analyzer, float[] signal)
    {
        var seen = new List<Seen>();
        var notices = new List<JuceOnsetNotice>();
        var padded = new float[signal.Length + Sr / 2];
        Array.Copy(signal, padded, signal.Length);
        for (var off = 0; off < padded.Length; off += Block)
        {
            notices.Clear();
            var events = analyzer.Push(padded.AsSpan(off, Math.Min(Block, padded.Length - off)), notices);
            foreach (var n in notices) seen.Add(new Seen(analyzer.StreamPosition, n, null));
            foreach (var e in events) seen.Add(new Seen(analyzer.StreamPosition, null, e));
        }

        return seen;
    }

    [Theory]
    [InlineData(82.41f)]   // E2
    [InlineData(110f)]     // A2
    [InlineData(196f)]     // G3
    [InlineData(329.63f)]  // E4
    public void ProvisionalOnset_LandsWithinOneHopOfTheAttack_LongBeforeTheFinalizedEvent(float f0)
    {
        int[] attacks = [(int)(0.10 * Sr), (int)(0.75 * Sr), (int)(1.40 * Sr)];
        var signal = new float[(int)(2.0 * Sr)];
        foreach (var a in attacks) Mix(signal, Pluck(f0, 0.5), a);

        var analyzer = new JuceGuitarAnalyzer(Sr);
        var seen = Run(analyzer, signal);

        string Trace() => string.Join("; ", seen.Select(s => s.Notice is { } n
            ? $"{n.Kind} det {(long)n.DetectorSamplePosition} prov {(long)n.StreamSamplePosition} @ {s.EmittedAt}"
            : $"{s.Event!.Value.Kind} {s.Event.Value.StreamSamplePosition} det {s.Event.Value.DetectorSamplePosition} @ {s.EmittedAt}"));

        Assert.Equal(attacks.Length, seen.Count(s => s.Event is { Kind: JuceGuitarEventKind.Onset }));
        foreach (var attack in attacks)
        {
            // The finalized event for this attack, and a provisional onset for it within one hop
            // (the raw detector position is ~300 samples early; the provisional attack is snapped
            // onto the rise).
            var finalized = seen.Single(s => s.Event is { Kind: JuceGuitarEventKind.Onset } e &&
                                             Math.Abs((long)e.StreamSamplePosition - attack) <= JuceOnsetDetector.HopSize);
            var provisional = seen.Where(s => s.Notice is { Kind: JuceOnsetNoticeKind.Detected } n &&
                                              Math.Abs((long)n.StreamSamplePosition - attack) <= JuceOnsetDetector.HopSize)
                                  .MinBy(s => s.EmittedAt);
            Assert.True(provisional is not null, $"{f0} Hz: no provisional onset within one hop of the attack at {attack}: {Trace()}");
            var notice = provisional.Notice!.Value;
            Assert.Equal(notice.StreamSamplePosition / (double)Sr, notice.StreamSeconds, 9);
            Assert.True(notice.Strength > 1f);

            // ...out long before the event: within the detector's own report delay plus a hop
            // and a block of the attack, where the event needs the analyzer's whole LatencySamples.
            var provisionalAge = (long)provisional.EmittedAt - attack;
            var finalizedAge = (long)finalized.EmittedAt - attack;
            Assert.True(provisionalAge <= JuceOnsetDetector.LatencySamples + JuceOnsetDetector.HopSize + Block,
                $"{f0} Hz: provisional onset out {provisionalAge} samples after the attack at {attack}");
            Assert.True(finalizedAge >= analyzer.LatencySamples - JuceOnsetDetector.HopSize - Block,
                $"{f0} Hz: event out {finalizedAge} samples after the attack at {attack}");
            Assert.True(finalizedAge - provisionalAge >= analyzer.LatencySamples / 2,
                $"{f0} Hz: provisional {provisionalAge}, finalized {finalizedAge} samples after the attack at {attack}");

            // ...and it is the one the event finalizes (paired by the detector position).
            Assert.Equal(notice.DetectorSamplePosition, finalized.Event!.Value.DetectorSamplePosition);
        }
    }

    [Fact]
    public void EveryDetectedNotice_IsResolvedExactlyOnce_ByItsEventOrAWithdrawal()
    {
        // Two real plucks and one far below the noise gate: the detector fires on all three
        // (its flux threshold is relative), the analyzer reports only the two.
        var signal = new float[(int)(2.2 * Sr)];
        Mix(signal, Pluck(110f, 0.6), (int)(0.10 * Sr));
        Mix(signal, Pluck(196f, 0.6, gain: 0.002f), (int)(0.90 * Sr));
        Mix(signal, Pluck(146.83f, 0.6), (int)(1.50 * Sr));

        var seen = Run(new JuceGuitarAnalyzer(Sr), signal);

        var detected = seen.Where(s => s.Notice is { Kind: JuceOnsetNoticeKind.Detected }).ToList();
        Assert.NotEmpty(detected);
        var withdrawn = 0;
        foreach (var d in detected)
        {
            var key = d.Notice!.Value.DetectorSamplePosition;
            var resolutions = seen.Where(s =>
                    (s.Event is { } e && e.DetectorSamplePosition == key) ||
                    (s.Notice is { Kind: JuceOnsetNoticeKind.Withdrawn } w && w.DetectorSamplePosition == key))
                .ToList();
            Assert.Single(resolutions);
            Assert.True(resolutions[0].EmittedAt >= d.EmittedAt, "a resolution came out before its notice");
            if (resolutions[0].Notice is { } r)
            {
                withdrawn++;
                Assert.Equal(d.Notice!.Value.Strength, r.Strength);
            }
        }

        Assert.Equal(2, seen.Count(s => s.Event is { Kind: JuceGuitarEventKind.Onset }));
        Assert.True(withdrawn >= 1, "the sub-gate pluck was not withdrawn");
        Assert.DoesNotContain(seen, s => s.Event is { } e && e.DetectorSamplePosition is { } k &&
                                        detected.All(d => d.Notice!.Value.DetectorSamplePosition != k));
    }

    /// <summary>
    /// The 1024-point flux detector also fires every ~50 ms on the window-leakage ripple of a
    /// ringing low note (measured: ~10 detector onsets per 0.5 s E2 pluck, every one withdrawn by
    /// the novelty gate ~190 ms later). A provisional onset is read at once, so those must not be
    /// published: one ringing note is ONE provisional onset.
    /// </summary>
    [Theory]
    [InlineData(82.41f)]
    [InlineData(110f)]
    [InlineData(329.63f)]
    public void TheRippleOfARingingNote_IsNotPublishedAsAProvisionalOnset(float f0)
    {
        var signal = new float[(int)(2.2 * Sr)];
        Mix(signal, Pluck(f0, 2.0), (int)(0.10 * Sr));

        var seen = Run(new JuceGuitarAnalyzer(Sr), signal);

        var detected = seen.Where(s => s.Notice is { Kind: JuceOnsetNoticeKind.Detected }).ToList();
        var ev = Assert.Single(seen, s => s.Event is { Kind: JuceGuitarEventKind.Onset });
        var notice = Assert.Single(detected).Notice!.Value;
        Assert.Equal(notice.DetectorSamplePosition, ev.Event!.Value.DetectorSamplePosition);
        Assert.DoesNotContain(seen, s => s.Notice is { Kind: JuceOnsetNoticeKind.Withdrawn });
    }

    [Fact]
    public void NoticeOverload_EmitsTheSameEventsAsThePlainPush()
    {
        var signal = new float[(int)(1.6 * Sr)];
        Mix(signal, Pluck(82.41f, 0.5), (int)(0.10 * Sr));
        Mix(signal, Pluck(246.94f, 0.5), (int)(0.70 * Sr));

        var plain = new JuceGuitarAnalyzer(Sr);
        var withNotices = new JuceGuitarAnalyzer(Sr);
        var a = new List<JuceGuitarEvent>();
        var b = new List<JuceGuitarEvent>();
        var notices = new List<JuceOnsetNotice>();
        for (var off = 0; off < signal.Length; off += 1600)
        {
            var span = signal.AsSpan(off, Math.Min(1600, signal.Length - off));
            a.AddRange(plain.Push(span));
            b.AddRange(withNotices.Push(span, notices));
        }

        Assert.Equal(2, a.Count);
        Assert.Equal(a.Count, b.Count);
        for (var i = 0; i < a.Count; i++)
        {
            Assert.Equal(a[i].StreamSamplePosition, b[i].StreamSamplePosition);
            Assert.Equal(a[i].DetectorSamplePosition, b[i].DetectorSamplePosition);
            Assert.Equal(a[i].Techniques, b[i].Techniques);
            Assert.Equal(a[i].Pitches, b[i].Pitches);
            Assert.Equal(a[i].Chroma, b[i].Chroma);
        }

        Assert.True(notices.Count >= 2);
    }

    [Fact]
    public void NoticeOverload_RejectsANullList() =>
        Assert.Throws<ArgumentNullException>(() => new JuceGuitarAnalyzer(Sr).Push(new float[16], null!));
}
