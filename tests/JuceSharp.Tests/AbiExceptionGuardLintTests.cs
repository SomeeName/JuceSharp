using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// An exception thrown out of an <c>extern "C"</c> export does not become an
/// error code — it unwinds through the caller's C frames, which is undefined
/// behaviour and in practice <c>std::terminate</c>. The ABI has a
/// <c>guard()</c> helper for exactly this, and the list below names the 70 of the
/// header's 232 exports that reach code which can throw. (Re-measure both numbers
/// rather than carrying them forward — <see cref="TheHeaderExportCount_IsWhatThisFileClaims" />
/// now does it for you. It was 23 of 148 when this lint was written and 36 of 162
/// when the wave-2 audit corrected it, which was ALREADY stale the day it was
/// written: the fix branch was merged on top of the engine branch's 14 new
/// exports, none of which was in this list.)
/// <para>
/// The list below all reach code that can throw: <c>make_unique</c> and
/// <c>map::operator[]</c> and a <c>std::thread</c> (the DSP add-node family),
/// the IR WAV loader and the Miku phrase parsers (the node-string pair), plugin
/// code this project does not compile (every <c>dca_plugin_*</c> lifecycle
/// entry point), and JUCE's device enumeration. Teardown counts too — a plugin
/// destructor is somebody else's code.
/// </para>
/// <para>
/// The hot POLL getters are deliberately not in this list: they read atomics,
/// they cannot throw, and wrapping them would put a try frame on a path the UI
/// drives at frame rate. This is a targeted pass, not a blanket one — which is
/// why the list is explicit rather than derived, and why a new export that can
/// throw has to be added to it by hand.
/// </para>
/// </summary>
public sealed class AbiExceptionGuardLintTests
{
    private const string AbiCpp = "src/JuceSharp.Native/src/dotcharter_audio.cpp";

