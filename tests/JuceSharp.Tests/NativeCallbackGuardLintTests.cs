using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Source lints over the two engine rules that are about a CALL SITE existing in
/// the right shape, which neither the RT gate nor a managed test can see.
/// <para>
/// <b>Rule 1 — nothing escapes the driver callback.</b> Every
/// <c>processBlock</c> / <c>prepareToPlay</c> the engine calls is THIRD-PARTY
/// code. An exception out of one unwinds through JUCE's ASIO/WASAPI frame into
/// the driver's, which is undefined behaviour and in practice
/// <c>std::terminate</c> — a process kill, not a dropped block. The engine has
/// always stated the rule and applied it at two of its three plugin call sites;
/// the input chain, which is the path every amp-sim user is on, called
/// <c>processBlock</c> bare until W4-N1, and
/// <c>audioDeviceIOCallbackWithContext</c> had no backstop either.
/// </para>
/// <para>
/// <b>Rule 2 — the device pointer is read on the thread that owns it.</b>
/// <c>AudioDeviceManager::getCurrentAudioDevice()</c> hands back a raw pointer
/// <c>setAudioDeviceSetup</c> DELETES. Readers take
/// <c>AudioEngine::deviceSnapshot()</c> instead; the exceptions are the few
/// sites that genuinely need the DEVICE and run marshalled on the message
/// thread. <c>dca_engine_show_asio_panel</c> was neither: it dereferenced the
/// pointer from the caller's thread and held it across a MODAL vendor dialog.
/// </para>
/// <para>
/// Both are lints and not behaviour tests on purpose. The RT gate CAN see rule 1
/// (<c>RtAThrowingInputPluginCannotKillTheCallback</c> kills the gate process
/// without the fix, which is as non-vacuous as a test gets) but it can only
/// reach the sites a scenario happens to drive; a THIRD plugin call site added
/// tomorrow would be invisible to it. Rule 2 has no deterministic reproduction
/// at all — the window is a device teardown racing a dialog nobody can schedule.
/// </para>
/// </summary>
public sealed class NativeCallbackGuardLintTests
{
    private const string EngineH = "src/JuceSharp.Native/src/AudioEngine.h";
    private const string EngineCpp = "src/JuceSharp.Native/src/AudioEngine.cpp";
    private const string AbiCpp = "src/JuceSharp.Native/src/dotcharter_audio.cpp";

    /// <summary>
    /// Every <c>processBlock</c> the engine calls on a plugin it did not write
    /// is inside a <c>try</c> whose <c>catch</c> is within a few lines of it.
    /// <para>
    /// Deliberately a proximity check rather than a parse: the three sites are
    /// each a two-to-four-line <c>try { … } catch (…) { … }</c>, and a
    /// <c>catch</c> fifty lines away would be a different guard covering
    /// something else. The counted-sites assertion below is what stops a new,
    /// unguarded fourth site from passing by simply not matching.
    /// </para>
    /// </summary>
    [Theory]
    [InlineData(EngineH)]
    [InlineData(EngineCpp)]
    public void EveryPluginProcessBlockCall_IsWrappedInACatchAll(string file)
    {
        var src = StripComments(File.ReadAllText(FindRepoFile(file)));

        // `x->processBlock(` / `x.processBlock(` on something the engine holds a
        // pointer to. The AudioProcessor base class's own declaration is not a
        // call and has no arrow.
        var calls = Regex.Matches(src, @"\w+\s*->\s*processBlock\s*\(");
        Assert.True(calls.Count > 0,
            $"no plugin processBlock call sites found in {file} at all — the extractor is broken "
            + "and this lint is vacuous");

        foreach (Match call in calls)
        {
            // Look back for the opening `try` and forward for the `catch`, both
            // within a small window. 400 characters is comfortably more than the
            // longest of the three sites and far less than the distance to any
            // unrelated guard.
            var back = src[Math.Max(0, call.Index - 400)..call.Index];
            var fwd = src[call.Index..Math.Min(src.Length, call.Index + 400)];

            Assert.True(
                Regex.IsMatch(back, @"\btry\b") && Regex.IsMatch(fwd, @"\bcatch\s*\("),
                $"{file}: a plugin processBlock call at offset {call.Index} is not inside a "
                + "try/catch. An exception out of third-party plugin code on the audio thread "
                + "unwinds through JUCE's driver callback frame with nothing to catch it — that "
                + "is a process kill, not a dropped block. Wrap it, the way renderInstrumentBus "
                + "and processInputChain do.\n"
                + "  context: ..." + Collapse(back[^Math.Min(120, back.Length)..])
                + " >>> " + Collapse(fwd[..Math.Min(120, fwd.Length)]));
        }
    }

