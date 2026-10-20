using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// Packaging invariants. These are read off the project files rather than off
/// a produced <c>.nupkg</c> because running <c>dotnet pack</c> from inside the
/// test host is both slow and self-referential — but every one of them is a
/// fault that shipped, and each was verified once against a real package.
/// </summary>
public sealed class PackagingLintTests
{
    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "JuceSharp.sln")))
            dir = dir.Parent;
        Assert.NotNull(dir);
        return dir!.FullName;
    }

    private static IEnumerable<(string Path, XDocument Doc)> Projects(string subdir)
    {
        var root = Path.Combine(RepoRoot(), subdir);
        // The native tree's CMake output (build/, _deps/) and third_party/ carry
        // vendored csprojs (rubberband-sharp) that are not ours to lint.
        static bool IsVendoredOrBuildOutput(string f)
        {
            var rel = f.Replace('\\', '/');
            return rel.Contains("/build/") || rel.Contains("/_deps/") || rel.Contains("/third_party/")
                || rel.Contains("/bin/") || rel.Contains("/obj/");
        }

        foreach (var f in Directory.EnumerateFiles(root, "*.csproj", SearchOption.AllDirectories)
                                   .Where(f => !IsVendoredOrBuildOutput(f))
                                   .OrderBy(f => f, StringComparer.Ordinal))
        {
            yield return (f, XDocument.Load(f));
        }
    }

    private static string? Prop(XDocument doc, string name) =>
        doc.Descendants(name).Select(e => e.Value).FirstOrDefault();

    /// <summary>
    /// <c>src/JuceSharp/JuceSharp.csproj</c> declared
    /// <c>Version 0.8.0</c> beside <c>AssemblyVersion 0.4.0.0</c>, and the
    /// four other packable projects declared a PackageId with no version at
    /// all — which packs as 1.0.0. One <c>Version</c> in Directory.Build.props
    /// is the fix; a csproj that re-declares it has escaped again.
    /// </summary>
    [Fact]
    public void No_project_declares_its_own_version()
    {
        foreach (var (path, doc) in Projects("src").Concat(Projects("examples")).Concat(Projects("tests")))
        {
            foreach (var name in new[] { "Version", "AssemblyVersion", "FileVersion", "PackageVersion" })
                Assert.True(Prop(doc, name) is null,
                    $"{Path.GetFileName(path)} declares <{name}>; the single source of truth is Directory.Build.props.");
        }
    }

    [Fact]
    public void The_repository_root_pins_exactly_one_version_and_the_shared_metadata()
    {
        var props = XDocument.Load(Path.Combine(RepoRoot(), "Directory.Build.props"));

        // The INVARIANT, not the literal. This read Assert.Equal("0.8.0", …)
        // and therefore turned the required 0.9 bump RED — a lint that has to be
        // edited to ship a release teaches the next agent to "fix" the version
        // back. What matters is that exactly one version exists, that it is a
        // parseable SemVer, and that CMakeLists.txt agrees with it (the native
        // project VERSION feeds dca_version_string, so a disagreement means the
        // DLL reports a version the package does not have).
        var version = Prop(props, "Version");
        Assert.False(string.IsNullOrWhiteSpace(version));
        Assert.True(Version.TryParse(version, out _), $"<Version>{version}</Version> is not parseable.");
        Assert.Single(props.Descendants("Version"));

        var cmake = File.ReadAllText(Path.Combine(RepoRoot(), "src", "JuceSharp.Native", "CMakeLists.txt"));
        var declared = Regex.Match(cmake, @"^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$", RegexOptions.Multiline);
        Assert.True(declared.Success, "CMakeLists.txt declares no project VERSION.");
        Assert.Equal(version, declared.Groups[1].Value);

        Assert.Equal("git", Prop(props, "RepositoryType"));
        Assert.NotNull(Prop(props, "Authors"));
        Assert.NotNull(Prop(props, "Company"));
        Assert.NotNull(Prop(props, "Copyright"));
        Assert.NotNull(Prop(props, "PackageTags"));
        Assert.Equal("true", Prop(props, "Deterministic"));
        Assert.Equal("true", Prop(props, "EmbedUntrackedSources"));
        Assert.Equal("true", Prop(props, "PublishRepositoryUrl"));
        Assert.Equal("true", Prop(props, "IncludeSymbols"));
        Assert.Equal("snupkg", Prop(props, "SymbolPackageFormat"));
        Assert.Contains("GITHUB_ACTIONS",
            props.Descendants("ContinuousIntegrationBuild").Single().Attribute("Condition")!.Value,
            StringComparison.Ordinal);
    }

    /// <summary>
    /// Every csproj pointed <c>RepositoryUrl</c> at
    /// <c>github.com/SomeeName/DotCharter</c> — the CONSUMER of this
    /// submodule. SourceLink would have resolved every source file against the
    /// wrong repository, and the package page would have linked there too.
    /// </summary>
    [Fact]
    public void The_repository_url_names_this_repo_and_not_its_consumer()
    {
        var props = XDocument.Load(Path.Combine(RepoRoot(), "Directory.Build.props"));
        var url = Prop(props, "RepositoryUrl");

        Assert.Equal("https://github.com/SomeeName/JuceSharp", url);
        Assert.Equal(url, Prop(props, "PackageProjectUrl"));

        foreach (var (path, doc) in Projects("src"))
            Assert.True(Prop(doc, "RepositoryUrl") is null,
                $"{Path.GetFileName(path)} overrides RepositoryUrl.");
    }

    /// <summary>
    /// The bug: the <c>None</c> item for the native payload carried no
    /// <c>Pack</c> attribute and the catch-all item below it EXCLUDED
    /// <c>*.dll</c>/<c>*.exe</c>, so <c>dotnet pack</c> produced a package
    /// containing <c>lib/net10.0/JuceSharp.dll</c> and nothing else — every
    /// P/Invoke in it threw <c>DllNotFoundException</c>. Verified against a
    /// real nupkg: <c>runtimes/win-x64/native/juce_sharp.dll</c> (9,492,480 B)
    /// and <c>juce_vst_host.exe</c> (5,645,312 B) are now inside it.
    /// </summary>
    [Fact]
    public void The_wrapper_packs_its_native_payload_and_still_copies_it_to_output()
    {
        var csproj = Path.Combine(RepoRoot(), "src", "JuceSharp", "JuceSharp.csproj");
        var doc = XDocument.Load(csproj);

        var native = doc.Descendants("None").Single(e =>
            (e.Attribute("Include")?.Value ?? string.Empty).Contains(@"runtimes\win-x64\native\*.dll", StringComparison.Ordinal));

        Assert.Equal("true", native.Attribute("Pack")?.Value);
        Assert.Equal(@"runtimes\win-x64\native\", native.Attribute("PackagePath")?.Value);
        // Route 1 — a ProjectReference consumer gets them beside its exe.
        Assert.Equal("PreserveNewest", native.Attribute("CopyToOutputDirectory")?.Value);
        Assert.Contains(@"runtimes\win-x64\native\*.exe", native.Attribute("Include")!.Value, StringComparison.Ordinal);

        // The catch-all item must keep excluding them, or they would be packed
        // TWICE (once as a RID asset, once as loose content).
        var catchAll = doc.Descendants("None").Single(e =>
            (e.Attribute("Include")?.Value ?? string.Empty) == @"runtimes\**\*.*");
        var exclude = catchAll.Attribute("Exclude")!.Value;
        Assert.Contains(@"runtimes\win-x64\native\*.dll", exclude, StringComparison.Ordinal);
        Assert.Contains(@"runtimes\win-x64\native\*.exe", exclude, StringComparison.Ordinal);
        // …and it must not drag runtimes/.gitignore in (NU5119).
        Assert.Contains(".gitignore", exclude, StringComparison.Ordinal);

        // W5-P2: the catch-all must declare WHERE it packs. Pack="true" with no
        // PackagePath does not preserve the layout — NuGet falls back to
        // ContentTargetFolders and the file leaves the RID folder — and <Link>
        // is build-output only. Only the EXCLUDE was checked before, so the
        // first non-.dll/.exe payload deployed beside the DLL would have left
        // the package silently.
        var catchAllPath = catchAll.Attribute("PackagePath")?.Value;
        Assert.False(string.IsNullOrEmpty(catchAllPath),
            @"the runtimes\**\*.* item packs with no PackagePath, so its payload lands under "
            + "content/ instead of runtimes/ — see W5-P2.");
        Assert.StartsWith("runtimes", catchAllPath, StringComparison.Ordinal);

        // W5-P6.1: the EXE half must reach the SAME folder as the DLL half. The
        // Include string was all that was checked, so splitting the item and
        // giving the exe a different PackagePath would have kept this green
        // while juce_vst_host.exe stopped being co-located with juce_sharp.dll
        // for every NuGet consumer — which is the whole of the engine's "the
        // child sits next to the DLL" contract.
        var exeItems = doc.Descendants("None").Where(e =>
            (e.Attribute("Include")?.Value ?? string.Empty)
                .Contains(@"runtimes\win-x64\native\*.exe", StringComparison.Ordinal)).ToList();
        Assert.Single(exeItems);
        Assert.Equal(@"runtimes\win-x64\native\", exeItems[0].Attribute("PackagePath")?.Value);
    }

    /// <summary>
    /// The analyzer configuration was entirely IMPLICIT: nothing declared
    /// <c>EnableNETAnalyzers</c>, <c>AnalysisLevel</c> or <c>AnalysisMode</c>,
    /// so the rule set gating this repo tracked the target framework and the
    /// installed SDK and moved on its own — and no warning was ever an error.
    ///
    /// <para>
    /// The level is the highest one that is CLEAN across the solution without a
    /// code change; the numbers behind the choice are in Directory.Build.props.
    /// This test exists because the failure mode is silent: a project (or a
    /// well-meant "fix the build" commit) sets <c>AnalysisMode=None</c> or drops
    /// <c>TreatWarningsAsErrors</c>, and everything still compiles.
    /// </para>
    /// </summary>
    [Fact]
    public void The_static_analysis_settings_are_declared()
    {
        var props = XDocument.Load(Path.Combine(RepoRoot(), "Directory.Build.props"));

        Assert.Equal("true", Prop(props, "EnableNETAnalyzers"));
        // PINNED, never floating. This asserted `latest` until W5-P1, which is
        // the lint PINNING THE HAZARD: the value that lets a newer SDK's new CA
        // rules break every contributor's build under TreatWarningsAsErrors, and
        // fixing the hazard turned the gate red. A numeric level is the whole
        // requirement; which number is a deliberate, reviewable bump.
        var level = Prop(props, "AnalysisLevel");
        Assert.NotNull(level);
        Assert.True(Version.TryParse(level, out _),
            $"<AnalysisLevel> is '{level}'; it must be a pinned numeric level (e.g. 10.0), "
            + "never 'latest'/'preview' — those float with the installed SDK.");
        Assert.Equal("Default", Prop(props, "AnalysisMode"));
        // The two categories measured clean at All. Dropping either is a
        // silent loss of coverage, so they are named, not merely non-None.
        Assert.Equal("All", Prop(props, "AnalysisModeDocumentation"));
        Assert.Equal("All", Prop(props, "AnalysisModeSingleFile"));
        Assert.Equal("true", Prop(props, "TreatWarningsAsErrors"));

        // …and no project may quietly opt itself out of any of it. The whole
        // solution is at 0 warnings; an exemption here would be a hidden one.
        foreach (var (path, doc) in Projects("src").Concat(Projects("examples")).Concat(Projects("tests")))
            foreach (var name in new[]
                     {
                         "EnableNETAnalyzers", "AnalysisLevel", "AnalysisMode",
                         "AnalysisModeDocumentation", "AnalysisModeSingleFile",
                         "TreatWarningsAsErrors", "NoWarn", "WarningLevel",
                     })
                Assert.True(Prop(doc, name) is null,
                    $"{Path.GetFileName(path)} overrides <{name}>; static analysis is configured in Directory.Build.props only.");
    }

    /// <summary>
    /// <b>A <c>PackAsTool</c> project must NOT produce a symbol package
    /// (W5-P5).</b> A <c>.snupkg</c> may only carry PDBs under <c>lib/</c> and
    /// <c>runtimes/</c>; a tool's payload lives under <c>tools/net10.0/any/</c>.
    /// <para>
    /// The root props turns <c>IncludeSymbols</c> + <c>SymbolPackageFormat</c>
    /// on for the library packages, and the tool inherited it. Measured with a
    /// real <c>dotnet pack</c>: it SUCCEEDS and emits a 283 KB snupkg containing
    /// nothing but <c>tools/net10.0/any/*.pdb</c> — so the failure is at PUSH
    /// time, where nuget.org's symbol server rejects the layout, long after a
    /// green build. That is exactly why it cannot be a build assertion, and why
    /// this lint reads the DECISION instead.
    /// </para>
    /// </summary>
    [Fact]
    public void The_tool_package_does_not_try_to_ship_a_symbol_package()
    {
        var root = RepoRoot();
        // The library default is ON and stays on.
        var props = XDocument.Load(Path.Combine(root, "Directory.Build.props"));
        Assert.Equal("true", Prop(props, "IncludeSymbols"));
        Assert.Equal("snupkg", Prop(props, "SymbolPackageFormat"));

        var tools = Projects("src")
            .Where(p => string.Equals(Prop(p.Doc, "PackAsTool"), "true", StringComparison.OrdinalIgnoreCase))
            .ToList();

        // …and the sweep must actually have found one, or this is vacuous.
        Assert.NotEmpty(tools);

        foreach (var (path, doc) in tools)
            Assert.True(string.Equals(Prop(doc, "IncludeSymbols"), "false", StringComparison.OrdinalIgnoreCase),
                $"{Path.GetFileName(path)} is a PackAsTool project and inherits IncludeSymbols=true; "
                + "its .snupkg would contain tools/**/*.pdb, which nuget.org's symbol server rejects "
                + "at PUSH time (W5-P5). PackAsTool already ships those PDBs inside the tool package.");
    }

    /// <summary>
    /// <b>A nested <c>Directory.Build.props</c> STOPS MSBuild's upward search.</b>
    /// <c>tests/</c> and <c>examples/</c> each have one, and each re-imports the
    /// root explicitly with <c>GetPathOfFileAbove</c>. Delete that one line and
    /// both trees silently lose <c>Version</c>, all six analysis properties,
    /// <c>Deterministic</c>, the SourceLink knobs and the licence metadata —
    /// and EVERY existing packaging assertion still passes, because they read
    /// the root props and the csprojs and neither of those changed (W5-P6.4).
    /// <para>
    /// The other half is the split those files EXIST for:
    /// <c>GenerateDocumentationFile</c> is true at the root (which is what makes
    /// CS1591 an error on the six packable projects) and false in both nested
    /// files. Nothing pinned that either, so "docs are enforced where they ship
    /// and nowhere else" was an unguarded convention (W5-P6.5).
    /// </para>
    /// </summary>
    [Fact]
    public void The_nested_props_files_re_import_the_root_and_keep_the_docs_split()
    {
        var root = RepoRoot();
        Assert.Equal("true", Prop(XDocument.Load(Path.Combine(root, "Directory.Build.props")),
                                  "GenerateDocumentationFile"));

        foreach (var dir in new[] { "tests", "examples" })
        {
            var path = Path.Combine(root, dir, "Directory.Build.props");
            var doc = XDocument.Load(path);

            var imports = doc.Descendants("Import")
                             .Select(e => e.Attribute("Project")?.Value ?? string.Empty)
                             .ToList();
            Assert.Contains(imports, i =>
                i.Contains("GetPathOfFileAbove", StringComparison.Ordinal)
                && i.Contains("Directory.Build.props", StringComparison.Ordinal));

            // The split itself.
            Assert.Equal("false", Prop(doc, "GenerateDocumentationFile"));
            Assert.Equal("false", Prop(doc, "IsPackable"));
        }
    }

    /// <summary>
    /// <b>The packages declared <c>MIT</c> while shipping a DLL that statically
    /// links AGPLv3 JUCE and GPLv2 Rubber Band.</b>
    ///
    /// <para>
    /// A consumer reading "MIT" off nuget.org was told the opposite of the truth
    /// about the binary they were about to redistribute. Five of the six now
    /// declare <c>PackageLicenseFile</c> and ship the repository LICENSE, whose
    /// native-binaries statement names each linked component. The sixth,
    /// <c>JuceSharp.Dsp</c>, is genuinely MIT: pure managed code, no
    /// ProjectReference, no PackageReference, no path to the native DLL.
    /// </para>
    /// <para>
    /// This asserts the FORM (file, not expression), that the file is actually
    /// packed, and that it NAMES each component — a licence file that has
    /// quietly lost the Rubber Band row is the same fault with a green test.
    /// </para>
    /// </summary>
    [Fact]
    public void The_packages_that_carry_native_code_ship_the_third_party_licence()
    {
        var root = RepoRoot();
        var props = XDocument.Load(Path.Combine(root, "Directory.Build.props"));

        // The default for the whole tree is the FILE form…
        Assert.Equal("LICENSE", Prop(props, "PackageLicenseFile"));
        // …and the expression must be gone from the shared metadata, or the two
        // collide (NU5035) and the one that wins is whichever the SDK reads last.
        Assert.Null(Prop(props, "PackageLicenseExpression"));

        // …and something must actually PUT the file in the package. NU5030 is
        // the only thing that catches its absence, and only at pack time.
        var targets = XDocument.Load(Path.Combine(root, "Directory.Build.targets"));
        var licenceItem = targets.Descendants("None").Single(e =>
            (e.Attribute("Include")?.Value ?? string.Empty).EndsWith("LICENSE", StringComparison.Ordinal));
        Assert.Equal("true", licenceItem.Attribute("Pack")?.Value);
        // BOTH halves of the condition (W5-P6.3). Only "IsPackable" was checked,
        // so dropping the `and '$(PackageLicenseFile)' != ''` half would leave
        // JuceSharp.Dsp — the one project that clears the file and declares an
        // MIT EXPRESSION instead — carrying a licence FILE and an expression at
        // once: NU5035 at pack time, with a green gate right up to it.
        var condition = licenceItem.Parent!.Attribute("Condition")!.Value;
        Assert.Contains("IsPackable", condition, StringComparison.Ordinal);
        Assert.Contains("PackageLicenseFile", condition, StringComparison.Ordinal);

        // Every component CMake links or vendors has to be named in the file a
        // consumer is handed.
        var licence = File.ReadAllText(Path.Combine(root, "LICENSE"));
        foreach (var component in new[]
                 {
                     "JUCE", "AGPLv3", "Rubber Band", "GPLv2", "VST 3 SDK",
                     "ASIO SDK", "NeuralAudio", "OnnxRuntime",
                 })
            Assert.Contains(component, licence, StringComparison.Ordinal);

        // The packed README carries the short version, because nobody opens a
        // LICENSE file before taking a dependency.
        var readme = File.ReadAllText(Path.Combine(root, "src", "JuceSharp", "README.md"));
        Assert.Contains("AGPLv3", readme, StringComparison.Ordinal);
        Assert.Contains("GPLv2", readme, StringComparison.Ordinal);

        // Exactly ONE project may opt back out to the MIT expression, and only
        // because it references nothing at all.
        var mit = new List<string>();
        foreach (var (path, doc) in Projects("src"))
        {
            if (Prop(doc, "PackageLicenseExpression") is null) continue;
            mit.Add(Path.GetFileNameWithoutExtension(path));

            Assert.Empty(doc.Descendants("ProjectReference"));
            Assert.Empty(doc.Descendants("PackageReference"));
            // …and it must CLEAR the inherited file, or pack fails NU5035.
            Assert.Equal(string.Empty, Prop(doc, "PackageLicenseFile"));
        }

        Assert.Equal(new[] { "JuceSharp.Dsp" }, mit.ToArray());
    }

    /// <summary>
    /// The python sidecar's scripts were packed but never DELIVERED: the item
    /// carried <c>PackagePath</c> and no <c>PackageCopyToOutput</c>, which
    /// produces
    /// <c>&lt;files include="any/any/PythonScripts/separate_track.py" buildAction="None" /&gt;</c>
    /// in the nuspec — linked into the consuming project and copied nowhere, so
    /// every NuGet consumer's sidecar died on a missing script while every
    /// ProjectReference consumer in this repo was fine.
    ///
    /// <para>
    /// This reads BOTH the project file and, when one has been generated, the
    /// real <c>obj/**/JuceSharp.StemSeparation.nuspec</c> — the artefact the
    /// bug actually lived in. Measured after the fix:
    /// <c>copyToOutput="true" flatten="false"</c> on both rows.
    /// </para>
    /// </summary>
    [Fact]
    public void The_python_sidecar_scripts_are_packed_so_a_consumer_gets_them()
    {
        var projectDir = Path.Combine(RepoRoot(), "src", "JuceSharp.StemSeparation");
        var doc = XDocument.Load(Path.Combine(projectDir, "JuceSharp.StemSeparation.csproj"));

        var scripts = doc.Descendants("None").Single(e =>
            (e.Attribute("Include")?.Value ?? string.Empty).Contains("PythonScripts", StringComparison.Ordinal));

        // Metadata is legal as an attribute or as a child element; read both.
        string? Meta(XElement item, string name) =>
            item.Attribute(name)?.Value
            ?? item.Elements().FirstOrDefault(c => c.Name.LocalName == name)?.Value;

        Assert.Equal("true", Meta(scripts, "Pack"));
        Assert.Equal("PreserveNewest", Meta(scripts, "CopyToOutputDirectory"));
        Assert.Contains("contentFiles", Meta(scripts, "PackagePath") ?? string.Empty, StringComparison.Ordinal);
        Assert.Equal("true", Meta(scripts, "PackageCopyToOutput"));
        // Two same-named scripts must not be flattened on top of each other,
        // and the code resolves them under a PythonScripts\ folder.
        Assert.Equal("false", Meta(scripts, "PackageFlatten"));

        // …and the same claim read off the generated nuspec, which is where the
        // fault was visible. Absent (never packed in this tree) is not a pass:
        // the project-file half above already fails without the fix.
        var objDir = Path.Combine(projectDir, "obj");
        if (!Directory.Exists(objDir)) return;

        foreach (var nuspec in Directory.EnumerateFiles(objDir, "JuceSharp.StemSeparation.nuspec", SearchOption.AllDirectories))
        {
            var rows = XDocument.Load(nuspec).Descendants()
                                .Where(e => e.Name.LocalName == "files")
                                .Where(e => (e.Attribute("include")?.Value ?? string.Empty)
                                            .Contains("PythonScripts", StringComparison.Ordinal))
                                .ToList();
            Assert.NotEmpty(rows);
            Assert.All(rows, r => Assert.Equal("true", r.Attribute("copyToOutput")?.Value));
        }
    }

    [Fact]
    public void The_wrapper_package_carries_a_readme()
    {
        var csprojPath = Path.Combine(RepoRoot(), "src", "JuceSharp", "JuceSharp.csproj");
        var doc = XDocument.Load(csprojPath);

        Assert.Equal("README.md", Prop(doc, "PackageReadmeFile"));
        Assert.Contains(doc.Descendants("None"), e =>
            e.Attribute("Include")?.Value == "README.md" && e.Attribute("Pack")?.Value == "true");
        Assert.True(File.Exists(Path.Combine(Path.GetDirectoryName(csprojPath)!, "README.md")));
    }

    [Fact]
    public void The_diagnostics_cli_ships_as_a_dotnet_tool()
    {
        var doc = XDocument.Load(Path.Combine(RepoRoot(), "src", "JuceSharp.Diagnostics", "JuceSharp.Diagnostics.csproj"));

        Assert.Equal("JuceSharp.Diagnostics", Prop(doc, "PackageId"));
        Assert.Equal("true", Prop(doc, "PackAsTool"));
        Assert.Equal("juce-stems-diag", Prop(doc, "ToolCommandName"));
        // The tool command and the assembly name must agree, or `dotnet tool
        // install` produces a shim that cannot find its entry assembly.
        Assert.Equal(Prop(doc, "ToolCommandName"), Prop(doc, "AssemblyName"));
    }

    [Fact]
    public void Tests_and_examples_are_never_packable()
    {
        foreach (var folder in new[] { "tests", "examples" })
        {
            var props = XDocument.Load(Path.Combine(RepoRoot(), folder, "Directory.Build.props"));
            Assert.Equal("false", Prop(props, "IsPackable"));
        }
    }

    /// <summary>
    /// A harness called <c>StemSmokeTest</c> was removed from the tree long
    /// ago; the Onnx project still granted it internals access, which reads to
    /// the next maintainer as "something out there depends on these
    /// internals".
    /// </summary>
    [Fact]
    public void No_project_grants_internals_to_an_assembly_that_does_not_exist()
    {
        var known = new HashSet<string>(StringComparer.Ordinal)
        {
            // In this repo.
            "JuceSharp", "JuceSharp.Tests", "JuceSharp.AudioIo", "JuceSharp.Dsp",
            "JuceSharp.StemSeparation", "JuceSharp.StemSeparation.Onnx",
            "juce-stems-diag",
            // In the consuming DotCharter repo — deliberate; the exact set is
            // pinned by Internals_are_granted_to_the_consumer_repo_exactly_once.
            "DotCharter.Tests",
        };

        foreach (var (path, doc) in Projects("src"))
        {
            foreach (var ivt in doc.Descendants("InternalsVisibleTo"))
            {
                var name = ivt.Attribute("Include")!.Value;
                Assert.True(known.Contains(name),
                    $"{Path.GetFileName(path)} grants internals to unknown assembly '{name}'.");
            }
        }
    }

    /// <summary>
    /// These packages ship UNSIGNED, so an <c>InternalsVisibleTo</c> naming an
    /// assembly outside this repo is a grant anyone can claim by naming their
    /// own assembly the same thing — a deliberate coupling, never a protection.
    /// Three of the four such grants were dead when checked on 2026-09-11:
    /// <list type="bullet">
    ///   <item><c>JuceSharp → DotCharter.Tests</c> — every hit in the consumer
    ///   was a comment, or <c>DropTuneNativeMirrorTests</c> reading
    ///   <c>NativeMethods.cs</c> off DISK, which needs no access.</item>
    ///   <item><c>JuceSharp.StemSeparation → DotCharter.Tests</c> — the only
    ///   mention was the STRING "ResetGlobalGateForTests" in a doc-link
    ///   allow-list.</item>
    ///   <item><c>JuceSharp.StemSeparation.Onnx → DotCharter.Services</c> —
    ///   <c>OnnxStemSeparationService</c> uses the PUBLIC five-argument
    ///   <c>OnnxStemSeparator</c> constructor, not the internal one.</item>
    /// </list>
    /// The one that survives is real:
    /// <c>DotCharter.Tests/Services/Audio/HtdemucsChunkScratchTests.cs</c>
    /// constructs the internal <c>HtdemucsChunkScratch</c>.
    ///
    /// <para>
    /// This test LISTS the set rather than bounding it, so re-adding a consumer
    /// grant is a deliberate edit here with the using site named, not a line
    /// that drifts back in beside a feature.
    /// </para>
    /// </summary>
    [Fact]
    public void Internals_are_granted_to_the_consumer_repo_exactly_once()
    {
        var grants = new List<string>();

        foreach (var (path, doc) in Projects("src"))
            foreach (var ivt in doc.Descendants("InternalsVisibleTo"))
            {
                var name = ivt.Attribute("Include")!.Value;
                if (name.StartsWith("JuceSharp", StringComparison.Ordinal) ||
                    name.StartsWith("juce-", StringComparison.Ordinal))
                    continue;   // in-repo; those are ordinary friend assemblies.
                grants.Add($"{Path.GetFileNameWithoutExtension(path)} -> {name}");
            }

        Assert.Equal(
            new[] { "JuceSharp.StemSeparation.Onnx -> DotCharter.Tests" },
            grants.OrderBy(g => g, StringComparer.Ordinal).ToArray());
    }
}
