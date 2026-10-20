using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>What <see cref="JuceAudioEngine.OpenExternalProcessInsert" /> did (ABI v47).</summary>
public enum ExternalProcessInsertOpenResult
{
    /// <summary>The block is mapped and the seat exchanges its audio through it.</summary>
    Opened = 0,

    /// <summary>No shared-memory block of that name exists (the other process has not created it).</summary>
    NotFound = 1,

    /// <summary>The header is not format 1, carries another magic, or describes a ring that does not fit the block.</summary>
    BadHeader = 2,

    /// <summary>The audio device does not run at the block's rate. Nothing was opened: request that rate and open again.</summary>
    SampleRateMismatch = 3,

    /// <summary>No audio device is running.</summary>
    NoDevice = 4,

    /// <summary>The OS refused to map the block.</summary>
    MapFailed = 5,

    /// <summary>The engine is disposed, or the call failed in a way the others do not describe.</summary>
    Failed = 6,
}

/// <summary>An external process insert's state (<c>DCA_EXTPROC_STATE_*</c>, ABI v47). The ordinals are the ABI.</summary>
public enum ExternalProcessInsertState
{
    /// <summary>Nothing open on the seat.</summary>
    Closed = 0,

    /// <summary>Open; the other side has not published its block size yet. The seat plays its dry input.</summary>
    Waiting = 1,

    /// <summary>Exchanging blocks (<see cref="ExternalProcessInsertStats.Connected" /> says whether audio comes back).</summary>
    Active = 2,

    /// <summary>Open, but the device rate differs from the block's: the seat runs its own rig.</summary>
    RateMismatch = 3,

    /// <summary>The header now describes a ring this build cannot run: the seat plays its dry input.</summary>
    BadLayout = 4,
}

/// <summary>
///     A snapshot of one seat's external process insert (ABI v47,
///     <c>dca_engine_extproc_get_stats_for_player</c>) — counted by the audio
///     callback, read without blocking it.
/// </summary>
public readonly record struct ExternalProcessInsertStats
{
    /// <summary>Where the insert stands.</summary>
    public ExternalProcessInsertState State { get; init; }

    /// <summary>Processed blocks are coming back in time.</summary>
    public bool Connected { get; init; }

    /// <summary>The block's rate (Hz); 0 when closed.</summary>
    public int SampleRate { get; init; }

    /// <summary>The block's ring block in frames; 0 until the other side publishes it.</summary>
    public int BlockFrames { get; init; }

    /// <summary>Blocks per ring.</summary>
    public int RingBlocks { get; init; }

    /// <summary>The delay this exchange adds, in frames at <see cref="SampleRate" /> (the device's rate):
    /// blocks in flight plus the block being gathered, measured.</summary>
    public int LatencySamples { get; init; }

    /// <summary>Input blocks handed to the other side since the open.</summary>
    public long BlocksSent { get; init; }

    /// <summary>Processed blocks played since the open.</summary>
    public long BlocksReceived { get; init; }

    /// <summary>Blocks the seat played DRY because nothing was back in time.</summary>
    public long Underruns { get; init; }

    /// <summary>Input blocks dropped because the other side's input ring was full.</summary>
    public long Overruns { get; init; }

    /// <summary>Processed blocks dropped because they piled up past the cushion.</summary>
    public long LateBlocks { get; init; }

    internal static ExternalProcessInsertStats From(in NativeMethods.DcaExtprocStats s) => new()
    {
        State = (ExternalProcessInsertState)s.State,
        Connected = s.Connected != 0,
        SampleRate = s.SampleRate,
        BlockFrames = s.BlockFrames,
        RingBlocks = s.RingBlocks,
        LatencySamples = s.LatencySamples,
        BlocksSent = s.BlocksSent,
        BlocksReceived = s.BlocksReceived,
        Underruns = s.Underruns,
        Overruns = s.Overruns,
        LateBlocks = s.LateBlocks,
    };
}
