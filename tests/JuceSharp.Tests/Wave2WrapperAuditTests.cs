using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using JuceSharp.Dsp;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     Wave-2 audit, the managed-wrapper half that needs no native DLL: findings
///     #16, #22, #23, #24 and the API-shape cleanups (the transposable exception
///     ctor, the un-annotated <c>Try</c>, the source ctor with no
///     <c>GC.SuppressFinalize</c>). Findings #4 / #11 / #12 are in
///     <c>Dsp/Wave2EditScopeLeakTests</c>, where a real rig is needed.
///     <para>
///         Three of these are SOURCE lints. That is a deliberate second choice,
///         stated where it applies: the behaviour they pin needs either a real
///         audio device or a race that cannot be driven deterministically, and a
///         lint that fails on the shape is worth more than no guard at all.
///     </para>
/// </summary>
public sealed class Wave2WrapperAuditTests
{
    // ------------------------------------------------- JuceAudioException shape

    /// <summary>
    ///     The public ctor used to be <c>(status, operation, message = null)</c> —
    ///     two ADJACENT strings, so <c>new JuceAudioException(s, detail, op)</c>
    ///     compiled cleanly and produced an exception whose
    ///     <see cref="JuceAudioException.Operation" /> was a sentence. Nothing but
    ///     a reader could catch that, so the status now sits BETWEEN the two
    ///     strings and the transposition no longer compiles.
    /// </summary>
    [Fact]
    public void JuceAudioException_PutsTheStatusBetweenTheTwoStrings()
    {
        var ctors = typeof(JuceAudioException)
            .GetConstructors(BindingFlags.Public | BindingFlags.Instance)
            .Select(c => c.GetParameters().Select(p => p.ParameterType).ToArray())
            .ToList();

        Assert.DoesNotContain(ctors, p => p.Length == 3
                                          && p[0] == typeof(JuceAudioStatus)
                                          && p[1] == typeof(string)
                                          && p[2] == typeof(string));
        Assert.Contains(ctors, p => p.Length == 3
                                    && p[0] == typeof(string)
                                    && p[1] == typeof(JuceAudioStatus)
                                    && p[2] == typeof(string));
        Assert.Contains(ctors, p => p.Length == 2
                                    && p[0] == typeof(JuceAudioStatus)
                                    && p[1] == typeof(string));
    }

    [Fact]
    public void JuceAudioException_CarriesTheDetailAndComposesOneWhenThereIsNone()
    {
        var detailed = new JuceAudioException("node 7 has no 'ir' parameter",
                                              JuceAudioStatus.InvalidArg,
                                              "dca_engine_dsp_get_node_string");
        Assert.Equal("node 7 has no 'ir' parameter", detailed.Message);
        Assert.Equal("dca_engine_dsp_get_node_string", detailed.Operation);
        Assert.Equal(JuceAudioStatus.InvalidArg, detailed.Status);

        var bare = new JuceAudioException(JuceAudioStatus.Decode, "dca_stems_separate_file");
        Assert.Equal("dca_stems_separate_file", bare.Operation);
        Assert.Contains("Decode", bare.Message, StringComparison.Ordinal);
    }

    // ------------------------------------------------- JuceAudioSource ctor

    /// <summary>
    ///     The one wrapper ctor that did not <c>GC.SuppressFinalize(this)</c>
    ///     before it could throw. It was safe only because it had nothing that
    ///     threw — and "safe because nobody has added a throw yet" is not a
    ///     property, it is a coincidence. The end-to-end shape is
    ///     <c>FinalizerSafetyTests.RejectedPluginHandle_...</c>: before that fix
    ///     the corpse did not fail an assertion, it took the test host down at a
    ///     GC-dependent point minutes later.
    /// </summary>
    [Fact]
    public void AZeroSourceHandle_IsRejected_AndLeavesNothingOnTheFinalizerQueue()
    {
        Assert.Throws<ArgumentException>(() => new JuceAudioSource(nint.Zero));

        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        GC.WaitForPendingFinalizers();
    }

    // ------------------------------------------------- nullable annotations

    /// <summary>
    ///     <c>TryGetNode</c> sets its out-param to null on a <c>false</c> return
    ///     and said so to nobody: without <see cref="MaybeNullWhenAttribute" />
    ///     the compiler told every caller the node was non-null, so
    ///     <c>if (!chain.TryGetNode(id, out var n)) … n.Bypass</c> produced no
    ///     warning at all.
    /// </summary>
    [Fact]
    public void TryGetNode_TellsNullableAnalysisTheTruthOnAFalseReturn()
    {
        var method = typeof(JuceDspChain).GetMethod(nameof(JuceDspChain.TryGetNode),
                                                    [typeof(int), typeof(JuceDspNode).MakeByRefType()]);
        Assert.NotNull(method);
        var node = method!.GetParameters()[1];
        Assert.True(node.IsOut);
        Assert.Contains(node.GetCustomAttributes(inherit: false),
                        a => a.GetType() == typeof(MaybeNullWhenAttribute));
    }

