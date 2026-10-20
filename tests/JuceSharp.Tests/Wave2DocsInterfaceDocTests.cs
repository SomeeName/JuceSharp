using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     The <c>IJuce*</c> seams (six when this was written, SEVEN since
///     <c>IJuceMidiOutput</c>) are the type consumers are told to program
///     against, and they shipped with 142 of their 144 members BARE — IntelliSense
///     on the contract blank while the concrete class beside it was fully
///     documented. <c>CS1591</c> was suppressed repo-wide when that happened; it
///     is NOT any more (<c>Directory.Build.props</c> stopped suppressing it in
///     0.9, and <c>PublicApiDocsLintTests</c> fails if the <c>NoWarn</c> comes
///     back), so the compiler now catches the plain case. This lint still earns
///     its place: <c>GenerateDocumentationFile</c> is on, and an
///     <c>&lt;inheritdoc&gt;</c> the compiler accepts is exactly what the shipped
///     XML cannot tell from a bare member.
///     <para>
///         The fix was mechanical — <c>&lt;inheritdoc cref="…"/&gt;</c> onto the
///         concrete counterpart, one source of truth — and this is the lint that
///         keeps it that way, because the NEXT interface is the one that will ship
///         bare otherwise. It reads the SOURCE rather than the generated XML:
///         <c>inheritdoc</c> is not expanded by the compiler, so the XML file says
///         nothing about whether a member was documented on purpose.
///     </para>
/// </summary>
public sealed class Wave2DocsInterfaceDocTests
{
    [Fact]
    public void EveryPublicMemberOfEveryIJuceInterface_CarriesADocComment()
    {
        var files = InterfaceSourceFiles();
        Assert.NotEmpty(files);

        var bare = new List<string>();
        var members = 0;

        foreach (var file in files)
        {
            var found = ScanInterfaceBody(File.ReadAllLines(file), out var undocumented);
            members += found;
            bare.AddRange(undocumented.Select(d => $"{Path.GetFileName(file)}: {d}"));
        }

        Assert.True(members >= 120,
            $"the scanner found only {members} interface members across {files.Count} files — it is "
            + "not reading what it thinks it is reading, so a green result would mean nothing.");

        Assert.True(bare.Count == 0,
            "undocumented public interface members (use /// <inheritdoc cref=\"Concrete.Member\"/>):"
            + Environment.NewLine + string.Join(Environment.NewLine, bare));
    }

    /// <summary>
    ///     Every public <c>IJuce*</c> interface the assembly exposes has a source
    ///     file the lint above actually scans. Without this, adding
    ///     <c>IJuceSomethingNew.cs</c> under a folder the glob misses would make
    ///     the lint pass VACUOUSLY for the one interface that needed it.
    /// </summary>
    [Fact]
    public void EveryPublicIJuceInterface_HasASourceFileTheLintScans()
    {
        var scanned = InterfaceSourceFiles()
            .Select(f => Path.GetFileNameWithoutExtension(f))
            .ToHashSet(StringComparer.Ordinal);

        var declared = typeof(JuceAudioEngine).Assembly
            .GetExportedTypes()
            .Where(t => t.IsInterface && t.Name.StartsWith("IJuce", StringComparison.Ordinal))
            .Select(t => t.Name)
            .ToArray();

        Assert.NotEmpty(declared);
        foreach (var name in declared)
            Assert.Contains(name, scanned);
    }

    /// <summary>
    ///     Non-vacuity. The scanner must actually report a bare member, and must
    ///     not be fooled by an attribute, a section comment or a signature that
    ///     wraps over several lines — all three occur in the real files.
    /// </summary>
    [Fact]
    public void TheScannerTellsADocumentedMemberFromABareOne()
    {
        var source = new[]
        {
            "namespace X;",
            "public interface IJuceProbe",
            "{",
            "    /// <inheritdoc cref=\"Y.A\" />",
            "    int A { get; }",
            "",
            "    // a section comment, not a doc comment",
            "    int B { get; }",
            "",
            "    /// <inheritdoc cref=\"Y.C\" />",
            "    [Obsolete(\"x\")]",
            "    void C(int one,",
            "           int two);",
            "}",
        };

        var count = ScanInterfaceBody(source, out var undocumented);

        Assert.Equal(3, count);
        Assert.Equal(new[] { "int B { get; }" }, undocumented);
    }

    /// <summary>
    ///     Walks one interface body and counts members, reporting the ones with no
    ///     <c>///</c> block immediately above. Deliberately line-based: the repo's
    ///     other doc/wiring lints read source the same way, and pulling in a
    ///     parser for nine files is not worth it.
    /// </summary>
    private static int ScanInterfaceBody(IReadOnlyList<string> lines, out List<string> undocumented)
    {
        undocumented = new List<string>();

        var i = 0;
        while (i < lines.Count && !lines[i].TrimStart().StartsWith("public interface IJuce", StringComparison.Ordinal))
            i++;
        if (i >= lines.Count) return 0;

        while (i < lines.Count && !lines[i].Trim().StartsWith("{", StringComparison.Ordinal)) i++;
        i++;

        var members = 0;
        var documented = false;

        for (; i < lines.Count; i++)
        {
            var text = lines[i].Trim();
            if (text.Length == 0) continue;
            if (text == "}") break;                                              // end of the interface
            if (text.StartsWith("///", StringComparison.Ordinal)) { documented = true; continue; }
            if (text.StartsWith("//", StringComparison.Ordinal)) continue;       // section comment
            if (text.StartsWith("[", StringComparison.Ordinal)) continue;        // attribute

            members++;
            var declaration = text;

            // A declaration ends at ';' (method / event / field) or at '}' (an
            // auto-property's accessor list). Both terminators are needed: a
            // property line ends with '}', so a ';'-only loop would swallow
            // every following member as a continuation and report ONE member
            // per file — which is exactly why the vacuity floor below exists.
            while (!text.EndsWith(";", StringComparison.Ordinal)
                   && !text.EndsWith("}", StringComparison.Ordinal)
                   && i + 1 < lines.Count)
            {
                i++;
                text = lines[i].Trim();
            }

            if (!documented) undocumented.Add(declaration);
            documented = false;
        }

        return members;
    }

    private static List<string> InterfaceSourceFiles()
    {
        var root = FindRepoDirectory(Path.Combine("src", "JuceSharp"));
        return Directory.EnumerateFiles(root, "IJuce*.cs", SearchOption.AllDirectories)
            .OrderBy(p => p, StringComparer.Ordinal)
            .ToList();
    }

    private static string FindRepoDirectory(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative);
            if (Directory.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
