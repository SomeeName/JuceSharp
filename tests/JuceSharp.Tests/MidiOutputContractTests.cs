using System.Reflection;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     The parts of <see cref="JuceMidiOutput" /> that need NO MIDI hardware —
///     which is every CI runner, and the reason the behavioural half of this
///     surface skips almost everywhere. The message contract, the seam and the
///     size limit are all checkable without a device, and none of them was.
/// </summary>
public sealed class MidiOutputContractTests
{
    private static void Validate(int length)
    {
        var m = typeof(JuceMidiOutput).GetMethod("ValidateMessageLength",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(m);
        try { m!.Invoke(null, [length]); }
        catch (TargetInvocationException ex) { throw ex.InnerException!; }
    }

    /// <summary>
    ///     <c>Send</c> documented "a raw short message (1..3 bytes)" and enforced
    ///     the limit at NEITHER end: an empty span returned silently and the
    ///     length went straight through, so an over-long message was refused by
    ///     the native dispatcher and surfaced as a generic
    ///     <c>DCA_ERR_INVALID_ARG</c> — i.e. SysEx was unsupported with nothing
    ///     anywhere saying so.
    /// </summary>
    [Fact]
    public void AnEmptyMessage_Throws_RatherThanReturningSilently()
        => Assert.Throws<ArgumentException>(() => Validate(0));

    [Fact]
    public void AMessageLongerThanTheDispatcherAccepts_ThrowsBeforeItReachesNative()
        => Assert.Throws<ArgumentOutOfRangeException>(() => Validate(4));

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    public void EveryLegalLength_IsAccepted(int length)
        => Validate(length);

    /// <summary>
    ///     The managed ceiling and the native one are the same number.
    ///     <c>MidiOutputDevice::kMaxMessageBytes</c> is where the limit really
    ///     lives; a managed constant that drifted from it would turn a legal
    ///     message into an <see cref="ArgumentOutOfRangeException" /> (or let an
    ///     illegal one through to be rejected as a bad argument).
    /// </summary>
    [Fact]
    public void TheManagedLimit_MatchesTheNativeOne()
    {
        var native = WrapperLifetimeAuditTests.ReadRepoFile(
            "src/JuceSharp.Native/src/MidiOutputDevice.h");
        Assert.Contains($"kMaxMessageBytes = {JuceMidiOutput.MaxMessageBytes};",
            native, StringComparison.Ordinal);
    }

    /// <summary>
    ///     <b>The seam is implementable.</b> <c>IJuceAudioEngine.OpenMidiOutput</c>
    ///     returned the sealed, internally-constructed <see cref="JuceMidiOutput" />
    ///     — the only member on any of the interfaces that no external implementer
    ///     could satisfy — so <c>FakeJuceAudioEngine</c> answered <c>default!</c>
    ///     and every host path that opened an output got a
    ///     <see cref="NullReferenceException" /> on first use through the one seam
    ///     that exists to make it testable.
    /// </summary>
    [Fact]
    public void TheEngineSeam_HandsBackAnInterface_AFakeCanSatisfy()
    {
        Assert.Equal(typeof(IJuceMidiOutput),
            typeof(IJuceAudioEngine).GetMethod(nameof(IJuceAudioEngine.OpenMidiOutput))!.ReturnType);
        Assert.True(typeof(IJuceMidiOutput).IsAssignableFrom(typeof(JuceMidiOutput)));

        IJuceAudioEngine engine = new FakeJuceAudioEngine();
        using var output = engine.OpenMidiOutput(3);
        Assert.Equal(3, output.DeviceIndex);

        output.SendAt(0x90, 60, 100, 4242);
        output.Send(0x80, 60, 0);
        Assert.Equal(2, output.SentCount);
        Assert.Equal((2L, 0L), output.ReadCounters());
    }

    /// <summary>
    ///     A queue-full DROP is counted and is not a bad argument. Until ABI v35
    ///     both surfaced as <c>DCA_ERR_INVALID_ARG</c>, so a saturated 4,096-entry
    ///     queue told the caller its ARGUMENTS were wrong — which is what the
    ///     caller would then believe — while the note-off it lost became a stuck
    ///     note. The engine side got this right in the same wave
    ///     (<c>midiDroppedEvents_</c>); the hardware-output path did not.
    /// </summary>
    [Fact]
    public void AQueueFullDrop_IsCounted_AndIsNotReportedAsABadArgument()
    {
        var native = WrapperLifetimeAuditTests.ReadRepoFile(
            "src/JuceSharp.Native/src/MidiOutputDevice.h");
        Assert.Contains("dropped_.fetch_add(1", native, StringComparison.Ordinal);
        Assert.Contains("SendResult::QueueFull", native, StringComparison.Ordinal);

        var abi = WrapperLifetimeAuditTests.ReadRepoFile(
            "src/JuceSharp.Native/src/dotcharter_audio.cpp");
        Assert.Contains("case dotcharter::MidiOutputDevice::SendResult::QueueFull: return DCA_ERR_DEVICE;",
            abi, StringComparison.Ordinal);

        // …and the managed surface exposes the count, in ONE call with the sent
        // count so a caller comparing them cannot read them a queue-full apart.
        Assert.NotNull(typeof(JuceMidiOutput).GetProperty(nameof(JuceMidiOutput.DroppedCount)));
        Assert.NotNull(typeof(IJuceMidiOutput).GetMethod(nameof(IJuceMidiOutput.ReadCounters)));
    }

    /// <summary>
    ///     <b>Never <c>stopThread</c>.</b> <c>juce::Thread::stopThread(ms)</c> calls
    ///     <c>killThread()</c> on expiry — <c>TerminateThread</c> on Windows — so
    ///     closing a wedged interface terminated the dispatcher inside the OS MIDI
    ///     API, quite possibly holding a driver lock, and then destroyed the
    ///     <c>juce::MidiOutput</c> under it: process-wide corruption on a user
    ///     gesture. The wrapper's own drain already settled this shape — on expiry,
    ///     log and LEAK.
    /// </summary>
    [Fact]
    public void ClosingAWedgedDispatcher_LeaksRatherThanTerminatingTheThread()
    {
        var native = WrapperLifetimeAuditTests.ReadRepoFile(
            "src/JuceSharp.Native/src/MidiOutputDevice.h");
        Assert.DoesNotContain("thread_.stopThread", native, StringComparison.Ordinal);
        Assert.Contains("waitForThreadToExit(timeoutMs)", native, StringComparison.Ordinal);

        var abi = WrapperLifetimeAuditTests.ReadRepoFile(
            "src/JuceSharp.Native/src/dotcharter_audio.cpp");
        Assert.Contains("if (! d->shutdown())", abi, StringComparison.Ordinal);
    }

    /// <summary>
    ///     A batch of messages that fall due in one dispatcher wake goes on the
    ///     wire in DEADLINE order. The queue is an unsorted vector whose
    ///     swap-with-back removal reorders it as the scan walks it, so without the
    ///     sort the batch went out in enqueue order — routine, because the sleep is
    ///     capped at 20 ms and any load overshoots it — while the class doc claimed
    ///     the queue was sorted.
    /// </summary>
    [Fact]
    public void ABatchOfDueMessages_IsSortedByDeadlineBeforeItIsSent()
    {
        var native = WrapperLifetimeAuditTests.ReadRepoFile(
            "src/JuceSharp.Native/src/MidiOutputDevice.h");
        Assert.Contains("std::stable_sort(due.begin(), due.end()", native, StringComparison.Ordinal);
    }
}
