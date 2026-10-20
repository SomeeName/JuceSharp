using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Attaching a plugin is a check, a graph edit and an attachment-state flip,
/// and the ABI serialises the whole transaction on the JUCE message thread
/// (<c>JuceHost::callSync</c>) precisely so two callers cannot insert one
/// instance twice. DETACHING is the same transaction in reverse, and two of the
/// three detach paths ran it on the CALLER's thread:
/// <c>dca_plugin_detach_from_instrument</c> and the teardown inside
/// <c>teardownPluginAndDelete</c>. A detach could therefore interleave with an
/// attach into the same slot and leave <c>attached</c> /
/// <c>attachedAsInstrument</c> describing a state that never existed.
/// <para>
/// <c>callSync</c> calls straight through when it is already ON the message
/// thread (<c>MessageManager::existsAndIsCurrentThread</c>), so routing these
/// through it cannot deadlock a caller arriving from a JUCE callback — see the
/// D-07 note on <c>callSync</c>.
/// </para>
/// <para>
/// A lint rather than a race test: the interleaving needs two threads to hit a
/// window a few instructions wide inside a message-thread round trip, and a
/// test that only sometimes reproduces it is worse than one that pins the
/// shape.
/// </para>
/// </summary>
public sealed class PluginAttachmentSerialisationLintTests
{
    private const string AbiCpp = "src/JuceSharp.Native/src/dotcharter_audio.cpp";

    [Theory]
    [InlineData("dca_plugin_attach_to_input_for_player")]
    [InlineData("dca_plugin_detach_from_input")]
    [InlineData("dca_plugin_attach_to_instrument")]
    [InlineData("dca_plugin_detach_from_instrument")]
    public void EveryAttachmentStateChange_RunsOnTheMessageThread(string export)
    {
        var body = StripComments(BodyAfter(File.ReadAllText(FindRepoFile(AbiCpp)),
                                           @"DCA_API\s+[\w\*: ]+?\s+DCA_CALL\s+" + Regex.Escape(export) + @"\s*\("));
        Assert.Contains("callSync", body, StringComparison.Ordinal);
    }

    /// <summary>
    /// Teardown counts too: it detaches from both the input chain and the
    /// instrument slot before destroying the instance.
    /// </summary>
    [Fact]
    public void PluginTeardown_DetachesOnTheMessageThread()
    {
        var body = StripComments(BodyAfter(File.ReadAllText(FindRepoFile(AbiCpp)),
                                           @"inline void teardownPluginAndDelete\s*\("));

        // Every detach call must sit inside a callSync lambda. The cheap
        // structural form: the FIRST callSync must come before the first
        // detach.
        var firstSync = body.IndexOf("callSync", StringComparison.Ordinal);
        var firstDetachInput = body.IndexOf("detachInputPlugin", StringComparison.Ordinal);
        var firstDetachInstr = body.IndexOf("detachInstrumentPlugin", StringComparison.Ordinal);

        Assert.True(firstSync >= 0, "teardownPluginAndDelete no longer uses callSync at all");
        Assert.True(firstDetachInput > firstSync,
            "teardownPluginAndDelete detaches the input plugin on the caller's thread, outside the "
            + "message-thread transaction every attach uses");
        Assert.True(firstDetachInstr > firstSync,
            "teardownPluginAndDelete detaches the instrument on the caller's thread, outside the "
            + "message-thread transaction every attach uses");
    }

    /// <summary>Non-vacuity: the extractor finds real bodies, and an export
    /// that legitimately does NOT need the message thread does not contain
    /// callSync — so the assertions above are not tautologies.</summary>
    [Fact]
    public void TheLintReadsRealBodies()
    {
        var src = File.ReadAllText(FindRepoFile(AbiCpp));
        var attach = BodyAfter(src, @"DCA_API\s+[\w\*: ]+?\s+DCA_CALL\s+dca_plugin_attach_to_instrument\s*\(");
        Assert.Contains("attachInstrumentPlugin", attach, StringComparison.Ordinal);

        var gain = BodyAfter(src, @"DCA_API\s+[\w\*: ]+?\s+DCA_CALL\s+dca_engine_set_instrument_gain\s*\(");
        Assert.DoesNotContain("callSync", gain, StringComparison.Ordinal);
    }

    private static string BodyAfter(string src, string pattern)
    {
        var m = Regex.Match(src, pattern);
        Assert.True(m.Success, $"no match for {pattern}");
        var open = src.IndexOf('{', m.Index + m.Length);
        var depth = 0;
        for (var i = open; i < src.Length; i++)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0) return src.Substring(open, i - open + 1);
        }
        throw new InvalidDataException($"unbalanced braces after {pattern}");
    }

    private static string StripComments(string src)
    {
        src = Regex.Replace(src, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        return Regex.Replace(src, @"//[^\n]*", " ");
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
