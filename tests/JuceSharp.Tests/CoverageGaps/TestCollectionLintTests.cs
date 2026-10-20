using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// <b>A test class that touches shared, process-global state has to say so with
/// a <c>[Collection]</c> attribute, and the attribute lives on the CLASS — not
/// on the file.</b>
///
/// <para>
/// The repo rule (AGENTS.md): everything that touches <c>StemSeparator</c>'s
/// process-global gate or <c>ChildProcessRegistry</c>'s live set belongs to
/// <c>[Collection("StemSeparatorConcurrency")]</c>. Everything that runs a real
/// transfer — the loopback file server, <c>VerifiedDownloader</c>, a catalogue
/// <c>EnsureAsync</c> — belongs to <c>[Collection("StemDownloads")]</c>. Both
/// collections are <c>DisableParallelization = true</c>; that cross-fire, not
/// the environment, was most of what "the StemSeparator tests are flaky" meant.
/// </para>
/// <para>
/// This exists because a C# file holds as many classes as it likes and an
/// attribute on the first one covers NONE of the others. The audit flagged
/// <c>OnnxModelCatalogTests</c> sitting below <c>[Collection("StemDownloads")]
/// class VerifiedDownloaderTests</c> with no attribute of its own. That one
/// turned out to be harmless — it reads the static catalogue table and nothing
/// else, so it is correctly uncollected, and blanket-attributing every class in
/// a collection-bearing file would be wrong for eight more files in this suite
/// that deliberately pair a device-free unit class with a serialized one. What
/// is NOT harmless is the same layout with a real download in it, which is what
/// this lint catches.
/// </para>
/// </summary>
public sealed class TestCollectionLintTests
{
    private static string TestRoot([CallerFilePath] string? thisFile = null)
    {
        var dir = Path.GetDirectoryName(thisFile);
        while (dir != null && !File.Exists(Path.Combine(dir, "JuceSharp.Tests.csproj")))
            dir = Path.GetDirectoryName(dir);
        return dir ?? throw new InvalidOperationException("Could not locate the test project.");
    }

    /// <summary>Top-level type declarations, with whatever attributes sit above them.</summary>
    private static readonly Regex Declaration = new(
        @"(?m)^(?<attrs>(?:\[[^\n]*\]\r?\n)*)(?<mods>(?:public|internal)[^\n]*?)\bclass\s+(?<name>\w+)",
        RegexOptions.Compiled);

    private sealed record TestClass(string File, string Name, string Attributes, string Body);

    private static IEnumerable<TestClass> Classes()
    {
        var root = TestRoot();
        foreach (var file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(root, file).Replace('\\', '/');
            if (rel.StartsWith("bin/", StringComparison.Ordinal) ||
                rel.StartsWith("obj/", StringComparison.Ordinal)) continue;

            var text = StripComments(File.ReadAllText(file));
            var matches = Declaration.Matches(text);
            for (var i = 0; i < matches.Count; i++)
            {
                var end = i + 1 < matches.Count ? matches[i + 1].Index : text.Length;
                yield return new TestClass(
                    Path.GetFileName(file),
                    matches[i].Groups["name"].Value,
                    matches[i].Groups["attrs"].Value,
                    text[matches[i].Index..end]);
            }
        }
    }

    /// <summary>
    /// This file names every marker it hunts for, so it matches ITSELF — the
    /// same shape HttpDownloadLintTests skips its own sanctioned site for.
    /// Comments are already stripped; string literals are not, and stripping
    /// those would make the lint blind to the very calls it is looking for.
    /// </summary>
    private const string Self = "TestCollectionLintTests.cs";

    private static bool IsTestClass(TestClass c) =>
        c.Body.Contains("[Fact", StringComparison.Ordinal) ||
        c.Body.Contains("[Theory", StringComparison.Ordinal) ||
        c.Body.Contains("[NativeAvailableFact", StringComparison.Ordinal) ||
        c.Body.Contains("[NativeAvailableTheory", StringComparison.Ordinal) ||
        c.Body.Contains("[OnnxResourceFact", StringComparison.Ordinal);

    private static bool InCollection(TestClass c, string name) =>
        c.Attributes.Contains($"[Collection(\"{name}\")]", StringComparison.Ordinal);

    private static bool InAnyCollection(TestClass c, params string[] names) =>
        names.Any(n => InCollection(c, n));

    /// <summary>
    /// Every collection this suite is allowed to put a shared-state class into.
    /// All of them are <c>DisableParallelization = true</c> — pinned below —
    /// which is what lets any one of them stand in for another: a class in ANY
    /// of these runs while nothing else runs.
    /// </summary>
    private static readonly string[] Sanctioned =
    [
        "NativeEngine", "NullDevice", "AllocationProbes",
        "StemSeparatorConcurrency", "StemDownloads",
    ];

