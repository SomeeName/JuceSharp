using System.Security.Cryptography;
using System.Text;

namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// The hash-locked package set the embedded interpreter installs: every
/// distribution, its exact version and the SHA-256 of each wheel CPython 3.11
/// on Windows x64 may install, generated from <see cref="PythonPackagePins"/>
/// by <c>tools/python-lock/generate_lock.py</c> and compiled in as a resource.
///
/// <para>
/// pip installs it with <c>--require-hashes --no-deps --only-binary :all:</c>,
/// so a package index that serves different bytes, a dependency nobody locked
/// or an sdist that would run a build script all fail the install instead of
/// landing in site-packages. The lock covers Windows x64 only — the one
/// platform the embedded interpreter exists for — and
/// <see cref="PythonEnvManager.EnsurePackagesAsync"/> refuses anywhere else.
/// </para>
/// </summary>
internal static class PythonPackageLock
{
    /// <summary>Manifest name of the embedded lock file.</summary>
    internal const string ResourceName = "JuceSharp.StemSeparation.requirements-win_amd64-cp311.lock";

    private static readonly Lazy<string> LazyText = new(Load);

    /// <summary>The lock file, verbatim.</summary>
    internal static string Text => LazyText.Value;

    /// <summary>First 16 hex characters of the lock's SHA-256. Part of the
    /// package stamp, so regenerating the lock re-runs pip.</summary>
    internal static string Digest =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Text)))[..16].ToLowerInvariant();

    /// <summary>
    /// The specs a caller may ask <see cref="PythonEnvManager.EnsurePackagesAsync"/>
    /// for: exactly the <see cref="PythonPackagePins"/> values the lock was
    /// generated from. Anything else is not in the lock, so there is no hash to
    /// install it by.
    /// </summary>
    internal static IReadOnlySet<string> CoveredPins { get; } =
        new HashSet<string>(PythonPackagePins.InstalledDirectories.Keys, StringComparer.Ordinal);

    /// <summary>Whether this platform can install the lock (Windows; the wheels
    /// are win_amd64, which Windows on Arm runs under emulation).</summary>
    internal static bool CoversThisPlatform => OperatingSystem.IsWindows();

    private static string Load()
    {
        using var s = typeof(PythonPackageLock).Assembly.GetManifestResourceStream(ResourceName)
                      ?? throw new InvalidOperationException(
                          $"The package lock resource '{ResourceName}' is missing from the assembly.");
        using var r = new StreamReader(s, Encoding.UTF8);
        return r.ReadToEnd();
    }
}
