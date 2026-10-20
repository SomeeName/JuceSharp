using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Concurrency audit #14: <c>JuceAudioEngine.OpenMidiInput</c> handed back
///     an input that NOTHING rooted. The engine's child registry is weak (it is
///     there for teardown order), and the app wires the callback and keeps no
///     handle — so MIDI worked until the first gen-2 collection ran the
///     finalizer and closed the device silently: the "a capability the app
///     cannot honour must never look live" shape. The comment on
///     <c>OpenMidiInput</c> claimed the weak registry had fixed this; it never
///     could.
/// </summary>
[Collection("NativeEngine")]
public sealed class JuceMidiInputRootingTests
{
    /// <summary>
    ///     Behavioural, on a real MIDI device when the box has one: open an
    ///     input, drop every reference, collect, and the device must still be
    ///     open. Skips honestly on a box with no MIDI input — the source lint
    ///     below is what runs everywhere.
    /// </summary>
    [NativeAvailableFact]
    public void ADiscardedMidiInput_SurvivesAFullCollection()
    {
        if (JuceAudioEngine.EnumerateMidiInputs().Count == 0)
            Assert.Skip("no MIDI input device on this machine");

        using var engine = new JuceAudioEngine();
        engine.Stop(); // the device is not needed for MIDI
        var weak = OpenAndDrop(engine);
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();

        Assert.True(weak.TryGetTarget(out var input), "the engine let the MIDI input be collected");
        Assert.False(input!.IsDisposed, "the finalizer closed a MIDI input the app still expects to deliver");
        Assert.Equal(1, engine.RootedMidiInputsForTests);

        input.Dispose();
        Assert.Equal(0, engine.RootedMidiInputsForTests); // an explicit Dispose un-roots it
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference<JuceMidiInput> OpenAndDrop(JuceAudioEngine engine)
        => new(engine.OpenMidiInput(0, static _ => { }));

    /// <summary>The strong root is in the code path, whatever hardware the
    /// test box has: OpenMidiInput adds to <c>_midiInputs</c>, an input's Dispose
    /// removes itself, and the engine's teardown clears the list.</summary>
    [Fact]
    public void OpenMidiInput_RootsTheInputStrongly_UntilItOrTheEngineIsDisposed()
    {
        var engine = StripComments(File.ReadAllText(Path.Combine(WrapperRoot(), "JuceAudioEngine.cs")));
        Assert.Contains("private readonly List<JuceMidiInput> _midiInputs", engine);

        var open = engine.IndexOf("public JuceMidiInput OpenMidiInput(", StringComparison.Ordinal);
        Assert.True(open >= 0);
        var body = engine[open..];
        body = body[..body.IndexOf("\n    }", StringComparison.Ordinal)];
        Assert.Contains("_midiInputs.Add(input)", body);
        Assert.Contains("_midiInputs.Remove(input)", body);

        var teardown = engine.IndexOf("private bool DisposeChildren(", StringComparison.Ordinal);
        Assert.True(teardown >= 0);
        var teardownBody = engine[teardown..];
        teardownBody = teardownBody[..teardownBody.IndexOf("\n    }", StringComparison.Ordinal)];
        Assert.Contains("_midiInputs.Clear()", teardownBody);

        var input = StripComments(File.ReadAllText(Path.Combine(WrapperRoot(), "JuceMidiInput.cs")));
        var dispose = input.IndexOf("public void Dispose()", StringComparison.Ordinal);
        Assert.True(dispose >= 0);
        Assert.Contains("_disposedCallback, null)?.Invoke()", input[dispose..]);
        // The finalizer must NOT reach into the engine's list.
        var finalizer = input.IndexOf("~JuceMidiInput()", StringComparison.Ordinal);
        Assert.True(finalizer >= 0);
        var finalizerBody = input[finalizer..];
        finalizerBody = finalizerBody[..finalizerBody.IndexOf("\n    }", StringComparison.Ordinal)];
        Assert.Contains("_disposedCallback = null;", finalizerBody);
    }

    private static string StripComments(string code)
    {
        code = Regex.Replace(code, @"/\*.*?\*/", "", RegexOptions.Singleline);
        return Regex.Replace(code, @"//[^\n]*", "");
    }

    private static string WrapperRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.sln")))
            dir = Path.GetDirectoryName(dir);
        return Path.Combine(dir ?? throw new InvalidOperationException("Could not locate JuceSharp.sln."),
            "src", "JuceSharp");
    }
}