    /// <summary>Offender report for a marker sweep: the classes that carry a marker and no sanctioned attribute.</summary>
    private static List<string> Offenders(string[] markers, params string[] allowed) =>
        Classes()
            .Where(c => c.File != Self)
            .Where(IsTestClass)
            .Where(c => markers.Any(m => c.Body.Contains(m, StringComparison.Ordinal)))
            .Where(c => !InAnyCollection(c, allowed))
            .Select(c => $"{c.File}: {c.Name}")
            .ToList();

    /// <summary>
    /// The gate is a process-global <see cref="SemaphoreSlim"/> and the registry
    /// is a process-global dictionary. Two classes driving either in parallel
    /// read as "the StemSeparator tests are flaky".
    /// </summary>
    [Fact]
    public void Every_class_that_drives_the_process_global_stem_state_is_serialized()
    {
        string[] markers =
        [
            "ResetGlobalGateForTests", "CurrentGateForTests", "AcquireSeparationSlotAsync",
            "ChildProcessRegistry.",
        ];

        var offenders = Classes()
            .Where(c => c.File != Self)
            .Where(IsTestClass)
            .Where(c => markers.Any(m => c.Body.Contains(m, StringComparison.Ordinal)))
            .Where(c => !InCollection(c, "StemSeparatorConcurrency"))
            .Select(c => $"{c.File}: {c.Name}")
            .ToList();

        Assert.True(offenders.Count == 0,
            "These classes touch StemSeparator's process-global gate or ChildProcessRegistry's live set " +
            "and are not in [Collection(\"StemSeparatorConcurrency\")]:\n  " + string.Join("\n  ", offenders));
    }

    /// <summary>
    /// A real transfer binds a loopback port and writes into a shared
    /// destination gate. Run two of those at once and one fails for a reason
    /// that has nothing to do with the code under test.
    /// </summary>
    [Fact]
    public void Every_class_that_performs_a_transfer_is_serialized()
    {
        string[] markers =
        [
            "LoopbackFileServer", "VerifiedDownloader.DownloadAsync",
            "OnnxModelCatalog.EnsureAsync", "DemucsGgmlModelCatalog.EnsureAsync",
            // Not a transfer but a STATIC on the transfer type: a class that
            // shortens VerifiedDownloader.RequestTimeout for its own duration
            // shortens it for whatever else is downloading at the time.
            "VerifiedDownloader.RequestTimeout",
        ];

        var offenders = Classes()
            .Where(c => c.File != Self)
            .Where(IsTestClass)
            .Where(c => markers.Any(m => c.Body.Contains(m, StringComparison.Ordinal)))
            .Where(c => !InCollection(c, "StemDownloads"))
            .Select(c => $"{c.File}: {c.Name}")
            .ToList();

        Assert.True(offenders.Count == 0,
            "These classes run a real transfer and are not in [Collection(\"StemDownloads\")]. " +
            "An attribute on the FIRST class in the file covers none of the others:\n  " +
            string.Join("\n  ", offenders));
    }

    /// <summary>
    /// <b>A bare <c>new JuceAudioEngine()</c> opens the host's REAL default
    /// output device.</b> Only <c>NullDevice.CreateEngine()</c> asks for the
    /// null type, and <c>NullDeviceScope</c> sets the PROCESS env var that
    /// registers it. xunit builds one class instance per test, so a class with
    /// an engine in its constructor opens and closes a device once per fact:
    /// ASIO is exclusive, a WASAPI open costs hundreds of milliseconds, and 35
    /// classes were doing it concurrently with each other and with the
    /// guard-off assertion in <c>NullDeviceTests</c>.
    /// </summary>
    [Fact]
    public void Every_class_that_reaches_the_native_engine_is_serialized()
    {
        string[] markers =
        [
            "new JuceAudioEngine(", "NullDevice.CreateEngine(",
            "NullDeviceScope", "JuceEngineFactory",
        ];

        var offenders = Offenders(markers, "NativeEngine", "NullDevice");

        Assert.True(offenders.Count == 0,
            "These classes create a native engine (or set the null-device env guard) and are in neither " +
            "[Collection(\"NativeEngine\")] nor [Collection(\"NullDevice\")]. A class with no attribute " +
            "gets an implicit collection of its OWN, which means it runs alongside EVERYTHING:\n  " +
            string.Join("\n  ", offenders));
    }