    public static TheoryData<string> MustBeGuarded =>
    [
        "dca_engine_create2",   // dca_engine_create is a pure alias for it (ABI v41)
        // ABI v42: the .wem decoder allocates (file read, packet table, libvorbis
        // state, read scratch). get_info and destroy only copy / delete.
        "dca_wem_open_file",
        "dca_wem_open_memory",
        "dca_wem_read_f32",
        "dca_wem_read_s16",
        "dca_wem_seek",
        // ABI v43: the device-rate converter decodes and allocates the whole song;
        // the preload digest is cheap but hashes through the guard all the same.
        "dca_convert_audio_for_device",
        "dca_source_get_preload_digest",
        // ABI v44: the .wem encoder runs libvorbis and allocates the whole file.
        "dca_wem_encode_vorbis",
        // ABI v45: loading the rsamp table parses and copies the whole blob
        // (vectors, unique_ptr) and takes the registry's mutex. The row count is
        // one atomic load and stays unguarded like the other hot polls.
        "dca_rsamp_set_table",
        "dca_engine_destroy",
        "dca_engine_enumerate_output_devices",
        "dca_engine_enumerate_input_devices",
        // ABI v49: the channel names come from JUCE's device as a StringArray.
        "dca_engine_get_channel_names",
        "dca_engine_set_mirror_device",
        // dca_engine_dsp_add_node is deliberately absent: it is a pure alias
        // for seat 0 and the guard lives in the export it delegates to.
        "dca_engine_dsp_add_node_for_player",
        "dca_engine_dsp_add_node_bypassed_for_player",
        "dca_engine_dsp_remove_node",
        "dca_engine_dsp_set_node_string",
        "dca_engine_dsp_get_node_string",
        "dca_engine_dsp_clear",
        "dca_engine_dsp_clear_for_player",
        "dca_engine_dsp_process_buffer",
        "dca_plugin_unload",
        "dca_plugin_attach_to_input",
        "dca_plugin_attach_to_input_for_player",
        "dca_plugin_detach_from_input",
        "dca_plugin_show_editor",
        "dca_plugin_set_editor_closed_callback",
        "dca_plugin_hide_editor",
        "dca_plugin_save_state",
        "dca_plugin_load_state",
        "dca_plugin_attach_to_instrument",
        "dca_plugin_detach_from_instrument",
        // ABI v33. dca_plugin_load_vst3 is deliberately ABSENT since v33: it is
        // now a pure alias for component 0 and the guard lives in the export it
        // delegates to, exactly like dca_engine_dsp_add_node above. Both of
        // these reach plugin code this project does not compile, and a cold
        // VST3 scan at that.
        "dca_plugin_count_vst3_components",
        "dca_plugin_load_vst3_component",
        // dca_engine_dsp_get_node_type copies a std::string out of the chain.
        "dca_engine_dsp_get_node_type",
        // These three reach DspChain::publishListLocked, which does
        // std::make_shared<Order>() + reserve(); move_node additionally does
        // idOrder_.insert(). dca_engine_dsp_remove_node sits three lines above
        // them in the ABI file and was guarded for exactly that reason, so the
        // omission was an oversight, not a decision (wave-2 audit finding #5).
        "dca_engine_dsp_move_node",
        "dca_engine_dsp_set_bypass",
        "dca_engine_dsp_set_node_branch",
        // ABI v34. The edit scope allocates the shadow (editAdded_/editRemoved_)
        // and commit publishes up to two ordering snapshots; set_node_sidechain
        // publishes as well. Named here so removing a guard from one of them
        // cannot pass silently — which it would have before this entry existed.
        "dca_engine_dsp_begin_edit_for_player",
        "dca_engine_dsp_commit_edit_for_player",
        "dca_engine_dsp_abort_edit_for_player",
        "dca_engine_dsp_set_node_sidechain",
        // The v34 ENGINE wave — the fourteen exports the wave-2 fix branch never
        // saw, because it was merged UNDER them. dca_midi_output_open starts a
        // std::thread (dca_midi_input_open is in this list for exactly that
        // reason), _enumerate builds a juce::String, _close joins a thread and
        // destroys a juce::MidiOutput, _send constructs a juce::MidiMessage and
        // push_backs it, and the automated render allocates and reaches every
        // module's setParam. Removing a guard from any of them passed silently
        // until now, which is the same gap that produced wave-2 finding #5.
        "dca_midi_output_enumerate",
        "dca_midi_output_open",
        "dca_midi_output_close",
        "dca_midi_output_send",
        "dca_midi_output_send_at",
        "dca_engine_dsp_process_buffer_automated_for_player",
        // W4-N5. It opens a THIRD-PARTY window — an ASIO driver's own control
        // panel — and it was one of the exports with no guard at all. A vendor
        // dialog is exactly the population guard() exists for. (The catch-all
        // INSIDE its callSync body is a SEPARATE requirement, pinned by
        // NativeCallbackGuardLintTests: callSync runs the body on the message
        // thread, so a guard() out here never sees an exception thrown in it.)
        "dca_engine_show_asio_panel",
        // ABI v36. dca_stems_separate_file2 reaches the same demucs.cpp
        // inference, filesystem and WAV writing as its v35 twin — which is in
        // this list one line down — and additionally invokes a MANAGED
        // delegate. dca_midi_output_get_counts3 is deliberately ABSENT for the
        // same reason its v35 twin is: three relaxed atomic loads cannot throw.
        "dca_stems_separate_file2",
        // ABI v37. The injection setter builds the take (a vector the size of
        // the take — bad_alloc territory for a long one) and both it and the
        // clear free a replaced take and may log; the tail count RETIRES finished
        // spill tails, i.e. publishes an ordering snapshot (make_shared). The
        // playing / loop / seek / status / capture-read exports are atomics and
        // a lock_guard and stay unguarded like the other hot polls.
        "dca_engine_set_input_injection_for_player",
        // v47: the external process insert maps, allocates and locks.
        "dca_engine_extproc_open_for_player",
        "dca_engine_extproc_close_for_player",
        "dca_engine_extproc_get_stats_for_player",
        "dca_engine_clear_input_injection_for_player",
        "dca_engine_dsp_get_spill_tail_count_for_player",
        // ABI v38. The note upload validates and COPIES a whole chart (vectors,
        // an unordered_map for chord membership, std::string errors), enabling
        // and the voice setter build and prepare a voice, the clock binding and
        // the status take a std::mutex. set_params / route / voice_is_available
        // are atomics and stay unguarded like the other hot setters.
        "dca_engine_set_song_clock_source",
        "dca_engine_chart_source_set_notes_for_player",
        "dca_engine_chart_source_set_notes_for_player2",
        "dca_engine_chart_source_set_enabled_for_player",
        "dca_engine_chart_source_set_voice_for_player",
        "dca_engine_chart_source_get_status_for_player",
        // ABI v40. Setting an instrument builds and prepares a voice (the pack
        // LOADS: file I/O, vectors); the getter takes a std::mutex; the model
        // info stats pack files (std::filesystem). dca_chart_voice_pack_bytes
        // carries its own try/catch (it returns a value, not a status).
        "dca_engine_chart_source_set_instrument_for_player",
        "dca_engine_chart_source_get_instrument_for_player",
        "dca_chart_model_get_info",
        // …and the ones that were already guarded before this pass, so removing
        // one of those is caught too.
        "dca_render_to_wav",
        "dca_midi_input_enumerate",
        "dca_midi_input_open",
    ];

