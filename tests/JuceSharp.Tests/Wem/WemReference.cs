using NVorbis;
using Xunit;
using Ww2Ogg.Core;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// The path the native decoder replaces, exactly as DotCharter's
/// <c>AudioFormatConverter</c> runs it: Ww2Ogg.Core rebuilds an Ogg from the
/// .wem, NVorbis decodes it to float, and the WAV cache stores
/// <see cref="ToPcm16" /> of that.
/// </summary>
internal static class WemReference
{
    internal sealed record Decoded(float[] Interleaved, int Channels, int SampleRate)
    {
        public long Frames => Interleaved.Length / Channels;
    }

    /// <summary>The managed variant list, in DotCharter's order (AudioFormatConverter.WemVariants).</summary>
    internal static readonly (JuceWemVariant Variant, Func<CodebookLibrary> Books, bool Inline, ForcePacketFormat Force)[] Variants =
    [
        (JuceWemVariant.AoTuV, () => CodebookLibrary.AoTuV, false, ForcePacketFormat.NoForce),
        (JuceWemVariant.Default, () => CodebookLibrary.Default, false, ForcePacketFormat.NoForce),
        (JuceWemVariant.Inline, () => new CodebookLibrary(), true, ForcePacketFormat.NoForce),
        (JuceWemVariant.DefaultMod, () => CodebookLibrary.Default, false, ForcePacketFormat.ForceModPackets),
        (JuceWemVariant.DefaultStandard, () => CodebookLibrary.Default, false, ForcePacketFormat.ForceNoModPackets),
    ];

    /// <summary>ww2ogg's rebuild with the first managed variant that converts and
    /// validates (DotCharter's loop), or the one asked for.</summary>
    internal static byte[] ToOgg(byte[] wem, JuceWemVariant only = JuceWemVariant.Auto)
    {
        Exception? last = null;
        foreach (var v in Variants)
        {
            if (only != JuceWemVariant.Auto && only != v.Variant) continue;
            try
            {
                using var input = new MemoryStream(wem, false);
                using var output = new MemoryStream();
                new WwiseRiffVorbis(input, v.Books(), v.Inline, false, v.Force).GenerateOgg(output);
                if (output.Length <= 0) continue;
                output.Position = 0;
                VorbisValidator.Validate(output);
                return output.ToArray();
            }
            catch (Exception ex) { last = ex; }
        }
        throw new InvalidDataException("no managed ww2ogg variant converted the .wem", last);
    }

    internal static Decoded DecodeOgg(byte[] ogg)
    {
        using var reader = new VorbisReader(new MemoryStream(ogg, false), true);
        var samples = new List<float>((int)Math.Min(int.MaxValue / 2, reader.TotalSamples * reader.Channels));
        var buffer = new float[reader.Channels * 8192];
        int n;
        while ((n = reader.ReadSamples(buffer, 0, buffer.Length)) > 0)
            samples.AddRange(buffer.AsSpan(0, n).ToArray());
        return new Decoded(samples.ToArray(), reader.Channels, reader.SampleRate);
    }

    internal static Decoded DecodeManaged(byte[] wem, JuceWemVariant only = JuceWemVariant.Auto)
        => DecodeOgg(ToOgg(wem, only));

    internal static Decoded DecodeNative(byte[] wem, JuceWemVariant variant = JuceWemVariant.Auto)
    {
        using var d = JuceWemDecoder.OpenMemory(wem, variant);
        var all = new float[d.Info.Frames * d.Info.Channels];
        var at = 0;
        while (at < all.Length)
        {
            var got = d.Read(all.AsSpan(at, Math.Min(all.Length - at, 4096 * d.Info.Channels)));
            if (got == 0) break;
            at += got * d.Info.Channels;
        }
        Assert.Equal(all.Length, at);
        return new Decoded(all, d.Info.Channels, d.Info.SampleRate);
    }

    /// <summary>DotCharter's AudioFormatConverter.ConvertToPcm16, verbatim.</summary>
    internal static short ToPcm16(float sample)
    {
        var s = Math.Clamp(sample, -1.0f, 1.0f);
        return s <= -1.0f ? short.MinValue : (short)Math.Round(s * short.MaxValue);
    }

    internal sealed record Comparison(long NativeFrames, long ManagedFrames, double MaxAbsDiff, long Pcm16Mismatches, int MaxPcm16Diff);

    /// <summary>Native vs managed: float difference after clipping the native
    /// samples the way NVorbis does, and the 16-bit WAV samples.</summary>
    internal static Comparison Compare(Decoded native, Decoded managed)
    {
        var n = Math.Min(native.Interleaved.Length, managed.Interleaved.Length);
        double max = 0;
        long pcmMismatch = 0;
        var maxPcm = 0;
        for (var i = 0; i < n; i++)
        {
            var a = Math.Clamp(native.Interleaved[i], -1f, 1f);
            var b = managed.Interleaved[i];
            var d = Math.Abs((double)a - b);
            if (d > max) max = d;
            var pa = ToPcm16(native.Interleaved[i]);
            var pb = ToPcm16(b);
            if (pa != pb)
            {
                pcmMismatch++;
                maxPcm = Math.Max(maxPcm, Math.Abs(pa - pb));
            }
        }
        return new Comparison(native.Frames, managed.Frames, max, pcmMismatch, maxPcm);
    }
}
