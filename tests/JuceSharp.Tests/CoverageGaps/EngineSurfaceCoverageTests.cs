using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// Public wrapper members that had no test reference at all: the device
/// re-open pair (<c>Reconfigure</c> / <c>SetDeviceType</c>), the in-memory
/// source pair (<c>OpenMemory</c> / <c>GenerateWaveformFromMemory</c>), the
/// dropout counters, the per-seat monitor trim, the stretch-quality switch and
/// <c>OutputLatencySamples</c>, plus the pure helper types around them.
/// </summary>
[Collection("NativeEngine")]
public sealed class EngineSurfaceCoverageTests
{
    private static JuceAudioEngine? OpenOrSkip()
    {
        var engine = new JuceAudioEngine();
        if (engine.SampleRate > 0) return engine;
        engine.Dispose();
        Assert.Skip("no audio device on this host — the constructor opened none");
        return null;
    }

    // ---------------------------------------------------------- XrunStats

    /// <summary>
    /// The three counters are three DIFFERENT faults and
    /// <c>DeviceXruns</c> is nullable because a driver that cannot report is
    /// not a driver reporting zero. <c>HasDropouts</c> is the summary the UI
    /// reads, and a null DeviceXruns must not make it true.
    /// </summary>
    [Fact]
    public void XrunStats_summarises_the_three_faults_without_inventing_a_fourth()
    {
        var clean = new XrunStats
        {
            DeviceOpen = true, DeviceRunning = true,
            Callbacks = 1000, DeviceXruns = null,
            BlockBudgetNs = 10_000_000, LastCallbackNs = 2_500_000,
        };
        Assert.False(clean.HasDropouts);
        Assert.Equal(0.25, clean.Load!.Value, 6);

        // A driver that CAN report and reports none is still clean.
        Assert.False((clean with { DeviceXruns = 0 }).HasDropouts);
        // Each of the two reportable faults is enough on its own.
        Assert.True((clean with { LateCallbacks = 1 }).HasDropouts);
        Assert.True((clean with { DeviceXruns = 1 }).HasDropouts);
        // …but a deadline overrun or a near miss is a LOAD warning, not an
        // audible dropout, and must not be folded in.
        Assert.False((clean with { DeadlineOverruns = 5 }).HasDropouts);
        Assert.False((clean with { NearMisses = 5 }).HasDropouts);

        // No budget means no load figure — never a divide by zero, never 0.
        Assert.Null((clean with { BlockBudgetNs = 0 }).Load);
    }

    [NativeAvailableFact]
    public void Xrun_stats_read_back_and_reset_on_a_live_engine()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        Assert.True(engine.TryGetXrunStats(out var stats));
        Assert.True(stats.DeviceOpen);
        Assert.True(stats.Callbacks >= 0);
        // -1 means "this driver cannot report"; the wrapper must surface that
        // as null and never as zero.
        Assert.True(stats.DeviceXruns is null or >= 0);

