using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using JuceSharp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// ABI v36: a stem separation that can be CANCELLED, and the MIDI-out
/// driver-failure counter that had no way out of the engine.
/// <para>
/// Both are additive. Every v35 entry point keeps its exact signature and
/// behaviour, which is the reason each of these is a SECOND export rather than
/// a changed one — <c>dca_stems_progress_cb</c> returns <c>void</c> and has no
/// room for an answer, and <c>dca_midi_output_get_counts</c> has callers that
/// pass exactly two out-pointers.
/// </para>
/// </summary>
public sealed class AbiV36Tests
{
    /// <summary>
    /// The version the wrapper demands and the version the header declares are
    /// the same number. A mismatch is the one failure mode that makes every
    /// other test in the suite meaningless — the DLL refuses to load — and it
    /// is exactly what a hand-mirrored constant gets wrong.
    /// </summary>
    [Fact]
    public void TheExpectedAbiVersion_MatchesTheHeader()
    {
        var header = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));
        var m = System.Text.RegularExpressions.Regex.Match(header, @"#define\s+DCA_ABI_VERSION\s+(\d+)");
        Assert.True(m.Success, "the header no longer defines DCA_ABI_VERSION");
        Assert.Equal(NativeMethods.ExpectedAbiVersion, uint.Parse(m.Groups[1].Value));
        Assert.True(NativeMethods.ExpectedAbiVersion >= 36u, "the v36 exports predate the current version");
    }

    /// <summary>
    /// <c>DCA_ERR_CANCELLED</c> is 6 on both sides, and it is NOT one of the
    /// failure codes. The two managed enums that mirror the native one have to
    /// agree ordinal-for-ordinal, because <c>JuceStemSeparation</c> casts
    /// straight between them.
    /// </summary>
    [Fact]
    public void TheCancelledStatus_IsSixEverywhere()
    {
        Assert.Equal(6, (int)NativeMethods.DcaStatus.Cancelled);
        Assert.Equal(6, (int)JuceAudioStatus.Cancelled);
        Assert.Equal((int)NativeMethods.DcaStatus.Cancelled, (int)JuceAudioStatus.Cancelled);

        var header = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));
        Assert.Matches(@"DCA_ERR_CANCELLED\s*=\s*6", header);
    }

    /// <summary>
    /// Both v36 entry points are really IN the DLL. The repo's own rule is that
    /// <c>dumpbin /EXPORTS</c> is the only proof an export exists — a
    /// declaration compiles perfectly cleanly whether or not the definition
    /// reaches the binary, which is how five stale ABI files once shipped. This
    /// is the managed equivalent: resolve the entry point at runtime.
    /// </summary>
    [Theory]
    [InlineData("dca_stems_separate_file2")]
    [InlineData("dca_midi_output_get_counts3")]
    [InlineData("dca_stems_separate_file")]     // the v35 twins must still be there
    [InlineData("dca_midi_output_get_counts")]
    public void TheV36Exports_AreInTheDll(string export)
    {
        if (!NativeProbe.IsAvailable) { Assert.Skip("native DLL not loadable here"); return; }

        var lib = System.Runtime.InteropServices.NativeLibrary.Load(
            NativeMethods.LibName, typeof(NativeMethods).Assembly, null);
        Assert.True(System.Runtime.InteropServices.NativeLibrary.TryGetExport(lib, export, out var addr),
            $"{export} is not exported by the built juce_sharp.dll. A declaration compiles whether "
            + "or not the definition reaches the binary — this is the check that catches that.");
        Assert.NotEqual(nint.Zero, addr);
    }

    /// <summary>
    /// The cancellable separation stops, and reports CANCELLED rather than a
    /// failure — which is the whole point: the repo's stem rule is that a
    /// cancelled separation raises <see cref="OperationCanceledException" />,
    /// never an error a consumer would read as "try the next backend".
    /// <para>
    /// SKIPS when the native backend is not compiled in
    /// (<c>DOTCHARTER_STEMS_DEMUCS_CPP=OFF</c>, which is this worktree's build):
    /// the export then short-circuits to <c>NOT_INITIALIZED</c> before it ever
    /// reaches the callback, so there is nothing to cancel. The export's
    /// PRESENCE is pinned regardless by the sweep above.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void ACancelledSeparation_ThrowsOperationCanceled_NotAFailure()
    {
        if (NativeMethods.StemsIsSupported() == 0)
        {
            Assert.Skip("native stem backend not compiled into this DLL "
                        + "(DOTCHARTER_STEMS_DEMUCS_CPP=OFF) — nothing reaches the progress callback");
            return;
        }

        var dir = Directory.CreateTempSubdirectory("juce-stems-cancel").FullName;
        try
        {
            var audio = Path.Combine(dir, "in.wav");
            File.WriteAllBytes(audio, Array.Empty<byte>());
            var model = Path.Combine(dir, "model.bin");
            File.WriteAllBytes(model, new byte[] { 1, 2, 3, 4 });

            using var cts = new CancellationTokenSource();
            cts.Cancel();   // already cancelled: the FIRST report stops it

            Assert.Throws<OperationCanceledException>(
                () => JuceStemSeparation.Separate(audio, dir, model, 1, null, cts.Token));
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch (IOException) { }
        }
    }

    /// <summary>
    /// The cancellable callback answers 0 once the token is signalled and
    /// non-zero before, and a THROWING progress handler does NOT cancel.
    /// <para>
    /// That last half is the decision worth pinning. A handler that faults is a
    /// broken observer; tearing down a multi-minute separation because
    /// somebody's progress bar threw is the wrong blast radius, and the
    /// non-cancellable callback has always swallowed-and-logged. Only the token
    /// stops the run. This needs no native backend — it drives the delegate
    /// directly, the way <c>NativeCallbackSafetyTests</c> does.
    /// </para>
    /// </summary>
    [Fact]
    public void TheCancellableCallback_StopsOnTheTokenAndNotOnAThrow()
    {
        using var cts = new CancellationTokenSource();

        var cb = JuceStemSeparation.WrapCancellableProgress(null, cts.Token);
        Assert.NotEqual(0, cb(nint.Zero, 0.25f, "running"));   // non-zero == continue
        cts.Cancel();
        Assert.Equal(0, cb(nint.Zero, 0.50f, "running"));      // 0 == stop

        using var live = new CancellationTokenSource();
        var thrower = JuceStemSeparation.WrapCancellableProgress(
            (_, _) => throw new InvalidOperationException("handler is broken"), live.Token);
        Assert.NotEqual(0, thrower(nint.Zero, 0.75f, "running"));
    }

    /// <summary>
    /// A null progress handler still produces a delegate, because the TOKEN has
    /// to be polled even when the host wants no progress. Returning null there
    /// would have made "cancel with no progress handler" silently impossible —
    /// the exact shape of gap v36 exists to close.
    /// </summary>
    [Fact]
    public void ANullProgressHandler_StillYieldsACancellableCallback()
    {
        using var cts = new CancellationTokenSource();
        var cb = JuceStemSeparation.WrapCancellableProgress(null, cts.Token);
        Assert.NotNull(cb);
        cts.Cancel();
        Assert.Equal(0, cb(nint.Zero, 0.1f, "running"));
    }

    /// <summary>
    /// <c>FailedCount</c> and <c>ReadAllCounters</c> reach the interface, the
    /// real wrapper and the fake, and the three counters are three distinct
    /// numbers rather than one read three times.
    /// </summary>
    [Fact]
    public void TheMidiOutputFailedCounter_IsOnTheInterfaceAndDistinct()
    {
        IJuceMidiOutput fake = new FakeJuceMidiOutput { DroppedCount = 7, FailedCount = 3 };
        fake.Send(0x90, 60, 100);
        fake.Send(0x80, 60, 0);

        Assert.Equal(2, fake.SentCount);
        Assert.Equal(7, fake.DroppedCount);
        Assert.Equal(3, fake.FailedCount);

        var all = fake.ReadAllCounters();
        Assert.Equal((2L, 7L, 3L), all);

        // Three DIFFERENT faults, not one number surfaced three ways: dropped is
        // "our queue was full and we never handed it over", failed is "it
        // reached the driver and did not come out the other side".
        Assert.NotEqual(all.Dropped, all.Failed);
        Assert.NotEqual(all.Sent, all.Failed);
    }

    /// <summary>
    /// THE CANCEL HAS TO LEAVE THE ASYNC ENTRY POINTS. Both public async
    /// surfaces take a <see cref="CancellationToken" />; both must hand it to
    /// the SIX-argument <c>Separate</c>, which is the only overload that reaches
    /// <c>dca_stems_separate_file2</c> with a live token.
    /// <para>
    /// This is a SOURCE lint and not a behavioural test on purpose: the
    /// behavioural route needs the demucs backend and a multi-minute model run,
    /// so it is skipped on every build with <c>DOTCHARTER_STEMS_DEMUCS_CPP=OFF</c>
    /// and on CI. The fault it pins (W5-E1) was invisible to ten passing tests
    /// over <c>WrapCancellableProgress</c> precisely because nothing checked the
    /// CALL: five arguments resolve to the convenience overload, which hard-coded
    /// <c>CancellationToken.None</c>, so the whole v36 feature was dark while its
    /// XML docs, four guides and the commit message all said it shipped.
    /// </para>
    /// </summary>
    [Fact]
    public void BothAsyncSeparationPaths_HandTheirTokenToTheCancellableOverload()
    {
        var src = File.ReadAllText(FindRepoFile("src/JuceSharp/JuceStemSeparation.cs"));

        // Nothing in this file may manufacture a dead token any more: the one
        // uncancellable path forwards `default` through the six-arg overload,
        // so there is exactly ONE route into SeparateCore.
        Assert.DoesNotContain("CancellationToken.None", src, StringComparison.Ordinal);

        foreach (var (member, tokenName) in new[]
                 {
                     ("public static Task SeparateAsync(", "cancellationToken"),
                     ("public static async IAsyncEnumerable<JuceStemProgress> SeparateWithProgressAsync(", "token"),
                 })
        {
            var body = BodyOf(src, member);
            var calls = SeparateCallArguments(body);
            Assert.NotEmpty(calls);
            foreach (var args in calls)
            {
                Assert.Equal(6, args.Count);
                Assert.Equal(tokenName, args[5].Trim());
            }
        }
    }

    /// <summary>The brace-matched body of the member whose declaration starts with
    /// <paramref name="signature" />.</summary>
    private static string BodyOf(string src, string signature)
    {
        var at = src.IndexOf(signature, StringComparison.Ordinal);
        Assert.True(at >= 0, $"JuceStemSeparation no longer declares `{signature}` — repoint this lint.");
        var open = src.IndexOf('{', at);
        Assert.True(open >= 0, $"no body found for `{signature}`");
        var depth = 0;
        for (var i = open; i < src.Length; i++)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0) return src[open..i];
        }
        throw new InvalidOperationException($"unbalanced braces after `{signature}`");
    }

    /// <summary>Top-level argument lists of every <c>Separate(</c> call in
    /// <paramref name="body" /> — not <c>SeparateCore</c>, not the async names.</summary>
    private static List<List<string>> SeparateCallArguments(string body)
    {
        var result = new List<List<string>>();
        for (var m = System.Text.RegularExpressions.Regex.Match(body, @"(?<![A-Za-z0-9_])Separate\s*\(");
             m.Success;
             m = m.NextMatch())
        {
            var open = body.IndexOf('(', m.Index);
            var depth = 0;
            var args = new List<string>();
            var start = open + 1;
            for (var i = open; i < body.Length; i++)
            {
                var c = body[i];
                if (c is '(' or '[') depth++;
                else if (c is ')' or ']')
                {
                    depth--;
                    if (depth == 0) { args.Add(body[start..i]); break; }
                }
                else if (c == ',' && depth == 1) { args.Add(body[start..i]); start = i + 1; }
            }

            result.Add(args);
        }

        return result;
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
