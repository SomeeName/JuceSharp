using System.Runtime.CompilerServices;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Wem;

/// <summary>
/// A .wem is copied into memory whole before it is decoded, so every entry point
/// caps it (<c>dcwem::kMaxWemBytes</c>, 256 MiB) and refuses a bigger one BEFORE
/// it is buffered: <c>dca_wem_open_memory</c>, <c>dca_wem_open_file</c> and the
/// engine's own format reader. Each refusal is paired with the same audio under
/// the cap opening fine, so the trailing padding is not what fails it.
/// </summary>
[Collection("NullDevice")]
public sealed class WemInputCapTests : IDisposable
{
    private const int CapBytes = 256 << 20;
    private readonly List<string> _files = [];

    public void Dispose()
    {
        foreach (var f in _files)
        {
            try { File.Delete(f); }
            catch (IOException) { }
        }
    }

    private static byte[] SmallWem()
    {
        var pcm = new float[4800 * 2];
        for (var i = 0; i < 4800; i++)
            pcm[2 * i] = pcm[2 * i + 1] = 0.25f * MathF.Sin(i * 2f * MathF.PI * 220f / 48000f);
        return JuceWemEncoder.EncodeVorbis(pcm);
    }

    /// <summary>The .wem at the front, zeros after it, <paramref name="length"/>
    /// bytes in all. The decoder only reads inside the RIFF, so padding alone
    /// never makes it fail.</summary>
    private string PaddedFile(byte[] wem, long length)
    {
        var path = Path.Combine(Path.GetTempPath(), $"jsw-cap-{Guid.NewGuid():N}.wem");
        _files.Add(path);
        using var fs = new FileStream(path, FileMode.CreateNew);
        fs.Write(wem);
        fs.SetLength(length);
        return path;
    }

    [NativeAvailableFact]
    public void A_buffer_over_the_cap_is_refused_and_one_under_it_opens()
    {
        var wem = SmallWem();
        var under = new byte[wem.Length + 4096];
        wem.CopyTo(under, 0);
        using (JuceWemDecoder.OpenMemory(under)) { }

        var over = new byte[CapBytes + 1];
        wem.CopyTo(over, 0);
        Assert.Throws<JuceAudioException>(() => JuceWemDecoder.OpenMemory(over).Dispose());
    }

    [NativeAvailableFact]
    public void A_file_over_the_cap_is_refused_and_one_under_it_opens()
    {
        var wem = SmallWem();
        using (JuceWemDecoder.OpenFile(PaddedFile(wem, wem.Length + 4096))) { }
        Assert.Throws<JuceAudioException>(() => JuceWemDecoder.OpenFile(PaddedFile(wem, CapBytes + 1L)).Dispose());
    }

    /// <summary>The engine's format reader (every <c>OpenFile</c> of a .wem song)
    /// buffers the file too, and is capped the same way.</summary>
    [NativeAvailableFact]
    public void The_engine_refuses_a_wem_source_over_the_cap()
    {
        var wem = SmallWem();
        using var scope = new NullDeviceScope();
        using var engine = NullDevice.CreateEngine(inputChannels: 1);
        using (engine.OpenFile(PaddedFile(wem, wem.Length + 4096))) { }
        Assert.ThrowsAny<Exception>(() => engine.OpenFile(PaddedFile(wem, CapBytes + 1L)).Dispose());
    }

    [Fact]
    public void The_cap_this_test_uses_is_the_decoders()
    {
        var header = File.ReadAllText(NativeSource("src/wem/WemVorbisDecoder.h"));
        Assert.Contains("kMaxWemBytes = (size_t) 256 << 20;", header);
        Assert.Equal(256 << 20, CapBytes);
    }

    private static string NativeSource(string rel, [CallerFilePath] string? thisFile = null)
    {
        var path = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(thisFile!)!, "..", "..", "..",
            "src", "JuceSharp.Native", rel));
        Assert.True(File.Exists(path), path);
        return path;
    }
}