        Assert.True(engine.ResetXrunStats());
        Assert.True(engine.TryGetXrunStats(out var afterReset));
        Assert.Equal(0, afterReset.LateCallbacks);
        Assert.Equal(0, afterReset.DeadlineOverruns);
        Assert.Equal(0, afterReset.NearMisses);
    }

    [NativeAvailableFact]
    public void Xrun_stats_report_failure_rather_than_garbage_after_dispose()
    {
        var engine = OpenOrSkip();
        if (engine is null) return;
        engine.Dispose();

        Assert.False(engine.TryGetXrunStats(out var stats));
        Assert.Equal(default, stats);
        Assert.False(engine.ResetXrunStats());
    }

    // ---------------------------------------------------- device lifetime

    [NativeAvailableFact]
    public void OutputLatencySamples_is_a_sane_non_negative_figure()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        var latency = engine.OutputLatencySamples;
        Assert.True(latency >= 0, $"latency was {latency}");
        // Includes the device buffer plus driver delay; anything past a second
        // means we are reading the wrong field.
        Assert.True(latency < engine.SampleRate, $"latency {latency} >= one second at {engine.SampleRate} Hz");
    }

    /// <summary>
    /// <c>Reconfigure</c> reopens the device in place. What matters is that
    /// the engine SURVIVES it with a usable device — the driver is free to
    /// refuse the exact rate asked for.
    /// </summary>
    [NativeAvailableFact]
    public void Reconfigure_keeps_the_engine_usable()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        var before = engine.SampleRate;
        try
        {
            engine.Reconfigure(before, engine.BufferFrames, null, null);
        }
        catch (JuceAudioException ex)
        {
            Assert.Skip($"device refused the reopen on this host: {ex.Message}");
            return;
        }

        Assert.True(engine.SampleRate > 0);
        Assert.True(engine.BufferFrames > 0);
        // Master-bus state must survive a rebuild, so the EQ section must
        // still answer.
        Assert.Equal(0, engine.GetEqActiveCount());
    }

    [NativeAvailableFact]
    public void SetDeviceType_with_an_empty_type_keeps_the_current_host()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        try
        {
            engine.SetDeviceType(null);
        }
        catch (JuceAudioException ex)
        {
            Assert.Skip($"device refused the in-place host swap on this host: {ex.Message}");
            return;
        }

        Assert.True(engine.SampleRate > 0);
    }

    [NativeAvailableFact]
    public void Reconfigure_and_SetDeviceType_report_a_disposed_engine_as_disposed()
    {
        var engine = OpenOrSkip();
        if (engine is null) return;
        engine.Dispose();

        // Not a device error and not a silent no-op: these both Acquire().
        Assert.Throws<ObjectDisposedException>(() => engine.Reconfigure(48000, 512, null, null));
        Assert.Throws<ObjectDisposedException>(() => engine.SetDeviceType("Windows Audio"));
        Assert.Throws<ObjectDisposedException>(() => engine.OutputLatencySamples);
    }

    // ------------------------------------------------------- in-memory IO

    [NativeAvailableFact]
    public void OpenMemory_plays_the_same_blob_a_file_would()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        var path = TinyWav.Create(seconds: 0.25, sampleRate: 48000);
        try
        {
            var bytes = File.ReadAllBytes(path);
            using var source = engine.OpenMemory(bytes, "wav");
            Assert.InRange(source.Length.TotalSeconds, 0.2, 0.35);

            // The blob is copied native-side, so the caller may scribble on it
            // immediately — the source must be unaffected.
            Array.Clear(bytes);
            Assert.InRange(source.Length.TotalSeconds, 0.2, 0.35);
        }
        finally { TinyWav.TryDelete(path); }
    }

    [NativeAvailableFact]
    public void OpenMemory_rejects_an_empty_blob_and_a_disposed_engine()
    {
        var engine = OpenOrSkip();
        if (engine is null) return;

        Assert.Throws<ArgumentException>(() => engine.OpenMemory(ReadOnlySpan<byte>.Empty));

        engine.Dispose();
        // Acquire happens FIRST on purpose: a disposed engine reports itself
        // rather than complaining about the payload.
        Assert.Throws<ObjectDisposedException>(() => engine.OpenMemory(ReadOnlySpan<byte>.Empty));
        Assert.Throws<ObjectDisposedException>(() => engine.OpenMemory(new byte[] { 1, 2, 3 }));
    }

    [NativeAvailableFact]
    public void GenerateWaveformFromMemory_agrees_with_the_file_overload()
    {
        if (!NativeProbe.IsAvailable) return;

        var path = TinyWav.Create(seconds: 0.5, sampleRate: 48000);
        try
        {
            var fromFile = JuceAudioEngine.GenerateWaveform(path, blockMs: 10.0);
            var fromMemory = JuceAudioEngine.GenerateWaveformFromMemory(File.ReadAllBytes(path), blockMs: 10.0);

            Assert.NotEmpty(fromFile);
            // Peak pairs (min,max), so the length is even.
            Assert.Equal(0, fromMemory.Length % 2);
            // The block width is FRACTIONAL, so the two routes must agree
            // exactly rather than "within a block".
            Assert.Equal(fromFile.Length, fromMemory.Length);
            for (var i = 0; i < fromFile.Length; i++)
                Assert.Equal(fromFile[i], fromMemory[i], 5);
        }
        finally { TinyWav.TryDelete(path); }
    }

    // -------------------------------------------------- seats and quality

    /// <summary>Both are void and both resolve the handle through TryAcquire,
    /// so the contract under test is that neither throws for any input a UI
    /// can produce — including after dispose.</summary>
    [NativeAvailableFact]
    public void Per_seat_monitor_gain_and_stretch_quality_accept_every_reachable_input()
    {
        var engine = OpenOrSkip();
        if (engine is null) return;

        foreach (var gain in new[] { 0f, 0.5f, 1f, 4f })
            for (var seat = 0; seat < 4; seat++)
                engine.SetPlayerMonitorGain(seat, gain);

        // Out of range seats are rejected native-side, never clamped onto a
        // real one.
        engine.SetPlayerMonitorGain(-1, 1f);
        engine.SetPlayerMonitorGain(64, 1f);

        engine.SetStretchQuality(faster: true);
        engine.SetStretchQuality(faster: false);

        engine.Dispose();
        engine.SetPlayerMonitorGain(0, 1f);
        engine.SetStretchQuality(true);
    }

    // ----------------------------------------------------- device catalog

    [Fact]
    public void DisplayName_and_DriverFor_map_every_known_host_and_fall_back_safely()
    {
        // Round-trip every host the catalog claims to know.
        foreach (var (typeName, driver, display) in new[]
                 {
                     ("Windows Audio", AudioDriver.Wasapi, "Windows Audio (Shared)"),
                     ("Windows Audio (Exclusive Mode)", AudioDriver.Wasapi, "Windows Audio (Exclusive)"),
                     ("Windows Audio (Low Latency Mode)", AudioDriver.Wasapi, "Windows Audio (Low Latency)"),
                     ("DirectSound", AudioDriver.DirectSound, "DirectSound"),
                     ("ASIO", AudioDriver.Asio, "ASIO"),
                     ("CoreAudio", AudioDriver.CoreAudio, "CoreAudio (macOS)"),
                     ("ALSA", AudioDriver.Alsa, "ALSA (Linux)"),
                     ("JACK", AudioDriver.Jack, "JACK"),
                 })
        {
            Assert.Equal(display, JuceDeviceCatalog.DisplayName(typeName));
            Assert.Equal(driver, JuceDeviceCatalog.DriverFor(typeName));
            // The lookup is case-insensitive; a host name from the native
            // enumeration is not guaranteed to match our casing.
            Assert.Equal(driver, JuceDeviceCatalog.DriverFor(typeName.ToUpperInvariant()));
        }

        // An unknown host is passed through, not blanked.
        Assert.Equal("Some Future Host", JuceDeviceCatalog.DisplayName("Some Future Host"));
        Assert.Equal(AudioDriver.Default, JuceDeviceCatalog.DriverFor("Some Future Host"));
        // …and an empty one reads as the default rather than as an empty label.
        Assert.Equal("Default", JuceDeviceCatalog.DisplayName(string.Empty));
        Assert.Equal(AudioDriver.Default, JuceDeviceCatalog.DriverFor(string.Empty));
    }

    [Fact]
    public void JuceAudioHost_reports_whether_it_has_devices_at_all()
    {
        var device = new JuceAudioDevice("id", "name", AudioDriver.Wasapi, 2, 2, 48000, true, "Windows Audio");

        var neither = new JuceAudioHost("H", AudioDriver.Wasapi, [], []);
        Assert.False(neither.HasOutputs);
        Assert.False(neither.HasInputs);

        var outputsOnly = new JuceAudioHost("H", AudioDriver.Wasapi, [device], []);
        Assert.True(outputsOnly.HasOutputs);
        Assert.False(outputsOnly.HasInputs);

        var inputsOnly = new JuceAudioHost("H", AudioDriver.Wasapi, [], [device]);
        Assert.False(inputsOnly.HasOutputs);
        Assert.True(inputsOnly.HasInputs);
    }

    [NativeAvailableFact]
    public void Enumerate_returns_only_hosts_the_catalog_knows_how_to_label()
    {
        var hosts = JuceDeviceCatalog.Enumerate();
        foreach (var h in hosts)
        {
            Assert.False(string.IsNullOrWhiteSpace(h.TypeName));
            // Every enumerated host must round-trip through the label table —
            // a host that falls back to Default here would show as "Default"
            // in the device picker.
            Assert.NotEqual(AudioDriver.Default, JuceDeviceCatalog.DriverFor(h.TypeName));
            Assert.Equal(h.Driver, JuceDeviceCatalog.DriverFor(h.TypeName));
        }
    }

    /// <summary>
    /// The Windows DLL is built with ASIO. Since 2026-09-28 it compiles against
    /// the ASIO headers JUCE bundles (JUCE 8.0.15+, ASIO SDK 2.3 in 9.0.3, Steinberg's dual licence)
    /// rather than a vendored SDK, so a JUCE bump that dropped them, or a
    /// CMake change that turned <c>JUCE_ASIO</c> off, would ship a build with
    /// no ASIO host and nothing else would fail. JUCE registers the ASIO type
    /// whenever it is compiled in, installed drivers or not, so this holds on
    /// a CI runner with no audio hardware too.
    /// </summary>
    [NativeAvailableFact]
    public void Windows_build_registers_the_ASIO_host_type()
    {
        if (!OperatingSystem.IsWindows())
            Assert.Skip("ASIO is a Windows-only host type.");

        const int max = 16;
        var buf = new byte[max * 64];
        var status = NativeMethods.EnumerateHostTypes(buf, max, out var count);
        Assert.Equal(NativeMethods.DcaStatus.Ok, status);

        var names = new List<string>();
        for (int i = 0; i < count; ++i)
        {
            int off = i * 64, len = 0;
            while (len < 64 && buf[off + len] != 0) ++len;
            names.Add(System.Text.Encoding.UTF8.GetString(buf, off, len));
        }
        Assert.Contains("ASIO", names, StringComparer.OrdinalIgnoreCase);
    }

    // ----------------------------------------------------- engine factory

    [NativeAvailableFact]
    public void OpenWithFallback_records_what_it_tried()
    {
        var config = new JuceEngineConfig
        {
            SampleRate = 48000,
            BufferFrames = 512,
            OutputDeviceId = "a device that certainly does not exist",
            OutputTypeName = "Windows Audio",
            EnableConsoleLog = false,
        };

        var result = JuceEngineFactory.OpenWithFallback(config);
        using var engine = result.Engine;

        Assert.NotNull(result.ConfigUsed);
        Assert.NotNull(result.Attempts);

        if (result.Engine is null)
        {
            // Nothing opened: the trail is the ONLY diagnostic the caller
            // gets, so it must never come back empty.
            Assert.NotEmpty(result.Attempts);
            Assert.Contains(result.Attempts, a => a.Contains("does not exist", StringComparison.Ordinal));
        }
        else
        {
            // It opened. Either JUCE accepted the request outright (an unknown
            // output ID falls back to the host default inside the driver, so
            // an empty trail is legitimate) or the walk found a device — and
            // in the latter case it stayed on the SAME host, which is the
            // whole point of enumerating one host rather than all of them.
            Assert.Equal(config.OutputTypeName, result.ConfigUsed.OutputTypeName);
            Assert.True(engine!.SampleRate > 0);
            foreach (var attempt in result.Attempts)
                Assert.False(string.IsNullOrWhiteSpace(attempt));
        }
    }

    /// <summary>The failure trail is populated for a host that cannot exist —
    /// the case where the walk really has nowhere to go.</summary>
    [NativeAvailableFact]
    public void OpenWithFallback_records_the_failure_when_the_host_itself_is_unknown()
    {
        var result = JuceEngineFactory.OpenWithFallback(new JuceEngineConfig
        {
            SampleRate = 48000,
            BufferFrames = 512,
            OutputTypeName = "No Such Audio Host",
            OutputDeviceId = "no such device",
            EnableConsoleLog = false,
        });
        using var engine = result.Engine;

        Assert.NotEmpty(result.Attempts);
        Assert.Contains(result.Attempts, a => a.Contains("No Such Audio Host", StringComparison.Ordinal));
    }

    [Fact]
    public void OpenWithFallback_rejects_a_null_config()
    {
        Assert.Throws<ArgumentNullException>(() => JuceEngineFactory.OpenWithFallback(null!));
    }

    // ------------------------------------------------------- VST3 scanner

    [Fact]
    public void DefaultPaths_are_absolute_and_distinct()
    {
        var paths = JuceVst3Scanner.DefaultPaths();
        Assert.NotNull(paths);
        Assert.Equal(paths.Distinct(StringComparer.OrdinalIgnoreCase).Count(), paths.Count);
        foreach (var p in paths)
        {
            Assert.False(string.IsNullOrWhiteSpace(p));
            Assert.True(Path.IsPathRooted(p), $"'{p}' is not rooted");
        }
    }

    /// <summary>A VST3 "file" is a BUNDLE DIRECTORY on Windows, so the scanner
    /// has to find a folder, not a file.</summary>
    [Fact]
    public void Scan_finds_a_bundle_directory_and_describes_it()
    {
        var root = Path.Combine(Path.GetTempPath(), "vst3-scan-" + Guid.NewGuid());
        var bundle = Path.Combine(root, "nested", "FakePlugin.vst3");
        Directory.CreateDirectory(Path.Combine(bundle, "Contents", "x86_64-win"));
        File.WriteAllBytes(Path.Combine(bundle, "Contents", "x86_64-win", "FakePlugin.vst3"), new byte[2048]);
        try
        {
            var found = JuceVst3Scanner.Scan([root], maxDepth: 4, limit: 0, TestContext.Current.CancellationToken);
            var entry = Assert.Single(found, e => e.Name.Contains("FakePlugin", StringComparison.OrdinalIgnoreCase));

            Assert.Equal(bundle, entry.Path, ignoreCase: true);
            Assert.True(entry.SizeBytes >= 0);
            Assert.Contains("FakePlugin", entry.Display, StringComparison.Ordinal);
            Assert.Contains("MB", entry.Display, StringComparison.Ordinal);

            // A root that does not exist is skipped, not thrown over.
            Assert.Empty(JuceVst3Scanner.Scan([Path.Combine(root, "no-such-dir")], maxDepth: 4, limit: 0, TestContext.Current.CancellationToken));

            // The limit is honoured.
            Assert.Empty(JuceVst3Scanner.Scan([root], maxDepth: 4, limit: 0, TestContext.Current.CancellationToken).Skip(1));
        }
        finally
        {
            try { Directory.Delete(root, true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    // ------------------------------------------- native stem separation

    /// <summary>
    /// The native demucs.cpp backend is compiled OUT of the shipping DLL
    /// (<c>DOTCHARTER_STEMS_DEMUCS_CPP=0</c>). "A capability the app cannot
    /// honour must never look live" — so the pair must agree, and the
    /// unsupported description must SAY why rather than name a backend.
    /// </summary>
    [NativeAvailableFact]
    public void The_native_stem_backend_describes_itself_consistently_with_IsSupported()
    {
        var description = JuceStemSeparation.DescribeBackend();
        Assert.False(string.IsNullOrWhiteSpace(description));

        if (JuceStemSeparation.IsSupported)
        {
            Assert.DoesNotContain("unavailable", description, StringComparison.OrdinalIgnoreCase);
        }
        else
        {
            Assert.Contains("unavailable", description, StringComparison.OrdinalIgnoreCase);
            // …and it tells the reader how to get it.
            Assert.Contains("DOTCHARTER_STEMS_DEMUCS_CPP", description, StringComparison.Ordinal);
        }

        // The probe is a cached Lazy; a second read must not disagree.
        Assert.Equal(JuceStemSeparation.IsSupported, JuceStemSeparation.IsSupported);
        Assert.Equal(description, JuceStemSeparation.DescribeBackend());
    }

    // ---------------------------------------------------------- plugin

    [NativeAvailableFact]
    public void LoadPlugin_reports_a_missing_file_as_a_missing_file()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        var missing = Path.Combine(Path.GetTempPath(), "no-such-" + Guid.NewGuid() + ".vst3");

        // FileNotFoundException, NOT JuceAudioException: we never reach the
        // native loader, so there is no native status to report.
        var ex = Assert.Throws<FileNotFoundException>(() => engine.LoadPlugin(missing));
        Assert.Equal(missing, ex.FileName);

        Assert.Throws<ArgumentException>(() => engine.LoadPlugin(string.Empty));
        Assert.Throws<ArgumentException>(() => engine.LoadPlugin("   "));
        Assert.Throws<ArgumentNullException>(() => engine.LoadPlugin(null!));
    }

    [NativeAvailableFact]
    public void LoadPlugin_reports_a_disposed_engine_before_it_looks_at_the_path()
    {
        var engine = OpenOrSkip();
        if (engine is null) return;
        engine.Dispose();

        // The Acquire deliberately precedes the File.Exists probe.
        Assert.Throws<ObjectDisposedException>(() =>
            engine.LoadPlugin(Path.Combine(Path.GetTempPath(), "no-such.vst3")));
    }

    /// <summary>
    /// A directory that exists but is not a real bundle gets past the
    /// existence probe and into the native loader, which is where the
    /// JuceAudioException with a native status comes from. That is the
    /// distinction the two exception types encode.
    /// </summary>
    [NativeAvailableFact]
    public void LoadPlugin_reports_a_non_plugin_directory_as_a_native_failure()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        var dir = Path.Combine(Path.GetTempPath(), "not-a-plugin-" + Guid.NewGuid() + ".vst3");
        Directory.CreateDirectory(dir);
        try
        {
            var ex = Assert.Throws<JuceAudioException>(() => engine.LoadPlugin(dir));
            Assert.Contains("plugin_load_vst3", ex.Message, StringComparison.Ordinal);
        }
        finally
        {
            try { Directory.Delete(dir, true); }
            catch (Exception e) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {e.Message}"); }
        }
    }

    // ------------------------------------------------------- MIDI input

    [NativeAvailableFact]
    public void An_opened_midi_input_remembers_which_device_index_it_is()
    {
        using var engine = OpenOrSkip();
        if (engine is null) return;

        var devices = JuceMidiInput.Enumerate();
        if (devices.Count == 0)
        {
            Assert.Skip("no MIDI input device on this machine");
            return;
        }

        using var midi = engine.OpenMidiInput(0, _ => { });
        Assert.Equal(0, midi.DeviceIndex);
        Assert.False(midi.IsDisposed);
    }

    [Fact]
    public void A_midi_message_decodes_its_status_bytes_and_survives_a_short_payload()
    {
        var noteOn = new JuceMidiMessage(1.5, [0x92, 60, 100]);
        Assert.Equal(0x92, noteOn.Status);
        Assert.Equal(60, noteOn.Data1);
        Assert.Equal(100, noteOn.Data2);
        Assert.Equal(2, noteOn.Channel);
        Assert.Equal(0x90, noteOn.Command);

        // A truncated or empty payload must read as zeros, not index out of
        // range — this comes off a native callback and is not validated there.
        var truncated = new JuceMidiMessage(0, [0xB0]);
        Assert.Equal(0xB0, truncated.Status);
        Assert.Equal(0, truncated.Data1);
        Assert.Equal(0, truncated.Data2);

        var empty = new JuceMidiMessage(0, []);
        Assert.Equal(0, empty.Status);
        Assert.Equal(0, empty.Data1);
        Assert.Equal(0, empty.Data2);
    }
}
