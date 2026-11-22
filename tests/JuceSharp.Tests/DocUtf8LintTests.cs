using System.Text;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// <c>tools/check-doc-links.py</c> reads every doc with Python's default text
/// decoding, which is strict UTF-8 on the CI runners. One stray cp1252 byte
/// (an em dash typed as 0x97) crashed the public CI's last step with a
/// <c>UnicodeDecodeError</c> while every local test stayed green. This asserts
/// the same set of files decodes as strict UTF-8 before it ever reaches CI.
/// </summary>
public sealed class DocUtf8LintTests
{
    [Fact]
    public void EveryDoc_DecodesAsStrictUtf8()
    {
        var root = FindRepoRoot();
        var strict = new UTF8Encoding(false, throwOnInvalidBytes: true);
        var bad = new List<string>();
        foreach (var file in DocFiles(root))
        {
            try
            {
                strict.GetString(File.ReadAllBytes(file));
            }
            catch (DecoderFallbackException e)
            {
                bad.Add($"{Path.GetRelativePath(root, file).Replace('\\', '/')}: {e.Message}");
            }
        }

        Assert.True(bad.Count == 0, "Docs that are not valid UTF-8 (replace cp1252 bytes such as 0x97 with the UTF-8 character):\n" + string.Join("\n", bad));
    }

    private static IEnumerable<string> DocFiles(string root)
    {
        foreach (var name in new[] { "README.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md" })
        {
            var p = Path.Combine(root, name);
            if (File.Exists(p)) yield return p;
        }

        var src = Path.Combine(root, "src");
        if (Directory.Exists(src))
        {
            foreach (var proj in Directory.EnumerateDirectories(src))
            {
                var p = Path.Combine(proj, "README.md");
                if (File.Exists(p)) yield return p;
            }
        }

        var docs = Path.Combine(root, "docs");
        if (Directory.Exists(docs))
        {
            foreach (var f in Directory.EnumerateFiles(docs, "*.md", SearchOption.AllDirectories))
            {
                if (f.Contains($"{Path.DirectorySeparatorChar}node_modules{Path.DirectorySeparatorChar}")) continue;
                yield return f;
            }
        }
    }

    private static string FindRepoRoot()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            if (File.Exists(Path.Combine(d.FullName, "tools", "check-doc-links.py"))) return d.FullName;
        }

        throw new InvalidOperationException($"Could not find tools/check-doc-links.py above {AppContext.BaseDirectory}.");
    }
}
