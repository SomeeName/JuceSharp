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

    // dcvb layout (bridge version 4).
    private const int TotalBytes = 197712;
    private const int V2TotalBytes = 197184;          // v2 had no MIDI-timing block
    private const int V3TotalBytes = 197704;          // v3 had no seat-routing block
    private const int OffRequestSeat = 197704;        // v4: uint32 seat this request is for
    private const int OffSeatChainMask = 197708;      // v4: uint32, bit s = seat s has a chain
    private const int InputOffset = 576;
    private const int ChainOutOffset = 66112;
    private const int OffMagic = 0;
    private const int OffVersion = 4;
    private const int OffMidiTiming = 197184;         // v3: int64 blockStartFrame, uint32 offset[128]
    private const int OffMidiOffsets = OffMidiTiming + 8;
    private const int OffSampleRate = 28;
    private const int OffPrepareBlockFrames = 44;   // was reserved0 in v1
    private const int OffPrepareGeneration = 572;   // was reserved1 in v1
    private const int OffRequestSeq = 16;
    // dcvb::kRequestWriting: the parent is mid-way through writing the next
    // request, so there is nothing to read yet (its event comes after the publish).
    private const uint RequestWriting = 0xFFFFFFFFu;
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
                if (seq == last || seq == RequestWriting) continue;
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
                if (seq == last || seq == RequestWriting) continue;
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

        // ...and so is a v3-SIZED mapping (#646: a v3 child ignores the seat).
        var shmV3 = "dcvb_v3_shm_" + suffix;
        using var mmfV3 = MemoryMappedFile.CreateOrOpen(shmV3, V3TotalBytes);
        using (var acc = mmfV3.CreateViewAccessor(0, V3TotalBytes))
        {
            acc.Write(OffMagic, 0x44435642u);
            acc.Write(OffVersion, 3u);
            acc.Write(8, 8192u);
            acc.Write(12, 2u);
        }

        Assert.False(engine.SetRemoteBridge(shmV3, "dcvb_v3_req_" + suffix, "dcvb_v3_resp_" + suffix, enable: true),
            "the parent armed a bridge over a v3-sized mapping");

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

    /// <summary>
    /// DotCharter #646 / #696 (ABI v50): a sandboxed effect belongs to its seat,
    /// and every seat's chain has a child of its OWN. Each seat round-trips with
    /// its own input and its seat number to its own child, but only once that
    /// child says the seat holds a chain plugin; seat 0 keeps its own trip, its
    /// own input and all the MIDI. Seat 1's fake child processes a seat-1
    /// request (it halves it) and seat 0's leaves seat 0 dry, so seat 1 is heard
    /// processed and seat 0 unchanged. A child's mask lives in its mapping, so a
    /// FRESH engine re-armed over both (an engine rebuild) routes seat 1 again
    /// with no word from the child.
    /// </summary>
    [NativeAvailableFact]
    public void EachSeatRoundTripsItsOwnInputToItsOwnChild_OnlyWhenThatChildHoldsAChainForIt_AndAgainAfterARebuild()
    {
        var suffix = Guid.NewGuid().ToString("N");
        using var child0 = new FakeSeatChild("dcvb_seat0_" + suffix, chainSeat: 0);
        using var child1 = new FakeSeatChild("dcvb_seat1_" + suffix, chainSeat: 1);

        using (var engine = ArmTwoSeats(child0, child1))
        {
            // No chain for seat 1 yet: only seat 0's trips, carrying seat 0's
            // input to seat 0's child, and both seats are heard dry.
            Assert.True(SpinUntil(() => child0.Count(0) >= 20), "seat 0 never round-tripped to its child");
            Assert.Equal(0, child1.Count(0) + child1.Count(1));
            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude);

            // Seat 1's child loads a plugin on seat 1 and publishes it. MIDI keeps
            // flowing, and only seat 0's trip may carry it.
            child1.PublishSeatMask(0b10u);
            Assert.True(SpinUntil(() =>
            {
                engine.SendMidi(0x90, 60, 100);
                return child1.Count(1) >= 20 && child0.MidiTrips(0) > 0;
            }), "seat 1 never round-tripped once its child held a chain, or no MIDI reached seat 0's trip");
            AssertEachChildCarriedOnlyItsSeat(child0, child1);
            // Seat 1 is processed (its child's chain halves it); seat 0 is unchanged.
            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude * 0.5f);
        }

        child0.Clear();
        child1.Clear();

        // Rebuild: a fresh engine over the surviving mappings and children.
        using (var rebuilt = ArmTwoSeats(child0, child1))
        {
            Assert.True(SpinUntil(() => child1.Count(1) >= 20 && child0.Count(0) >= 20),
                "a seat stopped round-tripping to its child after the engine was rebuilt");
            AssertEachChildCarriedOnlyItsSeat(child0, child1);
            AssertSeatsSettleAt(rebuilt, Seat0Amplitude, Seat1Amplitude * 0.5f);
        }
    }

    // Seat 0 hears a 0.1 sine, seat 1 a 0.4 one: a trip that carried the wrong
    // seat's input shows up as the wrong peak.
    private const float Seat0Amplitude = 0.1f;
    private const float Seat1Amplitude = 0.4f;

    private static JuceAudioEngine ArmTwoSeats(FakeSeatChild child0, FakeSeatChild child1)
    {
        var engine = NullDevice.CreateEngine(sampleRate: 48000, bufferFrames: 256);
        engine.SetPlayerCount(2);
        foreach (var (seat, amplitude) in new[] { (0, Seat0Amplitude), (1, Seat1Amplitude) })
        {
            var take = new float[4800];
            for (var i = 0; i < take.Length; i++) take[i] = amplitude * MathF.Sin(2f * MathF.PI * 1000f * i / 48000f);
            engine.SetInputInjection(seat, take, loop: true);
            Assert.True(engine.SetInputInjectionPlaying(seat, true), $"seat {seat} has no take to play");
        }

        Assert.True(engine.SetRemoteBridge(0, child0.Shm, child0.Request, child0.Response, enable: true), "seat 0's bridge enable failed");
        Assert.True(engine.SetRemoteBridge(1, child1.Shm, child1.Request, child1.Response, enable: true), "seat 1's bridge enable failed");
        return engine;
    }

    private static void AssertEachChildCarriedOnlyItsSeat(FakeSeatChild child0, FakeSeatChild child1)
    {
        var seen0 = child0.Snapshot();
        var seen1 = child1.Snapshot();
        Assert.All(seen0, r => Assert.True(r.Seat == 0, $"seat 0's child received a request for seat {r.Seat}"));
        Assert.All(seen1, r => Assert.True(r.Seat == 1, $"seat 1's child received a request for seat {r.Seat}"));
        // Only seat 0's trip carries MIDI: the instrument is seat 0's.
        Assert.All(seen1, r => Assert.Equal(0u, r.MidiCount));
        // Skip the 5 ms play ramp: from the tenth trip of a seat its input is at full level.
        foreach (var (seen, amplitude) in new[] { (seen0, Seat0Amplitude), (seen1, Seat1Amplitude) })
        {
            var settled = seen.Skip(10).ToList();
            Assert.NotEmpty(settled);
            Assert.All(settled, r => Assert.InRange(r.Peak, amplitude * 0.8f, amplitude * 1.05f));
        }
    }

    /// <summary>
    /// A managed fake of ONE seat's sandbox child: its own mapping and events,
    /// the child side of the bridge handshake, a log of the requests it served,
    /// and a "chain" that halves the requests of <c>chainSeat</c> once its seat
    /// mask names that seat. Requests for any other seat come back inactive (dry).
    /// </summary>
    private sealed class FakeSeatChild : IDisposable
    {
        private readonly MemoryMappedFile _mmf;
        private readonly MemoryMappedViewAccessor _acc;
        private readonly EventWaitHandle _req;
        private readonly EventWaitHandle _resp;
        private readonly Thread _thread;
        private readonly List<(uint Seat, uint MidiCount, float Peak)> _requests = [];
        private readonly uint _chainSeat;
        private volatile bool _stop;

        public FakeSeatChild(string prefix, uint chainSeat)
        {
            Shm = prefix + "_shm";
            Request = prefix + "_req";
            Response = prefix + "_resp";
            _chainSeat = chainSeat;
            _mmf = MemoryMappedFile.CreateOrOpen(Shm, TotalBytes);
            _acc = _mmf.CreateViewAccessor(0, TotalBytes);
            _req = new EventWaitHandle(false, EventResetMode.AutoReset, Request);
            _resp = new EventWaitHandle(false, EventResetMode.AutoReset, Response);
            _thread = new Thread(Loop) { IsBackground = true };
            _thread.Start();
        }

        public string Shm { get; }
        public string Request { get; }
        public string Response { get; }

        public void PublishSeatMask(uint mask) => _acc.Write(OffSeatChainMask, mask);

        public int Count(uint seat)
        {
            lock (_requests) return _requests.Count(r => r.Seat == seat);
        }

        public int MidiTrips(uint seat)
        {
            lock (_requests) return _requests.Count(r => r.Seat == seat && r.MidiCount > 0);
        }

        public List<(uint Seat, uint MidiCount, float Peak)> Snapshot()
        {
            lock (_requests) return [.. _requests];
        }

        public void Clear()
        {
            lock (_requests) _requests.Clear();
        }

        private void Loop()
        {
            var block = new float[8192];
            uint last = _acc.ReadUInt32(OffRequestSeq);
            uint beat = 0;
            while (!_stop)
            {
                _req.WaitOne(50);
                _acc.Write(OffHeartbeat, ++beat);
                var seq = _acc.ReadUInt32(OffRequestSeq);
                if (seq == last || seq == RequestWriting) continue;
                last = seq;

                var seat = _acc.ReadUInt32(OffRequestSeat);
                var frames = (int)Math.Min(_acc.ReadUInt32(OffBlockFrames), 8192u);
                _acc.ReadArray(InputOffset, block, 0, frames);
                var peak = 0f;
                for (var i = 0; i < frames; i++) peak = MathF.Max(peak, MathF.Abs(block[i]));
                lock (_requests) _requests.Add((seat, _acc.ReadUInt32(OffMidiCount), peak));

                var chain = seat == _chainSeat && (_acc.ReadUInt32(OffSeatChainMask) & (1u << (int)seat)) != 0;
                if (chain)
                {
                    for (var i = 0; i < frames; i++) block[i] *= 0.5f;
                    _acc.WriteArray(ChainOutOffset, block, 0, frames);
                    _acc.WriteArray(ChainOutOffset + ChannelBytes, block, 0, frames);
                }

                _acc.Write(OffChainActive, chain ? 1u : 0u);
                _acc.Write(OffInstrumentActive, 0u);
                _acc.Write(OffResponseSeq, seq);
                _resp.Set();
            }
        }

        public void Dispose()
        {
            _stop = true;
            _thread.Join();
            _req.Dispose();
            _resp.Dispose();
            _acc.Dispose();
            _mmf.Dispose();
        }
    }

    // What each seat is HEARD at: the per-seat post-chain peak, which the bridge's
    // answer feeds. One round trip that misses its block leaves that block dry,
    // so this waits for a clean read window instead of trusting the first one.
    private static void AssertSeatsSettleAt(JuceAudioEngine engine, float seat0, float seat1)
    {
        float p0 = 0f, p1 = 0f;
        _ = engine.GetAndResetPlayerInputPeak(0);
        _ = engine.GetAndResetPlayerInputPeak(1);
        var settled = SpinUntil(() =>
        {
            p0 = engine.GetAndResetPlayerInputPeak(0);
            p1 = engine.GetAndResetPlayerInputPeak(1);
            return p0 >= seat0 * 0.9f && p0 <= seat0 * 1.05f && p1 >= seat1 * 0.9f && p1 <= seat1 * 1.05f;
        });
        Assert.True(settled, $"seat 0 heard at {p0} (want {seat0}), seat 1 at {p1} (want {seat1})");
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
    private const int TotalBytes = 197712;

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