    /// <summary>
    ///     <c>TryReadMeters</c> is named <c>Try*</c>, returns an <c>int</c>, and
    ///     cannot report failure: a <c>0</c> means either "this seat has no nodes"
    ///     or "the engine is gone". The <c>int</c> overload is kept — it is the
    ///     shape <see cref="IJuceDspChain" /> declares — beside one that answers
    ///     both questions, which is what its own sibling <c>TryReadMeter</c>
    ///     already did.
    /// </summary>
    [Fact]
    public void TryReadMeters_HasAnOverloadThatCanActuallyReportFailure()
    {
        var honest = typeof(JuceDspChain).GetMethod(nameof(JuceDspChain.TryReadMeters),
            [typeof(Span<JuceDspNodeMeter>), typeof(int).MakeByRefType()]);
        Assert.NotNull(honest);
        Assert.Equal(typeof(bool), honest!.ReturnType);
        Assert.True(honest.GetParameters()[1].IsOut);

        // …and the int-returning one is still there, because the interface and
        // existing callers name it.
        var legacy = typeof(JuceDspChain).GetMethod(nameof(JuceDspChain.TryReadMeters),
                                                    [typeof(Span<JuceDspNodeMeter>)]);
        Assert.NotNull(legacy);
        Assert.Equal(typeof(int), legacy!.ReturnType);
    }

    // ------------------------------------------------- #22 dispose join

    /// <summary>
    ///     <b>#22.</b> <c>Dispose</c> joined the async worker with the 60 s handle
    ///     drain deadline. Dispose normally runs on the host's UI thread and the
    ///     item in flight can be <c>ShowAsioControlPanelAsync</c> — a MODAL vendor
    ///     dialog that returns when the USER closes it — so that join was a
    ///     minute-long frozen application at teardown, which is the exact failure
    ///     the async surface exists to prevent. The handle drain behind it already
    ///     handles a wedged call honestly (refuse to free, log at Error, leak), so
    ///     the join only has to cover the ordinary case.
    /// </summary>
    [Fact]
    public void TheWorkerJoinAtDispose_IsMuchShorterThanTheHandleDrainDeadline()
    {
        Assert.True(EngineWorker.DisposeJoinTimeoutMs > 0);
        Assert.True(EngineWorker.DisposeJoinTimeoutMs <= 5_000,
            $"the dispose join is {EngineWorker.DisposeJoinTimeoutMs} ms — long enough to freeze a UI");
        Assert.True(EngineWorker.DisposeJoinTimeoutMs < NativeHandleOwner.DefaultDrainTimeoutMs);

        // The source lint half: it is the CALL that regressed, not the constant.
        var text = ReadRepoFile("src/JuceSharp/JuceAudioEngine.Async.cs");
        Assert.Contains("EngineWorker.DisposeJoinTimeoutMs : 0", text, StringComparison.Ordinal);
        Assert.DoesNotContain("Shutdown(disposing ? NativeHandleOwner.DefaultDrainTimeoutMs", text,
                              StringComparison.Ordinal);
    }

    // ------------------------------------------------- #23 async corpses

    /// <summary>
    ///     <b>#23.</b> The work item runs, <c>OpenFile</c> / <c>LoadPlugin</c>
    ///     succeeds and TRACKS the child, <c>ShutdownWorker</c> joins and returns,
    ///     and <c>DisposeChildren</c> then disposes that very child — so the
    ///     awaiting caller received a live-looking wrapper whose every member
    ///     throws. A source lint because driving that ordering needs a real
    ///     multi-second native call to dispose underneath.
    /// </summary>
    [Fact]
    public void BothAsyncFactories_RouteTheirChildThroughTheCorpseCheck()
    {
        var text = ReadRepoFile("src/JuceSharp/JuceAudioEngine.Async.cs");
        Assert.Contains("NotACorpse(OpenFile(path)", text, StringComparison.Ordinal);
        Assert.Contains("NotACorpse(LoadPlugin(path, componentIndex)", text, StringComparison.Ordinal);
        // …and the check has to consult BOTH questions: has a dispose started,
        // and is this child already dead.
        Assert.Contains("_workerClosed", text, StringComparison.Ordinal);
        Assert.Contains("isDisposed(child)", text, StringComparison.Ordinal);
    }

