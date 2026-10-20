using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Out-of-process VST bridge round-trip (2026-07-16 audit test-gap list: the
/// shared-memory handshake was desk-checked only). A managed FAKE CHILD
/// implements the child side of dotcharter_vst_bridge.h — same mapping, same
/// events, same seq handshake — so the parent engine's real audio callback
/// runs real zero-input round trips against it: request sequencing, MIDI
/// forwarding, instrument-active responses, stats, and a clean disable.
/// Layout constants mirror dcvb::*; the header static_asserts pin them
/// native-side.
///
/// Runs on the NULL device: the bridge never touches the audio driver — it
/// only needs A callback pump at a steady cadence, and pumping it from a real
/// WASAPI endpoint added nothing but endpoint flake (the "expected >=10
/// processed round trips in 3 s" window depends on the callback actually
/// running). The real-device angle that IS worth keeping — an armed bridge
/// with an absent child must not wedge a real device callback — lives in
/// <see cref="RemoteBridgeRealDeviceSmokeTests"/>.
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")] // named shm/events — like the bridge itself
[Collection("NullDevice")]
public sealed class RemoteBridgeRoundTripTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    // dcvb layout (bridge version 3).
    private const int TotalBytes = 197704;
    private const int V2TotalBytes = 197184;          // v2 had no MIDI-timing block
    private const int OffMagic = 0;
    private const int OffVersion = 4;
    private const int OffMidiTiming = 197184;         // v3: int64 blockStartFrame, uint32 offset[128]
    private const int OffMidiOffsets = OffMidiTiming + 8;
    private const int OffSampleRate = 28;
    private const int OffPrepareBlockFrames = 44;   // was reserved0 in v1
    private const int OffPrepareGeneration = 572;   // was reserved1 in v1
    private const int OffRequestSeq = 16;
    private const int OffResponseSeq = 20;
    private const int OffHeartbeat = 24;
    private const int OffBlockFrames = 32;
    private const int OffMidiCount = 40;
    private const int OffMidi = 48;
    private const int OffChainActive = 564;
    private const int OffInstrumentActive = 568;
    private const int InstrOutOffset = 131648;
    private const int ChannelBytes = 8192 * 4;

    [NativeAvailableFact]
    public void ZeroInputRoundTrip_ForwardsMidi_CountsBlocks_AndDisablesCleanly()
    {
        using var engine = NullDevice.CreateEngine();
        Assert.True(engine.SampleRate > 0 && engine.BufferFrames > 0);

        var suffix = Guid.NewGuid().ToString("N");
        var shmName = "dcvb_test_shm_" + suffix;
        var reqName = "dcvb_test_req_" + suffix;
        var respName = "dcvb_test_resp_" + suffix;

        using var mmf = MemoryMappedFile.CreateOrOpen(shmName, TotalBytes);
        using var acc = mmf.CreateViewAccessor(0, TotalBytes);
        using var reqEvent = new EventWaitHandle(false, EventResetMode.AutoReset, reqName);
        using var respEvent = new EventWaitHandle(false, EventResetMode.AutoReset, respName);

        // Pre-write the instrument response pattern once — the child's only
        // per-block work is flags + seq.
        for (var ch = 0; ch < 2; ch++)
            for (var i = 0; i < 64; i++)
                acc.Write(InstrOutOffset + ch * ChannelBytes + i * 4, 0.25f);

        var stopChild = false;
        long childBlocks = 0;
        var midiSeen = new List<uint>();
        var child = new Thread(() =>
        {
            uint last = acc.ReadUInt32(OffRequestSeq);
            uint beat = 0;
            while (!Volatile.Read(ref stopChild))
            {
                reqEvent.WaitOne(50);
                acc.Write(OffHeartbeat, ++beat);
                var seq = acc.ReadUInt32(OffRequestSeq);
                if (seq == last) continue;
                last = seq;

                var midiCount = Math.Min(acc.ReadUInt32(OffMidiCount), 128u);
                for (var i = 0; i < midiCount; i++)
                {
                    lock (midiSeen) midiSeen.Add(acc.ReadUInt32(OffMidi + 4 * i));
                }

                acc.Write(OffChainActive, 0u);
                acc.Write(OffInstrumentActive, 1u);
                acc.Write(OffResponseSeq, seq);
                Interlocked.Increment(ref childBlocks);
                respEvent.Set();
            }
        });
        child.Start();

        try
        {
            Assert.True(engine.SetRemoteBridge(shmName, reqName, respName, enable: true),
                "bridge enable failed");
            engine.SetInstrumentGain(1.0f);

            // MIDI queued while the bridge is armed must reach the child on
            // the next round trip, packed status | d1<<8 | d2<<16.
            engine.SendMidi(0x90, 60, 100);

            var deadline = Environment.TickCount64 + 3000;
            uint processed = 0, timeouts = 0, heartbeat = 0;
            var sawMidi = false;
            while (Environment.TickCount64 < deadline)
            {
                engine.TryGetRemoteBridgeStats(out processed, out timeouts, out heartbeat);
                lock (midiSeen) sawMidi = midiSeen.Contains(0x90u | (60u << 8) | (100u << 16));
                if (processed >= 10 && sawMidi) break;
                Thread.Sleep(25);
            }

            Assert.True(processed >= 10,
                $"expected >=10 processed round trips, got {processed} (timeouts={timeouts}, childBlocks={Interlocked.Read(ref childBlocks)})");
            Assert.True(sawMidi, "child never received the forwarded MIDI note-on");
            Assert.True(heartbeat > 0, "parent never observed the child heartbeat");

            // Disable is a barriered teardown: stats must stop advancing and
            // the child must stop receiving requests.
            Assert.True(engine.SetRemoteBridge(null, null, null, enable: false));
            engine.TryGetRemoteBridgeStats(out var processedAfterDisable, out _, out _);
            var childAtDisable = Interlocked.Read(ref childBlocks);
            Thread.Sleep(250);
            var childLater = Interlocked.Read(ref childBlocks);
            Assert.True(childLater <= childAtDisable + 1,
                $"child still receiving requests after disable ({childAtDisable} -> {childLater})");
            // Stats read as 0/valid after disable — either is fine, but they
            // must not keep counting.
            engine.TryGetRemoteBridgeStats(out var processedLater, out _, out _);
            Assert.True(processedLater <= Math.Max(processedAfterDisable, processed) + 1);
        }
        finally
        {
            Volatile.Write(ref stopChild, true);
            child.Join();
        }
    }

    /// <summary>
    /// The child is told its rate and block ONCE, as launch arguments, and
    /// bridge v1's <c>sampleRate</c> field was read exactly once in
    /// <c>openBridge</c>. So after <c>dca_engine_reconfigure</c> every
    /// sandboxed VST3 went on running at the rate the child was LAUNCHED with,
    /// and a plugin prepared for 256 frames could be handed 1024 — which
    /// entitles it to allocate, on the audio worker the parent's callback is
    /// blocked waiting for. That is exactly the bug
    /// <c>reprepareAttachedPlugins</c> closed for IN-PROCESS plugins, one
    /// process boundary away.
    /// <para>
    /// This is the PARENT half: rate and block must be republished on every
    /// device change, and the generation counter — the release that publishes
    /// the pair — must move. The child half (re-prepare on the message thread,
    /// silence until it lands) needs the real <c>juce_vst_host.exe</c> and is
    /// not reachable from a managed fake child.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void ADeviceChange_RepublishesRateAndBlockToTheChild()
    {
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 480);

        var suffix = Guid.NewGuid().ToString("N");
        var shmName = "dcvb_prep_shm_" + suffix;
        var reqName = "dcvb_prep_req_" + suffix;
        var respName = "dcvb_prep_resp_" + suffix;

        using var mmf = MemoryMappedFile.CreateOrOpen(shmName, TotalBytes);
        using var acc = mmf.CreateViewAccessor(0, TotalBytes);
        using var reqEvent = new EventWaitHandle(false, EventResetMode.AutoReset, reqName);
        using var respEvent = new EventWaitHandle(false, EventResetMode.AutoReset, respName);

        Assert.True(engine.SetRemoteBridge(shmName, reqName, respName, enable: true),
            "bridge enable failed");
        try
        {
            // Arming publishes the current configuration and bumps the
            // generation once, so the child can latch it without mistaking the
            // arm for a device change.
            Assert.Equal(48000, acc.ReadInt32(OffSampleRate));
            Assert.Equal(480u, acc.ReadUInt32(OffPrepareBlockFrames));
            var genAtArm = acc.ReadUInt32(OffPrepareGeneration);
            Assert.True(genAtArm > 0, "arming the bridge did not publish a configuration generation");

            engine.Reconfigure(44100, 256, null, null);
            Assert.Equal(44100, engine.SampleRate);
            Assert.Equal(256, engine.BufferFrames);

            Assert.Equal(44100, acc.ReadInt32(OffSampleRate));
            Assert.Equal(256u, acc.ReadUInt32(OffPrepareBlockFrames));
            Assert.True(acc.ReadUInt32(OffPrepareGeneration) > genAtArm,
                "the device changed and the child was never told: the prepare generation "
                + $"is still {genAtArm}, so a v1 child keeps running at 48000/480");
        }
        finally
        {
            engine.SetRemoteBridge(null, null, null, enable: false);
        }
    }

    /// <summary>
    /// JuceSharp #28 (bridge v3). MIDI to a SANDBOXED instrument used to cross
    /// the bridge with no sample offset: the child stamped every event at
    /// sample 0 of whichever block drained it, so DotCharter had to send the
    /// sandbox its notes "now" from a ~4 ms timer instead of scheduling them on
    /// the render clock. Now the parent PARKS a scheduled event until the block
    /// that contains its frame and sends its offset beside it — this asserts
    /// that what the child reads off the mapping puts each note on EXACTLY the
    /// frame it was scheduled for (<c>blockStartFrame + offset == frame</c>).
    /// <para>
    /// The deterministic, allocation-checked version of this is the native RT
    /// gate's <c>RtSandboxMidiLandsOnItsSampleWithoutAllocating</c> (manual
    /// pump). This one runs the real free-running null device through the
    /// public managed API, the way DotCharter drives it.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void ScheduledMidi_ReachesTheSandboxChildOnItsExactFrame()
    {
        using var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 256);

        var suffix = Guid.NewGuid().ToString("N");
        var shmName = "dcvb_midi_shm_" + suffix;
        var reqName = "dcvb_midi_req_" + suffix;
        var respName = "dcvb_midi_resp_" + suffix;

        using var mmf = MemoryMappedFile.CreateOrOpen(shmName, TotalBytes);
        using var acc = mmf.CreateViewAccessor(0, TotalBytes);
        using var reqEvent = new EventWaitHandle(false, EventResetMode.AutoReset, reqName);
        using var respEvent = new EventWaitHandle(false, EventResetMode.AutoReset, respName);

        var stopChild = false;
        var seen = new List<(uint Packed, long BlockStart, uint Offset, uint Frames)>();
        var child = new Thread(() =>
        {
            uint last = acc.ReadUInt32(OffRequestSeq);
            uint beat = 0;
            while (!Volatile.Read(ref stopChild))
            {
                reqEvent.WaitOne(50);
                acc.Write(OffHeartbeat, ++beat);
                var seq = acc.ReadUInt32(OffRequestSeq);
                if (seq == last) continue;
                last = seq;

                var frames = acc.ReadUInt32(OffBlockFrames);
                var blockStart = acc.ReadInt64(OffMidiTiming);
                var midiCount = Math.Min(acc.ReadUInt32(OffMidiCount), 128u);
                for (var i = 0; i < midiCount; i++)
                {
                    var packed = acc.ReadUInt32(OffMidi + 4 * i);
                    var offset = acc.ReadUInt32(OffMidiOffsets + 4 * i);
                    lock (seen) seen.Add((packed, blockStart, offset, frames));
                }

                acc.Write(OffChainActive, 0u);
                acc.Write(OffInstrumentActive, 1u);
                acc.Write(OffResponseSeq, seq);
                respEvent.Set();
            }
        });
        child.Start();

        try
        {
            Assert.True(engine.SetRemoteBridge(shmName, reqName, respName, enable: true),
                "bridge enable failed");

            // Let the round trips settle before reading the clock.
            Assert.True(SpinUntil(() => engine.TryGetRemoteBridgeStats(out var p, out _, out _) && p >= 8),
                "the bridge never completed a round trip");
            Assert.True(engine.TryGetRenderClock(out var now, out _), "no render clock");

            // Far enough ahead (~100 ms) that no event can be late, and spread
            // across blocks at offsets that are NOT block-aligned.
            var want = new (byte Note, long Frame)[]
            {
                (60, now + 4800 + 0), (61, now + 4800 + 37), (62, now + 4800 + 300), (63, now + 4800 + 1001),
            };
            foreach (var (note, frame) in want) engine.SendMidi(0x90, note, 100, frame);

            bool AllArrived()
            {
                lock (seen)
                    return want.All(w => seen.Any(s => ((s.Packed >> 8) & 0x7F) == w.Note));
            }

            Assert.True(SpinUntil(AllArrived), "not every scheduled note reached the child");

            List<(uint Packed, long BlockStart, uint Offset, uint Frames)> snapshot;
            lock (seen) snapshot = [.. seen];
            var report = string.Join(" ", snapshot.Select(s =>
                $"n{(s.Packed >> 8) & 0x7F}@{s.BlockStart}+{s.Offset}/{s.Frames}"));

            foreach (var (note, frame) in want)
            {
                var hit = snapshot.First(s => ((s.Packed >> 8) & 0x7F) == note);
                Assert.True(hit.Offset < hit.Frames, $"note {note}: offset {hit.Offset} outside its {hit.Frames}-frame block");
                Assert.True(hit.BlockStart + hit.Offset == frame,
                    $"note {note} was scheduled for frame {frame} and the child would play it at "
                    + $"{hit.BlockStart + hit.Offset} ({hit.BlockStart}+{hit.Offset}) — off by "
                    + $"{hit.BlockStart + hit.Offset - frame} frames. Seen: {report}");
            }

            // Not vacuous: at least one note must sit off sample 0, or a bridge
            // that ignored the offset would pass whenever the blocks happened
            // to line up.
            Assert.Contains(snapshot, s => s.Offset != 0);
            Assert.Equal(0L, engine.GetMidiTimingStats().Late);
        }
        finally
        {
            engine.SetRemoteBridge(null, null, null, enable: false);
            Volatile.Write(ref stopChild, true);
            child.Join();
        }
    }

    /// <summary>
    /// A mapping left by a bridge-v2 process is 520 bytes SHORTER than v3's
    /// (no MIDI-timing block), and a named mapping keeps the size it was
    /// created with. The parent must REFUSE it — returning false, which
    /// DotCharter turns into in-process hosting — rather than view past its
    /// end or write v3 offsets a v2 child would never read.
    /// </summary>
    [NativeAvailableFact]
    public void AMappingLeftByABridgeV2Process_IsRefused()
    {
        using var engine = NullDevice.CreateEngine();
        var suffix = Guid.NewGuid().ToString("N");
        var shmName = "dcvb_v2_shm_" + suffix;

        using var mmf = MemoryMappedFile.CreateOrOpen(shmName, V2TotalBytes);
        using (var acc = mmf.CreateViewAccessor(0, V2TotalBytes))
        {
            acc.Write(OffMagic, 0x44435642u);
            acc.Write(OffVersion, 2u);
        }

        Assert.False(engine.SetRemoteBridge(shmName, "dcvb_v2_req_" + suffix, "dcvb_v2_resp_" + suffix, enable: true),
            "the parent armed a bridge over a v2-sized mapping");

        // ...and a FULL-SIZED mapping stamped with another version is refused
        // too (the pre-existing check, now guarding v3).
        var shm3 = "dcvb_v3x_shm_" + suffix;
        using var mmf3 = MemoryMappedFile.CreateOrOpen(shm3, TotalBytes);
        using (var acc = mmf3.CreateViewAccessor(0, TotalBytes))
        {
            acc.Write(OffMagic, 0x44435642u);
            acc.Write(OffVersion, 2u);
            acc.Write(8, 8192u);
            acc.Write(12, 2u);
        }

        Assert.False(engine.SetRemoteBridge(shm3, "dcvb_v3x_req_" + suffix, "dcvb_v3x_resp_" + suffix, enable: true),
            "the parent armed a bridge over a mapping stamped bridge v2");

        // Non-vacuity: the same engine DOES arm over a fresh mapping.
        var ok = "dcvb_ok_shm_" + suffix;
        try
        {
            Assert.True(engine.SetRemoteBridge(ok, "dcvb_ok_req_" + suffix, "dcvb_ok_resp_" + suffix, enable: true));
        }
        finally
        {
            engine.SetRemoteBridge(null, null, null, enable: false);
        }
    }

    private static bool SpinUntil(Func<bool> condition)
    {
        // Deadlock detector, not a budget: each condition lands in tens of ms.
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < TimeSpan.FromSeconds(10))
        {
            if (condition()) return true;
            Thread.Sleep(5);
        }

        return condition();
    }
}