    /// <summary>
    /// <b>A GC allocation delta is a reading, not a fact.</b>
    /// <c>GC.GetAllocatedBytesForCurrentThread()</c> subtracts the unconsumed
    /// remainder of this thread's allocation context, and a collection RETIRES
    /// that context — so two reads straddling a GC differ with nothing
    /// allocated in between. Other threads making garbage is most of the
    /// exposure, and a serialized collection removes it.
    /// </summary>
    [Fact]
    public void Every_class_that_reads_the_gc_allocation_counter_is_serialized()
    {
        string[] markers = ["GC.GetAllocatedBytesForCurrentThread"];

        var offenders = Offenders(markers, Sanctioned);

        Assert.True(offenders.Count == 0,
            "These classes assert on a GC allocation delta and are in no serialized collection. " +
            "[Collection(\"AllocationProbes\")] is the one that exists for them:\n  " +
            string.Join("\n  ", offenders));
    }

    /// <summary>
    /// A child process is machine-global: a port, a pipe name, a job object, a
    /// CPU. The stem children have <c>StemSeparatorConcurrency</c>; the VST
    /// host child is the other one and belongs with the engine.
    /// </summary>
    [Fact]
    public void Every_class_that_spawns_a_child_process_is_serialized()
    {
        string[] markers = ["Process.Start"];

        var offenders = Offenders(markers, "NativeEngine", "StemSeparatorConcurrency");

        Assert.True(offenders.Count == 0,
            "These classes spawn a child process and are in neither [Collection(\"NativeEngine\")] " +
            "nor [Collection(\"StemSeparatorConcurrency\")]:\n  " + string.Join("\n  ", offenders));
    }

    /// <summary>
    /// An environment variable is the most process-global thing there is, and
    /// the failure it produces — passes alone, fails under load — is the one
    /// that gets blamed on the code under test. Setting one from an uncollected
    /// class is what falsified <c>NullDeviceTests</c>' own safety claim for a
    /// whole release.
    /// </summary>
    [Fact]
    public void Every_class_that_sets_a_process_environment_variable_is_serialized()
    {
        string[] markers = ["Environment.SetEnvironmentVariable"];

        var offenders = Offenders(markers, Sanctioned);

        Assert.True(offenders.Count == 0,
            "These classes write a process environment variable and are in no serialized collection:\n  " +
            string.Join("\n  ", offenders));
    }

    /// <summary>
    /// <b>The flag every rule above rests on.</b> xunit's own doc for
    /// <c>DisableParallelization</c> reads "determines whether tests in this
    /// collection runs in parallel with any other tests" — so it is exclusion
    /// against the WHOLE run, not merely against the collection's own members.
    /// Without it a collection only serializes itself, every sanctioned
    /// collection can run against every other one, and "put the class in a
    /// collection" stops meaning anything. It was missing from
    /// <c>"NullDevice"</c>, which is why that collection's documented promise
    /// was untrue in two directions at once.
    /// </summary>
    [Fact]
    public void Every_sanctioned_collection_disables_parallelization()
    {
        var definitions = Definitions();

        var missing = Sanctioned
            .Where(n => !definitions.TryGetValue(n, out var attr) ||
                        !attr.Contains("DisableParallelization", StringComparison.Ordinal) ||
                        !attr.Contains("true", StringComparison.Ordinal))
            .ToList();

        Assert.True(missing.Count == 0,
            "These collection definitions do not disable parallelization, so membership in them " +
            "guarantees nothing against the other collections:\n  " + string.Join("\n  ", missing));

        var undefined = Sanctioned.Where(n => !definitions.ContainsKey(n)).ToList();
        Assert.True(undefined.Count == 0,
            "[Collection(\"X\")] with no matching [CollectionDefinition(\"X\")] is not an error to xunit — " +
            "it silently creates a fresh collection with default settings:\n  " + string.Join("\n  ", undefined));
    }

