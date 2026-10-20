namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Windowed-sinc resampler used to convert the input mix to/from the
/// 44.1 kHz rate that demucs/htdemucs / MDX-Net models expect. Quality
/// is "good enough for source separation" — Lanczos-a (a=8) kernel,
/// per-channel processing, no FFT.
/// </summary>
public static class SampleRateConverter
{
    private const int Lobes = 8;

    /// <summary>Resamples interleaved PCM. Returns the same buffer if no rate change is needed.</summary>
    public static float[] Resample(float[] interleaved, int channels, int srcRate, int dstRate)
        => Resample(interleaved, channels, srcRate, dstRate, polyphase: true);

    /// <summary>The resampler with the phase table switched off: the direct per-frame kernel
    /// the table must reproduce. Internal, for the test that holds the two together.</summary>
    internal static float[] ResampleDirect(float[] interleaved, int channels, int srcRate, int dstRate)
        => Resample(interleaved, channels, srcRate, dstRate, polyphase: false);

    private static float[] Resample(float[] interleaved, int channels, int srcRate, int dstRate, bool polyphase)
    {
        if (channels <= 0) throw new ArgumentOutOfRangeException(nameof(channels));
        if (srcRate <= 0 || dstRate <= 0) throw new ArgumentOutOfRangeException(nameof(srcRate));
        if (srcRate == dstRate) return interleaved;

        var srcFrames = interleaved.Length / channels;
        var ratio     = (double)dstRate / srcRate;
        var dstFrames = (int)Math.Round(srcFrames * ratio);
        var output    = new float[dstFrames * channels];

        // When downsampling, scale the cutoff to avoid aliasing.
        var cutoff = ratio < 1.0 ? ratio : 1.0;

        // …and WIDEN THE LOOP WITH IT. Scaling x by `cutoff` lowers the kernel's cutoff,
        // but it also stretches the kernel's SUPPORT by 1/cutoff in SOURCE samples:
        // LanczosKernel is zero only for |x| >= Lobes, i.e. for |idx - srcPos| >=
        // Lobes/cutoff. Visiting a fixed 2·Lobes source samples therefore HARD-TRUNCATED
        // the intended window — a rectangular window on a sinc, which is passband ripple
        // and stopband leakage, i.e. exactly the aliasing the `cutoff` line exists to
        // prevent. At 48 -> 44.1 kHz (cutoff 0.919) the error is small; at 96 -> 44.1
        // (0.459) the kernel was cut at HALF its support, and a 96 kHz master is a real
        // input that goes through here twice. Cost scales as 1/cutoff, which is the
        // correct price for the filter that was being asked for all along.
        // Measured on a 30 kHz tone at 96 kHz resampled to 44.1 kHz: its 14.1 kHz alias
        // sat at -28.6 dBFS truncated and -51.2 dBFS with the full support.
        var half = (int)Math.Ceiling(Lobes / cutoff);

        // A rational ratio (every pair of real sample rates) repeats its fractional source
        // position every `phases` output frames, so the kernel weights - two Math.Sin per tap
        // per channel per frame, which made the six post-resamples of a 224 s BS-RoFormer song
        // cost 3.3 s - are computed once per phase. Frames whose window reaches past either
        // end of the input are computed directly, exactly as before.
        var g = Gcd(srcRate, dstRate);
        var phases = dstRate / g;
        var step = srcRate / g;
        if (polyphase && phases <= MaxPhaseTable)
        {
            ResamplePolyphase(interleaved, output, channels, srcFrames, dstFrames, phases, step, half, cutoff);
            return output;
        }

        // Stripe destination frames across cores — each output frame is
        // independent. Captured locals (`ratio`, `cutoff`, `channels`, …)
        // are read-only; `interleaved` is shared read; `output` is written
        // at disjoint indices per d.
        Parallel.For(0, dstFrames, d => ResampleFrame(interleaved, output, channels, srcFrames, d, d / ratio, half, cutoff));
        return output;
    }

    /// <summary>Phase tables longer than this fall back to the direct kernel (a pair of rates
    /// whose ratio does not reduce, e.g. 44,100 and 47,999 Hz).</summary>
    internal const int MaxPhaseTable = 4096;

    private static void ResamplePolyphase(float[] input, float[] output, int channels, int srcFrames, int dstFrames,
        int phases, int step, int half, double cutoff)
    {
        var taps = 2 * half;
        var weights = new double[phases * taps];
        for (var p = 0; p < phases; p++)
        {
            var frac = (double)p / phases;
            var norm = 0.0;
            for (var k = -half + 1; k <= half; k++)
            {
                var w = LanczosKernel((k - frac) * cutoff) * cutoff;
                weights[p * taps + k + half - 1] = w;
                norm += w;
            }
            for (var t = 0; t < taps; t++)
                weights[p * taps + t] = norm > 1e-9 ? weights[p * taps + t] / norm : 0.0;
        }

        Parallel.For(0, dstFrames, d =>
        {
            var pos = (long)d * step;
            var center = (int)(pos / phases);
            var phase = (int)(pos % phases);
            var first = center - half + 1;
            if (first < 0 || center + half >= srcFrames)
            {
                ResampleFrame(input, output, channels, srcFrames, d, (double)pos / phases, half, cutoff);
                return;
            }

            var row = phase * taps;
            for (var c = 0; c < channels; c++)
            {
                var sum = 0.0;
                var idx = first * channels + c;
                for (var t = 0; t < taps; t++, idx += channels) sum += input[idx] * weights[row + t];
                output[d * channels + c] = (float)sum;
            }
        });
    }

    private static void ResampleFrame(float[] interleaved, float[] output, int channels, int srcFrames, int d,
        double srcPos, int half, double cutoff)
    {
        var center = (int)Math.Floor(srcPos);
        for (var c = 0; c < channels; c++)
        {
            var sum  = 0.0;
            var norm = 0.0;
            for (var k = -half + 1; k <= half; k++)
            {
                var idx = center + k;
                if (idx < 0 || idx >= srcFrames) continue;
                var x = (idx - srcPos) * cutoff;
                var w = LanczosKernel(x);
                sum  += interleaved[idx * channels + c] * w * cutoff;
                norm += w * cutoff;
            }
            output[d * channels + c] = norm > 1e-9 ? (float)(sum / norm) : 0f;
        }
    }

    private static int Gcd(int a, int b)
    {
        while (b != 0) (a, b) = (b, a % b);
        return a;
    }

    private static double LanczosKernel(double x)
    {
        if (x == 0.0) return 1.0;
        if (x <= -Lobes || x >= Lobes) return 0.0;
        var pix = Math.PI * x;
        return Lobes * Math.Sin(pix) * Math.Sin(pix / Lobes) / (pix * pix);
    }
}