/// <summary>
/// Real-device smoke for the bridge family: arming the bridge when the child
/// never answers must degrade to bounded per-block timeouts (dropping to a
/// zero-wait after three misses) without wedging a REAL device callback, and
/// disable must still tear down cleanly. That timeout path is the one bridge
/// behaviour whose cost lands on a real driver's deadline.
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
[Collection("NativeEngine")]
public sealed class RemoteBridgeRealDeviceSmokeTests
{
    private const int TotalBytes = 197704;

    /// <summary>
    ///     Deadlock detector for the two condition waits below — NOT a budget
    ///     anything is graded against. Both conditions land in tens of
    ///     milliseconds on an idle box; the cap only exists so a wedged engine
    ///     fails with a message instead of hanging the run.
    /// </summary>
    private static readonly TimeSpan ConditionCap = TimeSpan.FromSeconds(10);

    private static bool WaitFor(Func<bool> condition)
    {
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < ConditionCap)
        {
            if (condition()) return true;
            Thread.Sleep(5);
        }

        return condition();
    }

    [NativeAvailableFact]
    public void ArmedBridge_WithAbsentChild_DoesNotWedgeTheRealCallback()
    {
        using var engine = new JuceAudioEngine();
        if (engine.SampleRate <= 0 || engine.BufferFrames <= 0)
            Assert.Skip("no audio device on this host — needs a running callback");

        var suffix = Guid.NewGuid().ToString("N");
        var shmName = "dcvb_smoke_shm_" + suffix;
        var reqName = "dcvb_smoke_req_" + suffix;
        var respName = "dcvb_smoke_resp_" + suffix;

        // Kernel objects exist, but nothing ever services them.
        using var mmf = MemoryMappedFile.CreateOrOpen(shmName, TotalBytes);
        using var reqEvent = new EventWaitHandle(false, EventResetMode.AutoReset, reqName);
        using var respEvent = new EventWaitHandle(false, EventResetMode.AutoReset, respName);

        Assert.True(engine.SetRemoteBridge(shmName, reqName, respName, enable: true),
            "bridge enable failed");
        try
        {
            // Wait for the CONDITION, never for a fixed span. The old form slept
            // 400 ms and read the counters once: on a contended box a real
            // endpoint can miss that window entirely, so the test failed under
            // parallel load and passed 3/3 in isolation — a flake that says
            // nothing about the bridge. The cap below is a deadlock detector,
            // not an assertion: what is asserted is still "processed == 0 and
            // timeouts > 0", i.e. an armed bridge with no child times out
            // instead of servicing anything.
            uint processed = 0, timeouts = 0;
            var sawTimeout = WaitFor(() =>
            {
                engine.TryGetRemoteBridgeStats(out processed, out timeouts, out _);
                return timeouts > 0;
            });
            Assert.Equal(0u, processed);
            Assert.True(sawTimeout,
                $"armed bridge with absent child never registered a timeout within {ConditionCap.TotalSeconds:0} s");

            // The real callback must still be rendering underneath — again a
            // condition, so a starved thread costs time and not a failure.
            if (engine.TryGetRenderClock(out var f0, out _))
            {
                long f1 = 0;
                var advanced = WaitFor(() => engine.TryGetRenderClock(out f1, out _) && f1 > f0);
                Assert.True(advanced,
                    $"device callback stopped rendering while the bridge timed out (frames stuck at {f0})");
            }
        }
        finally
        {
            Assert.True(engine.SetRemoteBridge(null, null, null, enable: false));
        }
    }
}
