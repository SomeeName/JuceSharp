using System.Collections.Concurrent;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <c>AudioDeviceManager::getCurrentAudioDevice()</c> returns a raw pointer that
/// <c>setAudioDeviceSetup</c> DELETES — and both
/// <c>dca_engine_reconfigure</c> and <c>dca_engine_set_device_type</c> go
/// through it. The rate / block / latency getters, the xrun stats the managed
/// health timer polls, the source load thread and the offline rig's
/// "is a device live" check all dereferenced it from whatever thread they were
/// on, with nothing but a null check in the way.
/// <para>
/// The fix is a VALUE snapshot published on the device thread in
/// <c>audioDeviceAboutToStart</c> and cleared in <c>audioDeviceStopped</c>,
/// which JUCE calls before the device is torn down. The driver's own xrun
/// counter has no value form to cache, so it is sampled on the AUDIO thread —
/// which by construction cannot run outside the device session.
/// </para>
/// </summary>
[Collection("NullDevice")]
public sealed class DeviceSnapshotRaceTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>
    /// The live half: poll the getters from another thread while the device is
    /// closed and reopened underneath them. A use-after-free here is an access
    /// violation that takes the whole test host with it, so "the run finished"
    /// is a real assertion — and the value check makes it more than a smoke
    /// test, because a freed device reads as garbage rather than as one of the
    /// two configurations that actually existed.
    /// </summary>
    [NativeAvailableFact]
    public void PollingTheDeviceGetters_WhileReconfiguringLoops_NeverReadsAFreedDevice()
    {
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480);

        var stop = false;
        var polls = 0L;
        var bad = new ConcurrentQueue<string>();
        var legalRates = new HashSet<int> { 0, 48000, 44100 };
        var legalBlocks = new HashSet<int> { 0, 480, 256 };

        var poller = new Thread(() =>
        {
            while (!Volatile.Read(ref stop))
            {
                var sr = engine.SampleRate;
                var bf = engine.BufferFrames;
                var lat = engine.OutputLatencySamples;
                if (!legalRates.Contains(sr)) bad.Enqueue($"sample rate {sr}");
                if (!legalBlocks.Contains(bf)) bad.Enqueue($"buffer frames {bf}");
                if (lat < 0 || lat > 1 << 20) bad.Enqueue($"output latency {lat}");
                engine.TryGetRemoteBridgeStats(out _, out _, out _);
                Interlocked.Increment(ref polls);
            }
        }) { IsBackground = true };
        poller.Start();

        try
        {
            for (var i = 0; i < 12; i++)
                engine.Reconfigure(i % 2 == 0 ? 44100 : 48000, i % 2 == 0 ? 256 : 480, null, null);
        }
        finally
        {
            Volatile.Write(ref stop, true);
            Assert.True(poller.Join(TimeSpan.FromSeconds(10)), "the polling thread never finished");
        }

        Assert.True(Interlocked.Read(ref polls) > 100,
            $"only {Interlocked.Read(ref polls)} polls landed across 12 device rebuilds — "
            + "the race window was never actually opened");
        Assert.True(bad.IsEmpty,
            "a getter returned a value no configuration ever had: " + string.Join(", ", bad));
    }

    /// <summary>
    /// The structural half, and the one that fails on the old code: the getters
    /// must not name <c>getCurrentAudioDevice</c> at all. Only the sanctioned
    /// sites may — the ones that run on the message thread as part of opening
    /// or closing a device.
    /// </summary>
    [Fact]
    public void TheDeviceGetters_NoLongerDereferenceTheDevicePointer()
    {
        var abi = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/dotcharter_audio.cpp")));

        foreach (var export in new[]
                 {
                     "dca_engine_get_sample_rate",
                     "dca_engine_get_buffer_frames",
                     "dca_engine_get_output_latency_samples",
                 })
        {
            var body = BodyOf(abi, export);
            Assert.DoesNotContain("getCurrentAudioDevice", body, StringComparison.Ordinal);
            Assert.Contains("deviceSnapshot()", body, StringComparison.Ordinal);
        }

        var engine = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/AudioEngine.cpp")));
        foreach (var fn in new[] { "AudioEngine::fillXrunStats", "AudioEngine::resetXrunStats" })
            Assert.DoesNotContain("getCurrentAudioDevice", BodyOf(engine, fn), StringComparison.Ordinal);

        // Non-vacuity: the extractor finds real bodies, and a site that DOES
        // legitimately touch the device pointer still contains the call — so
        // "DoesNotContain" is not passing because nothing was extracted.
        Assert.Contains("deviceSnapshot", BodyOf(abi, "dca_engine_get_sample_rate"),
                        StringComparison.Ordinal);
        Assert.Contains("getCurrentAudioDevice", engine, StringComparison.Ordinal);
    }

    private static string BodyOf(string src, string name)
    {
        var at = src.IndexOf(name, StringComparison.Ordinal);
        Assert.True(at >= 0, $"{name} not found");
        var open = src.IndexOf('{', at);
        var depth = 0;
        for (var i = open; i < src.Length; i++)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0) return src.Substring(open, i - open + 1);
        }
        throw new InvalidDataException($"unbalanced braces after {name}");
    }

    private static string StripComments(string src)
    {
        src = Regex.Replace(src, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        return Regex.Replace(src, @"//[^\n]*", " ");
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
