using System.Text.RegularExpressions;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// W4-C1 and the third bullet of W4-C5, both on <see cref="JuceDspNode" />.
/// Neither has a behavioural expression: the first is a DOC that described a
/// mechanism the code does not have, and the second is a native failure between
/// two calls of a size-then-copy pair that no managed test can provoke. Both
/// assertions are therefore anchored to the member's own brace-matched body
/// rather than to the file.
/// </summary>
public sealed class DspNodeDocAndStatusTests
{
    /// <summary>
    ///     <see cref="JuceDspNode.TypeId" />'s doc said the id recorded at
    ///     creation "is kept only as a fast path and is returned only when the
    ///     engine agrees with it". There is no comparison against the engine's
    ///     answer anywhere and there is no fast path: every read is two P/Invokes
    ///     plus a <see cref="byte" />[], and <c>_createdTypeId</c> is returned in
    ///     exactly ONE case — when the engine is GONE. The second paragraph of
    ///     the same doc described the real behaviour correctly, so the two halves
    ///     contradicted each other and a reader could believe either.
    /// </summary>
    [Fact]
    public void TypeId_ClaimsNoFastPath_AndFallsBackOnlyWhenTheEngineIsGone()
    {
        var source = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/Dsp/JuceDspNode.cs");

        var at = source.IndexOf("public string TypeId", StringComparison.Ordinal);
        Assert.True(at >= 0, "JuceDspNode.TypeId is gone — repoint this lint.");

        // The DOC is everything from its <summary> to the member.
        var doc = source[..at];
        var summary = doc[doc.LastIndexOf("/// <summary>", StringComparison.Ordinal)..];
        Assert.DoesNotContain("only as a fast path", summary, StringComparison.Ordinal);
        Assert.DoesNotContain("when the engine agrees", summary, StringComparison.Ordinal);

        // …and the CODE still has exactly one use of the recorded id, on the
        // engine-gone branch, which is what the doc now says.
        var body = Body(source, at);
        Assert.Single(Regex.Matches(body, @"_createdTypeId"));
        Assert.Matches(new Regex(@"if\s*\(h\s*==\s*nint\.Zero\)\s*return\s+_createdTypeId\s*;"), body);
    }

    /// <summary>
    /// <c>GetString</c>'s size-then-copy pair discarded the COPY call's result,
    /// so a failure between the two (the node removed, the param retired)
    /// decoded a zero-filled buffer and returned <c>""</c> — which is a
    /// legitimate value for a string param, i.e. "unset". The sizing call's
    /// negative answer was already reported; the second one now is too.
    /// </summary>
    [Fact]
    public void GetString_ChecksBothHalvesOfItsSizeThenCopyPair()
    {
        var source = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/Dsp/JuceDspNode.cs");

        var at = source.IndexOf("public string GetString(", StringComparison.Ordinal);
        Assert.True(at >= 0, "JuceDspNode.GetString is gone — repoint this lint.");
        var body = Body(source, at);

        var calls = Regex.Matches(body, @"NativeMethods\.EngineDspGetNodeString\(");
        Assert.Equal(2, calls.Count);
        // Neither result may be dropped on the floor: every call's value is bound.
        Assert.Equal(2, Regex.Matches(body, @"var \w+ = NativeMethods\.EngineDspGetNodeString\(").Count);
        Assert.Matches(new Regex(@"if\s*\(copied\s*<\s*0\)"), body);
    }

    private static string Body(string source, int at)
    {
        var open = source.IndexOf('{', at);
        var depth = 0;
        for (var i = open; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            else if (source[i] == '}' && --depth == 0) return source[open..(i + 1)];
        }

        throw new InvalidOperationException("Unbalanced braces.");
    }
}
