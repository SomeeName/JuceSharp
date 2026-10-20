using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using JuceSharp.AudioIo;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.AudioIo;

/// <summary>
/// The JUCE 9 port (2026-09-28) is a PURE port, and JUCE 9 changed what
/// <c>AudioFormatManager::registerBasicFormats()</c> registers: 9.0.2 turned the
/// software <c>MP3AudioFormat</c> on by default and 9.0.3 added
/// <c>OpusAudioFormat</c>, both registered AHEAD of the Windows Media reader that
/// decoded every <c>.mp3</c> on 8.x. <c>CMakeLists.txt</c> pins
/// <c>JUCE_USE_MP3AUDIOFORMAT=0</c> and <c>JUCE_USE_OPUS=0</c> so the engine
/// opens the same files with the same decoders. Native Opus is a later,
/// deliberate change — when it lands, THIS is the file it edits.
/// </summary>
public sealed class JuceFormatSetTests
{
    /// <summary>
    /// A 60 ms, 48 kHz mono Ogg Opus file (ffmpeg <c>libopus</c> 12 kb/s, 319 bytes;
    /// <c>ffprobe</c>: opus, 48000, 1). Valid Opus, so the only reason the engine
    /// refuses it is that no Opus reader is registered.
    /// </summary>
    private const string TinyOggOpusBase64 =
        "T2dnUwACAAAAAAAAAAD9ZUDbAAAAACO29dMBE09wdXNIZWFkAQE4AYC7AAAAAABPZ2dTAAAAAAAAAAAAAP1lQNsBAAAAs7UzDQE8T3B1c1RhZ3MMAAAATGF2ZjYzLjEuMTAxAQAAABwAAABlbmNvZGVyPUxhdmM2My4xLjEwMSBsaWJvcHVzT2dnUwAEeAwAAAAAAAD9ZUDbAgAAAEuHXbwEMykpFEiCLrdsVrf0AAHlzZ3tGqbpBeoVkGdipAuSm1xMlTcQQ+vIZSzw/IMw2IfbWQidfiQSoEikiFesmIUDV0wCHxk5tJ5ByVjBmy/HmYiCH7aUyVrlQbOk2w90epXgSJwbUk/ZyaE+UJ3WIZ2RGBkU9SRFCYwBRiEEvqw/2OwozHJQTvN43ihIBldmgTFI0nrpla16f73QWZ7AlA==";

    [NativeAvailableFact]
    public void An_opus_file_is_not_decoded_natively_yet()
    {
        var path = Path.Combine(Path.GetTempPath(), $"jucesharp-format-{Guid.NewGuid():N}.opus");
        File.WriteAllBytes(path, Convert.FromBase64String(TinyOggOpusBase64));
        try
        {
            // dca_decode_audio_file answers DCA_ERR_DECODE when no registered
            // format claims the file, which the decoder maps to InvalidDataException.
            Assert.Throws<InvalidDataException>(() => JuceAudioDecoder.Probe(path));
        }
        finally
        {
            File.Delete(path);
        }
    }

    /// <summary>
    /// Reads the built DLL's own bytes for each format's display name — the
    /// string a reader passes to <c>AudioFormatReader</c>, present only if that
    /// codec was compiled in. Ogg Vorbis and FLAC must be there (the scan is
    /// real), Windows Media must be there (it is what reads <c>.mp3</c>), and
    /// JUCE 9's software MP3 and Opus readers must not.
    /// </summary>
    [NativeAvailableFact]
    public void The_dll_compiles_the_same_codecs_as_the_JUCE_8_build()
    {
        if (!OperatingSystem.IsWindows())
            Assert.Skip("The Windows Media reader, and this binary scan, are Windows-only.");

        var lib = NativeLibrary.Load(NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.NotEqual(nint.Zero, lib);
        var module = Process.GetCurrentProcess().Modules.Cast<ProcessModule>()
            .FirstOrDefault(m => string.Equals(Path.GetFileName(m.FileName), "juce_sharp.dll",
                                               StringComparison.OrdinalIgnoreCase));
        Assert.True(module is not null, "juce_sharp.dll is loaded but not in the process module list");
        var bytes = File.ReadAllBytes(module!.FileName);

        bool Has(string ascii) => bytes.AsSpan().IndexOf(Encoding.ASCII.GetBytes(ascii + "\0")) >= 0;

        Assert.True(Has("Ogg-Vorbis file"), "no Ogg Vorbis reader name in the DLL — the scan is vacuous");
        Assert.True(Has("FLAC file"), "no FLAC reader name in the DLL — the scan is vacuous");
        Assert.True(Has("Windows Media"),
            "the Windows Media reader is gone, so .mp3 no longer decodes the way it did on JUCE 8");
        Assert.False(Has("MP3 file"),
            "JUCE's software MP3AudioFormat is compiled in (JUCE_USE_MP3AUDIOFORMAT defaults to 1 "
            + "since 9.0.2) and would decode every .mp3 ahead of Windows Media — a different "
            + "decoder, a different length and first sample for every MP3 song");
        Assert.False(Has("Opus file"),
            "JUCE's OpusAudioFormat is compiled in (JUCE_USE_OPUS defaults to 1 since 9.0.3); "
            + "native Opus is its own change, not part of the port");
    }
}
