// ReSharper disable InconsistentNaming -- private static availability cache uses _camelCase consistent with project style.
using System.Runtime.CompilerServices;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Skip attribute for tests requiring the native juce_sharp.dll. When the
/// runtime can't load the DLL (CI without native build, sandbox, etc.) the
/// test is silently skipped so the rest of the suite still runs.
/// <para>
///     <b>Silence is the hazard.</b> This attribute guards roughly a quarter of
///     the suite — every rig, DSP and engine integration test — so a
///     <c>juce_sharp.dll</c> the CLR cannot load turns all of them into skips
///     with the run still GREEN. Set <c>JUCESHARP_REQUIRE_NATIVE=1</c> (CI does)
///     and the skip is withheld: the test runs, fails on the missing entry point,
///     and the reason is printed by
///     <see cref="Wave2NativeProbeTests" />. See
///     <c>docs/agents/rt-safety-and-tests.md</c>.
/// </para>
/// </summary>
public sealed class NativeAvailableFactAttribute : FactAttribute
{
    public NativeAvailableFactAttribute(
        [CallerFilePath] string? sourceFilePath = null,
        [CallerLineNumber] int sourceLineNumber = -1)
        : base(sourceFilePath, sourceLineNumber)
    {
        if (!NativeProbe.IsAvailable && !NativeProbe.RequireNative)
            Skip = "juce_sharp native DLL not available (skipping integration test).";
    }
}

public sealed class NativeAvailableTheoryAttribute : TheoryAttribute
{
    public NativeAvailableTheoryAttribute(
        [CallerFilePath] string? sourceFilePath = null,
        [CallerLineNumber] int sourceLineNumber = -1)
        : base(sourceFilePath, sourceLineNumber)
    {
        if (!NativeProbe.IsAvailable && !NativeProbe.RequireNative)
            Skip = "juce_sharp native DLL not available (skipping integration test).";
    }
}

internal static class NativeProbe
{
    /// <summary>The one env var that turns "native unavailable" from a skip into
    /// a failure. Set on the CI managed-test step; unset locally, so a developer
    /// without a native build still gets a usable run.</summary>
    internal const string RequireNativeVariable = "JUCESHARP_REQUIRE_NATIVE";

    private static readonly Lazy<bool> _avail = new(Probe);
    private static Exception? _failure;

    public static bool IsAvailable => _avail.Value;

    /// <summary>Whether <see cref="RequireNativeVariable" /> is set to <c>1</c>.</summary>
    public static bool RequireNative =>
        string.Equals(Environment.GetEnvironmentVariable(RequireNativeVariable), "1",
                      StringComparison.Ordinal);

    /// <summary>
    ///     Why the probe failed, or <c>null</c> when it succeeded (read it only
    ///     after <see cref="IsAvailable" />, which is what runs the probe).
    ///     <para>
    ///         The probe used to end in a bare <c>catch { return false; }</c>, so
    ///         "the file is not there" and "the file is there and the loader
    ///         refused it" were the same answer — and that answer was a silent
    ///         skip. The exception is kept so a failure can name its own cause.
    ///     </para>
    /// </summary>
    public static string? FailureDescription =>
        _avail.Value ? null : _failure is null ? "unknown (probe did not record a cause)"
                                               : $"{_failure.GetType().Name}: {_failure.Message}";

    private static bool Probe()
    {
        try
        {
            _ = JuceAudioEngine.NativeVersion;
            return true;
        }
        catch (Exception ex)
        {
            _failure = ex;
            return false;
        }
    }
}
