using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The v0.17.1 native crash-race fixes (the 1.0 blocker
/// list's N2 and N4) have no runtime test — both need a live device and a live
/// sandbox child. The SHAPE of each fix is checkable from the source, and a
/// shape that regresses is exactly how each one shipped in the first place:
/// <list type="bullet">
///   <item>N2 — <c>RubberBandAudioSource</c>: the 250 Hz managed poll reads an
///   atomic the audio thread publishes and never touches <c>stretcher_</c>;
///   every other <c>stretcher_</c> use is under <c>stretcherMutex_</c>.</item>
///   <item>N4 — <c>RemoteVstBridge</c>: <c>lastHeartbeat()</c> (polled by the
///   sandbox health timer) and <c>setSampleRate()</c> bypass the engine's bridge
///   barrier, so each takes <c>viewMutex_</c> before it touches
///   <c>view_</c>.</item>
/// </list>
/// N5 (the callback no longer allocates <c>bridgeSilentBuf_</c>) is covered by
/// the RT gate binary, <c>juce_sharp_rtcheck</c>, which runs the real callback
/// under an allocation hook; N1 by <c>NativeHandleDrainTests</c>.
/// </summary>
public sealed class NativeCrashRaceSourceLintTests
{
    [Fact]
    public void N2_ThePositionPoll_ReadsAnAtomic_AndNeverDereferencesTheStretcher()
    {
        var cpp = StripComments(File.ReadAllText(NativeSource("RubberBandAudioSource.cpp")));
        var body = FunctionBody(cpp, "getReadAheadOutputSamples");

        Assert.DoesNotContain("stretcher_", body);
        Assert.DoesNotContain("stretcherMutex_", body);
        Assert.Contains("readAheadOut_.load(", body);

        // Every remaining stretcher_ dereference sits in a function that takes
        // the mutex (plain lock_guard or the RT-checked guard on the callback),
        // or in a `*Locked` helper — whose every caller must then take it.
        var lockedHelpers = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(cpp, @"stretcher_->"))
        {
            var (name, fn) = EnclosingFunction(cpp, m.Index);
            if (name.EndsWith("Locked", StringComparison.Ordinal)) { lockedHelpers.Add(name); continue; }
            Assert.True(fn.Contains("stretcherMutex_", StringComparison.Ordinal),
                $"stretcher_-> at offset {m.Index} is in {name}, which does not take stretcherMutex_");
        }

        Assert.NotEmpty(lockedHelpers); // applyRatiosLocked — the lint must have seen it
        foreach (var helper in lockedHelpers)
        foreach (Match call in Regex.Matches(cpp, @"(?<!::)\b" + Regex.Escape(helper) + @"\s*\("))
        {
            var (caller, fn) = EnclosingFunction(cpp, call.Index);
            if (caller == helper) continue; // its own definition head
            Assert.True(fn.Contains("stretcherMutex_", StringComparison.Ordinal),
                $"{helper}() is called from {caller}, which does not take stretcherMutex_");
        }
    }

    [Theory]
    [InlineData("lastHeartbeat")]
    // Renamed from setSampleRate when the bridge learned to publish the block
    // size and a generation counter too (bridge v2). Renaming a native function
    // silently repoints a lint that locates it BY NAME, which is why this
    // comment exists rather than just the new string.
    [InlineData("setDevicePrepare")]
    public void N4_TheBridgeMethodsOutsideTheBarrier_TakeTheViewMutexBeforeTouchingTheView(string function)
    {
        var cpp = StripComments(File.ReadAllText(NativeSource("RemoteVstBridge.cpp")));
        var body = FunctionBody(cpp, function);

        var lockAt = body.IndexOf("std::lock_guard<std::mutex> lk(viewMutex_);", StringComparison.Ordinal);
        var viewAt = body.IndexOf("view_", StringComparison.Ordinal);
        Assert.True(lockAt >= 0, $"{function} does not take viewMutex_");
        Assert.True(viewAt > lockAt, $"{function} touches view_ before taking viewMutex_");
    }

    // ---- helpers -------------------------------------------------------------

    private static string FunctionBody(string cpp, string name)
    {
        var at = Regex.Match(cpp, @"::" + Regex.Escape(name) + @"\s*\([^)]*\)[^{;]*\{");
        Assert.True(at.Success, $"{name} not found");
        var open = cpp.IndexOf('{', at.Index);
        var depth = 0;
        for (var i = open; i < cpp.Length; i++)
        {
            if (cpp[i] == '{') depth++;
            else if (cpp[i] == '}' && --depth == 0) return cpp.Substring(open, i - open + 1);
        }
        throw new InvalidOperationException("unbalanced braces");
    }

    private static (string Name, string Body) EnclosingFunction(string cpp, int offset)
    {
        // Walk back to the nearest "Class::Name(" definition head, then take its body.
        // A DEFINITION starts a line (return type first) — `std::abs(x)` inside
        // an `if` is a call, not a head, and matched here until the anchor came.
        var heads = Regex.Matches(cpp, @"^[^\s#][^\n(]*\b\w+::(\w+)\s*\([^)]*\)[^{;]*\{", RegexOptions.Multiline);
        Match? last = null;
        foreach (Match h in heads)
        {
            if (h.Index > offset) break;
            last = h;
        }
        Assert.NotNull(last);
        var open = cpp.IndexOf('{', last!.Index);
        var depth = 0;
        for (var i = open; i < cpp.Length; i++)
        {
            if (cpp[i] == '{') depth++;
            else if (cpp[i] == '}' && --depth == 0) return (last.Groups[1].Value, cpp.Substring(open, i - open + 1));
        }
        throw new InvalidOperationException("unbalanced braces");
    }

    /// <summary>
    /// The stem-separation decode path (opt-in: it only compiles with
    /// <c>DOTCHARTER_STEMS_DEMUCS_CPP=1</c>, so no build in this repo's default
    /// configuration touches it, and no runtime test can reach it) had two
    /// faults that a reader would not notice:
    /// <list type="bullet">
    /// <item><c>(int) reader-&gt;lengthInSamples</c> TRUNCATED a 64-bit frame
    /// count, so a file past 2^31 frames separated a wrapped, mostly-negative
    /// length — the D-03 class of bug <c>dca_decode_audio_file</c> already
    /// guards against;</item>
    /// <item>the 44.1 kHz conversion the model requires ran through a bare
    /// <c>juce::CatmullRomInterpolator</c>, which has NO pre-filter, so every
    /// 48 or 96 kHz source folded everything above 22.05 kHz back into the
    /// band and the model separated the aliases along with the music.</item>
    /// </list>
    /// A lint is the only gate available here. The rewritten function was
    /// compile-checked against real JUCE headers by temporarily hoisting it
    /// into the stub branch.
    /// </summary>
    [Fact]
    public void StemDecode_RejectsOversizedFiles_AndAntiAliasesOnTheWayDown()
    {
        var cpp = StripComments(File.ReadAllText(NativeSource("StemSeparator.cpp")));

        Assert.DoesNotContain("CatmullRomInterpolator", cpp, StringComparison.Ordinal);
        Assert.Contains("ResamplingAudioSource", cpp, StringComparison.Ordinal);
        // The cast is still there — juce::AudioBuffer indexes with int, so
        // there is nowhere to widen TO. What matters is that the range check
        // comes FIRST.
        var guardAt = cpp.IndexOf("numeric_limits<int>::max()", StringComparison.Ordinal);
        var castAt = cpp.IndexOf("(int) reader->lengthInSamples", StringComparison.Ordinal);
        Assert.True(guardAt >= 0, "the oversized-file guard is gone");
        Assert.True(castAt > guardAt,
            "reader->lengthInSamples is narrowed to int before the range check that makes the "
            + "narrowing safe");

        // Non-vacuity: the file really is the one with the decode in it.
        Assert.Contains("load_and_resample", cpp, StringComparison.Ordinal);
        Assert.Contains("kModelRate", cpp, StringComparison.Ordinal);
    }

    private static string StripComments(string s)
    {
        s = Regex.Replace(s, @"/\*.*?\*/", "", RegexOptions.Singleline);
        return Regex.Replace(s, @"//[^\n]*", "");
    }

    private static string NativeSource(string file, [CallerFilePath] string? thisFile = null)
    {
        var testsDir = Path.GetDirectoryName(thisFile!)!;
        var path = Path.GetFullPath(Path.Combine(testsDir, "..", "..", "src", "JuceSharp.Native", "src", file));
        Assert.True(File.Exists(path), path);
        return path;
    }
}
