using System.Xml.Linq;

namespace JuceSharp.Tests;

/// <summary>
/// Shared IDMT-SMT-GUITAR_V2 plumbing for the CQT evaluation harnesses
/// (<see cref="CqtDatasetEvalTests"/>, <see cref="CqtStreamingEvalTests"/>):
/// dataset discovery, track pairing, annotation parsing and a minimal WAV reader.
///
/// <para>Opt-in: the evals skip unless <c>GND_IDMT_ROOT</c> names an existing
/// directory. There is deliberately no fallback path — see
/// <see cref="ResolveRoot()"/>.</para>
/// </summary>
internal static class IdmtDataset
{
    /// <summary>One annotated reference note: onset/offset (s) + sounding MIDI pitch.</summary>
    internal readonly record struct Ref(double Onset, double Offset, double Midi);

    /// <summary>
    /// Is <paramref name="midi"/> annotated as SOUNDING anywhere in [<paramref name="from"/>,
    /// <paramref name="to"/>]?
    /// <para>This is what makes a false-accept number honest on polyphonic material.
    /// IDMT dataset3/dataset4 are chords and licks, so a decoy at +7 or +12 is very often
    /// a note the player is genuinely holding (a power chord IS root+fifth+octave). Counting
    /// those as "false accepts" measures the dataset, not the verifier.</para>
    /// </summary>
    internal static bool Sounding(List<Ref> refs, double midi, double from, double to)
    {
        foreach (var r in refs)
            if (Math.Abs(r.Midi - midi) < 0.5 && r.Onset <= to && r.Offset >= from)
                return true;
        return false;
    }

    /// <summary>
    /// The dataset root, or <c>null</c> when the eval must skip.
    ///
    /// <para><b>`GND_IDMT_ROOT` is the ONLY source, and that is the whole
    /// point.</b> This used to fall back to one developer's absolute checkout
    /// path, so on that ONE machine — the owner's, the only one whose gate
    /// anybody watches — both IDMT harnesses ran on every `dotnet test`,
    /// despite three doc comments (here, `CqtStreamingEvalTests`,
    /// `EvalKnobs`) promising they are opt-in and "never run in the gate".
    /// Measured at the default cap of 60 tracks they take ~10 minutes and end
    /// in either a plain assertion failure or a silent test-host death, which
    /// is a large part of what "the JuceSharp suite is flaky" meant. A
    /// research harness is opt-in or it is part of the gate; a hard-coded
    /// home directory makes it both at once.</para>
    /// </summary>
    internal static string? ResolveRoot() => ResolveRoot(Environment.GetEnvironmentVariable);

    /// <summary>Testable seam — see <c>IdmtDatasetTests</c>.</summary>
    internal static string? ResolveRoot(Func<string, string?> readEnv)
    {
        var root = readEnv("GND_IDMT_ROOT");
        return !string.IsNullOrWhiteSpace(root) && Directory.Exists(root) ? root : null;
    }

    /// <summary>Mirror load_idmt: every dir named "audio" pairs to a sibling "annotation".</summary>
    internal static List<(string Wav, string Xml)> PairTracks(string root)
    {
        var pairs = new List<(string, string)>();
        foreach (var audioDir in Directory.EnumerateDirectories(root, "audio", SearchOption.AllDirectories).OrderBy(p => p))
        {
            var annDir = Path.Combine(Path.GetDirectoryName(audioDir)!, "annotation");
            if (!Directory.Exists(annDir)) continue;
            foreach (var wav in Directory.EnumerateFiles(audioDir, "*.wav", SearchOption.AllDirectories).OrderBy(p => p))
            {
                var xml = Path.Combine(annDir, Path.GetFileNameWithoutExtension(wav) + ".xml");
                if (File.Exists(xml)) pairs.Add((wav, xml));
            }
        }
        return pairs;
    }

    /// <summary>Deterministic evenly-spaced sample of the track list (stable across runs,
    /// so before/after comparisons at the SAME cap see the SAME tracks).</summary>
    internal static List<T> Stride<T>(List<T> all, int cap)
    {
        if (all.Count <= cap) return all;
        var step = all.Count / (double)cap;
        var outp = new List<T>(cap);
        for (int i = 0; i < cap; i++) outp.Add(all[(int)(i * step)]);
        return outp;
    }

