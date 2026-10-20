using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     The structural gap the wave-2 audit put at the head of its test section:
///     <see cref="NativeAvailableFactAttribute" /> guards roughly a QUARTER of
///     this suite — 228 of 815 methods when measured, including every rig, DSP
///     and engine test the 0.9 wave added — and it decides by calling
///     <c>JuceAudioEngine.NativeVersion</c> inside a <c>try</c>. Nothing asserted
///     the probe was ever true, and the probe swallowed the reason it was not, so
///     a <c>juce_sharp.dll</c> the CLR could not load read as a green run with a
///     pile of skips.
///     <para>
///         Two nets, deliberately different. The env gate is the CI answer: with
///         <c>JUCESHARP_REQUIRE_NATIVE=1</c> an unavailable engine is a FAILURE,
///         not a skip — and the skip is withheld from the attribute itself, so the
///         guarded tests run and fail rather than vanishing. The on-disk check is
///         the local answer: it needs no env var, and it distinguishes "no native
///         build in this tree" (a legitimate skip) from "the file is right there
///         and the loader refused it" (a real fault, reported with its exception).
///     </para>
/// </summary>
public sealed class Wave2NativeProbeTests
{
    /// <summary>The flattened deployment name — <c>JuceSharp.csproj</c> gives the
    /// native items a <c>&lt;Link&gt;</c> of <c>%(Filename)%(Extension)</c>, so it
    /// lands beside the test assembly rather than under <c>runtimes/</c>.</summary>
    private const string NativeDll = "juce_sharp.dll";

    [Fact]
    public void RequireNative_TurnsAnUnavailableEngineIntoAFailure()
    {
        if (!NativeProbe.RequireNative)
        {
            Assert.Skip($"{NativeProbe.RequireNativeVariable} is not set to 1 — this box is allowed "
                        + "to run without a native build. CI sets it; see .github/workflows/build.yml.");
            return;
        }

        Assert.True(NativeProbe.IsAvailable,
            $"{NativeProbe.RequireNativeVariable}=1 but the native engine could not be loaded, so every "
            + "NativeAvailableFact/Theory in this suite would have been skipped. Cause: "
            + NativeProbe.FailureDescription);
    }

    /// <summary>
    ///     If the DLL is on disk, the probe MUST be true. A false here is a load
    ///     failure — a bitness mismatch, a missing VC++ runtime, an ABI-mismatched
    ///     build — and it is exactly what the bare <c>catch</c> used to hide.
    /// </summary>
    [Fact]
    public void TheProbeIsTrue_WhenTheNativeDllIsPresentOnDisk()
    {
        var path = FindNativeDll();
        if (path is null)
        {
            Assert.Skip($"{NativeDll} is not in this tree (it is a build artifact, git-ignored) — "
                        + "nothing to distinguish an absent file from a load failure.");
            return;
        }

        Assert.True(NativeProbe.IsAvailable,
            $"{NativeDll} exists at {path} but the runtime could not load it, so {nameof(NativeProbe)} "
            + "reports unavailable and a quarter of this suite silently skips. Cause: "
            + NativeProbe.FailureDescription);
    }

    /// <summary>Non-vacuity: the probe records a CAUSE, not just a bool. When the
    /// engine is available the description is null; when it is not, it names the
    /// exception type — never the empty string the bare catch left behind.</summary>
    [Fact]
    public void TheProbeReportsItsCause_RatherThanSwallowingIt()
    {
        if (NativeProbe.IsAvailable)
        {
            Assert.Null(NativeProbe.FailureDescription);
            return;
        }

        Assert.False(string.IsNullOrWhiteSpace(NativeProbe.FailureDescription));
    }

    private static string? FindNativeDll()
    {
        var beside = Path.Combine(AppContext.BaseDirectory, NativeDll);
        if (File.Exists(beside)) return beside;

        // …and the source location the CMake POST_BUILD step deploys to, in case
        // the test assembly was run from somewhere the copy did not reach.
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, "src", "JuceSharp", "runtimes", "win-x64",
                                         "native", NativeDll);
            if (File.Exists(candidate)) return candidate;
        }

        return null;
    }
}
