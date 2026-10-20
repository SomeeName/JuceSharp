using JuceSharp.Interop;

namespace JuceSharp;

/// <content>
///     ABI v37: the cursorless positional capture read a DI recorder follows the
///     input with, and the per-seat INPUT INJECTION a re-amp plays a recorded
///     take through. Kept out of <c>JuceAudioEngine.cs</c> so that file reads
///     unchanged. Every call pins the engine handle for its duration.
/// </content>
public sealed partial class JuceAudioEngine
{
    /// <summary>
    ///     Cursorless read of one seat's capture ring: copies up to
    ///     <c>dest.Length</c> samples starting at ABSOLUTE stream position
    ///     <paramref name="from" /> (0 = the first sample the ring ever captured).
    ///     <paramref name="firstPosition" /> is where the copy really began — later
    ///     than <paramref name="from" /> when those samples were already overwritten
    ///     (the difference is the gap), earlier when <paramref name="from" /> lies
    ///     past what was captured (a rebuilt engine restarted the stream; re-anchor
    ///     there). The caller owns the cursor, so this never steals samples from
    ///     the streaming reader live detection drains
    ///     (<c>dca_engine_read_input_capture_from_for_player</c>). Any non-audio
    ///     thread; returns 0 on a disposed engine or a failed read.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public int ReadInputCaptureFrom(int player, ulong from, Span<float> dest, out ulong firstPosition)
    {
        ValidatePlayer(player, nameof(player));
        firstPosition = 0;
        if (dest.IsEmpty) return 0;
        var h = TryAcquire();
        if (h == nint.Zero) return 0;
        try
        {
            unsafe
            {
                fixed (float* p = dest)
                {
                    var status = NativeMethods.EngineReadInputCaptureFromForPlayer(h, player, from, p, dest.Length,
                        out var written, out firstPosition);
                    WarnPollFailure(status, "engine_read_input_capture_from_for_player");
                    return status == NativeMethods.DcaStatus.Ok ? written : 0;
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Loads a MONO take at the ENGINE's sample rate into
    ///     <paramref name="player" />'s input, PAUSED at frame 0, replacing any take
    ///     already there. From the next block that seat's input IS the take — its
    ///     rig, capture ring and meter hear it and its live input is muted (a
    ///     paused take is silence) until <see cref="ClearInputInjection" />. The
    ///     samples are copied; the span may be reused on return
    ///     (<c>dca_engine_set_input_injection_for_player</c>). Control thread.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    /// <exception cref="ArgumentException"><paramref name="mono" /> is empty.</exception>
    /// <exception cref="JuceAudioException">The engine refused the take.</exception>
    public void SetInputInjection(int player, ReadOnlySpan<float> mono, bool loop)
    {
        ValidatePlayer(player, nameof(player));
        if (mono.IsEmpty) throw new ArgumentException("A take needs at least one sample.", nameof(mono));
        var h = Acquire();
        try
        {
            unsafe
            {
                fixed (float* p = mono)
                {
                    Check(NativeMethods.EngineSetInputInjectionForPlayer(h, player, p, mono.Length, loop ? 1 : 0),
                        "dca_engine_set_input_injection_for_player");
                }
            }
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Removes <paramref name="player" />'s take: the seat is back on its live
    ///     input from the next block. A no-op on a seat with no take
    ///     (<c>dca_engine_clear_input_injection_for_player</c>).
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public void ClearInputInjection(int player)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return;
        try
        {
            WarnIfFailed(NativeMethods.EngineClearInputInjectionForPlayer(h, player),
                "engine_clear_input_injection_for_player");
        }
        finally { Release(); }
    }

    /// <summary>
    ///     Plays or pauses the loaded take through a 5 ms ramp. Returns false when
    ///     the seat has no take (<c>dca_engine_set_input_injection_playing_for_player</c>).
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool SetInputInjectionPlaying(int player, bool playing)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try { return NativeMethods.EngineSetInputInjectionPlayingForPlayer(h, player, playing ? 1 : 0) == NativeMethods.DcaStatus.Ok; }
        finally { Release(); }
    }

    /// <summary>
    ///     Whether the loaded take wraps at its end (true) or stops and rewinds
    ///     (false). Returns false when the seat has no take
    ///     (<c>dca_engine_set_input_injection_loop_for_player</c>).
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool SetInputInjectionLoop(int player, bool loop)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try { return NativeMethods.EngineSetInputInjectionLoopForPlayer(h, player, loop ? 1 : 0) == NativeMethods.DcaStatus.Ok; }
        finally { Release(); }
    }

    /// <summary>
    ///     Moves the take's play head to <paramref name="frame" /> (engine-rate
    ///     frames, clamped into the take) at the next block. Returns false when the
    ///     seat has no take (<c>dca_engine_seek_input_injection_for_player</c>).
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool SeekInputInjection(int player, long frame)
    {
        ValidatePlayer(player, nameof(player));
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try { return NativeMethods.EngineSeekInputInjectionForPlayer(h, player, frame) == NativeMethods.DcaStatus.Ok; }
        finally { Release(); }
    }

    /// <summary>
    ///     The loaded take's transport: position and length in engine-rate frames,
    ///     playing and loop. False (and a default status) when the seat has no take
    ///     or the engine is gone (<c>dca_engine_get_input_injection_status_for_player</c>).
    ///     Any non-audio thread; cheap enough to poll per UI frame.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="player" /> is
    /// not a seat this engine has.</exception>
    public bool TryGetInputInjectionStatus(int player, out JuceInputInjectionStatus status)
    {
        ValidatePlayer(player, nameof(player));
        status = default;
        var h = TryAcquire();
        if (h == nint.Zero) return false;
        try
        {
            if (NativeMethods.EngineGetInputInjectionStatusForPlayer(h, player, out var pos, out var len,
                    out var playing, out var loop) != NativeMethods.DcaStatus.Ok) return false;
            status = new JuceInputInjectionStatus(pos, len, playing != 0, loop != 0);
            return true;
        }
        finally { Release(); }
    }
}

/// <summary>
///     A loaded re-amp take's transport, as <see cref="JuceAudioEngine.TryGetInputInjectionStatus" />
///     reads it. Frames are at the ENGINE's sample rate.
/// </summary>
public readonly struct JuceInputInjectionStatus
{
    /// <summary>Creates a status snapshot.</summary>
    /// <param name="positionFrames">The play head.</param>
    /// <param name="lengthFrames">The take's length.</param>
    /// <param name="isPlaying">Whether the take is playing.</param>
    /// <param name="isLooping">Whether the take wraps at its end.</param>
    public JuceInputInjectionStatus(long positionFrames, long lengthFrames, bool isPlaying, bool isLooping)
    {
        PositionFrames = positionFrames;
        LengthFrames = lengthFrames;
        IsPlaying = isPlaying;
        IsLooping = isLooping;
    }

    /// <summary>The play head in engine-rate frames, from 0 up to <see cref="LengthFrames" />.</summary>
    public long PositionFrames { get; }

    /// <summary>The take's length in engine-rate frames.</summary>
    public long LengthFrames { get; }

    /// <summary>True while the take is playing: false when paused, or after a one-shot take ran off its end.</summary>
    public bool IsPlaying { get; }

    /// <summary>True when the take wraps at its end instead of stopping.</summary>
    public bool IsLooping { get; }
}