    /// <summary>Both child wrappers can answer "am I already dead?", which is
    /// what the corpse check reads. <c>JuceAudioSource</c> could not until
    /// wave 2.</summary>
    [Fact]
    public void EveryAsyncFactoryChild_CanReportThatItIsDisposed()
    {
        Assert.NotNull(typeof(JuceAudioSource).GetProperty(nameof(JuceAudioSource.IsDisposed)));
        Assert.NotNull(typeof(JuceAudioPlugin).GetProperty(nameof(JuceAudioPlugin.IsDisposed)));
    }

    // ------------------------------------------------- #24 loop swap order

    /// <summary>
    ///     <b>#24.</b> <c>JuceAudioSource.Loop</c> stored the new region and
    ///     disposed the old one BEFORE calling <c>Attach</c> — which throws when
    ///     the region already belongs to a different source. On that throw the
    ///     property reported a loop, the old loop was dead, the new one was never
    ///     armed, and nothing was looping: the exact failure the surrounding doc
    ///     says was just fixed. A source lint because the behavioural version
    ///     needs two live native sources, i.e. an open device and two decoded
    ///     files.
    /// </summary>
    [Fact]
    public void TheLoopSetter_AttachesBeforeItSwapsAndDisposes()
    {
        var text = ReadRepoFile("src/JuceSharp/JuceAudioSource.cs");
        var attach = text.IndexOf("value?.Attach(this);", StringComparison.Ordinal);
        var swap   = text.IndexOf("Interlocked.Exchange(ref _loop, value)", StringComparison.Ordinal);
        var kill   = text.IndexOf("previous?.Dispose();", StringComparison.Ordinal);

        Assert.True(attach >= 0 && swap >= 0 && kill >= 0, "the Loop setter no longer has the shape this pins");
        Assert.True(attach < swap,
            "Attach must run FIRST: it is the only step that can throw, and everything after it is a mutation");
        Assert.True(swap < kill, "the old region must not be disposed before the new one is published");
    }

    // ------------------------------------------------- #16 progress stream

    /// <summary>
    ///     <b>#16.</b> Abandoning the enumeration (a <c>break</c>, which disposes
    ///     the enumerator) used to <c>Cancel()</c> and then <c>Dispose()</c> the
    ///     linked <see cref="CancellationTokenSource" /> WITHOUT awaiting the
    ///     worker — so the backend kept running against a token whose source was
    ///     gone. <c>CancellationToken.WaitHandle</c> throws
    ///     <see cref="ObjectDisposedException" /> outright on such a token, and
    ///     both <c>CreateLinkedTokenSource(ct)</c> and <c>Task.Delay(250, ct)</c>
    ///     are on exactly this one in the python path.
    ///     <para>
    ///         The timeout below is a HANG guard, not a measurement: the assertion
    ///         is on the exception, never on how long anything took.
    ///     </para>
    /// </summary>
    [Fact]
    public async Task AnAbandonedProgressStream_LeavesTheWorkersTokenUsable()
    {
        var observed = new TaskCompletionSource<Exception?>(TaskCreationOptions.RunContinuationsAsynchronously);

        // The CONSUMER-side token is the test's; the WORKER's token below is
        // deliberately not, which is the whole point of this test.
        var stream = CoalescingProgressStream.Create<int>(ct: TestContext.Current.CancellationToken,
                                                         work: async (report, ct) =>
        {
            report(1);
            try
            {
                while (!ct.IsCancellationRequested)
                    // CancellationToken.None ON PURPOSE: this models the backend
                    // worker that keeps running after the consumer abandons the
                    // enumeration. Wiring the test token in here would cancel the
                    // very thing the test exists to observe.
#pragma warning disable xUnit1051
                    await Task.Delay(5, CancellationToken.None).ConfigureAwait(false);
#pragma warning restore xUnit1051
                // The token has to survive the consumer walking away. WaitHandle
                // is the cheapest member that notices a disposed source.
                _ = ct.WaitHandle.WaitOne(0);
                observed.TrySetResult(null);
            }
            catch (Exception ex)
            {
                observed.TrySetResult(ex);
            }
        });

        await foreach (var _ in stream) break;   // abandon mid-stream

        var error = await observed.Task.WaitAsync(
            TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
        Assert.Null(error);
    }

    /// <summary>The class doc must SAY that abandonment is fire-and-forget —
    /// contract item 4 implies the work has stopped by the time the enumeration
    /// ends, and it has only been asked to.</summary>
    [Fact]
    public void TheProgressStreamDoc_SaysAbandonmentDoesNotWaitForTheBackend()
    {
        var text = ReadRepoFile("src/JuceSharp/CoalescingProgressStream.cs");
        Assert.Contains("ABANDONMENT IS FIRE-AND-FORGET", text, StringComparison.Ordinal);
    }

    // ------------------------------------------------- helpers

    private static string ReadRepoFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
        }

        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
