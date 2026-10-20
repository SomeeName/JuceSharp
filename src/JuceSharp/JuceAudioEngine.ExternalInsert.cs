using System.Runtime.InteropServices;
using JuceSharp.Interop;

namespace JuceSharp;

/// <content>
///     ABI v47: the EXTERNAL PROCESS INSERT. A seat's dry input is written into a
///     named shared-memory ring another process created, and that process's
///     processed audio replaces the seat's rig (see the header comment in
///     <c>dotcharter_audio.h</c> for the ring format and the real-time contract).
///     Every call pins the engine handle for its duration.
/// </content>
public sealed partial class JuceAudioEngine
{
    /// <summary>
    ///     Maps the shared-memory block <paramref name="mappingName" /> (a kernel
    ///     object name such as <c>Local\name</c>) and starts exchanging
    ///     <paramref name="player" />'s audio through it, replacing any insert
    ///     already open on the seat. <paramref name="magic" /> (null = not checked)
    ///     must equal the header's first 8 bytes. The device must already run at
    ///     the block's rate (<see cref="ExternalProcessInsertOpenResult.SampleRateMismatch" />
    ///     otherwise; nothing is opened). Never throws for a refusal. Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is not a seat this engine has.</exception>
    public ExternalProcessInsertOpenResult OpenExternalProcessInsert(int player, string mappingName, string? magic = null)
    {
        ValidatePlayer(player, nameof(player));
        ArgumentException.ThrowIfNullOrEmpty(mappingName);
        if (magic != null && System.Text.Encoding.UTF8.GetByteCount(magic) > 8)
            throw new ArgumentException("The magic is at most 8 bytes.", nameof(magic));
        var h = TryAcquire();
        if (h == nint.Zero) return ExternalProcessInsertOpenResult.Failed;
        try
        {
            var status = NativeMethods.EngineExtprocOpenForPlayer(h, player, mappingName, magic);
            return status switch
            {
                NativeMethods.DcaStatus.Ok => ExternalProcessInsertOpenResult.Opened,
                NativeMethods.DcaStatus.NotFound => ExternalProcessInsertOpenResult.NotFound,
                NativeMethods.DcaStatus.Decode => ExternalProcessInsertOpenResult.BadHeader,
                NativeMethods.DcaStatus.SampleRate => ExternalProcessInsertOpenResult.SampleRateMismatch,
                NativeMethods.DcaStatus.NotInitialized => ExternalProcessInsertOpenResult.NoDevice,
                NativeMethods.DcaStatus.Device => ExternalProcessInsertOpenResult.MapFailed,
                _ => ExternalProcessInsertOpenResult.Failed,
            };
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Stops <paramref name="player" />'s exchange: the seat dips back to its own
    ///     rig and the block is unmapped once the audio thread let go (a bounded wait
    ///     on THIS thread, ~200 ms at most). Closing a seat with nothing open is fine.
    ///     False only for a disposed engine. Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is not a seat this engine has.</exception>
    public bool CloseExternalProcessInsert(int player)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try { return NativeMethods.EngineExtprocCloseForPlayer(h, player) == NativeMethods.DcaStatus.Ok; }
        finally { Release(); }
    }

    /// <summary>
    ///     Snapshot of <paramref name="player" />'s insert; a closed seat reads
    ///     <see cref="ExternalProcessInsertState.Closed" />. False for a disposed
    ///     engine. Any thread; never blocks the audio thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is not a seat this engine has.</exception>
    public bool TryGetExternalProcessInsertStats(int player, out ExternalProcessInsertStats stats)
    {
        ValidatePlayer(player, nameof(player));
        stats = default;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            var raw = new NativeMethods.DcaExtprocStats { StructBytes = Marshal.SizeOf<NativeMethods.DcaExtprocStats>() };
            if (NativeMethods.EngineExtprocGetStatsForPlayer(h, player, ref raw) != NativeMethods.DcaStatus.Ok)
                return false;
            stats = ExternalProcessInsertStats.From(raw);
            return true;
        }
        finally { Release(); }
    }
}