    /// <summary>
    /// <b>A runner configuration that is not copied next to the assembly does
    /// not exist.</b> Nothing in the xunit.v3 build targets copies
    /// <c>xunit.runner.json</c> — the csproj carries an explicit
    /// <c>&lt;Content CopyToOutputDirectory&gt;</c> item for it — and the
    /// failure mode of getting that wrong is silent: every setting reverts to
    /// the xunit default and the file sits unread in the project folder. This
    /// suite ran that way for its whole life, which is the root cause behind
    /// most of the collection findings above.
    /// </summary>
    [Fact]
    public void The_runner_configuration_ships_next_to_the_assembly()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "xunit.runner.json");
        Assert.True(File.Exists(path),
            $"xunit.runner.json is not in the output directory ({AppContext.BaseDirectory}). " +
            "The <Content Include=\"xunit.runner.json\" CopyToOutputDirectory=\"PreserveNewest\" /> item " +
            "in JuceSharp.Tests.csproj is what puts it there; without it the runner reads nothing.");

        var json = File.ReadAllText(path);
        Assert.Contains("\"parallelAlgorithm\": \"conservative\"", json, StringComparison.Ordinal);
        Assert.Contains("\"maxParallelThreads\"", json, StringComparison.Ordinal);

        // Never this one. Turning collection parallelism off globally serialises
        // the whole 2,000-case gate to fix a few dozen classes — the wrong trade,
        // and the parent repo warns against the equivalent.
        Assert.Contains("\"parallelizeTestCollections\": true", json, StringComparison.Ordinal);
    }

    /// <summary>Every <c>[CollectionDefinition("name", …)]</c> in the suite, keyed by name.</summary>
    private static Dictionary<string, string> Definitions()
    {
        var root = TestRoot();
        var found = new Dictionary<string, string>(StringComparer.Ordinal);
        var pattern = new Regex(@"\[CollectionDefinition\(\s*""(?<name>[^""]+)""(?<rest>[^\]]*)\]", RegexOptions.Compiled);

        foreach (var file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(root, file).Replace('\\', '/');
            if (rel.StartsWith("bin/", StringComparison.Ordinal) ||
                rel.StartsWith("obj/", StringComparison.Ordinal)) continue;

            foreach (Match m in pattern.Matches(StripComments(File.ReadAllText(file))))
                found[m.Groups["name"].Value] = m.Groups["rest"].Value;
        }

        return found;
    }

    /// <summary>
    /// Non-vacuity: the parser must actually find the classes it is meant to
    /// judge, and the markers must be present SOMEWHERE. A regex that matched
    /// nothing would pass both tests above for the wrong reason.
    /// </summary>
    [Fact]
    public void The_parser_sees_the_known_multi_class_files()
    {
        var all = Classes().ToList();
        Assert.True(all.Count > 100, $"only {all.Count} classes parsed — the declaration regex has stopped matching.");

        // Both marker sets must actually MATCH something, or the two lints
        // above are green over an empty set.
        var serialized = all.Where(c => c.File != Self && IsTestClass(c)).ToList();
        Assert.Contains(serialized, c => c.Body.Contains("ResetGlobalGateForTests", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("ChildProcessRegistry.", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("LoopbackFileServer", StringComparison.Ordinal));

        // The file the finding named: two top-level classes, the first
        // serialized, the second deliberately not.
        var inFile = all.Where(c => c.File == "VerifiedDownloaderTests.cs").Select(c => c.Name).ToList();
        Assert.Contains("VerifiedDownloaderTests", inFile);
        Assert.Contains("OnnxModelCatalogTests", inFile);

        var downloader = all.Single(c => c.Name == "VerifiedDownloaderTests");
        Assert.True(InCollection(downloader, "StemDownloads"));
        Assert.Contains("LoopbackFileServer", downloader.Body, StringComparison.Ordinal);

        // …and the one the audit flagged really is inert: no transfer, no gate.
        var catalogue = all.Single(c => c.Name == "OnnxModelCatalogTests");
        Assert.False(InCollection(catalogue, "StemDownloads"));
        Assert.DoesNotContain("LoopbackFileServer", catalogue.Body, StringComparison.Ordinal);
        Assert.DoesNotContain("EnsureAsync", catalogue.Body, StringComparison.Ordinal);

        // The four sweeps added for the collection wave: each marker set has to
        // match a real class, and each sanctioned collection has to have members.
        // A typo in any one marker turns its lint green over an empty set.
        Assert.Contains(serialized, c => c.Body.Contains("new JuceAudioEngine(", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("NullDeviceScope", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("GC.GetAllocatedBytesForCurrentThread", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("Process.Start", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("Environment.SetEnvironmentVariable", StringComparison.Ordinal));
        Assert.Contains(serialized, c => c.Body.Contains("VerifiedDownloader.RequestTimeout", StringComparison.Ordinal));

        foreach (var name in Sanctioned)
            Assert.True(serialized.Any(c => InCollection(c, name)),
                $"no test class is in [Collection(\"{name}\")] — either the collection is dead or " +
                "the attribute parser has stopped seeing it, and every lint that names it is vacuous.");

        // The class the finding named: it sets the process env guard through
        // NullDeviceScope and is the reason that guard's safety claim was false.
        var ergonomics = all.Single(c => c.Name == "WrapperErgonomicsTests");
        Assert.Contains("NullDeviceScope", ergonomics.Body, StringComparison.Ordinal);
        Assert.True(InCollection(ergonomics, "NullDevice"));
    }

    private static string StripComments(string source)
    {
        source = Regex.Replace(source, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        source = Regex.Replace(source, @"//[^\n]*", " ");
        return source;
    }
}
