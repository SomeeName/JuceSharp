using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Microsoft.Extensions.Logging;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The managed-wrapper audit's LIFETIME + CORRECTNESS findings, one test each.
/// Every one of these guards a bug that compiled cleanly and passed every test
/// that existed at the time, so each says out loud what breaking it looks like.
/// </summary>
[Collection("NullDevice")]
public sealed class WrapperLifetimeAuditTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    // ---------------------------------------------------------------- M1

    /// <summary>
    ///     The "child destruction did not complete ⇒ LEAK the engine" exit
    ///     returned WITHOUT decrementing the process-wide log-callback refcount.
    ///     That exit is not exotic: it is the NORMAL finalizer outcome, because
    ///     finalization order is undefined and <c>DisposeChildren(false)</c> only
    ///     CHECKS completion tokens. So every finalized engine leaked one
    ///     registration, <c>s_logRegistrations</c> never fell back to 0 again, and
    ///     <c>dca_set_log_callback(NULL)</c> was never issued for the process.
    ///     <para>
    ///         Driven deterministically down the finalizer path — a live tracked
    ///         child makes <c>DisposeChildren(false)</c> report incomplete every
    ///         time, with no GC timing in the assertion.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void EngineLeakedByIncompleteChildTeardown_StillUnregistersItsLogCallback()
    {
        var wav = TinyWav.Create();
        var engine = NullDevice.CreateEngine();
        _ = engine.OpenFile(wav); // tracked, and deliberately never disposed
        Assert.True(JuceAudioEngine.IsRegisteredForNativeLog(engine));

        // The finalizer's exact call, without waiting for a GC.
        InvokeDispose(engine, disposing: false);

        Assert.False(JuceAudioEngine.IsRegisteredForNativeLog(engine),
            "a finalized engine whose children had not completed kept its share of the process-wide "
            + "log registration, so dca_set_log_callback(NULL) is never issued again for anybody");
    }

    [NativeAvailableFact]
    public void OrdinaryDispose_UnregistersItsLogCallback()
    {
        var engine = NullDevice.CreateEngine();
        Assert.True(JuceAudioEngine.IsRegisteredForNativeLog(engine));
        engine.Dispose();
        Assert.False(JuceAudioEngine.IsRegisteredForNativeLog(engine));
    }

    private static void InvokeDispose(JuceAudioEngine engine, bool disposing)
    {
        var m = typeof(JuceAudioEngine).GetMethod("Dispose",
            BindingFlags.NonPublic | BindingFlags.Instance, [typeof(bool)]);
        Assert.NotNull(m);
        m.Invoke(engine, [disposing]);
    }

    // ---------------------------------------------------------------- M2

    /// <summary>
    ///     Every per-seat capture / peak read used to DISCARD the status and hand
    ///     back <c>written</c> / <c>peak</c> untouched. For a seat index the engine
    ///     rejects, that is 0 — indistinguishable from "the ring is empty", i.e. a
    ///     guitar that is plugged in and silent with nothing anywhere saying why.
    ///     The native contract rejects such a seat rather than clamping it (clamping
    ///     would judge somebody else's signal), and so does the wrapper.
    /// </summary>
    [NativeAvailableTheory]
    [InlineData(-1)]
    [InlineData(NativeMethodsAudit.MaxPlayersMirror)]
    [InlineData(9999)]
    public void PerSeatReads_RejectASeatTheEngineDoesNotHave(int seat)
    {
        using var engine = NullDevice.CreateEngine();
        var buf = new float[16];

        Assert.Throws<ArgumentOutOfRangeException>(() => engine.GetAndResetPlayerInputPeak(seat));
        Assert.Throws<ArgumentOutOfRangeException>(() => engine.ReadInputCapture(seat, buf));
        Assert.Throws<ArgumentOutOfRangeException>(() => engine.ReadInputCaptureStream(seat, buf));
        Assert.Throws<ArgumentOutOfRangeException>(() => engine.ReadInputCaptureStream(seat, buf, out _));
        Assert.Throws<ArgumentOutOfRangeException>(() => engine.TryGetInputCaptureSync(seat, out _, out _));
    }

    [NativeAvailableFact]
    public void PerSeatReads_AcceptEverySeatTheEngineDoesHave()
    {
        using var engine = NullDevice.CreateEngine(inputChannels: 1);
        var buf = new float[16];
        for (var seat = 0; seat < JuceAudioEngine.MaxPlayers; seat++)
        {
            _ = engine.GetAndResetPlayerInputPeak(seat);
            _ = engine.ReadInputCapture(seat, buf);
            _ = engine.ReadInputCaptureStream(seat, buf);
            _ = engine.ReadInputCaptureStream(seat, buf, out _);
            _ = engine.TryGetInputCaptureSync(seat, out _, out _);
        }
    }

    // ---------------------------------------------------------------- M11

    /// <summary>
    ///     A fire-and-forget setter must not throw, but it must not be SILENT
    ///     either: a persistently failing knob used to be invisible. Driven with a
    ///     band TYPE the native side rejects, so the failure is real rather than
    ///     mocked.
    ///     <para>
    ///         It used to drive an out-of-range band INDEX, which W4-C4 made a
    ///         thrown <see cref="ArgumentOutOfRangeException" /> — a caller bug the
    ///         wrapper now refuses rather than forwards. The type is still the
    ///         engine's to reject, so the fire-and-forget contract is what this
    ///         case measures either way.
    ///     </para>
    /// </summary>
    [NativeAvailableFact]
    public void AFailedFireAndForgetSetter_IsReported()
    {
        var log = new CapturingLogger();
        using var engine = new JuceAudioEngine(new JuceEngineConfig
        {
            OutputTypeName = NullDevice.TypeName, SampleRate = 48000, BufferFrames = 480,
        }, log);

        engine.SetEqBand(index: 0, type: 99, freqHz: 1000f, q: 1f, gainDb: 0f, enabled: true);

        Assert.Contains(log.Records, r =>
            r.Level == LogLevel.Warning && r.Message.Contains("engine_set_eq_band", StringComparison.Ordinal));
    }

    // ---------------------------------------------------------------- M-12

    /// <summary>
    ///     One contract for the stem-group setters, stated and pinned: they no-op
    ///     when the engine is gone, never throw, and the getter always reports the
    ///     value last REQUESTED. They used to <c>Check</c> (throw) on a native
    ///     failure while silently no-opping when disposed — two contracts in one
    ///     property, which is why every DotCharter call site wraps them in a bare
    ///     <c>try/catch</c>.
    /// </summary>
    [NativeAvailableFact]
    public void StemGroupSetters_AreFireAndForget_AndTheGetterReportsTheRequest()
    {
        var engine = NullDevice.CreateEngine();
        engine.Dispose();

        engine.StemGroupTempo = 0.75f;             // no throw on a disposed engine
        engine.StemGroupPitchSemitones = -2.0f;

        Assert.Equal(0.75f, engine.StemGroupTempo);
        Assert.Equal(-2.0f, engine.StemGroupPitchSemitones);
    }

    // ---------------------------------------------------------------- M20

    [Fact]
    public void AbiMismatch_IsItsOwnTypeButStillAnInvalidOperationException()
    {
        var ex = (JuceAbiMismatchException)Activator.CreateInstance(
            typeof(JuceAbiMismatchException),
            BindingFlags.Instance | BindingFlags.NonPublic, null, [7u, 3u], null)!;

        Assert.IsAssignableFrom<InvalidOperationException>(ex);
        Assert.Equal(7u, ex.ExpectedAbiVersion);
        Assert.Equal(3u, ex.NativeAbiVersion);
    }

    [Fact]
    public void JuceAudioException_CanBeBuiltFromOutsideTheWrapper()
    {
        var ex = new JuceAudioException(JuceAudioStatus.Decode, "dca_stems_separate_file");
        Assert.Equal(JuceAudioStatus.Decode, ex.Status);
        Assert.Equal((int)JuceAudioStatus.Decode, ex.StatusCode);
        Assert.Equal("dca_stems_separate_file", ex.Operation);
        Assert.Contains("Decode", ex.Message, StringComparison.Ordinal);
    }

    // ---------------------------------------------------------------- M5

    /// <summary>
    ///     The nine raw-nint plugin members are GONE. Eight were <c>[Obsolete]</c>;
    ///     the ninth, <c>GetPluginLatencySamples</c>, never was — it took a raw
    ///     handle, skipped <see cref="JuceAudioPlugin" />'s drain and duplicated
    ///     <c>JuceAudioPlugin.LatencySamples</c>, so an unmarked raw-handle door
    ///     stood next to eight marked ones and read as the supported way in.
    /// </summary>
    [Theory]
    [InlineData("LoadVst3")]
    [InlineData("UnloadPlugin")]
    [InlineData("AttachPluginToInput")]
    [InlineData("DetachPluginFromInput")]
    [InlineData("ShowPluginEditor")]
    [InlineData("HidePluginEditor")]
    [InlineData("AttachPluginToInstrument")]
    [InlineData("DetachPluginFromInstrument")]
    [InlineData("GetPluginLatencySamples")]
    public void TheRawNintPluginSurface_IsGone(string member)
        => Assert.Empty(typeof(JuceAudioEngine).GetMember(member, BindingFlags.Public | BindingFlags.Instance));

    // ---------------------------------------------------------------- M17

    [Fact]
    public void DeviceProjection_HasExactlyOneImplementation_AndItGuardsAnUnknownDriver()
    {
        var info = default(NativeMethods.DcaDeviceInfo);
        info.Id = "id"; info.Name = "name"; info.TypeName = "ASIO";
        info.MaxInputChannels = 2; info.MaxOutputChannels = 8;
        info.DefaultSampleRate = 96000; info.IsDefault = 1;
        info.Driver = NativeMethods.DcaDriver.Asio;

        var mapped = JuceAudioDevice.FromNative(info);
        Assert.Equal("id", mapped.Id);
        Assert.Equal("name", mapped.Name);
        Assert.Equal(AudioDriver.Asio, mapped.Driver);
        Assert.Equal(2, mapped.MaxInputChannels);
        Assert.Equal(8, mapped.MaxOutputChannels);
        Assert.Equal(96000, mapped.DefaultSampleRate);
        Assert.True(mapped.IsDefault);
        Assert.Equal("ASIO", mapped.TypeName);

        // A driver ordinal a newer DLL invented must not become an AudioDriver
        // outside the enum — every switch over it would fall through.
        info.Driver = (NativeMethods.DcaDriver)9999;
        Assert.Equal(AudioDriver.Default, JuceAudioDevice.FromNative(info).Driver);

        // …and the host-scoped path still wins over the row's own field.
        Assert.Equal(AudioDriver.Jack, JuceAudioDevice.FromNative(info, AudioDriver.Jack).Driver);
    }

    // ---------------------------------------------------------------- M12

    /// <summary>
    ///     The MIDI callback allocated a <c>byte[]</c> per message on the JUCE MIDI
    ///     thread. A pedal held under a chord or one CC sweep is hundreds a second
    ///     straight into gen0 from a real-time driver thread. The span overload
    ///     borrows the native buffer in place; the record overload keeps its copy.
    ///     <para>Driven through the real private callback on an uninitialized
    ///     instance, so no MIDI hardware is needed to prove the path.</para>
    /// </summary>
    [Fact]
    public unsafe void TheSpanMidiHandler_SeesTheBytesWithoutAllocatingAPerMessageArray()
    {
        var input = (JuceMidiInput)RuntimeHelpers.GetUninitializedObject(typeof(JuceMidiInput));
        GC.SuppressFinalize(input);

        double seenTime = 0;
        byte[]? seen = null;
        JuceMidiSpanHandler handler = (t, d) => { seenTime = t; seen = d.ToArray(); };
        typeof(JuceMidiInput).GetField("_spanCallback", BindingFlags.NonPublic | BindingFlags.Instance)!
            .SetValue(input, handler);
        // OnMidi PINS the handle for its body (W4-M1), so a detached instance
        // needs an owner or the dispatch correctly returns without calling the
        // handler — which is the whole point of the fix, and is asserted on its
        // own in MidiInputDispatchDrainTests.
        typeof(JuceMidiInput).GetField("_owner", BindingFlags.NonPublic | BindingFlags.Instance)!
            .SetValue(input, new NativeHandleOwner(0x4D1D1));

        var packet = stackalloc byte[3];
        packet[0] = 0x90; packet[1] = 60; packet[2] = 100;

        var onMidi = typeof(JuceMidiInput).GetMethod("OnMidi", BindingFlags.NonPublic | BindingFlags.Instance)!;
        var before = GC.GetAllocatedBytesForCurrentThread();
        onMidi.Invoke(input, [nint.Zero, 1.5, (nint)packet, 3]);
        var after = GC.GetAllocatedBytesForCurrentThread();

        Assert.Equal(1.5, seenTime);
        Assert.Equal(new byte[] { 0x90, 60, 100 }, seen);
        // The handler above deliberately copies (ToArray), so this only says the
        // wrapper itself did not add a second array on top — reflection dispatch
        // dominates the number, hence the generous ceiling.
        Assert.True(after - before < 4096, $"allocated {after - before} bytes for one 3-byte MIDI packet");
    }

    [Fact]
    public void TheSpanMidiOverload_IsOnTheEngine()
    {
        var m = typeof(JuceAudioEngine).GetMethod(nameof(JuceAudioEngine.OpenMidiInput),
            [typeof(int), typeof(JuceMidiSpanHandler)]);
        Assert.NotNull(m);
        Assert.Equal(typeof(JuceMidiInput), m.ReturnType);
    }

    // ---------------------------------------------------------------- M8 / M13 source lints

    /// <summary>
    ///     <b>The finalizer may not wait.</b> One finalizer thread serves the whole
    ///     process, so draining 60 s here does not stall this wrapper — it stalls
    ///     every finalizer queued behind it. All three handle owners must pass a
    ///     0 ms deadline on the <c>!disposing</c> path (leak + log, which is the
    ///     trade the drain already makes on expiry). A source lint because the
    ///     alternative is a test that waits on a real GC.
    /// </summary>
    [Theory]
    [InlineData("src/JuceSharp/JuceAudioEngine.cs")]
    [InlineData("src/JuceSharp/JuceAudioPlugin.cs")]
    [InlineData("src/JuceSharp/JuceAudioSource.cs")]
    [InlineData("src/JuceSharp/JuceMidiInput.cs")]
    [InlineData("src/JuceSharp/JuceMidiOutput.cs")]
    public void EveryFinalizerPath_DrainsWithADeadlineOfZero(string relative)
    {
        var text = ReadRepoFile(relative);
        Assert.Contains("disposing ? NativeHandleOwner.DefaultDrainTimeoutMs : 0", text, StringComparison.Ordinal);
        // …and no call left on the single-argument overload, which silently
        // means the 60 s default on BOTH paths.
        Assert.DoesNotMatch(new Regex(@"TakeForDispose\(nameof\(\w+\)\)"), text);
    }

    /// <summary>
    ///     A constructor that can throw must <c>GC.SuppressFinalize(this)</c>
    ///     first. The CLR queues the finalizer the moment the allocation succeeds,
    ///     and a throwing finalizer is a process-wide FailFast — the shape that made
    ///     a PASSING <see cref="JuceAudioPlugin" /> unit test kill the whole test
    ///     host hundreds of tests later. <see cref="JuceAudioPlugin" /> got the fix;
    ///     the engine and the MIDI input did not, and both have throwing ctors
    ///     (<c>Validate</c>, <c>VerifyAbi</c>, <c>engine_create</c>,
    ///     <c>midi_input_open</c>).
    /// </summary>
    [Theory]
    [InlineData("src/JuceSharp/JuceAudioEngine.cs")]
    [InlineData("src/JuceSharp/JuceAudioPlugin.cs")]
    [InlineData("src/JuceSharp/JuceMidiInput.cs")]
    [InlineData("src/JuceSharp/JuceMidiOutput.cs")]
    public void EveryFinalizableWrapperCtor_SuppressesFinalizationBeforeItThrows(string relative)
    {
        var text = ReadRepoFile(relative);
        Assert.Matches(new Regex(@"GC\.SuppressFinalize\(this\);\s*\r?\n\s*throw", RegexOptions.Multiline), text);
    }

    /// <summary>
    ///     <b>Every handle-owning wrapper owns its handle through
    ///     <c>NativeHandleOwner</c>, so Dispose DRAINS in-flight P/Invokes before
    ///     the native destroy.</b> Two did not until the wave-3 audit, and they
    ///     were the two that looked like an exception rather than a gap:
    ///     <c>JuceMidiInput</c> (wave-2 #25, left open) and then
    ///     <c>JuceMidiOutput</c>, written the same way because the input one was
    ///     there to copy. The output is the dangerous half — it makes live native
    ///     calls (<c>Send</c>, <c>SendAt</c>, the counters) and
    ///     <c>dca_midi_output_close</c> was a bare <c>delete</c> that joins the
    ///     dispatcher thread and destroys the <c>juce::MidiOutput</c>, so a
    ///     sequencer thread flushing note-offs while the UI tears the engine down
    ///     — the ORDINARY shutdown path — was a native use-after-free rather than
    ///     a caught <see cref="ObjectDisposedException" />.
    ///     <para>
    ///         A source lint for the same reason the sibling above is one: the
    ///         alternative needs a real MIDI device, and CI has none.
    ///     </para>
    /// </summary>
    [Theory]
    [InlineData("src/JuceSharp/JuceAudioEngine.cs")]
    [InlineData("src/JuceSharp/JuceAudioPlugin.cs")]
    [InlineData("src/JuceSharp/JuceAudioSource.cs")]
    [InlineData("src/JuceSharp/JuceMidiInput.cs")]
    [InlineData("src/JuceSharp/JuceMidiOutput.cs")]
    public void EveryHandleOwningWrapper_DrainsThroughNativeHandleOwner(string relative)
    {
        var text = ReadRepoFile(relative);
        Assert.Contains("NativeHandleOwner? _owner", text, StringComparison.Ordinal);
        Assert.Contains("TakeForDispose(", text, StringComparison.Ordinal);
        // …and no raw handle field left behind for a member to read into a local
        // and then P/Invoke with, which is the shape the drain exists to remove.
        Assert.DoesNotMatch(new Regex(@"private\s+nint\s+_handle\s*;"), text);
    }

    /// <summary>
    ///     <b>The three assertions above all passed on a drain that could not
    ///     drain anything</b> (W4-M1): <see cref="JuceMidiInput" /> had the owner
    ///     field, called <c>TakeForDispose</c>, kept no raw handle — and no path
    ///     on the type incremented the in-flight count after construction, so the
    ///     close never waited for the JUCE driver thread inside <c>OnMidi</c>.
    ///     That is the "a guard nobody has seen fail is a guard nobody should
    ///     trust" shape, and it is why the real pin for that finding is
    ///     BEHAVIOURAL — <c>MidiInputDispatchDrainTests</c>, which drives a
    ///     detached instance's callback and asserts the drain blocks.
    ///     <para>
    ///         This is the cheap structural backstop for the same thing, and the
    ///         difference that matters is that it reads the CALLBACK'S BODY
    ///         rather than the file: a native-invoked entry point on a
    ///         handle-owning wrapper must pin the handle for its whole body, and
    ///         a substring anywhere in a 200-line file does not say that. One row
    ///         today, because <c>OnMidi</c> is the only reverse-P/Invoke whose own
    ///         wrapper closes the native object it dispatches through —
    ///         <see cref="JuceAudioPlugin" />'s editor-closed thunk touches no
    ///         handle. Add a row when a second one appears.
    ///     </para>
    /// </summary>
    [Theory]
    [InlineData("src/JuceSharp/JuceMidiInput.cs", "private unsafe void OnMidi(")]
    public void EveryNativeInvokedCallbackOnAHandleOwner_PinsTheHandleForItsBody(
        string relative, string signature)
    {
        var body = MethodBody(ReadRepoFile(relative), signature);

        // A pin, not merely a mention: the acquire has to be the first thing the
        // body does with the owner, and the release has to be in a finally.
        Assert.Matches(new Regex(@"_owner\?\.TryAcquire\(\)"), body);
        Assert.Matches(new Regex(@"if\s*\(\s*h\s*==\s*nint\.Zero\s*\)\s*return\s*;"), body);
        Assert.Matches(new Regex(@"finally\s*\{[^}]*_owner!?\.Release\(\)"), body);
    }

    /// <summary>Extract one method's body by brace matching from the opening
    /// brace after its signature, so an assertion about a method is not an
    /// assertion about the file that happens to contain it.</summary>
    private static string MethodBody(string source, string signature)
    {
        var at = source.IndexOf(signature, StringComparison.Ordinal);
        Assert.True(at >= 0, $"'{signature}' is gone — repoint this lint.");
        var open = source.IndexOf('{', at);
        Assert.True(open > 0, $"'{signature}' has no body.");
        var depth = 0;
        for (var i = open; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            else if (source[i] == '}' && --depth == 0) return source[open..(i + 1)];
        }

        throw new InvalidOperationException($"Unbalanced braces after '{signature}'.");
    }

    internal static string ReadRepoFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
        }

        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}

/// <summary>Compile-time mirror used by an <c>[InlineData]</c>, which cannot
/// reference <c>JuceAudioEngine.MaxPlayers</c> (a property, not a constant).
/// Pinned to the real value by <see cref="NativeMethodsTests" />.</summary>
internal static class NativeMethodsAudit
{
    internal const int MaxPlayersMirror = 4;
}
