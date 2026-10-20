using System.Diagnostics;
using System.Runtime.CompilerServices;
using JuceSharp.StemSeparation.Onnx;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Skip attribute for ONNX integration tests that require both an installed
/// model file under <c>%APPDATA%\DotCharter\models\</c> and the bundled
/// TestData WAV. Missing resources cause the test to be skipped (not failed),
/// so CI without the model still passes.
/// </summary>
public sealed class OnnxResourceFactAttribute : FactAttribute
{
    public OnnxResourceFactAttribute(
        string modelRelativePath,
        bool requiresTestWav = false,
        [CallerFilePath] string? sourceFilePath = null,
        [CallerLineNumber] int sourceLineNumber = -1)
        : base(sourceFilePath, sourceLineNumber)
    {
        var modelPath = OnnxStemSeparatorIntegrationTests.ResolveModel(modelRelativePath);
        if (!File.Exists(modelPath))
        {
            Skip = $"ONNX model missing at {modelPath}.";
            return;
        }
        if (requiresTestWav && !File.Exists(OnnxStemSeparatorIntegrationTests.TestWavPathResolved))
            Skip = $"Test WAV missing at {OnnxStemSeparatorIntegrationTests.TestWavPathResolved}.";
    }
}

/// <summary>
/// Integration tests for <see cref="OnnxStemSeparator"/> that exercise the
/// real ONNX runtime against the user's installed model files.
/// </summary>
[Collection("NativeEngine")]
public sealed class OnnxStemSeparatorIntegrationTests
{
    internal static readonly string ModelsDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "DotCharter", "models");

    internal static string ResolveModel(string file) => Path.Combine(ModelsDir, file);

    internal static readonly string TestWavPathResolved = Path.GetFullPath(Path.Combine(
        AppContext.BaseDirectory, "..", "..", "..", "..", "..", "..",
        "TestData", "EOF", "Atarayo - Natsu ga Kuru Tabi", "guitar_preview.wav"));

    [OnnxResourceFact("htdemucs_6s.onnx", requiresTestWav: true)]
    public async Task Htdemucs6s_ProducesSixNonSilentStems_OnRealAudio()
    {
        var (interleaved, sampleRate, channels) = ReadWav(TestWavPathResolved);
        Assert.Equal(2, channels);

        using var sep = new OnnxStemSeparator(
            ResolveModel("htdemucs_6s.onnx"),
            OnnxStemModelKind.Htdemucs);

        var sw = Stopwatch.StartNew();
        var stems = await sep.SeparateAsync(interleaved, sampleRate);
        sw.Stop();

        Assert.Equal(6, stems.Count);
        var expected = new[] { "drums", "bass", "other", "vocals", "guitar", "piano" };
        for (var i = 0; i < expected.Length; i++)
            Assert.Equal(expected[i], stems[i].Name);

        foreach (var stem in stems)
        {
            Assert.Equal(sampleRate, stem.SampleRate);
            Assert.Equal(interleaved.Length, stem.InterleavedStereo.Length);
        }

        double maxRms = 0;
        foreach (var stem in stems)
        {
            var rms = ComputeRms(stem.InterleavedStereo);
            if (rms > maxRms) maxRms = rms;
        }
        Assert.True(maxRms > 0.01,
            $"All stems essentially silent (max RMS = {maxRms}). " +
            $"Backend was {sep.BackendDescription}. Inference took {sw.ElapsedMilliseconds} ms.");
    }

    [Fact]
    public void OnnxStemSeparator_Throws_OnMissingModel()
    {
        Assert.Throws<FileNotFoundException>(() =>
            new OnnxStemSeparator("Z:/definitely/does/not/exist.onnx",
                OnnxStemModelKind.Htdemucs,
                OnnxExecutionProvider.Cpu));
    }

    [OnnxResourceFact("htdemucs_6s.onnx")]
    public async Task SeparateAsync_Rejects_OddLengthInterleavedInput()
    {
        using var sep = new OnnxStemSeparator(ResolveModel("htdemucs_6s.onnx"),
            OnnxStemModelKind.Htdemucs,
            OnnxExecutionProvider.Cpu);

        var oddLength = new float[12345];
        await Assert.ThrowsAsync<ArgumentException>(() =>
            sep.SeparateAsync(oddLength, 44100));
    }

    internal static (float[] Interleaved, int SampleRate, int Channels) ReadWav(string path)
    {
        using var fs = File.OpenRead(path);
        using var br = new BinaryReader(fs);
        Span<byte> tag = stackalloc byte[4];
        br.BaseStream.ReadExactly(tag);
        if (System.Text.Encoding.ASCII.GetString(tag) != "RIFF") throw new InvalidDataException("Not a WAV file.");
        br.ReadInt32();
        br.BaseStream.ReadExactly(tag);
        if (System.Text.Encoding.ASCII.GetString(tag) != "WAVE") throw new InvalidDataException("Missing WAVE.");

        short format = 0, channels = 0, bitsPerSample = 0;
        int sampleRate = 0;
        byte[] dataBytes = [];

        while (fs.Position < fs.Length)
        {
            br.BaseStream.ReadExactly(tag);
            var chunkName = System.Text.Encoding.ASCII.GetString(tag);
            var chunkSize = br.ReadInt32();
            if (chunkName == "fmt ")
            {
                format = br.ReadInt16();
                channels = br.ReadInt16();
                sampleRate = br.ReadInt32();
                br.ReadInt32();
                br.ReadInt16();
                bitsPerSample = br.ReadInt16();
                if (chunkSize > 16) br.ReadBytes(chunkSize - 16);
            }
            else if (chunkName == "data")
            {
                dataBytes = br.ReadBytes(chunkSize);
            }
            else
            {
                br.ReadBytes(chunkSize);
            }
        }

        var bytesPerSample = Math.Max(1, bitsPerSample / 8);
        var totalSamples = dataBytes.Length / bytesPerSample;
        var floats = new float[totalSamples];
        if (format == 3)
        {
            Buffer.BlockCopy(dataBytes, 0, floats, 0, dataBytes.Length);
        }
        else if (format == 1)
        {
            for (var i = 0; i < totalSamples; i++)
            {
                switch (bitsPerSample)
                {
                    case 16:
                        floats[i] = BitConverter.ToInt16(dataBytes, i * 2) / 32768f;
                        break;
                    case 24:
                        {
                            var b0 = dataBytes[i * 3];
                            var b1 = dataBytes[i * 3 + 1];
                            var b2 = dataBytes[i * 3 + 2];
                            var v = (b2 << 24 | b1 << 16 | b0 << 8) >> 8;
                            floats[i] = v / 8388608f;
                            break;
                        }
                    case 32:
                        floats[i] = BitConverter.ToInt32(dataBytes, i * 4) / 2147483648f;
                        break;
                    default:
                        throw new NotSupportedException($"Unsupported PCM bit depth {bitsPerSample}.");
                }
            }
        }
        else
        {
            throw new NotSupportedException($"Unsupported WAV format tag {format}.");
        }
        return (floats, sampleRate, channels);
    }

    private static double ComputeRms(float[] x)
    {
        if (x.Length == 0) return 0;
        double s = 0;
        for (var i = 0; i < x.Length; i++) s += x[i] * (double)x[i];
        return Math.Sqrt(s / x.Length);
    }
}