    internal static List<Ref> Parse(string xmlPath)
    {
        var refs = new List<Ref>();
        var doc = XDocument.Load(xmlPath);
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        foreach (var ev in doc.Descendants("event"))
        {
            var pitch = (string?)ev.Element("pitch") ?? (string?)ev.Element("pitchMidi");
            var onset = (string?)ev.Element("onsetSec") ?? (string?)ev.Element("onset");
            if (pitch is null || onset is null) continue;
            if (!double.TryParse(pitch, inv, out var p) || !double.TryParse(onset, inv, out var t)) continue;

            var offset = (string?)ev.Element("offsetSec") ?? (string?)ev.Element("offset");
            // No annotated offset ⇒ treat the note as sounding only at its onset.
            var end = offset is not null && double.TryParse(offset, inv, out var e) ? e : t;
            refs.Add(new Ref(t, Math.Max(end, t), p));
        }
        return refs;
    }

    /// <summary>Group notes struck together (chords/strums) within <paramref name="windowSec"/>.</summary>
    internal static List<List<Ref>> ClusterEvents(List<Ref> refs, double windowSec)
    {
        var groups = new List<List<Ref>>();
        foreach (var n in refs.OrderBy(r => r.Onset))
        {
            if (groups.Count > 0 && n.Onset - groups[^1][0].Onset <= windowSec) groups[^1].Add(n);
            else groups.Add(new List<Ref> { n });
        }
        return groups;
    }

    /// <summary>Minimal PCM WAV reader → mono float[-1,1]. Handles mono/stereo and
    /// 16/24/32-bit int + 32-bit float, skipping unknown chunks (fact/LIST/…).</summary>
    internal static float[] ReadWavMono(string path, out int sampleRate)
    {
        sampleRate = 44100;
        using var fs = File.OpenRead(path);
        using var br = new BinaryReader(fs);
        if (new string(br.ReadChars(4)) != "RIFF") return Array.Empty<float>();
        br.ReadInt32();
        if (new string(br.ReadChars(4)) != "WAVE") return Array.Empty<float>();

        int channels = 1, bits = 16, format = 1;
        byte[]? data = null;
        while (fs.Position + 8 <= fs.Length)
        {
            var id = new string(br.ReadChars(4));
            int size = br.ReadInt32();
            if (id == "fmt ")
            {
                format = br.ReadInt16();              // 1 = PCM int, 3 = IEEE float, 0xFFFE = extensible
                channels = br.ReadInt16();
                sampleRate = br.ReadInt32();
                br.ReadInt32();                       // byte rate
                br.ReadInt16();                       // block align
                bits = br.ReadInt16();
                if (size > 16) br.ReadBytes(size - 16);
            }
            else if (id == "data")
            {
                data = br.ReadBytes(size);
            }
            else
            {
                br.ReadBytes(size + (size & 1));      // skip (chunks are word-aligned)
            }
            if (data != null) break;
        }
        int bytesPer = bits / 8;
        if (data == null || channels < 1 || bytesPer < 2 || bytesPer > 4) return Array.Empty<float>();

        bool isFloat = format == 3;
        float intScale = 1f / (channels * (float)Math.Pow(2, bits - 1));  // signed full-scale → [-1,1]
        int frames = data.Length / (bytesPer * channels);
        var y = new float[frames];
        for (int i = 0; i < frames; i++)
        {
            float acc = 0;
            for (int ch = 0; ch < channels; ch++)
            {
                int o = (i * channels + ch) * bytesPer;
                if (isFloat) acc += BitConverter.ToSingle(data, o);       // 32-bit float PCM, already [-1,1]
                else if (bytesPer == 2) acc += (short)(data[o] | (data[o + 1] << 8));
                else if (bytesPer == 3)
                {
                    int s = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16);
                    if ((s & 0x800000) != 0) s |= unchecked((int)0xFF000000);  // sign-extend 24→32
                    acc += s;
                }
                else acc += data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (data[o + 3] << 24);
            }
            y[i] = isFloat ? acc / channels : acc * intScale;
        }
        return y;
    }
}
