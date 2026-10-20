using System.Runtime.InteropServices;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// A NULL handle is a caller bug and every export in this ABI says so with
/// <c>DCA_ERR_INVALID_ARG</c>. TEN did not: they said <c>DCA_OK</c> and wrote a
/// plausible-looking zero into the caller's out parameters.
/// <para>
/// Eight were fixed in 0.48. Seven of those are the capture-read family, whose
/// callers use <c>written</c> and discard the status, so a dead engine reads as
/// "the ring was simply empty" — silence that looks like a quiet guitar,
/// forever, with nothing logged. The eighth is <c>dca_plugin_save_state</c>,
/// whose caller DOES check: it would have reported an empty state for a dead
/// plugin and written that into a saved rig.
/// </para>
/// <para>
/// The last two are <c>dca_source_get_position_seconds</c> and
/// <c>dca_source_get_length_seconds</c> (2026-08-29). Their own family already
/// disagreed with them: play / stop / seek / set_volume and
/// <c>dca_source_get_audible_position_seconds</c> have always rejected. A
/// position of 0.0 reads as "the song is at the start" and a length of 0.0 as
/// "an empty file", so the failure was a stuck playhead or a zero-length chart
/// rather than an error — including for an ORPHANED source, the shell
/// <c>dca_engine_destroy</c> leaves behind when the caller frees the engine
/// first (see <c>AuditRegressionTests.audit9_SourceOutlivingEngine_*</c>, which
/// asserts the new status).
/// </para>
/// <para>
/// NOT changed, and deliberately: <c>dca_engine_get_player_count</c> returns
/// <c>int32_t</c> with no status channel, and 0 is its documented sentinel —
/// the wrapper's <c>JuceAudioEngine.GetPlayerCount</c> returns 0 for a disposed
/// engine of its own accord. Unlike the getters above it is not a plausible
/// answer: a live engine clamps to 1..DCA_MAX_PLAYERS and can never report 0.
/// It is pinned below so the sentinel cannot drift.
/// </para>
/// <para>
/// These call the exports directly, the way <c>AbiV4Tests</c> and
/// <c>DspPerPlayerChainTests</c> do, because the wrapper cannot produce a zero
/// handle — <c>NativeHandleOwner</c> throws <c>ObjectDisposedException</c>
/// first, which is exactly why the wrapper's own tests could never have caught
/// this.
/// </para>
/// <para>
/// NOT covered here, and deliberately unchanged: a LIVE plugin that has not
/// been instantiated yet still returns <c>DCA_OK</c> with zero bytes from
/// <c>dca_plugin_save_state</c> — it genuinely has no state, which is a correct
/// answer and not a bad argument. There is no way to build one from managed
/// code, so it is written down rather than asserted; do not tidy it into the
/// handle check.
/// </para>
/// </summary>
public sealed class NullHandleRejectionTests
{
    [NativeAvailableFact]
    public unsafe void EveryCaptureRead_RejectsANullEngine()
    {
        var buf = stackalloc float[16];

        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineGetInputPeak(nint.Zero, out _));

        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCapture(nint.Zero, buf, 16, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCaptureStream(nint.Zero, buf, 16, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCaptureStream2(nint.Zero, buf, 16, out _, out _));

        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCaptureForPlayer(nint.Zero, 0, buf, 16, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCaptureStreamForPlayer(nint.Zero, 0, buf, 16, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCaptureStream2ForPlayer(nint.Zero, 0, buf, 16, out _, out _));
    }

    [NativeAvailableFact]
    public void SourcePositionAndLength_RejectANullSource()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.SourceGetPosition(nint.Zero, out var pos));
        Assert.Equal(0.0, pos);
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.SourceGetLength(nint.Zero, out var len));
        Assert.Equal(0.0, len);

        // The rest of the family, for the record: they always rejected, and
        // these two now agree with them.
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.SourceGetAudiblePosition(nint.Zero, out _));
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.SourcePlay(nint.Zero));
    }

    /// <summary>
    /// <c>dca_engine_get_player_count</c> has no status to return, so 0 is its
    /// sentinel — and a correct one, because a live engine clamps the count to
    /// 1..DCA_MAX_PLAYERS and can never answer 0. Pinned so nobody "fixes" it to
    /// -1 without changing the wrapper's documented contract too.
    /// </summary>
    [NativeAvailableFact]
    public void PlayerCount_AnswersItsSentinelForANullEngine()
    {
        Assert.Equal(0, NativeMethods.EngineGetPlayerCount(nint.Zero));
    }

    [NativeAvailableFact]
    public void PluginSaveState_RejectsANullPlugin()
    {
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.PluginSaveState(nint.Zero, null, 0, out _));

        var buf = new byte[8];
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.PluginSaveState(nint.Zero, buf, buf.Length, out _));
    }

    /// <summary>
    /// AN OUT PARAMETER IS WRITTEN BEFORE ANY FAILURE PATH. Most of this ABI
    /// already does it — <c>dca_source_get_position_seconds</c>,
    /// <c>dca_engine_get_eq_active_count</c>, <c>dca_engine_read_eq_spectrum</c>
    /// all zero first and reject second, so a caller that ignores the status is
    /// no worse off. Three did not: <c>dca_engine_dsp_get_param</c>,
    /// <c>dca_engine_get_render_clock</c> and <c>dca_source_open_file</c> (plus
    /// <c>dca_engine_create</c>) left the caller's variable untouched on the
    /// rejection path, so a discarded status meant a stale value — or, for the
    /// two that hand back a HANDLE, an uninitialised pointer the caller might
    /// then pass back in.
    /// <para>
    /// The return value is unchanged in every case: this is about what is in
    /// the caller's memory when the rejection happens.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void OutParameters_AreWrittenBeforeTheHandleIsEvenLookedAt()
    {
        var poison = 12345.0f;
        Assert.Equal((int) NativeMethods.DcaStatus.InvalidArg,
            DspGetParam(nint.Zero, 0, "gainDb", ref poison));
        Assert.Equal(0.0f, poison);

        long frames = 999, ticks = 999;
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineGetRenderClock(nint.Zero, out frames, out ticks));
        Assert.Equal(0, frames);
        Assert.Equal(0, ticks);

        var src = new nint(0xDEAD);
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            SourceOpenFile(nint.Zero, "no-such-file.wav", ref src));
        Assert.Equal(nint.Zero, src);

        // dca_engine_create is NOT in this list, and the reason is worth
        // recording: a NULL cfg is LEGAL there — it means "create the engine
        // but open no device" — so it has no cheap rejection path to probe.
        // It zeroes `*out_engine` before anything can fail all the same, which
        // is what its device-failure path needs.
    }

    [DllImport("juce_sharp", EntryPoint = "dca_engine_dsp_get_param",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspGetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, ref float value);

    [DllImport("juce_sharp", EntryPoint = "dca_source_open_file",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern NativeMethods.DcaStatus SourceOpenFile(nint engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, ref nint source);


    /// <summary>
    /// Argument validation still comes FIRST: a bad buffer or a bad seat is
    /// rejected before the handle is even looked at, so the new handle check
    /// cannot have shadowed one of those. (It also means these exports keep
    /// returning InvalidArg for a null engine AND a null dest, rather than one
    /// masking the other.)
    /// </summary>
    [NativeAvailableFact]
    public unsafe void ArgumentValidation_StillPrecedesTheHandleCheck()
    {
        // Both of these hold under the OLD behaviour too, which is the point:
        // they are testing the ORDER, not the handle check, so they stay green
        // when the handle fix is reverted and only the test above goes red.
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCapture(nint.Zero, null, 0, out _));

        var buf = stackalloc float[4];
        // Seat 9999 is past DCA_MAX_PLAYERS: rejected, never clamped.
        Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
            NativeMethods.EngineReadInputCaptureForPlayer(nint.Zero, 9999, buf, 4, out _));
    }
}
