using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Three ABI conventions this header states and that nothing mechanical
/// enforced, plus the one build-ordering fault behind them in spirit.
/// <para>
/// These are source lints because the behaviour they pin is only observable
/// through a DEAD handle, and a managed test cannot manufacture one safely: the
/// wrapper's whole design is that you never hold a native pointer after
/// disposal, and the ABI is <c>extern "C"</c>, so "call it with a garbage
/// handle" is undefined behaviour rather than a test.
/// </para>
/// </summary>
public sealed class AbiSentinelAndAliasTests
{
    private const string AbiCpp = "src/JuceSharp.Native/src/dotcharter_audio.cpp";
    private const string AbiHeader = "src/JuceSharp.Native/include/dotcharter_audio.h";
    private const string NativeCMake = "src/JuceSharp.Native/CMakeLists.txt";

    /// <summary>
    /// W4-N8. Every value-returning rig export answers -1 for "bad handle / bad
    /// seat", so 0 can mean what it says. Five of them answered 0 for both,
    /// which made a disposed engine indistinguishable from an empty pedalboard.
    /// </summary>
    [Theory]
    [InlineData("dca_engine_dsp_node_count_for_player")]
    [InlineData("dca_engine_dsp_get_node_ids_for_player")]
    [InlineData("dca_engine_dsp_read_node_meters")]
    [InlineData("dca_engine_dsp_get_chain_latency_for_player")]
    public void AValueReturningExport_UsesMinusOneAsItsFailureSentinel(string export)
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));

        // LINE-based, not a single regex: the rejection reads
        //   if (!e || !dotcharter::AudioEngine::validPlayer((int) player)) return -1;
        // and a `[^)]*` cannot span the nested parens of `((int) player)`, which
        // is what the first version of this lint got wrong — it failed against
        // the CORRECT source, which is at least the honest direction.
        var reject = RejectionValue(body);
        Assert.True(reject is not null, $"{export} has no recognisable bad-handle rejection");

        Assert.True(reject == "-1",
            $"{export} returns '{reject}' for a bad handle or a bad seat. "
            + "The sentinel for every value-returning export in this ABI is -1, so that 0 can stay "
            + "a legal ANSWER — a caller polling a rig must be able to tell a dead engine from an "
            + "empty board. The one deliberate exception is dca_engine_dsp_get_metering, which is "
            + "a predicate; see APredicateExport_KeepsZero.");
    }

    /// <summary>
    /// ...and the exception, kept honest from the other side: a lint that only
    /// said "-1 everywhere" would have been satisfied by breaking this one.
    /// <c>dca_engine_dsp_get_metering</c> is read as <c>!= 0</c> by the wrapper
    /// (<c>JuceDspChain</c> twice, <c>JuceDspMeters</c> once), and -1 is
    /// non-zero: a dead handle would report "metering is ON".
    /// </summary>
    [Fact]
    public void APredicateExport_KeepsZero()
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)),
                                                  "dca_engine_dsp_get_metering"));
        Assert.Equal("0", RejectionValue(body));

        // ...and the header says WHY, so the next reader does not "fix" it.
        // The DECLARATION, not the first mention: the rule paragraph three
        // hundred lines above also names this export (as the exception), and
        // matching that instead would let the note beside the declaration
        // disappear unnoticed.
        var header = File.ReadAllText(FindRepoFile(AbiHeader));
        var decl = Regex.Match(header, @"^DCA_API[^\n]*dca_engine_dsp_get_metering\s*\(",
                               RegexOptions.Multiline);
        Assert.True(decl.Success, "dca_engine_dsp_get_metering is no longer declared in the header");
        var preamble = header[Math.Max(0, decl.Index - 700)..decl.Index];
        Assert.Contains("PREDICATE", preamble, StringComparison.Ordinal);
    }

    /// <summary>
    /// W4-N10. A pre-v28 export is a ONE-LINE ALIAS for seat 0 and the guard
    /// lives in the export it delegates to — the convention the v35 aliases
    /// state in as many words. The four capture exports were full COPIES of
    /// their <c>_for_player</c> twins instead: two independent drift surfaces
    /// for one behaviour, and the copies left <c>*out_written</c> untouched on
    /// every failure path.
    /// </summary>
    [Theory]
    [InlineData("dca_engine_read_input_capture")]
    [InlineData("dca_engine_read_input_capture_stream")]
    [InlineData("dca_engine_read_input_capture_stream2")]
    [InlineData("dca_engine_get_input_capture_sync")]
    public void ASeatZeroExport_IsAPureAliasForItsForPlayerTwin(string export)
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));

        Assert.Contains(export + "_for_player", body, StringComparison.Ordinal);

        // "Pure" means it does not re-implement the guards: no reinterpret_cast
        // to the engine, no impl call of its own.
        Assert.DoesNotContain("reinterpret_cast", body, StringComparison.Ordinal);
        Assert.DoesNotContain("e->impl.", body, StringComparison.Ordinal);

        // One statement. `return f(...);` and nothing else.
        var statements = body.Count(c => c == ';');
        Assert.True(statements == 1,
            $"{export} has {statements} statements; a seat-0 alias is one `return "
            + $"{export}_for_player(handle, 0, ...);` and the guards live in the twin");
    }

    /// <summary>
    /// ...and the twins zero every out-param BEFORE the first possible failure,
    /// which is the rule the copies broke and which
    /// <c>dca_engine_dsp_get_param</c> states verbatim. Without this the alias
    /// change above would have made the failure paths quietly WORSE, because
    /// there would be one implementation of the omission instead of two.
    /// </summary>
    [Theory]
    [InlineData("dca_engine_read_input_capture_for_player", "out_written")]
    [InlineData("dca_engine_read_input_capture_stream_for_player", "out_written")]
    [InlineData("dca_engine_read_input_capture_stream2_for_player", "out_written")]
    [InlineData("dca_engine_read_input_capture_stream2_for_player", "out_stream_position")]
    [InlineData("dca_engine_get_master_limiter", "out_enabled")]
    public void AnOutParam_IsZeroedBeforeTheFirstPossibleFailure(string export, string param)
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));

        var zero = Regex.Match(body, @"if\s*\(\s*" + Regex.Escape(param) + @"[^)]*\)\s*\*"
                                     + Regex.Escape(param) + @"\s*=\s*0");
        Assert.True(zero.Success,
            $"{export} never zeroes *{param}. Every out-param in this ABI is zeroed before the "
            + "first possible failure, so a caller that discards the status is no worse off than "
            + "it was — see dca_engine_dsp_get_param, which states it, and the thirteen v33/v34 "
            + "getters that honour it.");

        var firstReturn = body.IndexOf("return DCA_ERR", StringComparison.Ordinal);
        Assert.True(firstReturn < 0 || zero.Index < firstReturn,
            $"{export} zeroes *{param} AFTER a path that can already have returned — on that path "
            + "the caller's variable still holds whatever its stack held");
    }

    /// <summary>
    /// W4-N21. <c>JUCESHARP_CORE_DEFINITIONS</c> is applied to
    /// <c>juce_sharp</c> exactly once, and AFTER the last edit to it. It used to
    /// be applied sixteen lines BEFORE the ASIO block's
    /// <c>list(REMOVE_ITEM)</c>/<c>list(APPEND)</c> pair, so with no ASIO SDK
    /// present the shipping DLL got both <c>/DJUCE_ASIO=1</c> and
    /// <c>/DJUCE_ASIO=0</c> and worked only because the last <c>/D</c> wins —
    /// measured: the old file generates 8 of each in juce_sharp.vcxproj under
    /// <c>-DASIO_SDK_DIR=C:/nonexistent</c>, the new one generates 8 of
    /// <c>JUCE_ASIO=0</c> and none of <c>=1</c>. The test targets, added later,
    /// read the corrected list, so the DLL and the RT gate agreed by two
    /// different mechanisms, one of them ordering luck.
    /// <para>
    /// A source lint because the CI workflow has never run on a runner. The ASIO
    /// block itself is gone since 2026-09-28 (JUCE bundles the headers); the
    /// rule it broke is kept for every other edit to the list.
    /// </para>
    /// </summary>
    [Fact]
    public void NoMacroIsDefinedTwiceForTheShippingDll()
    {
        var cmake = File.ReadAllText(FindRepoFile(NativeCMake));

        var applies = Regex.Matches(
            cmake, @"target_compile_definitions\s*\(\s*juce_sharp\s+PRIVATE\s+\$\{JUCESHARP_CORE_DEFINITIONS\}\s*\)");
        Assert.True(applies.Count == 1,
            $"JUCESHARP_CORE_DEFINITIONS is applied to juce_sharp {applies.Count} times; once.");

        // THE ACTUAL FAULT, not "apply after the last edit" — this file
        // legitimately appends to the list AFTER the apply (the demucs and
        // neural flags at the bottom), because those are for the test
        // subdirectory and their values are handed to juce_sharp directly in the
        // same branch. That is fine as long as the macro is not ALREADY in the
        // resolved list with the other value, which is exactly what JUCE_ASIO
        // was: /DJUCE_ASIO=1 from the list plus /DJUCE_ASIO=0 from a separate
        // call, working only because the last /D wins (MSVC warning D9025, which
        // /WX does not catch).
        var setBlock = Regex.Match(cmake, @"set\s*\(\s*JUCESHARP_CORE_DEFINITIONS(?<body>[^)]*)\)");
        Assert.True(setBlock.Success, "no set(JUCESHARP_CORE_DEFINITIONS ...) block — lint is vacuous");
        var inList = Regex.Matches(setBlock.Groups["body"].Value, @"^\s*(?<name>[A-Za-z_]\w*)\s*=",
                                   RegexOptions.Multiline)
                          .Select(m => m.Groups["name"].Value)
                          .ToHashSet(StringComparer.Ordinal);
        Assert.Contains("JUCE_ASIO", inList);   // non-vacuity: the list really parses

        foreach (Match direct in Regex.Matches(
                     cmake, @"target_compile_definitions\s*\(\s*juce_sharp\s+PRIVATE\s+(?<name>[A-Za-z_]\w*)\s*="))
        {
            var name = direct.Groups["name"].Value;
            Assert.False(inList.Contains(name),
                $"{name} is in JUCESHARP_CORE_DEFINITIONS *and* handed to juce_sharp again by a "
                + $"separate target_compile_definitions. The DLL then gets two /D{name}= on one "
                + "command line and compiles only because the last one wins. Edit the list and let "
                + "the single apply carry it — that is what the ASIO branch does now.");
        }

        // ...and nothing edits the list BEFORE the single apply, which is the
        // ordering that made the old arrangement wrong: an edit there is dead for
        // the shipping DLL while the test targets (added later) do see it. The
        // ASIO-absent branch that used to be that edit went on 2026-09-28 with
        // the vendored SDK; this now holds for any future edit.
        foreach (Match edit in Regex.Matches(
                     cmake, @"list\s*\(\s*(?:APPEND|REMOVE_ITEM|INSERT|PREPEND)\s+JUCESHARP_CORE_DEFINITIONS\b"))
        {
            Assert.True(edit.Index > applies[0].Index,
                "JUCESHARP_CORE_DEFINITIONS is edited BEFORE it is applied to juce_sharp, so the "
                + "shipping DLL and the test targets see different lists — two mechanisms, one of "
                + "them ordering luck");
        }

        // ASIO is compiled in unconditionally now (JUCE bundles the headers), so
        // no branch may turn it off; EngineSurfaceCoverageTests checks the built DLL.
        Assert.DoesNotContain("JUCE_ASIO=0", cmake, StringComparison.Ordinal);
    }

    /// <summary>
    /// The value the FIRST `if (!e ...) return X;` line of a body answers, or
    /// null. Line-based because the condition contains nested parentheses.
    /// </summary>
    private static string? RejectionValue(string body)
    {
        foreach (var raw in body.Split('\n'))
        {
            var line = raw.Trim();
            if (! line.StartsWith("if", StringComparison.Ordinal)) continue;
            if (! line.Contains("!e", StringComparison.Ordinal)) continue;
            var m = Regex.Match(line, @"\breturn\s+(?<v>[^;]+);");
            if (m.Success) return m.Groups["v"].Value.Trim();
        }
        return null;
    }

    private static string DefinitionBodyOf(string src, string export)
    {
        var m = Regex.Match(src, @"DCA_API\s+[\w\*: ]+?\s+DCA_CALL\s+" + Regex.Escape(export) + @"\s*\(");
        Assert.True(m.Success, $"no definition of {export} in {AbiCpp}");

        var open = src.IndexOf('{', m.Index + m.Length);
        Assert.True(open > 0, $"{export} has no body");
        var depth = 0;
        for (var i = open; i < src.Length; i++)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0) return src.Substring(open, i - open + 1);
        }
        throw new InvalidDataException($"unbalanced braces after {export}");
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
