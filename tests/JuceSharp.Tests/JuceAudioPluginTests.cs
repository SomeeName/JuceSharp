using Xunit;

namespace JuceSharp.Tests;

[Collection("NativeEngine")]
public sealed class JuceAudioPluginTests
{
    [Fact]
    public void Constructor_RejectsZeroHandle()
    {
        Assert.Throws<ArgumentException>(
            () => new JuceAudioPlugin(nint.Zero, "x.vst3"));
    }

    [NativeAvailableFact]
    public void LoadPlugin_BadPath_Throws_FileNotFound()
    {
        using var engine = new JuceAudioEngine(new JuceEngineConfig { EnableConsoleLog = false });
        Assert.Throws<FileNotFoundException>(
            () => engine.LoadPlugin(@"C:\definitely\not\a\real\plugin.vst3"));
    }

    [NativeAvailableFact]
    public void LoadPlugin_NullOrWhitespace_Throws_ArgumentException()
    {
        using var engine = new JuceAudioEngine(new JuceEngineConfig { EnableConsoleLog = false });
        Assert.Throws<ArgumentException>(() => engine.LoadPlugin(""));
        Assert.Throws<ArgumentException>(() => engine.LoadPlugin("   "));
    }

    [NativeAvailableFact]
    public void LoadPlugin_AfterDispose_Throws_ObjectDisposed()
    {
        var engine = new JuceAudioEngine(new JuceEngineConfig { EnableConsoleLog = false });
        engine.Dispose();
        Assert.Throws<ObjectDisposedException>(
            () => engine.LoadPlugin(@"C:\fake.vst3"));
    }
}

/// <summary>
///     W4-M2. <see cref="JuceAudioPlugin.Bypass" /> cached a <c>bool</c> that
///     <see cref="JuceAudioPlugin.AttachToInput(int)" /> and
///     <see cref="JuceAudioPlugin.DetachFromInput" /> changed underneath it —
///     both drive the same native attachment and neither wrote the flag. Two
///     ordinary sequences lied in opposite directions:
///     <c>DetachFromInput(); Bypass = false;</c> early-outed on a flag that was
///     already <c>false</c> and left the plugin DETACHED while reporting
///     "active"; <c>Bypass = true; AttachToInput(2);</c> processed audio while
///     reporting "bypassed". The fix deletes the cache: the getter asks the
///     engine, which <see cref="JuceAudioPlugin.InputPlayer" /> already did and
///     which is why that member was the one attachment-derived property that was
///     always right.
///     <para>
///         <b>Why not the attach → bypass → detach round trip the audit asked
///         for:</b> it needs a real VST3, which no CI box here has, and a FAKE
///         handle cannot stand in — <c>dca_plugin_*</c> is
///         <c>reinterpret_cast&lt;Plugin*&gt;(handle)</c> followed by a
///         dereference, so a bogus non-zero handle is an access violation, not a
///         rejected argument. These two therefore read STRUCTURE: the first over
///         the compiled type (no attachment cache can exist), the second anchored
///         to the property's own body (the answer comes from the engine).
///     </para>
/// </summary>
public sealed class PluginBypassIsNotCachedTests
{
    /// <summary>
    /// The cache cannot come back. Any instance <c>bool</c> on this type is an
    /// attachment/ state mirror of something the engine owns — which is the
    /// whole finding — so the type carries none.
    /// </summary>
    [Fact]
    public void JuceAudioPlugin_CachesNoAttachmentFlag()
    {
        var cached = typeof(JuceAudioPlugin)
            .GetFields(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)
            .Where(f => f.FieldType == typeof(bool))
            .Select(f => f.Name)
            .ToArray();

        Assert.True(cached.Length == 0,
            "JuceAudioPlugin has a cached bool (" + string.Join(", ", cached) + "). Bypass cached one "
            + "until W4-M2 and AttachToInput/DetachFromInput wrote nothing to it, so the property "
            + "reported a state the engine was not running. Ask the engine instead.");
    }

    /// <summary>
    /// …and the getter is the engine's answer rather than any field: read off
    /// the PROPERTY'S body, not the file, so a mention of
    /// <c>InputPlayer</c> elsewhere in 400 lines cannot satisfy it.
    /// </summary>
    [Fact]
    public void BypassReadsTheEngine_InItsGetterAndInItsIdempotenceCheck()
    {
        var body = PropertyBody(
            WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/JuceAudioPlugin.cs"),
            "public bool Bypass");

        Assert.Matches(new System.Text.RegularExpressions.Regex(@"get\s*=>\s*InputPlayer\s*<\s*0\s*;"), body);
        // The setter's early-out is the half that actually lied: it compared the
        // requested value against the CACHE, so a detach nobody told it about
        // made "un-bypass" a no-op.
        Assert.Contains("NativeMethods.PluginGetInputPlayer(h)", body, StringComparison.Ordinal);
    }

    /// <summary>Brace-matched body of one property, so the assertions above are
    /// about that property and not about the file containing it.</summary>
    private static string PropertyBody(string source, string signature)
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
}