    /// <summary>
    /// ...and the BACKSTOP, which is what makes "nothing escapes the driver
    /// callback" a property of the class rather than of a list of call sites
    /// somebody has to keep complete. <c>NullAudioIODevice::renderOneBlock</c> —
    /// and every real driver frame — is <c>noexcept</c>, so an escape is
    /// <c>std::terminate</c>.
    /// </summary>
    [Fact]
    public void TheDriverCallback_HasACatchAllBackstop()
    {
        var src = StripComments(File.ReadAllText(FindRepoFile(EngineH)));
        var m = Regex.Match(src,
            @"void\s+audioDeviceIOCallbackWithContext\s*\([^)]*\)\s*override\s*\{(?<body>(.|\n){0,1200})");
        Assert.True(m.Success, $"audioDeviceIOCallbackWithContext not found in {EngineH}");

        var body = m.Groups["body"].Value;
        Assert.Contains("try", body, StringComparison.Ordinal);
        Assert.Matches(@"catch\s*\(\s*\.\.\.\s*\)", body);

        // Non-vacuity: the override really delegates rather than inlining ~480
        // lines of callback under the try, which is what the extractor's 1,200
        // character window assumes.
        Assert.Contains("renderBlock(", body, StringComparison.Ordinal);
    }

    /// <summary>
    /// <c>getCurrentAudioDevice()</c> appears only where the raw pointer is
    /// legitimately needed AND the caller is on the message thread. Five sites,
    /// named explicitly — a sixth has to be argued for here rather than added
    /// silently. The fifth is <c>channelNames</c> (ABI v49): the names are not
    /// in the snapshot, they are a property of the device object, and its only
    /// caller marshals onto the message thread (pinned below).
    /// </summary>
    [Fact]
    public void TheRawDevicePointer_IsOnlyReadAtTheSanctionedSites()
    {
        var engine = StripComments(File.ReadAllText(FindRepoFile(EngineCpp)));
        var abi = StripComments(File.ReadAllText(FindRepoFile(AbiCpp)));

        // AudioEngine.cpp: start() (the belt-and-braces "did a device actually
        // open" check, on the thread that just opened it), showAsioControlPanel
        // (which needs the DEVICE, and is marshalled — see the next assertion)
        // and channelNames (the device's own names, read inside a callSync).
        var inEngine = Regex.Matches(engine, @"getCurrentAudioDevice\s*\(").Count;
        Assert.True(inEngine == 3,
            $"{EngineCpp} reads the raw device pointer at {inEngine} sites; three are sanctioned "
            + "(start()'s open check, showAsioControlPanel and channelNames). Every other reader takes "
            + "deviceSnapshot(), because setAudioDeviceSetup DELETES what this returns.");
        var names = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), "dca_engine_get_channel_names"));
        Assert.Contains("callSync", names, StringComparison.Ordinal);
        Assert.Contains("channelNames(", names, StringComparison.Ordinal);

        // dotcharter_audio.cpp: only inside the two callSync bodies that
        // reconfigure the device and then confirm one is still open. The two
        // offline-render exports used to read it too (W4-D73) and now take the
        // snapshot.
        var inAbi = Regex.Matches(abi, @"getCurrentAudioDevice\s*\(").Count;
        Assert.True(inAbi == 2,
            $"{AbiCpp} reads the raw device pointer at {inAbi} sites; two are sanctioned "
            + "(the post-swap 'is a device open' check inside dca_engine_reconfigure's and "
            + "dca_engine_set_device_type's callSync bodies, which run on the message thread). "
            + "An ABI export polled from a managed thread must use deviceSnapshot().");

        // ...and specifically: the offline renders gate on the snapshot.
        foreach (var export in new[]
                 {
                     "dca_engine_dsp_process_buffer_for_player",
                     "dca_engine_dsp_process_buffer_automated_for_player",
                 })
        {
            var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));
            Assert.True(body.Contains("deviceSnapshot()", StringComparison.Ordinal),
                $"{export} no longer gates 'offline means offline' on deviceSnapshot() — if it "
                + "went back to getCurrentAudioDevice() it is dereferencing a pointer "
                + "setAudioDeviceSetup can free, from whatever thread the host called on");
            Assert.DoesNotContain("getCurrentAudioDevice", body, StringComparison.Ordinal);
        }
    }

    /// <summary>
    /// <c>dca_engine_show_asio_panel</c> runs its whole body on the message
    /// thread and catches. It is the one export that opens third-party UI: an
    /// ASIO control panel re-enters the driver's COM apartment (which is why
    /// create/destroy/start/stop/reconfigure are all marshalled) and it is
    /// MODAL, so the device pointer it holds is live for as long as the dialog
    /// is up — the longest window any reader of that pointer has.
    /// </summary>
    [Fact]
    public void ShowingTheAsioPanel_IsMarshalledAndGuarded()
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)),
                                                  "dca_engine_show_asio_panel"));

        Assert.Contains("callSync", body, StringComparison.Ordinal);
        Assert.Contains("guard(", body, StringComparison.Ordinal);
        Assert.Contains("showAsioControlPanel", body, StringComparison.Ordinal);

        // The catch has to be INSIDE the marshalled body: callSync runs it on
        // the message thread, so an exception unwinds THERE and a guard() around
        // the outside never sees it. This is the shape dca_engine_start uses.
        var sync = body.IndexOf("callSync", StringComparison.Ordinal);
        var call = body.IndexOf("showAsioControlPanel", StringComparison.Ordinal);
        Assert.True(sync >= 0 && call > sync,
            "showAsioControlPanel is called OUTSIDE the callSync body — it is back on the "
            + "caller's thread, dereferencing a device pointer it does not own across a modal "
            + "vendor dialog");
        Assert.Matches(@"catch\s*\(\s*\.\.\.\s*\)", body[sync..]);
    }

    /// <summary>
    /// W4-D73. "Offline means offline" was a TOCTOU: the two offline rig renders
    /// checked that no device was open and then ran for minutes with nothing
    /// holding that true, so a device opening mid-render got its live tone
    /// retuned by the lazy <c>rig.prepare</c>. FIVE exports now share one
    /// <c>AudioEngine::DeviceLifecycleLock</c> — the two renders and the three
    /// that OPEN a device — and both sides TRY-lock, so whichever arrives second
    /// is refused rather than made to wait (blocking a user's play button behind
    /// a bounce is a worse answer than an error code).
    /// <para>
    /// A source lint because the RT gate cannot reach it: the gate drives
    /// <c>AudioEngine::dspProcessOffline</c> DIRECTLY, and that method is
    /// deliberately ungated so <c>RtEveryCatalogueModuleIsAllocationFree</c>
    /// does not go vacuous — the guard lives on the EXPORT. Losing the lock at
    /// any one of the five sites is silent and only reproducible by winning a
    /// race, which is exactly the shape a lint is for.
    /// </para>
    /// </summary>
    [Theory]
    [InlineData("dca_engine_dsp_process_buffer_for_player")]
    [InlineData("dca_engine_dsp_process_buffer_automated_for_player")]
    [InlineData("dca_engine_start")]
    [InlineData("dca_engine_reconfigure")]
    [InlineData("dca_engine_set_device_type")]
    public void ADeviceOpenAndAnOfflineRender_ShareOneLifecycleLock(string export)
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));

        Assert.True(body.Contains("DeviceLifecycleLock", StringComparison.Ordinal),
            $"{export} does not take AudioEngine::DeviceLifecycleLock. The two offline rig "
            + "renders and the three exports that OPEN a device must share it, or the renders' "
            + "'no device is open' check goes stale the instant after it is read.");

        // TRY-lock, never a blocking wait: the refusal is the design.
        Assert.True(body.Contains("held()", StringComparison.Ordinal),
            $"{export} takes the lock but never checks held() — a DeviceLifecycleLock that is "
            + "not owned protects nothing, and the class deliberately cannot block");
    }

    /// <summary>
    /// ...and the ORDER, which is the half that actually closes the race: the
    /// lock must be taken BEFORE the snapshot is read. Reading first and locking
    /// second is the original bug with an extra mutex — the answer is already
    /// stale by the time it is protected.
    /// </summary>
    [Theory]
    [InlineData("dca_engine_dsp_process_buffer_for_player")]
    [InlineData("dca_engine_dsp_process_buffer_automated_for_player")]
    public void AnOfflineRender_LocksBeforeItReadsTheDeviceSnapshot(string export)
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));

        var lockAt = body.IndexOf("DeviceLifecycleLock", StringComparison.Ordinal);
        var readAt = body.IndexOf("deviceSnapshot()", StringComparison.Ordinal);

        Assert.True(lockAt >= 0, $"{export} does not take the lifecycle lock at all");
        Assert.True(readAt >= 0, $"{export} no longer reads deviceSnapshot() — lint is vacuous");
        Assert.True(lockAt < readAt,
            $"{export} reads deviceSnapshot() BEFORE it takes the lifecycle lock. That is the "
            + "original TOCTOU with an extra mutex bolted on: the answer is already stale by the "
            + "time anything protects it.");
    }

    /// <summary>
    /// Every engine <c>delete</c> in the ABI unit runs on the JuceHost message
    /// thread. <c>start()</c> creates the device types there, and WASAPI's
    /// <c>DeviceChangeDetector</c> owns a hidden window that only its creating
    /// thread can destroy. <c>dca_engine_create</c>'s failure path deleted the
    /// engine on the calling managed thread, so the window outlived the freed
    /// detector, and the next <c>WM_DEVICECHANGE</c> (an interface unplugged or
    /// replugged) crashed the message thread (DotCharter #273).
    /// </summary>
    [Fact]
    public void EveryEngineDelete_RunsOnTheMessageThread()
    {
        var src = StripComments(File.ReadAllText(FindRepoFile(AbiCpp)));

        var deletes = Regex.Matches(src, @"\bdelete\s+e\s*;");
        Assert.True(deletes.Count >= 2,
            $"found {deletes.Count} engine delete(s) in {AbiCpp}; the create-failure and destroy "
            + "paths both delete one, so the extractor is broken and this lint is vacuous");

        foreach (Match del in deletes)
        {
            var back = Collapse(src[Math.Max(0, del.Index - 80)..del.Index]);
            Assert.True(back.Contains("callSync(", StringComparison.Ordinal),
                $"an engine is deleted outside JuceHost::callSync near offset {del.Index} of {AbiCpp}: "
                + "the device types' hidden windows then outlive it and the next WM_DEVICECHANGE "
                + "dereferences freed memory. …" + back);
        }
    }

    private static string Collapse(string s) => Regex.Replace(s, @"\s+", " ").Trim();

    /// <summary>The DEFINITION, not a call site or a forward declaration.</summary>
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