    /// <summary>
    ///     The export count in this file's own summary, MEASURED. Both numbers in
    ///     it have been wrong before — 162 was true on the wave-2 fix branch and
    ///     false the moment it merged — and <c>docs/native/c-abi.md</c> carries the
    ///     same figure with its own "re-measure rather than carrying this number
    ///     forward" warning, which was ignored in that very range. Nothing
    ///     asserted it, so it simply kept rotting.
    /// </summary>
    [Fact]
    public void TheHeaderExportCount_IsWhatThisFileClaims()
    {
        var header = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/include/dotcharter_audio.h"));
        var declared = Regex.Matches(header, @"^DCA_API\b", RegexOptions.Multiline).Count;
        var claimed  = Regex.Match(File.ReadAllText(FindRepoFile("tests/JuceSharp.Tests/AbiExceptionGuardLintTests.cs")),
                                   @"header's (\d+) exports");
        Assert.True(claimed.Success, "this file no longer states an export count at all");
        Assert.Equal(declared, int.Parse(claimed.Groups[1].Value));

        // …and the guarded list's own size, so "41 of" cannot drift either.
        var listed = Regex.Match(File.ReadAllText(FindRepoFile("tests/JuceSharp.Tests/AbiExceptionGuardLintTests.cs")),
                                 @"names the (\d+) of the");
        Assert.True(listed.Success);
        Assert.Equal(MustBeGuarded.Count(), int.Parse(listed.Groups[1].Value));
    }

    [Theory]
    [MemberData(nameof(MustBeGuarded))]
    public void AnExportThatCanThrow_CatchesAtTheBoundary(string export)
    {
        var body = StripComments(DefinitionBodyOf(File.ReadAllText(FindRepoFile(AbiCpp)), export));
        Assert.True(
            body.Contains("guard(", StringComparison.Ordinal)
            || body.Contains("guardValue<", StringComparison.Ordinal),
            $"{export} can throw and does not catch at the ABI boundary — an exception unwinding "
            + "through extern \"C\" is std::terminate, not an error code");
    }

    /// <summary>
    /// Non-vacuity: the body extractor really finds definitions, and an export
    /// that is deliberately NOT guarded (a hot poll getter reading an atomic)
    /// really has no guard — so the assertion above is not a tautology over
    /// empty strings.
    /// </summary>
    [Fact]
    public void TheLintCanTellAGuardedExportFromAnUnguardedOne()
    {
        var src = File.ReadAllText(FindRepoFile(AbiCpp));

        var guarded = StripComments(DefinitionBodyOf(src, "dca_engine_dsp_add_node_for_player"));
        Assert.Contains("guardValue<", guarded, StringComparison.Ordinal);
        Assert.Contains("dspAddNode", guarded, StringComparison.Ordinal);

        var poll = StripComments(DefinitionBodyOf(src, "dca_engine_get_sample_rate"));
        Assert.DoesNotContain("guard(", poll, StringComparison.Ordinal);
        Assert.Contains("deviceSnapshot", poll, StringComparison.Ordinal);
    }

    /// <summary>The DEFINITION, not a call site or a forward declaration: the
    /// export list is one <c>DCA_API ... DCA_CALL name(</c> per function.</summary>
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
