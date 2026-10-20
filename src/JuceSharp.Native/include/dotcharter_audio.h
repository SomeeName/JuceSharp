// dotcharter_audio.h
//
// Stable C ABI exported by the DotCharter native audio engine (JUCE-based).
//
// Rules:
//   * Everything here is plain C — no C++ types in the signatures.
//   * Handles are opaque pointers; never deref on the managed side.
//   * All calls are non-throwing; failures return a dca_status code.
//   * Callbacks may be invoked from the audio thread. The managed side
//     must not allocate or take locks inside them.
//   * Backwards-incompatible changes bump DCA_ABI_VERSION.

#ifndef DOTCHARTER_AUDIO_H
#define DOTCHARTER_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#  if defined(DOTCHARTER_AUDIO_EXPORTS)
#    define DCA_API __declspec(dllexport)
#  else
#    define DCA_API __declspec(dllimport)
#  endif
#  define DCA_CALL __cdecl
#else
#  define DCA_API  __attribute__((visibility("default")))
#  define DCA_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// v30: ten additive exports across two independent surfaces — the per-node DSP
// meters + latency block (dca_engine_dsp_set_metering &c) and the engine
// dropout/xrun counters (dca_engine_get_xrun_stats &c). Nothing was reordered
// and no existing signature changed, so a v29 caller is unaffected; the bump is
// how a managed caller knows the new symbols are there to resolve at all.
// v31: two additive exports — dca_engine_set_player_input_pitch_shift{,_mode}.
// The drop-tune pedal is PER SEAT now. Until v31 the engine held ONE shifter and
// applied it to the raw device input BEFORE the per-player split, so on a couch
// with two guitars in different tunings, dropping one dropped BOTH — and the
// seat that did not ask for it was detuned with nothing on screen saying so.
// Nothing was reordered and no existing signature changed: the two pre-v31
// exports are now exactly their per-player form with player = 0.
// v32: one additive export — dca_engine_dsp_add_node_bypassed_for_player, which
// creates a regular node at ZERO wet so a caller can configure it before
// set_bypass(false) fades it in. The existing add exports stay immediately wet
// (offline compatibility), and splitter/merger topology is still immediate:
// this does not make a whole preset change atomic. Nothing was reordered and no
// existing signature changed.
// v33: five additive exports, all introspection — dca_engine_dsp_get_node_type,
// dca_engine_dsp_get_bypass, dca_dsp_module_type_category,
// dca_plugin_count_vst3_components and dca_plugin_load_vst3_component.
// The first three close a gap the managed wrapper had to report honestly rather
// than guess: a rig node's TYPE and BYPASS state could only be remembered by
// whoever created the node, so a chain enumerated by a second binding came back
// with empty type ids and every node reading un-bypassed, and the catalogue's
// category was permanently empty because nothing exported one. The last two
// reach the components of a MULTI-COMPONENT VST3 bundle: dca_plugin_load_vst3
// takes descriptor 0 and always did, so every component past the first was
// unreachable through this ABI. Nothing was reordered, no existing signature
// changed, and dca_plugin_load_vst3(engine, path, out) is exactly
// dca_plugin_load_vst3_component(engine, path, 0, out).
// v34: additive rig exports in three groups, no reordering and no existing
// signature changed. (1) EDIT SCOPES —
// dca_engine_dsp_{begin,commit,abort}_edit_for_player: a preset swap used to be
// one ordering publication and one barrier PER NODE, each audible on its own,
// so a twelve-pedal board assembled itself pedal by pedal; between begin and
// commit the structure and param writes accumulate against a shadow and the
// commit publishes them together. (2) BRANCH DELAY COMPENSATION —
// dca_engine_dsp_{set,get}_branch_pdc_for_player: the merger summed branches of
// different latency, which is a comb filter, and v23 accepted that; it is now
// compensated by DEFAULT and dca_engine_dsp_get_chain_latency describes a
// region whose branches are actually aligned. (3) SIDECHAIN —
// dca_engine_dsp_{set,get}_node_sidechain plus dca_dsp_module_type_flags: a
// compressor / gate / swell can key off the seat's dry input or the song mix
// instead of its own input. dca_dsp_module_type_info could not grow a field
// without changing its signature, so the capability is a separate flags export.
// v35: eight ADDITIVE exports and two BEHAVIOUR reversals; nothing reordered and
// no existing signature changed. (1) SEAT-0 ALIASES —
// dca_engine_dsp_{begin,commit,abort}_edit, _get_editing, _{set,get}_branch_pdc
// and _process_buffer_automated. Every pre-v28 export is an alias for seat 0 and
// the v34 additions broke that shape; the last is the most visible, because its
// non-automated sibling dca_engine_dsp_process_buffer has had one since v28.
// (2) dca_midi_output_get_counts: a message the dispatcher DROPPED because its
// queue was full was neither counted nor distinguishable from a bad argument,
// so the caller was told its ARGUMENTS were wrong while the note-off it lost
// became a stuck note. `_send`/`_send_at` now return DCA_ERR_DEVICE for a full
// queue. (3) THE MASTER LIMITER IS DEFAULT OFF (it was ON in v34) — it sits on
// the live monitor path, so default-ON cost every player 1 ms of latency on
// their own playing and shifted every consumer's click alignment by 48 samples
// at 48 kHz, for a feature they had not asked for. A host that wants a bounded
// streaming mirror must call dca_engine_set_master_limiter. (4) The automated
// offline render RESOLVES every event up front, so a rejected list can no longer
// leave the rig half-swept, and an event naming ANOTHER SEAT's node is rejected;
// `event_count` is capped at 262,144.
// v36: TWO ADDITIVE exports, one additive status code and ONE new callback
// typedef (dca_stems_progress_cb2); nothing reordered, nothing resignatured, and every v35 entry point
// keeps its exact behaviour. (1) dca_stems_separate_file2 +
// dca_stems_progress_cb2 — the v35 progress callback returns void, so a
// multi-minute separation could not be CANCELLED and the managed side honoured
// "a cancelled separation throws OperationCanceledException" only by abandoning
// a thread that went on writing stems nobody wanted. The new callback returns
// int (0 = stop) and the separator checks it between the coarse phases AND
// between the per-target WAV writes; a cancelled run answers DCA_ERR_CANCELLED.
// A SECOND typedef rather than a changed one, deliberately: changing
// dca_stems_progress_cb would silently break every existing binding that
// compiled against it, and its `void` return has no room for an answer.
// (2) dca_midi_output_get_counts3 — the dispatcher has counted messages the
// DRIVER threw on since v35 (`failedCount()`), and there was no way to read it:
// sentCount() used to move on a throw too, so a wedged interface read as
// working. dca_midi_output_get_counts keeps its two-out-param shape.
// (3) DCA_ERR_CANCELLED = 6, which is the first new status since the enum was
// written. It is NOT a failure and a caller must not treat it as one.
// v37: TEN ADDITIVE exports in three groups; nothing reordered, nothing
// resignatured, and every v36 entry point keeps its exact behaviour (both new
// features are OFF / empty at engine start). (1) SPILLOVER —
// dca_engine_dsp_{set,get}_spillover_for_player and
// dca_engine_dsp_get_spill_tail_count_for_player: a delay or reverb that a
// structure change REMOVES rings out as a post-chain tail instead of being cut.
// (2) RE-AMP — dca_engine_set_input_injection_for_player, _clear_, _set_playing_,
// _set_loop_, _seek_ and dca_engine_get_input_injection_status_for_player: a
// mono take at the engine rate REPLACES one seat's live input (rig, capture
// ring and meter all hear it). (3) dca_engine_read_input_capture_from_for_player
// — a CURSORLESS positional capture read, so a recorder can follow the stream
// without stealing from the one streaming cursor live detection drains.
// v38: the CHART SOURCE — a per-seat note generator that plays a chart's notes
// as a DI signal THROUGH that seat's rig — plus the SONG CLOCK it follows and an
// option to route the in-process instrument (VSTi) into a seat's input instead
// of after the master bus. NINE ADDITIVE exports and three structs; nothing
// reordered, nothing resignatured, everything OFF / empty at engine start.
// dca_engine_set_song_clock_source, dca_engine_chart_source_set_notes_for_player,
// _set_enabled_, _set_voice_, _set_params_, dca_engine_chart_source_get_status_for_player,
// dca_chart_voice_is_available, dca_engine_set_instrument_route and
// dca_engine_get_instrument_route. The generator is mixed into the seat's rig
// input AFTER its capture ring, so note detection, scoring and the tuner never
// hear it. An engine rebuild starts empty — the host replays it.
// v39: dca_chart_note gains `capo` (int32 at offset 64, taken from the first
// reserved float; the struct stays 80 bytes and DCA_CHART_NOTE_V38_BYTES is
// unchanged). `pitch` is exactly as in v38 — the capo was always folded into it —
// but a voice needs the capo itself for anything measured FROM the capo: a
// natural harmonic's node (capo 2 + charted fret 14 = the octave node). A v38
// caller's zeroed reserved slot reads as capo 0. No export changed (#51).
// v40: INSTRUMENT MODELS for the built-in guitar and bass (#50) and the release
// of a disabled chart source's voice + sample pack (#61 part 2). Four additive
// exports — dca_engine_chart_source_set_instrument_for_player,
// _get_instrument_for_player, dca_chart_model_get_info, dca_chart_voice_pack_bytes —
// the DCA_CHART_MODEL_* / DCA_CHART_PICKUP_* / DCA_CHART_INSTRUMENT_* values, and
// six fields APPENDED to dca_chart_source_status (cbSize: a v38 caller's smaller
// struct is still filled). Behaviour change: _set_voice_ no longer builds a voice
// while the source is disabled (it records the request), and disabling releases
// the voice once it is silent — so packs are held only while the synth is on.
// v41: the headless Null device is requested through the CREATE call, not only the
// process environment (DotCharter #85). One additive export,
// dca_engine_create2(cfg, flags, out), and two flag bits: DCA_ENGINE_CREATE_NULL_DEVICE
// registers the "Null" device type with THIS engine's device manager (and its
// mirror's), DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL also puts that device in
// manual-pump mode (implies NULL_DEVICE). The env vars DCA_ENABLE_NULL_DEVICE /
// DCA_NULL_DEVICE_MANUAL still work and are OR-ed with the flags: either source
// turns the feature on, neither can turn the other off. dca_engine_create is
// dca_engine_create2 with flags 0. No struct changed.
// v42: native Wwise-Vorbis (.wem) decoding (DotCharter #426). Seven additive
// exports -- dca_wem_open_file, dca_wem_open_memory, dca_wem_get_info,
// dca_wem_read_f32, dca_wem_read_s16, dca_wem_seek, dca_wem_destroy -- the
// dca_wem_info struct and the DCA_WEM_VARIANT_* values. Behaviour change: every
// engine's format manager (and dca_decode_audio_file / dca_generate_waveform*)
// also knows ".wem", so dca_source_open_file on a Rocksmith .wem opens it as a
// transport source -- decoded on the load thread and preloaded as 16-bit like
// any other file -- with no ww2ogg step. No existing struct or signature changed.
// v43: the load-time rate conversion, done once into a file (DotCharter #435).
// Two additive exports: dca_convert_audio_for_device(src, dst, device_rate,
// out_bits) writes the song at the device rate EXACTLY as a preload holds it --
// int16 samples plus the preload step in a "dcps" WAV chunk that every open
// honours, so opening the result plays BIT-IDENTICAL samples at 16 bits -- or
// writes nothing when the load would not convert;
// dca_source_get_preload_digest(source, ...) fingerprints what a preloaded
// source holds (the test seam that proves it). No struct or signature changed;
// dca_source_open_* refuse a DAMAGED device-rate file (DCA_ERR_DECODE).
// v44: Wwise-free Wwise-Vorbis (.wem) ENCODING (DotCharter #317). Two additive
// exports -- dca_wem_encode_vorbis (48 kHz stereo float PCM in, a natively
// allocated .wem out, written the way Wwise 2021 writes it for Rocksmith) and
// dca_wem_encode_free. No struct or signature changed.
// v45: the `rsamp` module's chain table is loaded at RUN TIME (JuceSharp #25,
// DotCharter #755): no number read out of Rocksmith's files is compiled in any
// more. Two additive exports, dca_rsamp_set_table and dca_rsamp_table_row_count.
// The table is PROCESS-wide, not per engine, so an engine rebuild keeps it;
// until a table is loaded every rsamp module passes audio through. No struct or
// signature changed.
// v46: the audio callback's LOAD (JuceSharp #29), so a host can show a real CPU
// meter. One additive export, dca_engine_get_callback_load. No struct or
// signature changed.
// v47: the EXTERNAL PROCESS INSERT. A seat's dry input goes to another process
// through a named shared-memory ring and that process's processed audio comes back
// in place of the seat's rig. Six additive exports (dca_engine_extproc_open /
// _close / _get_stats, each with a _for_player twin), the dca_extproc_stats struct,
// the DCA_EXTPROC_STATE_* values and two APPENDED status codes,
// DCA_ERR_SAMPLE_RATE = 7 and DCA_ERR_NOT_FOUND = 8. No existing struct or
// signature changed.
#define DCA_ABI_VERSION 47

// ---- Per-player live-input rigs (ABI v28) ---------------------------------
// The engine hosts DCA_MAX_PLAYERS independent live-input rigs ("seats"). Each
// one owns its own DSP tone chain, its own VST3 input chain, its own mono
// capture ring (so live note detection can judge every player) and its own
// hardware INPUT CHANNEL. They all share ONE open device: this is channel
// routing inside a single AudioIODevice, never a second device open — an ASIO
// driver is typically exclusive, so two engines cannot split one interface's
// two inputs.
//
// dca_engine_set_player_count decides how many seats are LIVE. It defaults to
// 1, and at 1 the audio callback runs the pre-v28 code path byte-for-byte.
//
// THE NODE-ID CONTRACT, which is what kept this ABI change small: node ids are
// unique per ENGINE, not per chain — seat `p` hands out
// `p * DCA_PLAYER_NODE_ID_STRIDE + local`. So every NODE-scoped export below
// (remove / move / bypass / set_param / get_param / node string / node branch)
// keeps its pre-v28 signature and resolves the seat from the id, and only the
// CHAIN-scoped ones (add / count / list / clear / process_buffer) needed
// player-indexed variants. Seat 0's ids are 0, 1, 2, … exactly as before, so
// every pre-v28 export is an alias for "seat 0" and existing callers are
// unaffected. A node id naming a seat >= DCA_MAX_PLAYERS is rejected, never
// clamped onto another seat's rig.
#define DCA_MAX_PLAYERS 4
#define DCA_PLAYER_NODE_ID_STRIDE 1000000

typedef enum dca_status {
    DCA_OK                  = 0,
    DCA_ERR_INVALID_ARG     = 1,
    DCA_ERR_NOT_INITIALIZED = 2,
    DCA_ERR_DEVICE          = 3,
    DCA_ERR_DECODE          = 4,
    DCA_ERR_PLUGIN          = 5,
    // v36. The caller ASKED to stop, so this is not a failure — a host that
    // lumps it in with the errors above turns a user's Cancel into an error
    // dialog, and (in this repo's own words) into "a reason to fall through to
    // the next backend". Only dca_stems_separate_file2 can return it today.
    DCA_ERR_CANCELLED       = 6,
    // v47 (external process insert). The audio device does not run at the rate
    // the other process's ring carries; nothing was opened. The host requests
    // that rate and opens again, or shows the reason.
    DCA_ERR_SAMPLE_RATE     = 7,
    // v47. The named object (a shared-memory block) does not exist.
    DCA_ERR_NOT_FOUND       = 8,
    DCA_ERR_INTERNAL        = 99
} dca_status;

typedef enum dca_driver {
    DCA_DRIVER_DEFAULT     = 0,
    DCA_DRIVER_WASAPI      = 1,
    DCA_DRIVER_DIRECTSOUND = 2,
    DCA_DRIVER_ASIO        = 3,
    DCA_DRIVER_COREAUDIO   = 4,
    DCA_DRIVER_ALSA        = 5,
    DCA_DRIVER_JACK        = 6
} dca_driver;

typedef struct dca_engine*  dca_engine_t;
typedef struct dca_source*  dca_source_t;
typedef struct dca_plugin*  dca_plugin_t;
typedef struct dca_midi_in* dca_midi_in_t;

// MIDI input callback. Invoked on the JUCE MIDI input thread. Do NOT
// block, allocate, or take locks inside; copy data and return.
typedef void (DCA_CALL *dca_midi_callback)(void* user, double timestamp_seconds,
                                           const uint8_t* data, int32_t length);

typedef struct dca_device_info {
    char       id[128];
    char       name[256];
    dca_driver driver;
    int32_t    max_input_channels;
    int32_t    max_output_channels;
    int32_t    default_sample_rate;
    int32_t    is_default;
    // ABI v7: JUCE device-type name as reported by AudioIODeviceType::getTypeName()
    // (e.g. "Windows Audio", "Windows Audio (Exclusive Mode)",
    // "Windows Audio (Low Latency Mode)", "ASIO", "CoreAudio"). The driver
    // enum collapses all WASAPI variants into DCA_DRIVER_WASAPI; type_name
    // is the only way to distinguish them. Empty string for legacy device
    // info structures returned by the no-engine enumeration path.
    char       type_name[64];
} dca_device_info;

typedef struct dca_engine_config {
    int32_t    sample_rate;
    int32_t    buffer_frames;
    int32_t    output_channels;
    int32_t    input_channels;
    uint32_t   output_channel_mask; // 0 = default; LSB=channel 0
    uint32_t   input_channel_mask;  // 0 = default; LSB=channel 0
    dca_driver preferred_driver;
    char       output_device_id[128];
    char       input_device_id[128];
    // ABI v7: when non-empty, pin the JUCE AudioIODeviceType by name (e.g.
    // "Windows Audio (Exclusive Mode)") instead of letting preferred_driver
    // pick the first matching type. Required to actually open a device in
    // WASAPI Shared / Exclusive / Low-Latency mode, since all three share
    // DCA_DRIVER_WASAPI. If empty, falls back to preferred_driver lookup.
    char       output_type_name[64];
    char       input_type_name[64];
} dca_engine_config;

typedef enum dca_log_level {
    DCA_LOG_TRACE = 0,
    DCA_LOG_DEBUG = 1,
    DCA_LOG_INFO  = 2,
    DCA_LOG_WARN  = 3,
    DCA_LOG_ERROR = 4
} dca_log_level;

typedef void (DCA_CALL *dca_log_callback)(void* user, dca_log_level level, const char* msg);

// ---- Library lifecycle ----------------------------------------------------
DCA_API uint32_t    DCA_CALL dca_abi_version(void);
DCA_API const char* DCA_CALL dca_version_string(void);
DCA_API void        DCA_CALL dca_set_log_callback(dca_log_callback cb, void* user);

// ---- Device enumeration ---------------------------------------------------
DCA_API dca_status DCA_CALL dca_enumerate_output_devices(dca_device_info* devices, int32_t capacity, int32_t* out_count);
DCA_API dca_status DCA_CALL dca_enumerate_input_devices (dca_device_info* devices, int32_t capacity, int32_t* out_count);

// ABI v13: enumerate devices for a specific host type WITHOUT opening any
// device. Spins a private AudioDeviceManager on the JUCE message thread,
// switches to the requested type, calls scanForDevices() and returns the
// device list with their default sample rate and channel counts. Safe to
// call while another engine is running. Pass NULL for type_name to use the
// current default device type. Returns DCA_ERR_DEVICE if the type is not
// available on this platform.
DCA_API dca_status DCA_CALL dca_enumerate_host_devices(
    const char*       type_name,
    int32_t           is_input,
    dca_device_info*  devices,
    int32_t           capacity,
    int32_t*          out_count);

// ABI v13: list every audio host (AudioIODeviceType) JUCE recognises on this
// platform. Each name (max 64 chars) is null-terminated; an empty string
// marks the end of the list. type_names must have capacity for at least
// 16 * 64 bytes. Out parameter receives the number of populated entries.
DCA_API dca_status DCA_CALL dca_enumerate_host_types(
    char*    type_names,   // [capacity_entries * 64]
    int32_t  capacity_entries,
    int32_t* out_count);

// ---- Engine lifecycle -----------------------------------------------------
// `cfg == NULL` is LEGAL and means "create the engine but do not open a device".
// The handle comes back live, dca_engine_get_sample_rate / _get_buffer_frames
// answer 0, and every offline surface works — the DSP rig's
// dca_engine_dsp_process_buffer, the waveform generators, dca_render_to_wav.
// Call dca_engine_reconfigure or dca_engine_set_device_type afterwards to open
// one. `out_engine` is written before any failure path, so it is NULL on
// every error return.
DCA_API dca_status DCA_CALL dca_engine_create (const dca_engine_config* cfg, dca_engine_t* out_engine);

// ABI v41 (DotCharter #85): create with per-ENGINE options instead of process-wide
// environment variables. A concurrent engine built by another thread must not see
// a test/offline engine's Null device, and an env var is shared by every engine in
// the process. `flags` is a mask of DCA_ENGINE_CREATE_*; an unknown bit is
// DCA_ERR_INVALID_ARG (never silently ignored). The flags are remembered by the
// engine, so a later dca_engine_start / _set_device_type / mirror open on the SAME
// engine sees the same Null registration. Precedence: flag OR env — the env vars
// DCA_ENABLE_NULL_DEVICE=1 / DCA_NULL_DEVICE_MANUAL=1 still enable the feature for
// every engine (tests and tools rely on it); flags 0 does not disable it.
#define DCA_ENGINE_CREATE_NULL_DEVICE        (1u << 0)
#define DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL (1u << 1)
#define DCA_ENGINE_CREATE_KNOWN_FLAGS        (DCA_ENGINE_CREATE_NULL_DEVICE | DCA_ENGINE_CREATE_NULL_DEVICE_MANUAL)
DCA_API dca_status DCA_CALL dca_engine_create2(const dca_engine_config* cfg, uint32_t flags, dca_engine_t* out_engine);
DCA_API dca_status DCA_CALL dca_engine_destroy(dca_engine_t engine);
// dca_engine_start opens the default device on an engine created with
// `cfg == NULL` (dca_engine_reconfigure / _set_device_type pick a specific one).
// It REFUSES an engine that dca_engine_stop stopped: DCA_ERR_DEVICE, with the
// reason on the log callback (#27). dca_engine_stop detaches every source from
// the mixer for good, so a restart used to run a live device with silence on it
// and report DCA_OK. dca_engine_stop also removes the audio callback, which
// dca_engine_reconfigure does not re-add, so the only way back is to destroy the
// engine and create a new one (reconfigure changes device WITHOUT stopping).
// dca_engine_stop on a stopped engine is a no-op.
DCA_API dca_status DCA_CALL dca_engine_start  (dca_engine_t engine);
DCA_API dca_status DCA_CALL dca_engine_stop   (dca_engine_t engine);
DCA_API dca_status DCA_CALL dca_engine_get_sample_rate  (dca_engine_t engine, int32_t* out_sr);
DCA_API dca_status DCA_CALL dca_engine_get_buffer_frames(dca_engine_t engine, int32_t* out_frames);
// Output latency reported by the currently active audio device, in samples
// at the device sample rate. Includes the device buffer plus any extra
// driver/hardware delay the OS layer reports. Returns 0 when no device is
// open. Additive ABI v7+; older runtimes do not export this entry point.
DCA_API dca_status DCA_CALL dca_engine_get_output_latency_samples(dca_engine_t engine, int32_t* out_samples);

// In-place reconfigure of the active audio device. Wraps JUCE's
// AudioDeviceManager::setAudioDeviceSetup() so settings changes that only
// touch sample rate / buffer frames / device selection do NOT have to tear
// down and recreate the engine. Loaded VSTs, transport sources, mixer state
// and stem mappings are preserved.
//
// Pass 0 / NULL for any field that should stay at its current value:
//   * sample_rate <= 0        -> keep current SR
//   * buffer_frames <= 0      -> keep current buffer
//   * output_device_id == NULL or "" -> keep current output device
//   * input_device_id  == NULL or "" -> keep current input device
//
// Changing the driver TYPE (e.g. WASAPI -> ASIO) is NOT supported by this
// path; callers must continue to use dca_engine_destroy + dca_engine_create
// for that. Returns DCA_ERR_DEVICE if JUCE refuses the new setup; the engine
// is left in its previous working state when that happens.
// Additive ABI v8+; older runtimes do not export this entry point.
DCA_API dca_status DCA_CALL dca_engine_reconfigure(
    dca_engine_t engine,
    int32_t      sample_rate,
    int32_t      buffer_frames,
    const char*  output_device_id,
    const char*  input_device_id);

// In-place host swap (ABI v12). Switch the AudioDeviceManager to a different
// AudioIODeviceType (e.g. from ASIO to "Windows Audio (Exclusive Mode)") and
// reopen with the supplied device IDs / channel masks in a single call. Avoids
// the engine-destroy/rebuild round trip — plugins, transports, mixer state are
// all preserved. Pass an empty type_name to keep the current host. Empty
// device IDs mean "let JUCE pick the default device for the new host". Channel
// masks: 0 means "use defaults" (preserve old behaviour); a non-zero mask
// pins the specific input/output channels (e.g. 0b10 = capture channel 2 only).
DCA_API dca_status DCA_CALL dca_engine_set_device_type(
    dca_engine_t engine,
    const char*  type_name,
    const char*  output_device_id,
    const char*  input_device_id,
    uint32_t     output_channel_mask,
    uint32_t     input_channel_mask);

// ---- Live input monitoring ------------------------------------------------
DCA_API dca_status DCA_CALL dca_engine_set_monitor_gain (dca_engine_t engine, float linear_gain);
DCA_API dca_status DCA_CALL dca_engine_set_input_gain   (dca_engine_t engine, float linear_gain);
DCA_API dca_status DCA_CALL dca_engine_get_input_peak   (dca_engine_t engine, float* out_peak);
/* ABI v17: digital drop-tune pedal. Shifts the live input by `semitones`
 * (negative = drop) BEFORE the capture ring and the input VST chain, so
 * detection, plugins and monitoring all hear the shifted signal. 0 = true
 * bypass (no processing, no added latency). */
DCA_API dca_status DCA_CALL dca_engine_set_input_pitch_shift(dca_engine_t engine, float semitones);
/* ABI v18, MEANING CHANGED AT v25: drop-tune quality mode. The Rubber Band and
 * dual-head OLA engines this used to select are both GONE — the shifter is CSVD
 * (src/dsp/cores/dcdsp_droptune.h) and `mode` now picks a splice-lag floor:
 *   0 = Tight    (10 ms)
 *   1 = Balanced (20 ms) — the default
 *   2 = Smooth   (35 ms) — fewest splices, best on sustained chords
 * Attack latency (~1.5-4 ms) is set by the transient anchor and is the SAME in
 * all three. There is no dry-attack punch-through: emitting unshifted audio at
 * the pick is the artifact that design exists to avoid. Applied atomically on
 * the audio thread at the next callback. Values outside 0..2 are rejected. */
DCA_API dca_status DCA_CALL dca_engine_set_input_pitch_shift_mode(dca_engine_t engine, int32_t mode);
/* ABI v31: the same two knobs, PER SEAT. Until v31 there was one shifter and it
 * ran on the raw device input BEFORE the per-player split, so a couch with two
 * guitars in different tunings could not use it: dropping one dropped both, and
 * the seat that did not ask for it was detuned with nothing on screen saying so.
 * Each seat now owns its own CSVD shifter, its own semitone target and its own
 * lag ceiling.
 *
 * The two exports above are exactly these with player = 0 — the same
 * relationship dca_engine_set_input_capture_channel has with
 * dca_engine_set_player_input_channel. A player index outside
 * 0..DCA_MAX_PLAYERS-1 is REJECTED, never clamped.
 *
 * WHICH SIGNAL A SEAT SHIFTS: with one live seat on a 1-2 in device, channels
 * 0/1 (a genuine stereo pickup stays stereo) — unchanged from v30. Otherwise
 * exactly the signal that seat's capture ring consumes, as MONO. Never
 * blindly ch0/ch1: with the guitar on input 6, that used to shift inputs 1-2
 * and then hand the capture ring a 2-channel buffer, whose channel index
 * clamped from 5 down to 1 — so note detection silently listened to a
 * different physical input than the one being shifted. */
DCA_API dca_status DCA_CALL dca_engine_set_player_input_pitch_shift(dca_engine_t engine,
                                                                     int32_t player,
                                                                     float semitones);
DCA_API dca_status DCA_CALL dca_engine_set_player_input_pitch_shift_mode(dca_engine_t engine,
                                                                          int32_t player,
                                                                          int32_t mode);
DCA_API dca_status DCA_CALL dca_engine_show_asio_panel  (dca_engine_t engine, void* parent_window);

// ---- Live input capture (ABI v11, dry pre-gain pre-chain) ----------------
// Select which input channel (zero-based) feeds the mono capture ring. Pass
// -1 to mix all input channels down to mono. The selection is clamped to the
// device's actual input count on the audio thread.
DCA_API dca_status DCA_CALL dca_engine_set_input_capture_channel(dca_engine_t engine, int32_t channel);
// Pull the most recent `count` samples (mono float32, device sample rate)
// from the capture ring into `dest`. The ring is rotating so callers always
// observe the latest window; older samples are simply overwritten. Returns
// the actual count written via `out_written`.
DCA_API dca_status DCA_CALL dca_engine_read_input_capture(dca_engine_t engine,
                                                          float* dest,
                                                          int32_t count,
                                                          int32_t* out_written);
// ABI v15: streaming variant with its own cursor. Returns only samples not
// yet consumed by a previous streaming read (out_written == 0 when caught
// up), skipping ahead when the caller falls more than one ring behind. Use
// for continuous consumers (live note detection); the windowed read above
// always returns the full latest window and MUST NOT be drain-polled.
DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream(dca_engine_t engine,
                                                                 float* dest,
                                                                 int32_t count,
                                                                 int32_t* out_written);
// ABI v16: like the streaming read, but also reports the ABSOLUTE stream
// position (total samples ever captured before dest[0]) of the first sample
// returned. Consumers use it to re-anchor after a ring-overrun skip and to
// map their analysis timeline onto the capture axis exactly.
DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream2(dca_engine_t engine,
                                                                  float* dest,
                                                                  int32_t count,
                                                                  int32_t* out_written,
                                                                  uint64_t* out_stream_position);
// ABI v16: atomically sample {total captured input samples written, host
// high-resolution ticks at the end of the same audio-callback block}. The
// tick source is juce::Time::getHighResolutionTicks — on Windows that is
// QueryPerformanceCounter, i.e. the SAME timebase as .NET's Stopwatch — so a
// managed consumer can translate any capture-stream sample index into host
// time (and from there into a transport position) sample-accurately instead
// of curve-fitting two free-running clocks. Returns DCA_ERR_INVALID_ARG when
// no input block has been captured yet.
DCA_API dca_status DCA_CALL dca_engine_get_input_capture_sync(dca_engine_t engine,
                                                              uint64_t* out_captured_samples,
                                                              int64_t*  out_host_ticks);

// ---- Per-player seats: routing + capture (ABI v28) ------------------------
// How many live-input rigs run. 1 (the default) is the pre-v28 signal path
// byte-for-byte; 2 is couch co-op. Clamped to 1..DCA_MAX_PLAYERS. Applied
// atomically on the audio thread at the next callback — every seat's chain
// buffer and DSP rig is prepared at device start whether or not it is live, so
// raising this never allocates on the callback.
DCA_API dca_status DCA_CALL dca_engine_set_player_count(dca_engine_t engine, int32_t count);
DCA_API int32_t    DCA_CALL dca_engine_get_player_count(dca_engine_t engine);

// Which hardware input channel feeds seat `player`'s capture ring AND — when
// more than one seat is live — that seat's DSP + VST rig. -1 mixes every input
// channel to mono. Defaults to `player` (seat 0 -> input 0, seat 1 -> input 1),
// which is two guitars into one 2-in interface. Clamped to the device's actual
// input count on the audio thread.
//
// The rig and the capture ring resolve the channel with the SAME rule, so a
// player's TONE and a player's JUDGING can never end up listening to different
// guitars. dca_engine_set_input_capture_channel is exactly
// dca_engine_set_player_input_channel(engine, 0, channel).
//
// WHAT THE SEAT'S RIG HEARS:
//   player_count == 1 : unchanged from v27 — channels 0/1 of the effective
//                       input, so a genuine stereo pickup stays stereo.
//   player_count  > 1 : each seat is MONO from its own channel, duplicated
//                       across L/R. It has to be: with two guitars on inputs 0
//                       and 1, the v27 stereo fill would put BOTH players into
//                       seat 0's amp.
DCA_API dca_status DCA_CALL dca_engine_set_player_input_channel(dca_engine_t engine,
                                                                 int32_t player, int32_t channel);
DCA_API int32_t    DCA_CALL dca_engine_get_player_input_channel(dca_engine_t engine, int32_t player);

// Per-seat monitor trim, MULTIPLIED into the global monitor gain when that
// seat's processed rig output is summed into the device output. 1.0 (the
// default) leaves the pre-v28 level untouched. This is how two guitars get
// balanced against each other in one monitor mix.
DCA_API dca_status DCA_CALL dca_engine_set_player_monitor_gain(dca_engine_t engine,
                                                                int32_t player, float linear_gain);
// Peak absolute POST-rig sample for one seat since the last call; resets on
// read, like dca_engine_get_input_peak (which is seat 0).
DCA_API dca_status DCA_CALL dca_engine_get_player_input_peak(dca_engine_t engine,
                                                              int32_t player, float* out_peak);

// Per-seat capture rings. Each seat has its own windowed cursor, its own
// streaming cursor and its own {captured, ticks} sync pair, so two live
// detectors never steal each other's samples. The pre-v28 reads above are
// these with player = 0.
DCA_API dca_status DCA_CALL dca_engine_read_input_capture_for_player(dca_engine_t engine,
                                                                      int32_t player,
                                                                      float* dest,
                                                                      int32_t count,
                                                                      int32_t* out_written);
DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream_for_player(dca_engine_t engine,
                                                                            int32_t player,
                                                                            float* dest,
                                                                            int32_t count,
                                                                            int32_t* out_written);
DCA_API dca_status DCA_CALL dca_engine_read_input_capture_stream2_for_player(dca_engine_t engine,
                                                                             int32_t player,
                                                                             float* dest,
                                                                             int32_t count,
                                                                             int32_t* out_written,
                                                                             uint64_t* out_stream_position);
DCA_API dca_status DCA_CALL dca_engine_get_input_capture_sync_for_player(dca_engine_t engine,
                                                                         int32_t player,
                                                                         uint64_t* out_captured_samples,
                                                                         int64_t*  out_host_ticks);

// ABI v37: a CURSORLESS positional read of one seat's capture ring. Copies up
// to `count` samples starting at ABSOLUTE stream position `from` (0 = the first
// sample the ring ever captured) and reports the position the copy really began
// at: later than `from` when those samples were already overwritten (the gap is
// `*out_first - from`), EARLIER when `from` lies beyond what was captured (a
// rebuilt engine restarted the stream: the copy starts at the new stream's
// first surviving sample, so re-anchor at `*out_first`). The caller
// owns the cursor, so any number of such readers coexist with the ONE streaming
// cursor live detection drains; none steals from another. Any non-audio
// thread; both outs are zeroed first.
DCA_API dca_status DCA_CALL dca_engine_read_input_capture_from_for_player(dca_engine_t engine,
                                                                          int32_t player,
                                                                          uint64_t from,
                                                                          float* dest,
                                                                          int32_t count,
                                                                          int32_t* out_written,
                                                                          uint64_t* out_first);

// ---- Input injection / re-amp (ABI v37) ------------------------------------
// A recorded DI take played INTO a seat's rig in place of the guitar. The take
// is MONO float32 at the ENGINE's sample rate (the caller resamples; a take at
// another rate plays at the wrong pitch) and is COPIED — the caller may free its
// buffer on return. From the next block the seat's input IS the take: its
// capture ring (tuner, detection), its rig and its meter all hear it, and its
// live input is muted for as long as a take is loaded — a PAUSED take is
// silence, not the guitar. Clear the take to get the guitar back.
//
// set_ loads the take PAUSED at frame 0 with the given loop flag, replacing any
// take already loaded. Playing starts and pauses through a 5 ms ramp; a one-shot
// (loop == 0) take that reaches its end stops and rewinds to 0; a looping take
// wraps. A seek lands at the next block, clamped into the take.
//
// The audio thread neither allocates nor frees: the take is built on the
// caller's thread, published by an atomic swap, and a replaced take is freed on
// the caller's thread once no block can still be reading it. An engine rebuild
// starts EMPTY — the host replays the take if it wants it back. Control thread
// (any non-audio thread); `player` outside 0..DCA_MAX_PLAYERS-1 is rejected.
DCA_API dca_status DCA_CALL dca_engine_set_input_injection_for_player(dca_engine_t engine,
                                                                      int32_t player,
                                                                      const float* mono,
                                                                      int64_t frames,
                                                                      int32_t loop);
DCA_API dca_status DCA_CALL dca_engine_clear_input_injection_for_player(dca_engine_t engine,
                                                                        int32_t player);
// DCA_ERR_INVALID_ARG when no take is loaded on the seat.
DCA_API dca_status DCA_CALL dca_engine_set_input_injection_playing_for_player(dca_engine_t engine,
                                                                              int32_t player,
                                                                              int32_t playing);
DCA_API dca_status DCA_CALL dca_engine_set_input_injection_loop_for_player(dca_engine_t engine,
                                                                           int32_t player,
                                                                           int32_t loop);
DCA_API dca_status DCA_CALL dca_engine_seek_input_injection_for_player(dca_engine_t engine,
                                                                       int32_t player,
                                                                       int64_t frame);
// Every out is zeroed first. DCA_ERR_INVALID_ARG when no take is loaded (so a
// caller can tell "nothing loaded" from "loaded, paused at 0"). Positions and
// length are frames at the engine rate.
DCA_API dca_status DCA_CALL dca_engine_get_input_injection_status_for_player(dca_engine_t engine,
                                                                             int32_t player,
                                                                             int64_t* out_position,
                                                                             int64_t* out_length,
                                                                             int32_t* out_playing,
                                                                             int32_t* out_loop);

// ---- Chart source (ABI v38) ------------------------------------------------
// A per-seat NOTE GENERATOR that renders a chart's notes as a MONO DI signal
// into that seat's RIG INPUT, so the whole rig — drop tune excepted, pedals,
// amp, cab, NAM, the song's tone — processes it exactly as it processes the
// guitar. It is mixed in AFTER the seat's capture ring: note detection, scoring,
// the tuner and the DI recorder NEVER hear it. It does not feed the post-master
// instrument bus. A seat whose device has no input at all still runs its rig
// while its chart source is enabled.
//
// TIMING. The notes are uploaded ONCE (per arrangement change) and the engine
// renders them against the SONG CLOCK: the audible position of ONE transport
// source the host names with dca_engine_set_song_clock_source (DotCharter: the
// song). The engine reads that position on the audio thread after every pull,
// so the notes follow play, pause, seeks, loops (which are seeks) and practice
// speed by construction, sample-accurately, with no managed code on the hot
// path. The voice renders at the DEVICE rate whatever the speed, so slow
// practice changes WHEN notes sound, never their PITCH. A seek inside a note
// starts that note at the seek point (`chased`). With no song clock bound, or
// the clock paused, every note is released and nothing new starts.
//
// Everything is OFF / empty at engine start, and an engine REBUILD (device
// change, 48 kHz request, watchdog recovery) starts empty too — the host
// replays the notes, voice, params, enabled flag and song clock.

// Articulation bits of dca_chart_note.flags. Values are permanent; new bits are
// appended. Unknown bits are ignored by a voice that does not render them.
#define DCA_CHART_NOTE_PALM_MUTE       (1u << 0)
#define DCA_CHART_NOTE_FRET_HAND_MUTE  (1u << 1)   // "full" mute: a percussive, pitchless hit
#define DCA_CHART_NOTE_HARMONIC        (1u << 2)   // natural harmonic
#define DCA_CHART_NOTE_PINCH_HARMONIC  (1u << 3)
#define DCA_CHART_NOTE_HAMMER_ON       (1u << 4)
#define DCA_CHART_NOTE_PULL_OFF        (1u << 5)
#define DCA_CHART_NOTE_TAP             (1u << 6)
#define DCA_CHART_NOTE_VIBRATO         (1u << 7)
#define DCA_CHART_NOTE_TREMOLO         (1u << 8)   // tremolo picking
#define DCA_CHART_NOTE_ACCENT          (1u << 9)
#define DCA_CHART_NOTE_UNPITCHED_SLIDE (1u << 10)  // slide_to_fret is an unpitched target
#define DCA_CHART_NOTE_SLAP            (1u << 11)  // bass slap
#define DCA_CHART_NOTE_POP             (1u << 12)  // bass pop
#define DCA_CHART_NOTE_PICKED          (1u << 13)  // bass played with a pick (else fingers)
#define DCA_CHART_NOTE_LET_RING        (1u << 14)  // the host held it past its sustain (hand shape)
#define DCA_CHART_NOTE_LINK_NEXT       (1u << 15)  // tied into the next note on its string
#define DCA_CHART_NOTE_STACCATO        (1u << 16)  // cut short on purpose
#define DCA_CHART_NOTE_GHOST           (1u << 17)  // very quiet (ghost) note
#define DCA_CHART_NOTE_BASS            (1u << 18)  // the arrangement is a bass
// HOW the note ends, decided by the host (it owns release_seconds): exactly one
// of these two, or neither for an ordinary release at the note's own sustain.
#define DCA_CHART_NOTE_END_DAMPED      (1u << 19)  // cut by the next note on its string (a fretting-hand damp)
#define DCA_CHART_NOTE_END_SHAPE       (1u << 20)  // rang on to its hand shape's end (then released)

// Strum direction of a chord note (dca_chart_note.strum). The chart model has
// no strum direction today, so DotCharter sends UNKNOWN and the voice decides.
#define DCA_CHART_STRUM_UNKNOWN 0
#define DCA_CHART_STRUM_DOWN    1
#define DCA_CHART_STRUM_UP      2

// One note. Times are CHART seconds — the song's own timeline, the axis
// dca_source_get_audible_position_seconds reports — not wall time.
//
// APPEND-EXTENSIBLE: the upload names its element stride, which must be at
// least DCA_CHART_NOTE_V38_BYTES; the engine reads the smaller of the stride and
// its own sizeof and zero-fills the rest. A field appended at the END therefore
// needs no ABI bump; reordering or resizing one does. 80 bytes, 8-aligned.
typedef struct dca_chart_note {
    double   start_seconds;    // attack
    double   release_seconds;  // when the note is released (>= start). The HOST
                               // owns ring / legato / hand-shape holds and puts
                               // the result here.
    double   sustain_seconds;  // AUTHORED sustain length (0 = none); slides and
                               // bends are drawn over it
    float    pitch;            // MIDI note number the string sounds at the attack,
                               // tuning, capo, cent offset and bass octave
                               // included; fractional is legal (A4 = 69 = 440 Hz)
    float    velocity;         // 0..1
    int32_t  string_index;     // 0 = lowest-pitched string
    int32_t  fret;             // fret as charted, counted from the NUT (0 = open; the capo is
                               // already in `pitch`)
    float    slide_to_fret;    // < 0 = no slide; else the fret the slide ends on
    uint32_t flags;            // DCA_CHART_NOTE_*
    int32_t  chord_id;         // -1 = single note; notes sharing an id are one chord
    int32_t  bend_first;       // index of the first bend point in the bend array, -1 = none
    int32_t  bend_count;       // points belonging to this note (0 = none)
    int32_t  strum;            // DCA_CHART_STRUM_*
    int32_t  capo;             // the arrangement's capo fret (0 = none). `pitch` already
                               // includes it; a voice needs it for anything measured from
                               // the capo rather than the nut — natural-harmonic nodes sit
                               // at fret - capo (capo 2 + charted fret 14 = the octave node)
    float    reserved[3];      // zero
} dca_chart_note;
#define DCA_CHART_NOTE_V38_BYTES 80

// One point of a note's bend curve: at `time_seconds` (chart seconds, absolute)
// the string is bent `semitones` above its fretted pitch. Points of one note are
// in time order. The curve is linear between points and holds after the last;
// from the attack to the first point it ramps up from 0, unless that point sits
// on the attack — that is a PRE-BEND. 16 bytes.
typedef struct dca_chart_bend_point {
    double  time_seconds;
    float   semitones;
    int32_t reserved;          // zero
} dca_chart_bend_point;

// The built-in voices. A kind the build does not contain answers 0 from
// dca_chart_voice_is_available and DCA_ERR_INVALID_ARG from _set_voice_.
#define DCA_CHART_VOICE_PLACEHOLDER 0   // a plucked-string PLACEHOLDER (Karplus-Strong), not the real voice
#define DCA_CHART_VOICE_GUITAR      1   // reserved: the waveguide guitar (#39 step 1)
#define DCA_CHART_VOICE_BASS        2   // reserved: the waveguide bass

// How the generator meets the seat's live input in its rig.
#define DCA_CHART_SOURCE_MIX     0      // summed with the guitar (default)
#define DCA_CHART_SOURCE_REPLACE 1      // the rig hears only the generator; the capture
                                        // ring still hears the guitar

// Status snapshot. struct_bytes is a Win32-style cbSize, exactly as
// dca_xrun_stats: the caller sets it, the engine writes the smaller of the two
// and reports how many bytes it wrote. Append-only.
typedef struct dca_chart_source_status {
    int32_t struct_bytes;       // IN: sizeof. OUT: bytes written.
    int32_t enabled;            // 1 = rendering into the seat's rig
    int32_t voice;              // DCA_CHART_VOICE_*
    int32_t mode;               // DCA_CHART_SOURCE_*
    int32_t note_count;         // notes in the uploaded table
    int32_t active_voices;      // notes the voice is sounding now
    int32_t clock_bound;        // 1 = a song clock source is set
    int32_t clock_playing;      // 1 = the song clock advanced in the last block
    int64_t notes_started;      // note-ons since the table was uploaded (chased ones included)
    int64_t notes_chased;       // of which started mid-note after a seek / resume
    int64_t notes_dropped;      // note-ons the per-block cap or the voice pool refused
    double  song_seconds;       // song time at the END of the last rendered block
    float   gain;               // linear gain into the rig
    float   reserved0;
    double  offset_seconds;     // added to every note time (positive = later)
    // ---- ABI v40 (appended) ----
    int32_t model;              // DCA_CHART_MODEL_* of the selected voice's family
    int32_t pickup;             // DCA_CHART_PICKUP_*
    uint32_t instrument_flags;  // DCA_CHART_INSTRUMENT_*
    int32_t sampled;            // 1 = the voice plays real recordings (a pack is loaded)
    int64_t pack_bytes;         // sample-pack memory the seat's voice holds (0 = released)
    int32_t voice_loaded;       // 1 = a voice exists (0 while disabled, once released)
    int32_t reserved1;
} dca_chart_source_status;

// Bind the song clock: the transport source whose audible position the chart
// sources of EVERY seat follow. NULL unbinds (every chart source then releases
// its notes). The source must belong to this engine. A destroyed source unbinds
// itself. Control thread.
DCA_API dca_status DCA_CALL dca_engine_set_song_clock_source(dca_engine_t engine,
                                                             dca_source_t source);

// Upload the seat's notes, replacing the previous table (count 0 clears it).
// `notes` must be sorted by start_seconds; every time finite and >= 0, release
// >= start, pitch in [0, 127], string_index in [0, 15]; every bend range inside
// the bend array. A list that breaks a rule is REJECTED whole
// (DCA_ERR_INVALID_ARG) and the previous table stays. note_count <= 262,144,
// bend_count <= 1,048,576. COPIED — the caller may free both arrays on return.
// The audio thread sees the new table at its next block, chokes what was
// sounding and resumes from the song clock's position. Control thread.
DCA_API dca_status DCA_CALL dca_engine_chart_source_set_notes_for_player(dca_engine_t engine,
                                                                         int32_t player,
                                                                         const dca_chart_note* notes,
                                                                         int32_t note_count,
                                                                         int32_t note_stride_bytes,
                                                                         const dca_chart_bend_point* bends,
                                                                         int32_t bend_count);
// OFF at engine start. Enabling builds the selected voice first (loading only
// its sample pack). Disabling releases every note through the voice's fast fade
// and then (v40, #61) FREES the voice and its pack off the audio thread, so a
// disabled source holds no sample memory. Control thread.
DCA_API dca_status DCA_CALL dca_engine_chart_source_set_enabled_for_player(dca_engine_t engine,
                                                                           int32_t player,
                                                                           int32_t enabled);
// Select the voice (DCA_CHART_VOICE_*); PLACEHOLDER at engine start. While the
// source is ENABLED the voice is built and prepared on the caller's thread and
// cross-faded in at the next block; while it is disabled the choice is only
// recorded (v40) and the voice is built by the next enable.
DCA_API dca_status DCA_CALL dca_engine_chart_source_set_voice_for_player(dca_engine_t engine,
                                                                         int32_t player,
                                                                         int32_t voice);
// gain_linear in [0, 4] (1 at engine start), offset_seconds in [-2, +2]
// (0 at engine start; positive = every note sounds later), mode DCA_CHART_SOURCE_*.
// Out-of-range values are REJECTED, never clamped.
DCA_API dca_status DCA_CALL dca_engine_chart_source_set_params_for_player(dca_engine_t engine,
                                                                          int32_t player,
                                                                          float gain_linear,
                                                                          double offset_seconds,
                                                                          int32_t mode);
// ---- Instrument models (ABI v40, #50) ----------------------------------------
// Which guitar / bass the built-in voices sound like, the pickup selector and
// the coil split. Values are permanent. A guitar model (< 16) sets the GUITAR
// voice's selection, a bass model (>= 16) the BASS voice's; both are kept, so a
// bass chart after a guitar chart plays the bass selection. At engine start:
// guitar SEMI_HOLLOW / BRIDGE (the v38/39 sound: the Gretsch as recorded), bass
// JAZZ / NECK_BRIDGE.
// A rebuild starts there again — the host replays its selection.
//
// RECORDED vs SIMULATED: the Strat's five positions are real recordings
// (EGFxSet); Tele, Les Paul and the HSS bridge humbucker are the closest Strat
// position converted (pickup-position comb, pickup resonance, body EQ). See
// dca_chart_model_info.recorded_mask. Only the selection's own pack is loaded.
#define DCA_CHART_MODEL_SEMI_HOLLOW  0   // Gretsch Anniversary (recorded on its bridge pickup)
#define DCA_CHART_MODEL_STRAT_SSS    1   // Stratocaster, 5-way
#define DCA_CHART_MODEL_STRAT_HSS    2   // Strat, humbucker at the bridge, 5-way, coil split
#define DCA_CHART_MODEL_TELE         3   // Telecaster, 3-way
#define DCA_CHART_MODEL_LES_PAUL     4   // two humbuckers, 3-way, coil split
#define DCA_CHART_MODEL_JAZZ_BASS   16   // neck, both, bridge
#define DCA_CHART_MODEL_P_BASS      17   // one split-coil pickup (MIDDLE)
#define DCA_CHART_MODEL_PJ_BASS     18   // P (MIDDLE), P + J, J (BRIDGE)
#define DCA_CHART_MODEL_HOLLOW_BASS 19   // Washburn AB-95 hollowbody, neck pickup (recorded)

#define DCA_CHART_PICKUP_NECK          0
#define DCA_CHART_PICKUP_NECK_MIDDLE   1
#define DCA_CHART_PICKUP_MIDDLE        2
#define DCA_CHART_PICKUP_MIDDLE_BRIDGE 3
#define DCA_CHART_PICKUP_BRIDGE        4
#define DCA_CHART_PICKUP_NECK_BRIDGE   5

#define DCA_CHART_INSTRUMENT_COIL_SPLIT (1u << 0)   // humbuckers as one coil (no effect without one)

// What a model offers. struct_bytes is a Win32-style cbSize (append-only).
typedef struct dca_chart_model_info {
    int32_t  struct_bytes;      // IN: sizeof. OUT: bytes written.
    int32_t  bass;              // 1 = a bass model
    uint32_t pickup_mask;       // bit DCA_CHART_PICKUP_x set = the selector has it
    int32_t  default_pickup;    // DCA_CHART_PICKUP_*
    int32_t  has_coil_split;    // 1 = the coil-split switch changes something
    int32_t  installed;         // 1 = every pack its positions need is installed
    uint32_t recorded_mask;     // pickups (split off) whose every recording plays UNCONVERTED
    uint32_t recorded_any_mask; // pickups where at least one articulation set does
                                // (bass: fingers and pick are recorded on different basses)
    int32_t  reserved[4];
} dca_chart_model_info;

// Select the instrument model, pickup (DCA_CHART_PICKUP_*) and flags
// (DCA_CHART_INSTRUMENT_*) of the voice of the model's family. Rejected
// (DCA_ERR_INVALID_ARG) when the model is unknown, the pickup is not on its
// selector, or a flag bit is unknown. When that family's voice is the one
// playing (enabled), a new voice is built on the CALLER's thread — its pack
// loads here, outside every lock the audio thread or a status poll takes —
// and CROSS-FADED in (~10 ms); the replaced voice and, with the last
// reference, its pack are freed off the audio thread. Control thread.
DCA_API dca_status DCA_CALL dca_engine_chart_source_set_instrument_for_player(dca_engine_t engine,
                                                                              int32_t player,
                                                                              int32_t model,
                                                                              int32_t pickup,
                                                                              uint32_t flags);
// The selection of the guitar (bass = 0) or bass (bass = 1) voice. Out-params
// are zeroed before the first failure path. Any non-audio thread.
DCA_API dca_status DCA_CALL dca_engine_chart_source_get_instrument_for_player(dca_engine_t engine,
                                                                              int32_t player,
                                                                              int32_t bass,
                                                                              int32_t* out_model,
                                                                              int32_t* out_pickup,
                                                                              uint32_t* out_flags);
// Describe a model. DCA_ERR_INVALID_ARG for an unknown model or a struct_bytes
// below 8. Checks the pack files exist (I/O): control thread.
DCA_API dca_status DCA_CALL dca_chart_model_get_info(int32_t model, dca_chart_model_info* out_info);
// Bytes of sample-pack memory alive in this process (every engine's voices; a
// pack shared by two voices counts once). 0 once every chart source is off and
// its voice released. Never fails (-1 is not used). Control thread.
DCA_API int64_t DCA_CALL dca_chart_voice_pack_bytes(void);

// See dca_chart_source_status. Any thread; never blocks the audio thread.
DCA_API dca_status DCA_CALL dca_engine_chart_source_get_status_for_player(dca_engine_t engine,
                                                                          int32_t player,
                                                                          dca_chart_source_status* out_status);
// 1 = this build contains the voice, 0 = it does not, -1 = not a voice kind.
DCA_API int32_t DCA_CALL dca_chart_voice_is_available(int32_t voice);

// ---- Instrument route (ABI v38) --------------------------------------------
// Where the IN-PROCESS instrument (dca_plugin_attach_to_instrument) is mixed.
// -1 (engine start) = after the master bus, as it always was. 0..DCA_MAX_PLAYERS-1
// = summed to mono into THAT seat's rig input, after its capture ring (so, like
// the chart source, detection never hears it), and NOT on the master output.
// The SANDBOXED instrument cannot follow: its block comes back from the child in
// the same round trip that runs seat 0's input chain, i.e. after the rig has
// already run, so routing it would cost a whole block of latency. It stays after
// the master whatever this says — a host must show the option unavailable for a
// sandboxed instrument. The instrument gain still applies. A rebuild resets the
// route to -1; the host replays it. Control thread.
DCA_API dca_status DCA_CALL dca_engine_set_instrument_route(dca_engine_t engine, int32_t player);
// *out_player is zeroed to -1 first.
DCA_API dca_status DCA_CALL dca_engine_get_instrument_route(dca_engine_t engine, int32_t* out_player);

// ---- Master / filter bus -------------------------------------------------
DCA_API dca_status DCA_CALL dca_engine_set_master_volume(dca_engine_t engine, float linear_gain);
DCA_API dca_status DCA_CALL dca_engine_set_high_pass    (dca_engine_t engine, float freq_hz, float q); // freq<=0 disables
DCA_API dca_status DCA_CALL dca_engine_set_low_pass     (dca_engine_t engine, float freq_hz, float q);
DCA_API dca_status DCA_CALL dca_engine_set_band_pass    (dca_engine_t engine, float freq_hz, float q);

// ---- Parametric EQ (ABI v29) --------------------------------------------
// Six typed bands with per-band frequency / Q / gain, over the SAME master bus
// the three fixed filters above sit on, plus an optional copy over each seat's
// post-rig guitar signal. This is the editor's mixer EQ; the three fixed
// filters remain and are applied first, unchanged.
//
// EVERY band is off (`enabled == 0`) and the whole section is disabled at
// engine start, so an app that never calls these is byte-for-byte pre-v29.
#define DCA_EQ_BAND_COUNT 6

// Band shape. These values are ABI — append, never renumber.
typedef enum dca_eq_band_type
{
    DCA_EQ_HIGH_PASS  = 0,
    DCA_EQ_LOW_SHELF  = 1,
    DCA_EQ_BELL       = 2,
    DCA_EQ_HIGH_SHELF = 3,
    DCA_EQ_LOW_PASS   = 4,
    DCA_EQ_NOTCH      = 5
} dca_eq_band_type;

// Which bus the six bands are applied to.
typedef enum dca_eq_route
{
    DCA_EQ_ROUTE_SONG  = 0, // the mixed playback output only (default)
    DCA_EQ_ROUTE_INPUT = 1, // every seat's post-rig guitar signal only
    DCA_EQ_ROUTE_BOTH  = 2
} dca_eq_route;

// Set one band. `index` is [0, DCA_EQ_BAND_COUNT). `gain_db` is ignored by the
// shapes that have no gain (high pass / low pass / notch). Coefficients are
// computed HERE, on the calling (message) thread — the audio callback only ever
// copies 5 floats per section, exactly like the three fixed filters.
DCA_API dca_status DCA_CALL dca_engine_set_eq_band(dca_engine_t engine, int32_t index, int32_t type,
                                                   float freq_hz, float q, float gain_db, int32_t enabled);
// Master power for the whole section. 0 = every band bypassed (the curve is flat).
DCA_API dca_status DCA_CALL dca_engine_set_eq_enabled(dca_engine_t engine, int32_t enabled);
// Solo one band: while `band_index` is in range, ONLY that band is audible,
// whatever the per-band enables say. Pass < 0 for "no solo".
DCA_API dca_status DCA_CALL dca_engine_set_eq_solo(dca_engine_t engine, int32_t band_index);
DCA_API dca_status DCA_CALL dca_engine_set_eq_route(dca_engine_t engine, int32_t route);
// Output trim in dB, applied after the six bands on whichever bus they run on.
DCA_API dca_status DCA_CALL dca_engine_set_eq_trim(dca_engine_t engine, float trim_db);
// How many bands the ENGINE is currently running — the honest answer behind the
// "n/6 active" badge, and the one that already accounts for the master power
// and the solo. Never derive that number a second time in managed code.
DCA_API dca_status DCA_CALL dca_engine_get_eq_active_count(dca_engine_t engine, int32_t* out_count);
// Magnitude spectrum of the bus the EQ is routed to, sampled BEFORE the six
// bands (so the analyser does not chase the curve you are dragging). Writes
// `bin_count` dBFS values into `dest_db`, log-spaced from 20 Hz to 20 kHz.
// The FFT runs on the CALLING thread, from a lock-free ring the audio callback
// only appends to. Returns DCA_ERR_INVALID_ARG for bin_count outside [8, 512].
DCA_API dca_status DCA_CALL dca_engine_read_eq_spectrum(dca_engine_t engine, float* dest_db,
                                                        int32_t bin_count, int32_t* out_written);

// Per-engine enumeration (preferred: reuses the running AudioDeviceManager).
DCA_API dca_status DCA_CALL dca_engine_enumerate_output_devices(dca_engine_t engine, dca_device_info* devices, int32_t capacity, int32_t* out_count);
DCA_API dca_status DCA_CALL dca_engine_enumerate_input_devices (dca_engine_t engine, dca_device_info* devices, int32_t capacity, int32_t* out_count);

// ---- Streaming mirror (parallel WASAPI output) --------------------------
// Mirror the final master mix (including the post-VST guitar signal) to a
// secondary WASAPI device so OBS / Discord / Twitch can capture song +
// processed guitar via a virtual cable even while ASIO drives low-latency
// monitoring on the primary device. Pass NULL or "" to disable.
DCA_API dca_status DCA_CALL dca_engine_set_mirror_device(dca_engine_t engine, const char* device_id);

// ---- Click bus (metronome + clap) ---------------------------------------
// Trigger a short synthesised click on the master output. `click_type` is:
//   0 = beat tick           (1 kHz, ~55 ms)
//   1 = downbeat / measure  (1.5 kHz, ~55 ms)
//   2 = clap                (filtered noise, ~90 ms)
// Lock-free; safe to call from any thread, dropped silently if the FIFO is
// saturated. The voice starts at the head of the NEXT audio block, so the
// click jitters by up to one block plus the caller's own clock quantum, and it
// is heard one full output latency after the caller decided to fire it. Fine
// for a count-in or a "preview the metronome" button; use the scheduling API
// below for anything that has to land ON a point of the song.
DCA_API dca_status DCA_CALL dca_engine_play_click(dca_engine_t engine, int32_t click_type);

// ---- Sample-accurate click scheduling (ABI v26) --------------------------
// The engine's OUTPUT RENDER CLOCK: `out_render_frames` is the total number of
// frames the device callback has rendered since the device started, i.e. the
// index of the next sample it will render; `out_host_ticks` is
// juce::Time::getHighResolutionTicks() at that same block boundary (QPC on
// Windows — the same timebase as .NET's Stopwatch, matching
// dca_engine_get_input_capture_sync).
//
// The pair is published seqlock-style IMMEDIATELY after the transport pull
// inside the audio callback. That timing is load-bearing: it means this counter
// and dca_source_get_audible_position_seconds describe THE SAME output sample
// (the first of the next block), so a caller can read the two back-to-back,
// re-read this counter to confirm no block landed in between, and obtain an
// exact {render frame <-> song time} anchor:
//
//     frame(T) = render_frames + (T - audible_position) / speed * sample_rate
//
// Note what is NOT in that formula: output latency. Scheduling a click at
// frame(T) writes it into the very same output samples that carry the song's
// audio at time T, so the click and the note traverse the identical device
// buffer, driver and hardware delay and reach the ear TOGETHER — no matter what
// that delay actually is, and even if our estimate of it is wrong. (Anchoring
// on the LATENCY-COMPENSATED playhead instead would require adding the latency
// back in, and any error in the estimate would land straight in the result.)
//
// Returns DCA_ERR_INVALID_ARG before the device has rendered its first block.
DCA_API dca_status DCA_CALL dca_engine_get_render_clock(dca_engine_t engine,
                                                        int64_t* out_render_frames,
                                                        int64_t* out_host_ticks);

// Schedule a click (same click_type values as dca_engine_play_click) to start
// at an absolute frame on the render clock above. The audio thread starts the
// voice at that exact sample OFFSET inside the block that contains the frame —
// no block-boundary jitter. Lock-free; safe from any thread.
//
// A frame that has already been rendered fires at the head of the current block
// (never worse than dca_engine_play_click) unless it is more than ~250 ms stale,
// in which case it is dropped. A frame more than ~5 s in the future is dropped
// too — that can only be a leftover from a device restart, which rewinds the
// clock to 0. Up to 32 clicks may be pending at once.
DCA_API dca_status DCA_CALL dca_engine_schedule_click(dca_engine_t engine,
                                                      int32_t click_type,
                                                      int64_t render_frame);

// Drop every scheduled-but-not-yet-sounded click. Ordered against
// dca_engine_schedule_click (both travel through the same FIFO), so a clear
// issued after a schedule always cancels it. Call on seek / pause / stop /
// playback-speed change: the render clock keeps running while the transport
// does not, so a stale queue would otherwise fire into silence — or, worse,
// against the wrong song position.
DCA_API dca_status DCA_CALL dca_engine_clear_scheduled_clicks(dca_engine_t engine);

// ---- Dropout / xrun counters + device health ----------------------------
// A REAL dropout counter, measured inside the audio callback, replacing the
// managed-side heuristic that could not tell an audio-callback overrun from a
// scheduling hiccup in the polling thread.
//
// Three DIFFERENT conditions are counted, and they are not the same fault:
//
//   late_callbacks    The callback ARRIVED late. The gap between the start of
//                     this callback and the start of the previous one exceeded
//                     1.5x the previous block's own duration + 0.5 ms. The
//                     device therefore had to fill at least part of a buffer
//                     without us — this IS an audible dropout, whether the
//                     cause was us, another process, or the driver. This is the
//                     number to show a user.
//   deadline_overruns OUR callback took longer than the block it was rendering
//                     (numSamples/sampleRate). We were the cause. A deadline
//                     overrun usually produces a late_callback on the NEXT
//                     block, so the two correlate; when deadline_overruns stays
//                     0 while late_callbacks climbs, the machine is at fault,
//                     not the DSP rig.
//   near_misses       Over 80% of the block budget but still inside it. Not a
//                     dropout — a load warning. STRICTLY DISJOINT from
//                     deadline_overruns (a callback is counted as one or the
//                     other, never both).
//
// device_xruns is what the DRIVER itself reports (juce::AudioIODevice::
// getXRunCount): ASIO and DirectSound implement it, WASAPI and CoreAudio's
// aggregate path do not and report -1. It is authoritative when present and
// absent most of the time, which is exactly why the callback-side counters
// above exist.
//
// The counters are plain relaxed atomics bumped on the audio thread: no
// allocation, no lock, no logging, no syscall (the tick source is cached QPC).
// Everything except device_open/device_running/device_xruns is "since the last
// dca_engine_reset_xrun_stats", and every counter is also rebased when the
// device (re)starts — the first 4 callbacks of a device session are IGNORED,
// because a cold device thread is always late and would otherwise report a
// phantom dropout at every device change, sample-rate change and watchdog
// recovery.
//
// struct_bytes is a Win32-style cbSize: the CALLER sets it to
// sizeof(dca_xrun_stats) before the call, and the engine writes at most that
// many bytes and reports how many it wrote. Appending a field at the END of
// this struct is therefore additive and does NOT require an ABI bump;
// reordering or resizing an existing field does.
typedef struct dca_xrun_stats {
    int32_t struct_bytes;            // IN: sizeof(dca_xrun_stats). OUT: bytes written.
    int32_t device_open;             // 1 when an audio device is actually open
    int32_t device_running;          // 1 when that device is started (callbacks flowing)
    int32_t device_xruns;            // driver-reported since reset; -1 = driver cannot report
    int64_t callbacks;               // audio callbacks since reset
    int64_t late_callbacks;          // real dropouts (see above)
    int64_t deadline_overruns;       // callbacks that overran their own block budget
    int64_t near_misses;             // >80% of budget, inside it
    int64_t dropped_frames;          // estimated frames the device rendered without us
    int64_t last_xrun_host_ticks;    // juce::Time::getHighResolutionTicks() at the last
                                     // late_callback / deadline_overrun; 0 = none since reset.
                                     // Same timebase as dca_engine_get_render_clock and
                                     // .NET Stopwatch (QPC on Windows).
    int64_t last_xrun_render_frame;  // render-clock frame at that moment; -1 = none
    int64_t max_callback_ns;         // longest callback since reset
    int64_t last_callback_ns;        // most recent callback duration
    int64_t block_budget_ns;         // most recent block's budget; load = last_callback_ns / this
    int64_t input_plugin_faults;     // input-chain VST3s that threw out of processBlock and
                                     // were DISABLED for the life of their snapshot. Counted
                                     // ONCE per plugin, not once per block: a plugin that
                                     // faults deterministically (an expired licence check, a
                                     // layout it was not prepared for) used to throw on every
                                     // callback, and a C++ throw is not RT-safe. Re-armed by
                                     // the next attach / detach / device rebuild, which
                                     // republishes the chain. Appended in the 2026-09 wave —
                                     // ADDITIVE, no ABI bump (see the cbSize note above).
} dca_xrun_stats;

// Snapshot the counters. Safe from any thread; never blocks the audio thread.
// Returns DCA_ERR_INVALID_ARG when out_stats is NULL or its struct_bytes is
// smaller than the first four int32 fields.
DCA_API dca_status DCA_CALL dca_engine_get_xrun_stats(dca_engine_t engine,
                                                      dca_xrun_stats* out_stats);

// Zero every "since reset" counter and rebase device_xruns. Safe from any
// thread. Call it when a health window opens so the numbers describe the
// window rather than the whole process lifetime.
DCA_API dca_status DCA_CALL dca_engine_reset_xrun_stats(dca_engine_t engine);

// ---- Callback load (v46) --------------------------------------------------
// How much of the buffer period the audio callback spends rendering, measured
// inside the callback with the same QPC tick source as dca_xrun_stats. A
// FRACTION, not a percent: 0.25 = a quarter of the block's wall-clock budget
// (numSamples / sampleRate); above 1.0 the callback overran its deadline.
//
//   out_average  exponentially weighted average, time constant 0.5 s, weighted
//                by block DURATION (so a variable-block driver averages time).
//   out_peak     the largest single-block load, DECAYING exponentially with a
//                1.5 s time constant. It is NOT reset by reading: any number of
//                pollers (a UI meter at 4 Hz, a health logger) read the same
//                values without stealing each other's peak.
//
// Both are computed on the audio thread from relaxed atomics: no allocation, no
// lock, no syscall (RtCallbackLoadIsMeasuredWithoutAllocating). Both read 0
// while no device is running, and restart from 0 at every device session
// (device change, 48 kHz request, watchdog rebuild); the session's warm-up
// blocks are not counted. They are NOT touched by dca_engine_reset_xrun_stats.
//
// Safe from any thread; never blocks the audio thread. Either out-pointer may
// be NULL; both NULL, or a NULL engine, is DCA_ERR_INVALID_ARG (the non-NULL
// out-pointers are zeroed first).
DCA_API dca_status DCA_CALL dca_engine_get_callback_load(dca_engine_t engine,
                                                         float* out_average,
                                                         float* out_peak);

// ---- External process insert (v47) ----------------------------------------
// Per seat: while an insert is open, the seat's dry input (after the input trim and
// the chart source, before the rig) is written into another process's shared
// memory, and the processed audio that process writes back REPLACES the seat's rig,
// input VST3 chain and sandbox output. The input EQ, the meter and the monitor mix
// follow as usual. With nothing open the seat runs exactly as before (one atomic
// load per block).
//
// The block is a fixed ring format (src/ExternalProcessInsert.h has the layout):
// a 64-byte header (8-byte magic, version 1, sample rate, block frames, ring
// blocks, in/out channel counts, four free-running 32-bit block counters and four
// fault counters), then a mono float input ring and a 1- or 2-channel interleaved
// float output ring, each 64-byte aligned. The OTHER process creates it.
//
// Real time: the audio callback never allocates, locks, waits or makes a system
// call for it, and never waits on the other process. It reads processed blocks
// non-blocking, keeping a cushion of ceil(largest callback / block) + 1 blocks, so
// the added latency is that cushion plus the other process's own turnaround. A
// block that is not back in time is an UNDERRUN: the seat plays its DRY input for
// that block (never silence), with a short ramp at each switch; a full input ring
// is an OVERRUN (the block is dropped, never overwriting unread input); processed
// blocks that pile up beyond the cushion are dropped as LATE. Opening, mapping and
// unmapping happen on the caller's thread; the audio thread takes a new session at
// the bottom of a 5 ms dip.
//
// No resampler: the device must run at the block's rate. Engine rebuild: the open
// session is per-engine state. A RECONFIGURE keeps it (it bypasses itself while the
// new device rate differs, state DCA_EXTPROC_STATE_RATE_MISMATCH, and resumes when
// the rate matches again); a destroyed engine loses it, and the host re-opens it on
// the new one.
#define DCA_EXTPROC_STATE_CLOSED        0  // nothing open on the seat
#define DCA_EXTPROC_STATE_WAITING       1  // open; the other side has not published its block size yet
#define DCA_EXTPROC_STATE_ACTIVE        2  // exchanging blocks (see `connected` for whether audio comes back)
#define DCA_EXTPROC_STATE_RATE_MISMATCH 3  // open, but the device rate differs: the seat runs its own rig
#define DCA_EXTPROC_STATE_BAD_LAYOUT    4  // the header now describes a ring this build cannot run: dry

// struct_bytes contract as dca_xrun_stats: the caller sets it to its sizeof, the
// engine fills min(caller, own) bytes and writes back how many. Fields may only be
// APPENDED.
typedef struct dca_extproc_stats {
    int32_t struct_bytes;     // IN: sizeof(dca_extproc_stats). OUT: bytes written.
    int32_t state;            // DCA_EXTPROC_STATE_*
    int32_t connected;        // 1 while processed blocks are coming back in time
    int32_t sample_rate;      // the ring's rate (Hz); 0 when closed
    int32_t block_frames;     // the ring's block; 0 until the other side publishes it
    int32_t ring_blocks;      // blocks per ring
    int32_t latency_samples;  // delay this exchange adds, at the device rate (== sample_rate),
                              // measured: blocks in flight + the block being gathered
    int32_t reserved;         // 0
    int64_t blocks_sent;      // input blocks handed over since the open
    int64_t blocks_received;  // processed blocks played since the open
    int64_t underruns;        // blocks played DRY because nothing was back in time
    int64_t overruns;         // input blocks dropped because the input ring was full
    int64_t late_blocks;      // processed blocks dropped because they piled up past the cushion
} dca_extproc_stats;

// Map the shared-memory block `mapping_name` (UTF-8, a kernel object name such as
// Local\<name>) and start exchanging the seat's audio through it, REPLACING any
// insert already open on the seat. `magic` (NULL = not checked) is compared with
// the header's first 8 bytes, zero-padded. Statuses: DCA_ERR_INVALID_ARG (NULL
// engine or name, bad seat), DCA_ERR_NOT_FOUND (no such block), DCA_ERR_DECODE (the
// header is not format 1 or describes a ring that does not fit the block),
// DCA_ERR_NOT_INITIALIZED (no device running), DCA_ERR_SAMPLE_RATE (the device does
// not run at the block's rate; nothing is opened), DCA_ERR_DEVICE (the OS refused
// to map it). Control thread.
DCA_API dca_status DCA_CALL dca_engine_extproc_open_for_player(dca_engine_t engine, int32_t player,
                                                                 const char* mapping_name,
                                                                 const char* magic);
DCA_API dca_status DCA_CALL dca_engine_extproc_open(dca_engine_t engine,
                                                    const char* mapping_name,
                                                    const char* magic);

// Stop the exchange and unmap the block: the seat dips back to its own rig. The
// unmap waits (bounded, ~200 ms, on the CALLER's thread) for the audio thread to let
// go; a block still held after that is unmapped by the next call or at engine
// destruction. DCA_ERR_INVALID_ARG for a NULL engine or a bad seat; closing a seat
// with nothing open is DCA_OK. Control thread.
DCA_API dca_status DCA_CALL dca_engine_extproc_close_for_player(dca_engine_t engine, int32_t player);
DCA_API dca_status DCA_CALL dca_engine_extproc_close(dca_engine_t engine);

// Snapshot the seat's insert. A closed seat reports state DCA_EXTPROC_STATE_CLOSED
// with every other field 0. DCA_ERR_INVALID_ARG for a NULL engine, a NULL out_stats,
// a struct_bytes smaller than the first two int32 fields, or a bad seat. Any
// thread; never blocks the audio thread.
DCA_API dca_status DCA_CALL dca_engine_extproc_get_stats_for_player(dca_engine_t engine, int32_t player,
                                                                      dca_extproc_stats* out_stats);
DCA_API dca_status DCA_CALL dca_engine_extproc_get_stats(dca_engine_t engine,
                                                         dca_extproc_stats* out_stats);

// ---- In-house DSP tone rig (0.16) --------------------------------------
// A reorderable, in-process effect chain (gate / drive / amp / cab / mods)
// applied to the guitar INPUT before any user VST input chain. Modules are
// created from a static catalogue by string type id; parameters are addressed
// by string id and described by the catalogue so a generic UI needs no
// per-module code. All functions are thread-safe (the chain publishes an
// atomic ordering snapshot to the audio thread).
//
// ABI v28: there are DCA_MAX_PLAYERS of these chains, one per seat. The
// functions on THIS block are seat 0 (see the node-id contract at the top of
// this header); the *_for_player variants further down address any seat. Node
// ids are engine-global, so everything addressed BY NODE ID below serves every
// seat as it stands.
//
// Add a module of `module_type_id` at the end of the chain. Returns the new
// node id (>= 0) or a negative value on error (unknown type / null engine).
DCA_API int32_t    DCA_CALL dca_engine_dsp_add_node(dca_engine_t engine, const char* module_type_id);
DCA_API dca_status DCA_CALL dca_engine_dsp_remove_node(dca_engine_t engine, int32_t node_id);
DCA_API dca_status DCA_CALL dca_engine_dsp_move_node(dca_engine_t engine, int32_t node_id, int32_t new_index);
DCA_API dca_status DCA_CALL dca_engine_dsp_set_bypass(dca_engine_t engine, int32_t node_id, int32_t bypassed);
DCA_API dca_status DCA_CALL dca_engine_dsp_set_param(dca_engine_t engine, int32_t node_id,
                                                     const char* param_id, float value);
DCA_API dca_status DCA_CALL dca_engine_dsp_get_param(dca_engine_t engine, int32_t node_id,
                                                     const char* param_id, float* out_value);
// THE FAILURE SENTINEL OF A VALUE-RETURNING EXPORT IS -1, NEVER 0.
//
// Every export in this header that returns a value rather than a dca_status
// answers -1 for "bad handle / bad seat / unknown id", and 0 is a legal ANSWER
// everywhere. That was true of dca_engine_dsp_add_node,
// _get_chain_latency*, _get_node_string, dca_plugin_get_input_player and the
// module-label pair from the start; _node_count*, _get_node_ids* and
// _read_node_meters used 0 for BOTH until this was written, so a caller polling
// a rig could not tell a dead engine from an empty board — which is how a
// handle-lifetime bug reads as "the pedals disappeared". They now use -1 too.
//
// THE ONE DELIBERATE EXCEPTION is dca_engine_dsp_get_metering, which is a
// PREDICATE: it has two legal answers, every caller reads it as != 0, and -1
// would make a dead handle report "metering is on". It is documented at its own
// declaration.
DCA_API int32_t    DCA_CALL dca_engine_dsp_node_count(dca_engine_t engine);
// Copies current node ids (in chain order) into out_ids; returns the total
// count (which may exceed capacity — call once with capacity 0 to size), or -1
// for a null engine.
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_node_ids(dca_engine_t engine, int32_t* out_ids, int32_t capacity);
DCA_API dca_status DCA_CALL dca_engine_dsp_clear(dca_engine_t engine);

// ---- Per-player chain ops (ABI v28) --------------------------------------
// The CHAIN-scoped half of the tone-rig ABI, addressed by seat. Everything
// else (remove / move / bypass / params / strings / branches) is NODE-scoped
// and needs no player argument — a node id already names its seat.
//
// `player` outside 0..DCA_MAX_PLAYERS-1 is rejected: add returns a negative id,
// count/list return 0, clear and process_buffer return DCA_ERR_INVALID_ARG.
// The returned node ids are engine-global (player * DCA_PLAYER_NODE_ID_STRIDE
// + local), which is what makes them safe to hand straight back to the
// node-scoped calls.
DCA_API int32_t    DCA_CALL dca_engine_dsp_add_node_for_player(dca_engine_t engine, int32_t player,
                                                                const char* module_type_id);
// ABI v32: insert a parked node for live preset construction. Configure it,
// then use set_bypass(false) to fade it in. Existing add functions stay full wet.
// Splitter/merger topology remains immediate, as with ordinary bypass changes.
DCA_API int32_t    DCA_CALL dca_engine_dsp_add_node_bypassed_for_player(dca_engine_t engine, int32_t player,
                                                                     const char* module_type_id);
DCA_API int32_t    DCA_CALL dca_engine_dsp_node_count_for_player(dca_engine_t engine, int32_t player);
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_node_ids_for_player(dca_engine_t engine, int32_t player,
                                                                    int32_t* out_ids, int32_t capacity);
DCA_API dca_status DCA_CALL dca_engine_dsp_clear_for_player(dca_engine_t engine, int32_t player);

// ---- String-valued node parameters (ABI v22) ----------------------------
// Some modules take string-valued parameters (file paths, phrase tables):
//   namamp       "modelPath" — .nam/.aidax neural capture file to load
//   cab          "irPath"    — user impulse-response WAV ("" = bundled bank)
//   acousticsim  "irPath"    — user body-IR WAV ("" = bundled bank)
//   miku         "phrases"   — custom syllable table ("" = preset pattern)
// Values are UTF-8. Setting is message-thread work; any heavy processing
// (file parse, neural-model load) happens inside the module OFF the audio
// thread — the audio callback never blocks on it.
DCA_API dca_status DCA_CALL dca_engine_dsp_set_node_string(dca_engine_t engine, int32_t node_id,
                                                           const char* param_id, const char* utf8_value);
// Copies the current value (UTF-8, always NUL-terminated, truncated to
// capacity) into out_utf8 and returns the full byte length of the value
// (excluding the NUL), or -1 if the node/param is unknown. Call once with
// capacity 0 to size the buffer.
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_node_string(dca_engine_t engine, int32_t node_id,
                                                           const char* param_id, char* out_utf8, int32_t capacity);

// ---- Split / parallel chains (ABI v23) -----------------------------------
// AmpliTube-style multi-row routing inside the tone rig. Two catalogue module
// types drive it:
//   splitter  (params: enabledBranches 2..3)                — opens a region
//   merger    (params: level1..3 0..2, pan1..3 -1..1,
//              mode 0 = average / 1 = sum)                  — closes it
// Between a non-bypassed splitter and the next non-bypassed merger, every
// node processes ONE branch, chosen by the node's branch tag (set below):
// 1..3 select an explicit branch, 0 (the default) means branch 1. Each
// enabled branch starts from a COPY of the splitter's input; the merger
// OVERWRITES the main signal with the branch mix (per-branch level, and a
// balance-law pan applied when the engine runs stereo — unity at centre, so
// an untouched splitter->merger pair with mode 0 is a bit-exact passthrough;
// mode 1 is a raw sum).
// v1 semantics / accepted limits:
//   * Branch latency differences are NOT compensated — a latency-adding
//     module (e.g. klon) in one branch phase-combs against the others.
//   * No nesting: a second splitter inside an open region is a wire.
//   * A splitter without a merger is auto-merged (unity average) at the end
//     of the chain; a merger with no open region passes through.
//   * A BYPASSED splitter degrades the whole region to the plain linear
//     chain (branch tags are then ignored).
//   * Branch tags above the splitter's enabled branch count clamp down.
// The branch tag is node state: it survives reorders, is returned by the
// getter below, and is ignored outside split regions. Out-of-range ids
// (< 0 or > 3) are rejected with DCA_ERR_INVALID_ARG.
DCA_API dca_status DCA_CALL dca_engine_dsp_set_node_branch(dca_engine_t engine, int32_t node_id,
                                                           int32_t branch_id);
DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_branch(dca_engine_t engine, int32_t node_id,
                                                           int32_t* out_branch_id);

// ---- Node introspection (ABI v33) ----------------------------------------
// Two things the rig knew and never told anybody. Before v33 a caller could
// only know a node's type and bypass state by having created the node itself,
// so a chain enumerated through dca_engine_dsp_get_node_ids* came back as bare
// integers: a second binding (or a UI re-attaching to a live engine) could not
// look up a node's parameter list at all, and read every node as un-bypassed.
//
// Copies the node's catalogue type id (UTF-8, always NUL-terminated, truncated
// to `capacity` at a CODE POINT boundary) into out_utf8 and writes the FULL
// byte length excluding the NUL to *out_len — the same size-then-copy contract
// as dca_engine_dsp_get_node_string. Call with a null buffer / capacity 0 to
// size. *out_len is zeroed before anything is validated. Returns
// DCA_ERR_INVALID_ARG for a null engine, a null out_len, an unknown node, or a
// node id naming a seat >= DCA_MAX_PLAYERS (REJECTED, never clamped).
DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_type(dca_engine_t engine, int32_t node_id,
                                                         char* out_utf8, int32_t capacity,
                                                         int32_t* out_len);
// Non-zero into *out_bypassed when the node is bypassed. This is the value last
// SET (including the parked state dca_engine_dsp_add_node_bypassed_for_player
// starts a node in) — the message-thread INTENT, not the crossfade position:
// the ~10 ms ramp belongs to the audio thread, and a getter that reported it
// would flicker after every toggle. *out_bypassed is zeroed first. Same
// rejection rules as above.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_bypass(dca_engine_t engine, int32_t node_id,
                                                      int32_t* out_bypassed);

// ---- Edit scopes (ABI v34) ------------------------------------------------
// A preset change is ONE gesture, and until v34 this ABI had no way to say so.
// Every add / remove / move / retag / bypass published a fresh ordering
// snapshot and took the engine's one-block barrier, so loading a twelve-pedal
// board was twelve publications plus ~thirty param writes, each individually
// visible to the audio callback: the board assembled itself pedal by pedal,
// and v32's parked insertion only covered ONE node at a time.
//
// Between begin and commit, every structure and parameter write against this
// seat accumulates against a SHADOW chain and nothing is published. Reads
// (dca_engine_dsp_get_node_ids*, _node_count*, _get_param, _get_bypass,
// _get_node_branch, _get_chain_latency*) report that shadow, so a caller can
// build a preset and read it back inside the scope. Nodes ADDED inside a scope
// start parked at zero wet whatever their bypass flag and fade in together at
// commit; nodes REMOVED inside a scope keep playing out of the live chain and
// fade out together at commit, retiring after the barrier.
//
// Commit costs at most TWO publications whatever the preset size (one when the
// scope removed nothing audible), and all the ramps land in ONE ~10 ms window.
// Abort restores the pre-edit STRUCTURE — membership, order, bypass flags and
// branch tags — and destroys the nodes the scope added; parameter VALUES are
// not journalled and are not rolled back.
//
// A scope is per SEAT: one on player 0 does not suppress player 1's
// publications.
//
// A NESTED BEGIN ABORTS THE STALE SCOPE AND ALWAYS SUCCEEDS. It is still never
// a reference count and never a nesting level — the inner begin DISCARDS the
// outer scope's accumulated edits and opens a fresh one, logging a Warning.
// This reverses what shipped in v34 (DCA_ERR_INVALID_ARG) and what this header
// said until v35, and the reason is that refusing left the dead scope OPEN: an
// open scope publishes NOTHING, and nothing but the engine's destruction ever
// cleared it, so a caller that dropped a scope froze that seat's rig for the
// life of the engine while every add / remove / bypass went on returning
// success. Failing the NEW edit to protect a scope nobody owns is the wrong
// trade. A commit or abort with NO scope open is still DCA_ERR_INVALID_ARG, and
// so is an abort while a commit is in flight on another thread (the commit
// wins). The semantics changed WITHOUT an ABI version bump in v34->v35 terms
// only because 0.9 had not shipped; a released consumer would have had no way
// to detect which behaviour it was talking to.
//
// dca_engine_destroy ends an open scope rather than leaking the nodes it
// is holding for retirement.
DCA_API dca_status DCA_CALL dca_engine_dsp_begin_edit_for_player(dca_engine_t engine, int32_t player);
DCA_API dca_status DCA_CALL dca_engine_dsp_commit_edit_for_player(dca_engine_t engine, int32_t player);
DCA_API dca_status DCA_CALL dca_engine_dsp_abort_edit_for_player(dca_engine_t engine, int32_t player);
// Non-zero into *out_editing while a scope is open on that seat. Zeroed first.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_editing_for_player(dca_engine_t engine, int32_t player,
                                                                  int32_t* out_editing);

// Seat-0 aliases (v35). Every pre-v28 export is an alias for seat 0; these four
// broke that shape for a release. Purely additive.
DCA_API dca_status DCA_CALL dca_engine_dsp_begin_edit(dca_engine_t engine);
DCA_API dca_status DCA_CALL dca_engine_dsp_commit_edit(dca_engine_t engine);
DCA_API dca_status DCA_CALL dca_engine_dsp_abort_edit(dca_engine_t engine);
DCA_API dca_status DCA_CALL dca_engine_dsp_get_editing(dca_engine_t engine, int32_t* out_editing);

// ---- Branch delay compensation (ABI v34) ----------------------------------
// A parallel region whose branches carry different latencies is a COMB FILTER,
// not a blend: the merger summed x[n] from an empty branch with x[n-L] from a
// branch holding a pitch shifter or an oversampled drive, and v23 accepted
// that ("branch latency differences are NOT compensated"). The chain already
// computed the per-branch totals for dca_engine_dsp_get_chain_latency and then
// threw them away; the merger now delays each branch by (max - branch) so they
// arrive aligned, and that export therefore describes a region that is aligned
// rather than merely the latest of several that are not.
//
// ON by default. Off is the pre-v34 behaviour byte for byte, and is offered
// because the comb is a sound some players want. Per SEAT. The compensation
// follows structure AND parameter changes (a param can move a module's
// latency) and is re-derived on a device change. Its ceiling is 8,191 samples
// per branch — more than the largest single-module latency in the catalogue at
// any supported rate — and a branch that stacks past it CLAMPS: still better
// aligned than not, never a wrong-length delay. An UNMATCHED splitter
// auto-merges inside the chain rather than through a merger node and is not
// compensated.
DCA_API dca_status DCA_CALL dca_engine_dsp_set_branch_pdc_for_player(dca_engine_t engine, int32_t player,
                                                                     int32_t enabled);
// Non-zero into *out_enabled. Zeroed first.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_branch_pdc_for_player(dca_engine_t engine, int32_t player,
                                                                     int32_t* out_enabled);
// Seat-0 aliases (v35).
DCA_API dca_status DCA_CALL dca_engine_dsp_set_branch_pdc(dca_engine_t engine, int32_t enabled);
DCA_API dca_status DCA_CALL dca_engine_dsp_get_branch_pdc(dca_engine_t engine, int32_t* out_enabled);

// ---- Spillover (ABI v37) ---------------------------------------------------
// When a structure change REMOVES a time-based module — every catalogue entry
// whose dca_dsp_module_type_category is "delay" or "reverb" — while it is
// audible, and spillover is on for that seat, the module is not destroyed: its
// INPUT ramps to silence in place over the chain's 10 ms crossfade (so neither
// the dry path nor the tail steps), then it keeps running over silence AFTER
// the whole chain, summed into the seat's rig output (pre master), until it has
// rung for 8 s or stayed under -90 dBFS for 200 ms. It is then retired on the
// MESSAGE thread by the next publication (never freed on the callback). At most
// four tails ring per seat; a fifth asks the oldest to fade out over 10 ms.
// Works for a plain remove and for an edit-scope commit alike; a clear ends
// every tail (it is a reset). A module that SURVIVES the change (same node kept)
// was never removed and rings through as it always did.
//
// OFF at engine start (the pre-v37 path byte for byte); per SEAT, replay it
// after an engine rebuild. Turning it off fades every ringing tail out.
DCA_API dca_status DCA_CALL dca_engine_dsp_set_spillover_for_player(dca_engine_t engine, int32_t player,
                                                                    int32_t enabled);
// Non-zero into *out_enabled. Zeroed first.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_spillover_for_player(dca_engine_t engine, int32_t player,
                                                                    int32_t* out_enabled);
// Tails still RINGING on the seat, after retiring the finished ones (message
// thread only). -1 for a NULL handle or a seat outside 0..DCA_MAX_PLAYERS-1.
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_spill_tail_count_for_player(dca_engine_t engine, int32_t player);

// ---- Node sidechain (ABI v34) ---------------------------------------------
// Until v34 every module in the rig was single-buffer, and the compressor's
// "sidechain" was an internal 100 Hz high-pass on its OWN input — a detector
// shape, not a sidechain. A module that declares the capability can now be
// KEYED from another signal; the audio path, the crossfade and the scrub are
// unchanged, only the module's DETECTOR reads somewhere else.
//
// `source` is one of:
//   DCA_DSP_SIDECHAIN_NONE  (0) — key off the module's own input. The default,
//                                 and byte-for-byte the pre-v34 behaviour.
//   DCA_DSP_SIDECHAIN_INPUT (1) — the SEAT'S DRY INPUT, i.e. this chain's input
//                                 block, so a pedal late in the chain follows
//                                 the pick rather than what the drive left of
//                                 it produced.
//   DCA_DSP_SIDECHAIN_SONG  (2) — the song / master mix as it stands when this
//                                 seat is processed: the transport output after
//                                 the master bus, plus the monitor of any seat
//                                 processed earlier in the same callback. Duck
//                                 the rig under the backing track.
//
// REJECTED with DCA_ERR_INVALID_ARG for an unknown node, a source outside
// 0..2, or a module that does not advertise DCA_DSP_MODULE_FLAG_SIDECHAIN — a
// source a module silently ignored would read as a live feature that does
// nothing. If the requested source is UNAVAILABLE when the block runs (no song
// mix on the offline render path), the module keys off its own input: never off
// silence, because "keyed from silence" on a gate means closed.
#define DCA_DSP_SIDECHAIN_NONE  0
#define DCA_DSP_SIDECHAIN_INPUT 1
#define DCA_DSP_SIDECHAIN_SONG  2
DCA_API dca_status DCA_CALL dca_engine_dsp_set_node_sidechain(dca_engine_t engine, int32_t node_id,
                                                              int32_t source);
// The node's key source into *out_source. Zeroed first.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_sidechain(dca_engine_t engine, int32_t node_id,
                                                              int32_t* out_source);

// Static module catalogue (no engine required). Drives the pedalboard UI.
DCA_API int32_t    DCA_CALL dca_dsp_module_type_count(void);
DCA_API dca_status DCA_CALL dca_dsp_module_type_info(int32_t index, char* out_id, int32_t id_cap,
                                                     char* out_name, int32_t name_cap);
// ABI v33: which drawer of a palette this module type belongs in, out of a
// SHORT FIXED SET the engine owns — "dynamics", "drive", "amp", "cab",
// "modulation", "delay", "reverb", "pitch", "filter", "eq", "utility",
// "routing", "synth". Every catalogue entry has one and none is empty (pinned
// natively by tests/dsp/tone_regressions.cpp). Same size-then-copy contract as
// the other string getters: UTF-8, NUL-terminated, truncated at a code point,
// *out_len gets the full byte length excluding the NUL and is zeroed first.
// Returns DCA_ERR_INVALID_ARG for a null out_len or an index outside
// 0..dca_dsp_module_type_count()-1.
DCA_API dca_status DCA_CALL dca_dsp_module_type_category(int32_t type_index,
                                                         char* out_utf8, int32_t capacity,
                                                         int32_t* out_len);
// Capability bits for one catalogue entry (ABI v34), by the same type_index as
// dca_dsp_module_type_info / _category. A separate export because
// dca_dsp_module_type_info cannot change signature — and because the NEXT
// capability bit goes here too, instead of a second signature break. Bit 0 is
// DCA_DSP_MODULE_FLAG_SIDECHAIN: dca_engine_dsp_set_node_sidechain accepts a
// non-zero source on this module type and REJECTS it on every other. *out_flags
// is zeroed first; an index the catalogue does not have is DCA_ERR_INVALID_ARG,
// never clamped.
#define DCA_DSP_MODULE_FLAG_SIDECHAIN 0x1u
DCA_API dca_status DCA_CALL dca_dsp_module_type_flags(int32_t type_index, int32_t* out_flags);
DCA_API int32_t    DCA_CALL dca_dsp_module_param_count(const char* module_type_id);
DCA_API dca_status DCA_CALL dca_dsp_module_param_info(const char* module_type_id, int32_t index,
                                                      char* out_id, int32_t id_cap,
                                                      char* out_name, int32_t name_cap,
                                                      float* out_min, float* out_max, float* out_default);
// String-param catalogue (ABI v22): describes the string-valued parameters a
// module type accepts so the pedalboard UI stays fully catalogue-driven.
// `out_file_filter` is a ';'-separated wildcard list for a file picker
// (e.g. "*.nam;*.aidax"); empty when the param is not a file path.
DCA_API int32_t    DCA_CALL dca_dsp_module_string_param_count(const char* module_type_id);
DCA_API dca_status DCA_CALL dca_dsp_module_string_param_info(const char* module_type_id, int32_t index,
                                                             char* out_id, int32_t id_cap,
                                                             char* out_name, int32_t name_cap,
                                                             char* out_file_filter, int32_t filter_cap);

// ---- Enum value labels (ABI v24) ----------------------------------------
// Some params are enum-like: their integer value picks a MODEL rather than a
// continuous amount (amp "voicing", cab "model", overdrive/distortion
// "model"). These two calls name each selectable value so the pedalboard UI
// can render a named picker instead of a bare number, and so the managed
// catalogue mirror can label persisted model indices.
//
// dca_dsp_module_param_label_count returns the number of value labels the
// param carries (== max-min+1 for a fully-labelled enum), or -1 when the
// param is continuous / the module or param id is unknown (the -1 sentinel
// means "no labels — treat as a plain numeric param").
DCA_API int32_t    DCA_CALL dca_dsp_module_param_label_count(const char* module_type_id,
                                                             const char* param_id);
// Copies the UTF-8 label for value index `value_index` (0-based; value =
// minValue + value_index) into out_utf8, always NUL-terminated and truncated
// to capacity, and returns the FULL byte length of the label (excluding the
// NUL) — the same size-then-copy contract as dca_engine_dsp_get_node_string.
// Returns -1 when the param has no labels, the index is out of range, or the
// module/param id is unknown. Call with a null buffer / capacity 0 to size.
// THOSE THREE ARE ONE ANSWER ON PURPOSE, not an oversight: the contract is
// size-then-copy, so a caller asks _label_count FIRST and an out-of-range index
// is its own bug rather than a state the ABI has to describe. A combo box is
// built by counting and then walking [0, count), which never reaches it.
DCA_API int32_t    DCA_CALL dca_dsp_module_param_label(const char* module_type_id,
                                                       const char* param_id,
                                                       int32_t value_index,
                                                       char* out_utf8, int32_t capacity);

// ---- Discrete quantization step (ABI v27) --------------------------------
// A param whose module adapter lrounds it into an INT core field is discrete:
// the DSP only ever runs whole values. Two shapes, and every discrete param is
// exactly one of them:
//   * NAMED  — the value picks a named thing (amp voicing, tremolo shape,
//              wah mode, a footswitch): it carries value labels (above) and the
//              UI renders a PICKER.
//   * ORDINAL — the value is a COUNT (chorus voices, phaser stages, bitcrusher
//              bits): it carries a STEP and the UI renders a knob that SNAPS to
//              multiples of the step off minValue and displays an integer. A
//              continuous knob here would show "6.5 stages" while the core ran 6.
// Returns the step (> 0), 0.0f for a continuous param, and -1.0f when the
// module or param id is unknown. Note phaser "stages" is step 2, NOT 1 — the
// core forces the allpass count even.
DCA_API float      DCA_CALL dca_dsp_module_param_step(const char* module_type_id,
                                                      const char* param_id);

// ---- Rocksmith amp chain table (ABI v45) ---------------------------------
// The `rsamp` module runs Rocksmith 2014's own amp chains, and those numbers are
// the game's: this library ships none of them. The host reads them from the
// user's OWN install and hands them in here, packed as the blob documented in
// src/dsp/cores/dcdsp_rsamp_table.h (the managed writer is
// JuceSharp.Dsp.RsAmpChainTable). Rows are named by key and must match the
// public row list (src/dsp/cores/dcdsp_rsamp_models.h).
//
// The blob is validated WHOLE before anything changes: a malformed one answers
// DCA_ERR_INVALID_ARG (logged with the reason) and the table in force stays.
// data == NULL with length 0 UNLOADS the table; any other NULL / non-positive
// combination is DCA_ERR_INVALID_ARG. The bytes are copied; the caller keeps
// ownership. The table is PROCESS-wide: every rsamp module of every engine picks
// it up at its next block (one atomic load, no lock, no allocation on the audio
// thread), dipping out and back in over 5 ms each way as for a model change, and
// an engine rebuild keeps it. Reloading the identical bytes changes nothing. A
// table is never freed while the process lives (a module may still run it), so
// at most 32 DISTINCT tables may be loaded per process; past that the call is
// refused (DCA_ERR_INVALID_ARG). Control thread: it parses and allocates.
DCA_API dca_status DCA_CALL dca_rsamp_set_table(const void* data, int32_t length);
// How many rows the loaded table carries a chain for; 0 when none is loaded.
// Never fails. Any thread.
DCA_API int32_t    DCA_CALL dca_rsamp_table_row_count(void);

// ---- Per-node meters + latency (ABI v30) ---------------------------------
// Two things a rig UI cannot fake and the managed side had no way to ask for:
// how loud each pedal is running, and how late the chain is.
//
// Everything here is POLL-based and allocation-free on the audio thread. The
// callback folds each block into a decaying envelope in place; nothing is
// queued, no callback crosses the ABI, and a caller that never polls costs the
// audio thread nothing at all — metering is OFF until switched on, and while it
// is off the DSP callback is byte-for-byte the pre-v30 path.
//
// Enable/disable metering for a seat's chain. Turning it OFF also zeroes the
// meters, so a UI that stops polling does not leave a frozen picture behind.
// `enabled` is 0 / non-zero. Cheap; safe from any thread.
DCA_API dca_status DCA_CALL dca_engine_dsp_set_metering(dca_engine_t engine, int32_t enabled);
DCA_API dca_status DCA_CALL dca_engine_dsp_set_metering_for_player(dca_engine_t engine, int32_t player,
                                                                    int32_t enabled);
// Non-zero when metering is currently on for that seat — so a caller can tell
// "everything is silent" from "nobody switched the meters on".
// 0 for an unknown seat or a null engine: this is the ONE value-returning export
// that does not use the -1 sentinel, because it is a PREDICATE and -1 is
// non-zero, i.e. it would answer "metering is ON" for a dead handle. "Could not
// ask" and "off" are the same actionable answer.
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_metering(dca_engine_t engine, int32_t player);

// Levels for ONE node. All four values are LINEAR amplitudes (0..1 nominal, and
// legitimately > 1 for a node driving into a clipper) — never dB: the ABI
// reports what the samples did and the caller picks the scale. Peaks decay
// ~34 dB/s so a 30 Hz poll sees every transient; the RMS integrates over a
// ~300 ms window. All four read 0 while metering is off.
// Returns DCA_ERR_INVALID_ARG for an unknown node id.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_meter(dca_engine_t engine, int32_t node_id,
                                                           float* out_in_peak, float* out_in_rms,
                                                           float* out_out_peak, float* out_out_rms);

// Bulk read for one UI frame: writes 4 floats per node — in_peak, in_rms,
// out_peak, out_rms — in CHAIN ORDER, index-aligned with
// dca_engine_dsp_get_node_ids_for_player. Returns the node COUNT (call with a
// null buffer / capacity 0 to size; capacity is in FLOATS, i.e. 4 * nodes), or
// -1 for a null engine / an unknown seat.
// One P/Invoke per frame instead of one per pedal.
DCA_API int32_t    DCA_CALL dca_engine_dsp_read_node_meters(dca_engine_t engine, int32_t player,
                                                             float* out_values, int32_t capacity);

// Latency this node adds, in samples at the chain's prepared rate. A BYPASSED
// node reports 0 — it is not in the path, and a caller compensating for the rig
// must not budget for it. Most modules are 0; the ones that are not are the
// pitch/neural family (harmonizer, whammy, klon, namamp's rate converter, the
// drop tuner). Returns DCA_ERR_INVALID_ARG for an unknown node id.
DCA_API dca_status DCA_CALL dca_engine_dsp_get_node_latency(dca_engine_t engine, int32_t node_id,
                                                             int32_t* out_samples);

// Total latency of a seat's chain as currently ordered, in samples, or -1 for
// an invalid seat / null engine.
//
// Inside a split/parallel region this is the MAXIMUM over the enabled branches,
// not their sum: the branches run in parallel, so the region is as late as its
// latest branch. It is NOT a statement that the branches are ALIGNED — the
// engine still does not delay-compensate between branches (the v23 accepted
// limit), and this number deliberately does not pretend otherwise.
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_chain_latency(dca_engine_t engine);
DCA_API int32_t    DCA_CALL dca_engine_dsp_get_chain_latency_for_player(dca_engine_t engine,
                                                                         int32_t player);

// ---- Offline waveform peak generation ----------------------------------
// Decode `path` (any format the JUCE basic format manager supports) and emit
// one peak amplitude (0..1) per `block_ms` window into `out_levels`. Returns
// the total number of peaks the file produces; call once with capacity 0 to
// size the buffer, then again with a buffer large enough to hold it.
DCA_API dca_status DCA_CALL dca_generate_waveform(const char* path,
                                                  double      block_ms,
                                                  float*      out_levels,
                                                  int32_t     capacity,
                                                  int32_t*    out_count);

// Same contract as dca_generate_waveform, but decodes an in-memory audio
// blob (any container the basic format manager recognizes). Lets callers
// that already hold the file bytes (PSARC memory playback) skip a second
// end-to-end disk read. The blob is only read for the duration of the call.
DCA_API dca_status DCA_CALL dca_generate_waveform_from_memory(const void* data,
                                                              int64_t     data_size,
                                                              double      block_ms,
                                                              float*      out_levels,
                                                              int32_t     capacity,
                                                              int32_t*    out_count);

// ---- Offline decode-to-PCM ---------------------------------------------
// Decode `path` (any format the JUCE basic format manager supports — WAV,
// AIFF, FLAC, OGG Vorbis, MP3 on Windows) into a freshly allocated
// interleaved-float32 buffer. The caller takes ownership and MUST free the
// returned buffer via `dca_decode_free`. Two-step usage:
//   1. Call with `out_samples=NULL` to get sample_rate, channels, frames.
//   2. Call dca_decode_audio_file again — or directly use the values to
//      size your own buffer if you pre-allocate.
// `out_frames` is the per-channel frame count; total floats in the buffer
// are `(*out_frames) * (*out_channels)`.
DCA_API dca_status DCA_CALL dca_decode_audio_file(const char* path,
                                                  float**     out_samples,
                                                  int32_t*    out_sample_rate,
                                                  int32_t*    out_channels,
                                                  int64_t*    out_frames);

// Free a buffer returned by `dca_decode_audio_file`. Safe to call with NULL.
DCA_API void DCA_CALL dca_decode_free(float* samples);

// ---- Wwise Vorbis (.wem) decoding (ABI v42, DotCharter #426) ----------------
// A Rocksmith/Wwise .wem is Vorbis with the Ogg framing, two of the three
// headers and (usually) the codebooks stripped. This decoder rebuilds the
// headers in memory (a port of ww2ogg's header/codebook rebuild, codebook
// libraries embedded) and feeds the audio packets straight to libvorbis -- no
// Ogg file, no temporary file, no managed ww2ogg.
//
// Output contract (pinned against the managed Ww2Ogg.Core + NVorbis path):
// the same channel order and sample alignment, and the same LENGTH -- the
// decoded total clipped to the vorb chunk's sample count (granule-less files,
// i.e. every Rocksmith .wem) or to the last packet's granule.
//
// THREADING: a decoder handle is single-threaded -- one caller at a time. Every
// call allocates or decodes; NONE may be made from an audio callback.
// Handles are independent of any engine.
//
// A .wem as an ENGINE SOURCE needs none of these: dca_source_open_file(".wem")
// and dca_source_open_memory(hint "wem") open it through the transport like any
// other format since v42.
typedef struct dca_wem* dca_wem_t;

// Which codebook source / packet format to try. AUTO tries, in this order and
// keeps the first whose header rebuild AND libvorbis header parse succeed:
// AOTUV, DEFAULT, INLINE, DEFAULT_MOD, DEFAULT_STANDARD -- DotCharter's managed
// order (aoTuV first: every Rocksmith .wem uses it). A file that carries its own
// Vorbis headers (old Wwise, vorb chunk 0x28/0x2C) decodes as HEADER_TRIAD under
// any variant.
#define DCA_WEM_VARIANT_AUTO             0
#define DCA_WEM_VARIANT_AOTUV            1
#define DCA_WEM_VARIANT_DEFAULT          2
#define DCA_WEM_VARIANT_INLINE           3
#define DCA_WEM_VARIANT_DEFAULT_MOD      4
#define DCA_WEM_VARIANT_DEFAULT_STANDARD 5
#define DCA_WEM_VARIANT_HEADER_TRIAD     6

typedef struct dca_wem_info {
    int32_t channels;
    int32_t sample_rate;
    int64_t frames;               // per-channel output length: what the reads deliver in total
    int64_t header_sample_count;  // the vorb chunk's sample count
    int64_t decoded_frames;       // what the packets decode to before the end-granule clip
    int32_t variant;              // the DCA_WEM_VARIANT_* that decoded it (never AUTO)
    int32_t mod_packets;          // 1 when ww2ogg's "mod packet" rebuild is in use
    int32_t granule_present;      // 1 when packet headers carry granules
    int32_t packet_count;         // audio packets
} dca_wem_info;                   // 48 bytes

// Open a decoder over a .wem file (UTF-8 path; the whole file is read into
// memory) or over a caller buffer (COPIED -- the caller may free it on return).
// `out` is NULL on every failure. DCA_ERR_INVALID_ARG for a NULL argument, an
// empty buffer or an unknown variant; DCA_ERR_DECODE for a missing file or
// anything that is not a decodable .wem (the reason goes to the log callback).
DCA_API dca_status DCA_CALL dca_wem_open_file(const char* path, int32_t variant, dca_wem_t* out);
DCA_API dca_status DCA_CALL dca_wem_open_memory(const uint8_t* data, size_t length, int32_t variant, dca_wem_t* out);

DCA_API dca_status DCA_CALL dca_wem_get_info(dca_wem_t wem, dca_wem_info* out_info);

// Decode the next frames into an INTERLEAVED buffer of `capacity_frames *
// channels` samples. `*out_frames` is the frames written; 0 means the end.
// DCA_ERR_DECODE when a packet is corrupt (the handle stays valid; seek to recover).
// read_s16 converts with DotCharter's WAV formula: clamp to [-1, 1], -1 maps to
// -32768, everything else rounds half-to-even from (float) sample * 32767.
DCA_API dca_status DCA_CALL dca_wem_read_f32(dca_wem_t wem, float* interleaved, int32_t capacity_frames, int32_t* out_frames);
DCA_API dca_status DCA_CALL dca_wem_read_s16(dca_wem_t wem, int16_t* interleaved, int32_t capacity_frames, int32_t* out_frames);

// Position the next read at `frame` (clamped to [0, frames]). Bit-exact with a
// continuous decode.
DCA_API dca_status DCA_CALL dca_wem_seek(dca_wem_t wem, int64_t frame);

// Free the decoder. NULL is a no-op (DCA_OK).
DCA_API dca_status DCA_CALL dca_wem_destroy(dca_wem_t wem);

// ---- Wwise Vorbis (.wem) ENCODING (ABI v44, DotCharter #317) ----------------
// Encode interleaved float PCM to a Wwise Vorbis .wem with no Wwise installed:
// libvorbis (the bundled 1.3.7) at quality 0.4 with an 18 kHz lowpass -- the
// configuration whose setup header is byte-identical to the one Wwise 2021
// writes for Rocksmith -- framed the way Wwise frames it (codebooks named by
// their packed aoTuV 6.03 library index, 2-byte packet headers, "mod" audio
// packets, the 0x42 fmt chunk with the vorb fields, the relative seek table).
// dca_wem_open_* decodes every file this writes (the round trip is the test).
//
// Only sample_rate 48000 and channels 2 are accepted (DCA_ERR_INVALID_ARG
// otherwise): it is the one configuration whose Wwise header fields are known.
// Resample/upmix first. DCA_ERR_INVALID_ARG also for a NULL buffer or out
// pointer and frames <= 0; DCA_ERR_INTERNAL when the encode fails (the reason
// goes to the log callback). On success `*out_data` is a buffer of `*out_size`
// bytes that the caller MUST free with dca_wem_encode_free; on failure both
// outs are zeroed. Allocates and encodes on the calling thread (seconds for a
// song) -- never from an audio callback. Independent of any engine.
DCA_API dca_status DCA_CALL dca_wem_encode_vorbis(const float* interleaved, int64_t frames, int32_t channels, int32_t sample_rate, uint8_t** out_data, int64_t* out_size);

// Free a buffer returned by dca_wem_encode_vorbis. NULL is a no-op.
DCA_API void DCA_CALL dca_wem_encode_free(uint8_t* data);

// ---- Playback sources -----------------------------------------------------
DCA_API dca_status DCA_CALL dca_source_open_file  (dca_engine_t engine, const char* path, dca_source_t* out_source);
DCA_API dca_status DCA_CALL dca_source_open_memory(dca_engine_t engine, const uint8_t* data, size_t length, const char* format_hint, dca_source_t* out_source);
DCA_API dca_status DCA_CALL dca_source_destroy    (dca_source_t source);

DCA_API dca_status DCA_CALL dca_source_play (dca_source_t source);
DCA_API dca_status DCA_CALL dca_source_pause(dca_source_t source);
DCA_API dca_status DCA_CALL dca_source_stop (dca_source_t source);

DCA_API dca_status DCA_CALL dca_source_seek_seconds        (dca_source_t source, double seconds);
// Both getters return DCA_ERR_INVALID_ARG (writing 0.0 to *out_seconds first)
// for a NULL source AND for an ORPHANED one — the shell dca_engine_destroy
// leaves behind when the engine is freed before its sources. Until 0.48 they
// answered DCA_OK + 0.0 there, which a playhead reads as "at the start" and a
// chart as "zero length"; the rest of this family has always rejected.
DCA_API dca_status DCA_CALL dca_source_get_position_seconds(dca_source_t source, double* out_seconds);
DCA_API dca_status DCA_CALL dca_source_get_length_seconds  (dca_source_t source, double* out_seconds);

// ---- Device-rate conversion, once (ABI v43, DotCharter #435) ----------------
// A preloaded source whose file is at a LOWER rate than the device is upsampled
// on the load thread on EVERY open (src/LoadResampler.h) -- ~420 ms for a
// 10-minute 44.1 kHz song on a 48 kHz device. This export does that conversion
// once, into a WAV a host can keep and open instead.
//
// It decodes `src_path` and runs the steps a preload runs at `device_rate` --
// the same format readers, the same upsampler call, the same 16-bit preload
// quantiser -- and writes EXACTLY what the preload would hold: its int16
// samples as 16-bit PCM at `device_rate`, and its step (the value one int16
// unit is worth: 1/32768, or coarser when the upsample overshot full scale) in
// a "dcps" chunk before the data:
//   "dcps" 24 bytes: u32 version (1) | f32 step | u64 frames | u32 source rate | u32 0
// dca_source_open_* honours that chunk (preloaded and streaming alike), so the
// written file on a `device_rate` device holds BIT-IDENTICAL samples at the same
// step, the same frame count and the same length in seconds as opening
// `src_path` there -- at the size of any 16-bit WAV at that rate. Other readers
// see an ordinary 16-bit WAV, at most ~1.3 dB quiet (the loudest master
// measured) when the step is coarser; the engine's own waveform,
// dca_decode_audio_file and stem separator honour it.
// A file that carries the chunk but does not add up (a truncated or short data
// chunk, a frame count or fmt that disagrees, a step out of range) is DAMAGED:
// dca_source_open_* fail with DCA_ERR_DECODE rather than play it wrong.
// `src_path` may itself be a device-rate file (its step is honoured).
// *out_bits says what was written:
//   16  the device-rate file described above;
//    0  NOTHING was written, because the load would not convert: the engine only
//       ever UPsamples, so a file at or above `device_rate` is opened as it is.
// `dst_wav_path` is replaced; on a failure it is deleted rather than left half
// written (a host should still write to a temporary name and rename).
// `device_rate` must be a whole number of Hz (a WAV header stores an integer).
// DCA_ERR_INVALID_ARG for a NULL argument, a bad device_rate or src == dst;
// DCA_ERR_DECODE when the source is missing, unreadable or cannot be converted;
// DCA_ERR_INTERNAL when the destination cannot be written. Needs no engine.
// THREADING: any non-audio thread. It holds the whole song in memory and may
// spread the conversion over worker threads.
DCA_API dca_status DCA_CALL dca_convert_audio_for_device(const char* src_path, const char* dst_wav_path, double device_rate, int32_t* out_bits);

// Test seam: a fingerprint of the samples a PRELOADED source plays.
//   *out_hash       FNV-1a 64 over the channel count and frame count (each as a
//                   little-endian uint64) and then every stored int16 sample,
//                   channel by channel, little-endian
//   *out_step       the preload's step: a played sample is int16 x step
//   *out_frames     frames held
//   *out_rate       the rate the held buffer is at -- the device rate when the
//                   open converted it, the file's own rate otherwise
//   *out_converted  1 when this open rate-converted at load, else 0
// Every out is zeroed first. DCA_ERR_INVALID_ARG for a NULL out pointer, a NULL
// or orphaned source, or a STREAMING source (nothing preloaded). Any non-audio
// thread, never concurrent with destroying the same source. It hashes the whole
// song: tests and diagnostics only.
DCA_API dca_status DCA_CALL dca_source_get_preload_digest(dca_source_t source, uint64_t* out_hash, float* out_step, int64_t* out_frames, double* out_rate, int32_t* out_converted);

// Linear gain, clamped to [0, +inf). A NEGATIVE gain is a PHASE INVERSION, not
// a quieter source: it cancels against anything correlated already in the mix
// (the same take on another stem, a doubled guitar, the click), which reads as
// a hollow mix rather than as a wrong level — so it is clamped to 0 here rather
// than passed through to juce::AudioTransportSource::setGain, which accepts it.
// There is deliberately NO upper bound: boost above unity is a shipped feature
// (a host's stem faders reach +6 dB, and a per-stem gain multiplied by a master
// gain can legitimately arrive well above 4), and a ceiling invented at this
// boundary would silently cap it. A non-finite value is REJECTED, not clamped.
DCA_API dca_status DCA_CALL dca_source_set_volume          (dca_source_t source, float linear_gain);
// Stereo position, clamped to [-1, +1] (-1 = hard left). Clamped rather than
// rejected so a caller dragging past the end gets the hard left it asked for;
// clamped HERE as well as inside the panner so a caller that caches what it
// wrote agrees with what the engine plays.
DCA_API dca_status DCA_CALL dca_source_set_pan             (dca_source_t source, float pan_minus1_to_plus1);
DCA_API dca_status DCA_CALL dca_source_set_tempo           (dca_source_t source, float factor); // 1=normal, 2=twice fast
DCA_API dca_status DCA_CALL dca_source_set_pitch_semitones (dca_source_t source, float semitones);

// Toggle the Rubber Band engine quality FOR THIS ENGINE. quality=0 selects the
// default high-fidelity R3 (Finer) engine; quality=1 selects the lighter
// R2 (Faster) engine. The host flips this to 1 while stems are loaded so
// 5+ concurrent stretchers fit inside the audio thread's budget. Only
// affects sources opened (and prepareToPlay'd) AFTER the call.
//
// PER ENGINE, and this comment said "globally" until it was corrected. It WAS a
// process global (RubberBandAudioSource's g_engineQuality): the export took a
// handle and ignored it, so in a host running two engines — the app's playback
// engine and the device-less one a take re-amp drives — one of them loading
// stems silently re-voiced the OTHER's stretchers from R3 to R2. A host running
// two engines now gets two independent settings. Pinned by
// RtStretchQualityIsPerEngineNotPerProcess.
DCA_API dca_status DCA_CALL dca_engine_set_stretch_quality(dca_engine_t engine, int32_t quality);

// --- Stem group (shared stretcher) ---------------------------------------
// Flag a source as belonging to the engine's single shared "stem group".
// Grouped sources have their own Rubber Band stretcher bypassed; their
// post-gain post-pan output is summed into a single bus and time-stretched
// / pitch-shifted by ONE shared stretcher. Use for stem mixers (vocals /
// drums / bass / etc decomposed from the same recording) to avoid the
// phasey artefacts independent per-stem stretchers introduce at heavy
// tempo / pitch settings — windows from different stems are no longer
// aligned, so summing them re-creates comb-filter-style cancellations.
// grouped != 0 joins; grouped == 0 leaves. Per-source set_tempo /
// set_pitch are no-ops while grouped (the group stretcher owns those).
DCA_API dca_status DCA_CALL dca_source_set_stem_grouped(dca_source_t source, int32_t grouped);

// Tempo / pitch on the shared stem-group stretcher. Lazily created the
// first time a source joins the group; teardown when the last source
// leaves. No-op when no sources are currently grouped.
DCA_API dca_status DCA_CALL dca_engine_set_stem_group_tempo(dca_engine_t engine, float factor);
DCA_API dca_status DCA_CALL dca_engine_set_stem_group_pitch(dca_engine_t engine, float semitones);

// ---- VST3 plugin host -----------------------------------------------------
// Loads the FIRST component of the bundle at `path`. Exactly
// dca_plugin_load_vst3_component(engine, path, 0, out_plugin).
DCA_API dca_status DCA_CALL dca_plugin_load_vst3        (dca_engine_t engine, const char* path, dca_plugin_t* out_plugin);

// ---- Multi-component VST3 bundles (ABI v33) -------------------------------
// One .vst3 bundle may declare SEVERAL plugins (an amp and its cab, an effect
// and its companion instrument, a vendor's whole suite in one file). Until v33
// this ABI took descriptor 0 and nothing else, so every component past the
// first was simply unreachable — and unreported, because loading the bundle
// looked like it worked.
//
// How many components the bundle at `path` declares. Writes 0 to *out_count
// first. Returns DCA_ERR_INVALID_ARG for a null path / out_count and
// DCA_ERR_PLUGIN when the file yields no descriptors at all (missing, not a
// VST3, or a scan failure). This is a SCAN: it can take seconds on a cold
// bundle, so it is message-thread work like the load itself.
DCA_API dca_status DCA_CALL dca_plugin_count_vst3_components(const char* path, int32_t* out_count);
// Load one component by index. `component_index` outside
// 0..count-1 is REJECTED with DCA_ERR_INVALID_ARG, never clamped onto
// component 0 — a silently substituted plugin is a saved rig that reloads as a
// different effect. *out_plugin is nulled first.
DCA_API dca_status DCA_CALL dca_plugin_load_vst3_component(dca_engine_t engine, const char* path,
                                                           int32_t component_index,
                                                           dca_plugin_t* out_plugin);
DCA_API dca_status DCA_CALL dca_plugin_unload           (dca_plugin_t plugin);
DCA_API dca_status DCA_CALL dca_plugin_attach_to_input  (dca_plugin_t plugin);
// ABI v28: attach to ONE SEAT's input chain (see the per-player block at the
// top of this header). dca_plugin_attach_to_input is exactly
// dca_plugin_attach_to_input_for_player(plugin, 0). A plugin is a member of
// exactly one seat's chain at a time — attaching an already-attached plugin to
// a different seat MOVES it, so a re-seated amp sim can never be run twice in
// one block. `player` outside 0..DCA_MAX_PLAYERS-1 is rejected.
DCA_API dca_status DCA_CALL dca_plugin_attach_to_input_for_player(dca_plugin_t plugin, int32_t player);
// Detaches from whichever seat holds it (and waits out any in-flight block on
// every seat before returning, so the caller may destroy the instance).
DCA_API dca_status DCA_CALL dca_plugin_detach_from_input(dca_plugin_t plugin);
// The seat a plugin is currently attached to, or -1 when it is not attached.
DCA_API int32_t    DCA_CALL dca_plugin_get_input_player(dca_plugin_t plugin);
DCA_API dca_status DCA_CALL dca_plugin_show_editor      (dca_plugin_t plugin, void* native_parent_window);
DCA_API dca_status DCA_CALL dca_plugin_hide_editor      (dca_plugin_t plugin);
DCA_API dca_status DCA_CALL dca_plugin_save_state       (dca_plugin_t plugin, uint8_t* buffer, int32_t capacity, int32_t* out_written);
DCA_API dca_status DCA_CALL dca_plugin_load_state       (dca_plugin_t plugin, const uint8_t* data, int32_t length);

// ABI v14: fired on the JUCE message thread after the user closes the plugin
// editor window with its native close button and the window has been torn
// down. Without this the host's "editor open" state goes stale and the next
// open request toggles the wrong way. Do not block inside; marshal to your
// UI thread and return. Pass cb == NULL to clear.
typedef void (DCA_CALL *dca_editor_closed_callback)(void* user);
DCA_API dca_status DCA_CALL dca_plugin_set_editor_closed_callback(dca_plugin_t plugin, dca_editor_closed_callback cb, void* user);

// ---- Internal instrument bus (MIDI-driven VSTi) --------------------------
// Single-slot virtual instrument that receives MIDI events scheduled by the
// host and mixes its audio output into the master bus. Use to play chart
// notes through a VST3 instrument plugin (Ample Guitar, Kontakt, ...).
// Attach replaces any previously attached instrument; detach silences + frees
// the slot. `dca_engine_send_midi_short` is lock-free and safe from the UI
// thread; events queued while no instrument is attached are dropped.
DCA_API dca_status DCA_CALL dca_plugin_attach_to_instrument  (dca_plugin_t plugin);
DCA_API dca_status DCA_CALL dca_plugin_detach_from_instrument(dca_plugin_t plugin);
DCA_API dca_status DCA_CALL dca_engine_send_midi_short       (dca_engine_t engine, uint8_t status, uint8_t data1, uint8_t data2);
DCA_API dca_status DCA_CALL dca_engine_set_instrument_gain   (dca_engine_t engine, float linear_gain);

// ---- Out-of-process VST bridge (ABI v19) ---------------------------------
// Arm / disarm the shared-memory bridge to the sandboxed VST host process
// (juce_vst_host.exe). While armed, the engine's audio callback performs one
// bounded round trip per block through the named mapping: live input goes
// out for FX-chain processing, queued MIDI shorts (dca_engine_send_midi_short)
// go out for the remote instrument, and the child's processed chain /
// rendered instrument audio come back. On a missed response deadline
// (ONE block) the input passes through DRY and the timeout counter bumps —
// the audio thread never stalls on a dead child. The layout of the mapping
// is pinned in dotcharter_vst_bridge.h (bridge version 1).
// enable=0 tears the bridge down (waits out any in-flight block first).
DCA_API dca_status DCA_CALL dca_engine_set_remote_bridge(
    dca_engine_t engine,
    const char*  shm_name,
    const char*  request_event_name,
    const char*  response_event_name,
    int32_t      enable);

// Health/diagnostic counters for the bridge. `processed` / `timeouts` are
// cumulative block counts since engine creation; `heartbeat` is the child's
// free-running wakeup counter (stalls when the child dies or wedges). All
// zero while the bridge is disabled.
DCA_API dca_status DCA_CALL dca_engine_get_remote_bridge_stats(
    dca_engine_t engine,
    uint32_t*    out_processed,
    uint32_t*    out_timeouts,
    uint32_t*    out_heartbeat);

// ---- Playback position compensation -------------------------------------
// Audible position of a source in seconds, accounting for the read-ahead
// buffered inside the RubberBand tempo stretcher (which would otherwise make
// the reported position drift ahead of what's heard whenever tempo != 1.0).
DCA_API dca_status DCA_CALL dca_source_get_audible_position_seconds(dca_source_t source, double* out_seconds);

// ---- VST3 plugin parameters (ABI v4) -------------------------------------
// Hosted parameter accessors. Use the two-call pattern for the info string.
DCA_API dca_status DCA_CALL dca_plugin_get_parameter_count(dca_plugin_t plugin, int32_t* out_count);
DCA_API dca_status DCA_CALL dca_plugin_get_parameter_info (dca_plugin_t plugin,
                                                           int32_t      index,
                                                           char*        name_buffer,
                                                           int32_t      name_capacity,
                                                           int32_t*     out_name_length,
                                                           float*       out_default_value,
                                                           int32_t*     out_num_steps);
DCA_API dca_status DCA_CALL dca_plugin_get_parameter(dca_plugin_t plugin, int32_t index, float* out_value);
DCA_API dca_status DCA_CALL dca_plugin_set_parameter(dca_plugin_t plugin, int32_t index, float value);

// ---- Plugin latency reporting (ABI v10) ---------------------------------
// Returns the latency the plugin reports via JUCE's getLatencySamples(), in
// samples at the engine's current sample rate. Use this to compensate the
// MIDI scheduling horizon (fire MIDI events earlier by latency/sample_rate
// so the plugin's processed audio arrives in sync with the file/stem path).
// Returns 0 for plugins that don't report latency. Safe to call from any
// thread; readers should re-query whenever the engine sample rate changes
// because some plugins re-report after prepareToPlay.
DCA_API dca_status DCA_CALL dca_plugin_get_latency_samples(dca_plugin_t plugin, int32_t* out_samples);

// ---- MIDI hardware input (ABI v4) ---------------------------------------
// Enumerate available MIDI input devices. Writes UTF-8 newline-separated
// names into `buffer`. Use the two-call pattern: pass NULL/0 to discover
// the required buffer size.
DCA_API dca_status DCA_CALL dca_midi_input_enumerate(char* buffer, int32_t capacity, int32_t* out_size, int32_t* out_count);
DCA_API dca_status DCA_CALL dca_midi_input_open    (int32_t device_index, dca_midi_callback cb, void* user, dca_midi_in_t* out_handle);
DCA_API dca_status DCA_CALL dca_midi_input_close   (dca_midi_in_t handle);

// ---- Offline render (ABI v4) --------------------------------------------
// Render an audio file to a 16-bit WAV on disk. Faster-than-realtime; useful
// for stem export, preview clips and format/rate conversion.
//   src_path   : any decodable audio file
//   dst_wav    : output WAV path (16-bit PCM)
//   tempo      : MUST be 0 (source default) or 1.0
//   semitones  : MUST be 0
//   sample_rate: 0 = use source sample rate; anything else really CONVERTS the
//                audio (juce::ResamplingAudioSource, streamed, so file length
//                is unbounded). Until 2026-09-10 it only relabelled the WAV
//                header while copying samples 1:1, so a 44.1 kHz source
//                "rendered at 48000" came out 8.8% fast and 1.5 semitones
//                sharp, with DCA_OK.
// STRETCH AND PITCH SHIFT ARE NOT IMPLEMENTED HERE and are REJECTED with
// DCA_ERR_INVALID_ARG (this ABI has no NOT_SUPPORTED code, and adding one for a
// rejection would be an ABI change). This comment used to promise "the same
// Rubber Band stretcher used by the live engine" while the body did
// `(void) tempo; (void) semitones;` -- so a request for -2 semitones returned
// DCA_OK and a bit-exact copy at the original pitch. Wire RubberBandAudioSource
// in before relaxing the guard.
DCA_API dca_status DCA_CALL dca_render_to_wav(const char* src_path,
                                              const char* dst_wav,
                                              float       tempo,
                                              float       semitones,
                                              int32_t     sample_rate);

// ---- AI stem separation (ABI v5) ----------------------------------------
// Native htdemucs / demucs v3 inference (Facebook Research). Runs entirely
// in-process; no Python or subprocess required. The backend is
// demucs.cpp by sevagh, gated at native-build time on the CMake option
// `DOTCHARTER_STEMS_DEMUCS_CPP`. When the option is OFF (or when the model
// weights cannot be loaded) `dca_stems_is_supported` returns 0 and the
// managed side falls back to the Python `demucs` CLI.
//
// `dca_stems_separate_file` reads `src_path` (any decodable audio file),
// resamples to the model's native rate (44.1 kHz stereo), runs the model,
// and writes one 32-bit-float WAV per source under `out_dir`:
//   drums.wav, bass.wav, other.wav, vocals.wav  (htdemucs 4-source)
//   + guitar.wav, piano.wav                     (htdemucs 6-source)
// `model_path` points at a `ggml-model-htdemucs-*-f16.bin` produced by
// demucs.cpp's `scripts/convert-pth-to-ggml.py`. `num_threads <= 0` lets
// the engine pick a sensible default (hardware concurrency).
//
// `progress_cb` (may be NULL) is invoked from the inference thread with
// fraction in [0, 1] plus a short status string. Do NOT take locks or
// allocate inside the callback; copy and return.
//
// IT RETURNS void, SO dca_stems_separate_file CANNOT BE CANCELLED. That is now
// a property of this ENTRY POINT rather than of the feature: v36 adds
// dca_stems_progress_cb2 + dca_stems_separate_file2 below, and this pair is
// kept exactly as it was so no existing binding has to change.
typedef void (DCA_CALL *dca_stems_progress_cb)(void* user, float fraction, const char* stage);

// v36. The SAME callback with a cancellation channel: return 0 to STOP, any
// non-zero value to continue. (Non-zero = continue, not the other way round, so
// a binding that forgets to return a value in some path does not silently
// cancel every separation — the accident-prone direction is the safe one.)
//
// Checked between the coarse phases and BETWEEN THE PER-TARGET WAV WRITES,
// which is the interesting one: that stretch writes four or six files, is pure
// I/O after the model has already run, and was uninterruptible. Inference
// itself is demucs.cpp's own threaded call and takes no callback, so a cancel
// arriving during it is honoured when it returns — a real bound on how quickly
// a cancel lands, stated rather than pretended away.
//
// A cancelled run answers DCA_ERR_CANCELLED and LEAVES whatever stems were
// already written on disk: the caller asked to stop, and deleting a partial set
// is a decision for whoever owns out_dir.
typedef int32_t (DCA_CALL *dca_stems_progress_cb2)(void* user, float fraction, const char* stage);

// STATUS CODES, which are three different problems and not one:
//   DCA_ERR_INVALID_ARG     — a null or empty path argument. A caller bug.
//   DCA_ERR_NOT_INITIALIZED — the backend is not compiled in, OR `model_path`
//                             does not exist on disk. Both mean "this feature
//                             is not ready to run", and the second is a
//                             DOWNLOAD the host can go and perform (see
//                             DemucsGgmlModelCatalog). Before W4-N19 a missing
//                             model file answered DCA_ERR_INVALID_ARG, i.e. it
//                             was indistinguishable from a bad path.
//   DCA_ERR_DECODE          — the weights are there but would not load, or the
//                             input audio would not decode.

DCA_API int32_t    DCA_CALL dca_stems_is_supported     (void); // 1 if native backend compiled in
DCA_API dca_status DCA_CALL dca_stems_describe_backend (char* buffer, int32_t capacity, int32_t* out_size);
DCA_API dca_status DCA_CALL dca_stems_separate_file    (const char*           src_path,
                                                        const char*           out_dir,
                                                        const char*           model_path,
                                                        int32_t               num_threads,
                                                        dca_stems_progress_cb progress_cb,
                                                        void*                 progress_user);
// v36. Identical work and identical status codes, plus DCA_ERR_CANCELLED when
// `progress_cb` answered 0. A NULL callback means "no progress wanted", which
// is also "never cancelled", so this behaves exactly like the export above.
DCA_API dca_status DCA_CALL dca_stems_separate_file2   (const char*            src_path,
                                                        const char*            out_dir,
                                                        const char*            model_path,
                                                        int32_t                num_threads,
                                                        dca_stems_progress_cb2 progress_cb,
                                                        void*                  progress_user);

// ---- Offline test / tooling utilities ------------------------------------
// Additive exports (no ABI bump) that run engine DSP off the device path so
// the managed test gate can exercise it deterministically. They are real
// processing (the same code the live callback runs), so they double as
// offline preview/bounce hooks — but their contracts are tooling-grade.

// Drop-tune (CSVD, dcdsp_droptune.h) offline processor. MONO. Processes
// `input` (total_samples floats at sample_rate) into `output` through one
// DropTune instance, applying `semitones_per_segment[i]` for
// `segment_lengths[i]` samples in order — so engage/disengage transitions
// happen mid-stream against a primed ring, exactly like the live pedal.
// segment lengths must sum to total_samples. semitones are clamped to the
// core's down-only [-24, 0] range; max_lag_ms is the splice-lag floor knob
// (10/20/35 in the shipped modes). In-place (input == output) is allowed.
DCA_API dca_status DCA_CALL dca_droptune_process_offline(
    int32_t        sample_rate,
    float          max_lag_ms,
    const float*   semitones_per_segment,
    const int32_t* segment_lengths,
    int32_t        num_segments,
    const float*   input,
    float*         output,
    int32_t        total_samples);

// Run a STEREO buffer through the engine's in-house DSP rig (the same
// dca_engine_dsp_* chain the live input path runs), chunked internally to a
// prepared-safe block size. The rig is prepared lazily at `sample_rate`, which
// must be 8000..192000; concurrent structure/param mutations from other threads
// are safe (that is the point -- the DSP soak test drives exactly that). Do not
// call from two tooling threads at once before the rig was ever prepared.
//
// OFFLINE MEANS OFFLINE: returns DCA_ERR_DEVICE when a device is OPEN. The
// chunk loop takes the chain's process barrier -- the audio callback's own
// wait-one-block mutex -- once per 512 frames, so rendering a three-minute take
// against a running device contends the callback ~16,000 times, each time for a
// whole rig pass on a worker thread. Stop the engine (or use a dedicated
// device-less one, as DotCharter's take re-amp does) before calling.
DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer(
    dca_engine_t engine,
    float*       left,
    float*       right,
    int32_t      num_frames,
    int32_t      sample_rate);

// ABI v28: the same offline render for any seat's rig. This is how per-player
// tone is verified with NO audio device open at all — build seat 0 and seat 1
// differently, push the same input through both, compare. `player` outside
// 0..DCA_MAX_PLAYERS-1 returns DCA_ERR_INVALID_ARG; an open device returns
// DCA_ERR_DEVICE (see above).
DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer_for_player(
    dca_engine_t engine,
    int32_t      player,
    float*       left,
    float*       right,
    int32_t      num_frames,
    int32_t      sample_rate);

// ===========================================================================
// v34 (engine)
// ===========================================================================

// ---- Master-bus ceiling --------------------------------------------------
// A look-ahead soft limiter over the FINAL device buffer, mixed AFTER the song
// bus, every seat's monitor, the click bus and both instrument legs, and placed
// BEFORE the streaming-mirror pickup. Until v34 that sum was only NaN-scrubbed
// and never bounded, so a loud rig plus a loud backing track plus a click could
// hand the driver samples past +-1.0 — a hardware DAC clips them, but the mirror
// carries FLOAT, so the over-range samples survived into OBS / Discord's encoder
// and wrapped there.
//
// **DEFAULT OFF (v35; it was ON in v34.)** -0.3 dBFS and an 80 ms release are
// the defaults for the ceiling and release WHEN THE HOST ENABLES IT. Default ON
// contradicted this engine's own "everything is OFF at engine start" rule: the
// live monitor path runs through this stage, so it cost every guitarist 1 ms of
// latency on their own playing and shifted every existing consumer's
// click-alignment arithmetic by 48 samples at 48 kHz, for a feature they had not
// asked for. **A host that wants a bounded mirror must call this.**
//
// **Switched OFF it is not in the path at all** — the output is bit-for-bit the
// pre-v34 engine and the reported latency is 0. The cost of that is a one-block
// discontinuity when a user toggles it mid-song; see
// dca_engine_get_master_limiter_latency_samples.
//
//   enabled     : 0 = pass through, non-zero = limit.
//   ceiling_db  : clamped to [-24, 0] dBFS.
//   release_ms  : clamped to [1, 1000].
// A NON-FINITE ceiling or release is REJECTED with DCA_ERR_INVALID_ARG rather
// than clamped — every comparison a clamp is built from is false for a NaN, and
// a NaN ceiling is permanent silence.
DCA_API dca_status DCA_CALL dca_engine_set_master_limiter(
    dca_engine_t engine,
    int32_t      enabled,
    float        ceiling_db,
    float        release_ms);

// Read back what the engine actually holds (post-clamp). Any out pointer may
// be NULL.
DCA_API dca_status DCA_CALL dca_engine_get_master_limiter(
    dca_engine_t engine,
    int32_t*     out_enabled,
    float*       out_ceiling_db,
    float*       out_release_ms);

// POLL-ONLY, exactly like the ABI v30 node meters: a decaying hold (0.25 s) of
// the deepest gain reduction the limiter applied, as a POSITIVE number of dB.
// 0.0 while it is idle or disabled. Nothing is queued and no callback crosses
// the ABI; a caller that never polls pays nothing.
DCA_API dca_status DCA_CALL dca_engine_get_master_limiter_reduction_db(
    dca_engine_t engine,
    float*       out_db);

// The limiter's own contribution to the output latency, in samples at the
// device rate. It is ALREADY INCLUDED in dca_engine_get_output_latency_samples;
// this export exists so a caller can decompose that figure.
//
// **It FOLLOWS the `enabled` flag**: 0 while the limiter is off, because while
// it is off it is not in the path. 0 with no device open, too (the stage is
// prepared with the master bus).
//
// The consequence: toggling the limiter mid-song changes the engine's output
// latency by the look-ahead, so a caller that aligns scheduled clicks (ABI v26)
// against dca_engine_get_output_latency_samples must re-read it rather than
// cache it for the session. The alternative — keeping the delay line running
// while the limiter is off so this number never moved — makes every user pay
// 1 ms on their own monitored playing for a feature they switched off, and
// silently re-times the whole engine. It was tried, and the RT gate rejected
// it: 48 samples of unconditional delay broke
// RtEqTrimRampsAndSparseInputIsSilent.
DCA_API dca_status DCA_CALL dca_engine_get_master_limiter_latency_samples(
    dca_engine_t engine,
    int32_t*     out_samples);

// ---- Offline render with PARAMETER AUTOMATION -----------------------------
// dca_engine_dsp_process_buffer_for_player renders a whole take at ONE static
// setting, which is the one thing a re-amp is never asked to do: a volume
// swell, a wah pass and a footswitch mid-solo are all ordinary.
//
// A POD by value, like dca_device_info and dca_xrun_stats. Its size and field
// offsets are static_asserted native side and mirrored by a managed layout
// test; do not reorder it.
//
//   frame    : absolute sample index into the buffer, 0 <= frame < frames.
//   node_id  : a DSP node id (seat-scoped, the usual v28 stride rule). Since
//              v35 it is CHECKED against `player`: ids are engine-global, so an
//              event carrying seat 1's id, submitted to a render of seat 0, used
//              to be applied to SEAT 1's chain. The `player` parameter read as a
//              scope and was not one. A foreign id is rejected BY INDEX.
//   param    : the parameter id, UTF-8, NUL-padded. A caller may legally fill
//              all 32 bytes without a terminator; the engine bounds the read.
//              **"bypass" is RESERVED** and routes to the chain's bypass
//              (value != 0 means bypassed) — no module declares a parameter by
//              that id, because the chain owns bypass and not the module.
//   value    : the new value. NON-FINITE IS REJECTED, per event, by index.
typedef struct dca_dsp_automation_event {
    int64_t frame;
    int32_t node_id;
    char    param[32];
    float   value;
} dca_dsp_automation_event;

// Render `interleaved` (1 or 2 channels, in place) through seat `player`'s rig,
// applying `events` as the render reaches their frames.
//
// `events` MUST be sorted by non-decreasing frame; an unsorted array is
// REJECTED rather than silently reordered, because a caller that thinks it is
// automating a sweep and is actually automating a scramble gets a render that
// is plausible and wrong. The render is chunked at each event boundary (and at
// 512 frames otherwise), so an event at frame F is APPLIED at sample F — not
// necessarily HEARD at F, which is what this said until v35: a `bypass` event
// rides the chain's 10 ms crossfade and every user-facing gain inside a core is
// SmoothedValue-ramped by this engine's own rule, so the change BEGINS at F.
//
// `event_count` is capped at 262,144. Events at consecutive frames produce a
// chain of one-sample process calls, each taking the chain's process barrier and
// each paying the whole per-block overhead, so a caller that resamples an
// automation curve per sample turns a 5-minute render into 14 M of them. Past
// the cap is DCA_ERR_INVALID_ARG with out_bad_event_index = -1.
//
// Every event goes through the same `DspChain::setParam` mailbox the LIVE
// callback's setters use — which is what makes an automated render and a live
// performance of the same gesture unable to diverge, and what makes a NaN
// rejected here exactly as it would be rejected live.
//
// On DCA_ERR_INVALID_ARG, `out_bad_event_index` (may be NULL) receives the
// index of the offending event, or -1 when the failure was not event-specific.
//
// NOTHING IS TOUCHED WHEN AN EVENT IS REJECTED — neither the input buffer nor
// the RIG. The buffer half was always true (the write-back follows the render);
// the rig half is new in v35. Until then the whole-list pre-pass checked only
// frame / finiteness / sortedness / non-empty id, and an unknown node or a
// misspelt param id was discovered MID-RENDER, after every earlier event had
// already gone through DspChain::setParam — leaving the seat's chain at an
// arbitrary mid-sweep state the caller could neither see nor undo. The pre-pass
// now RESOLVES every event (seat, node, param id) as well.
//
// OFFLINE MEANS OFFLINE: returns DCA_ERR_DEVICE when a device is OPEN, for the
// same reason dca_engine_dsp_process_buffer_for_player does.
DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer_automated_for_player(
    dca_engine_t                    engine,
    int32_t                         player,
    float*                          interleaved,
    int32_t                         channels,
    int32_t                         frames,
    int32_t                         sample_rate,
    const dca_dsp_automation_event* events,
    int32_t                         event_count,
    int32_t*                        out_bad_event_index);

// Seat-0 alias (v35). Its NON-automated sibling dca_engine_dsp_process_buffer
// has had one since v28; this is the most visible of the seven that did not.
DCA_API dca_status DCA_CALL dca_engine_dsp_process_buffer_automated(
    dca_engine_t                    engine,
    float*                          interleaved,
    int32_t                         channels,
    int32_t                         frames,
    int32_t                         sample_rate,
    const dca_dsp_automation_event* events,
    int32_t                         event_count,
    int32_t*                        out_bad_event_index);

// ---- Sample-accurate MIDI to the instrument bus ---------------------------
// `dca_engine_send_midi_short` stamps every event at sample 0 of whichever
// block happens to drain it — up to one whole buffer of skew (5-10 ms) on every
// note. This is the same queue with an ABSOLUTE output-timeline frame, in the
// domain `dca_engine_get_render_clock` publishes:
//
//   int64_t frames, ticks;
//   dca_engine_get_render_clock(engine, &frames, &ticks);
//   dca_engine_send_midi_at(engine, packed, frames + offsetInSamples);
//
// The event is PARKED on the audio thread until the block that contains its
// frame and then placed at its exact sample offset inside it — exactly the
// mechanism ABI v26's scheduled clicks use, and for the same reason: output
// latency cancels out of the frame arithmetic, so the note leaves the engine in
// the very samples that carry its audio.
//
//   * `packed` is status | data1<<8 | data2<<16, the same word the queue has
//     always carried. A status byte below 0x80 is REJECTED: it is not a MIDI
//     message, and 0 is the queue's own empty-slot marker.
//   * A NEGATIVE `render_frame` means "as soon as possible" and is bit-for-bit
//     what `dca_engine_send_midi_short` produces.
//   * Scheduling AHEAD is legal (up to 5 seconds; past that an event is dropped
//     rather than held, because the only way to be that far ahead is a render
//     clock that rewound under a device restart).
//   * Events are dispatched by FRAME, not by arrival order, so an out-of-order
//     send cannot hold back the ones behind it.
//
// The OUT-OF-PROCESS VST sandbox is sample-accurate too (bridge v3, JuceSharp
// #28): the bridge parks a scheduled event the same way and carries its sample
// offset across the shared mapping, so the child's instrument plays it on the
// same sample the in-process one would. No ABI change: the bridge protocol is
// `kBridgeVersion` in dotcharter_vst_bridge.h, not DCA_ABI_VERSION.
DCA_API dca_status DCA_CALL dca_engine_send_midi_at(
    dca_engine_t engine,
    uint32_t     packed,
    int64_t      render_frame);

// Poll-only counters for the above; any out pointer may be NULL.
//   scheduled : events accepted with a non-negative frame.
//   late      : events whose frame had already gone past when their block ran.
//               They are placed at sample 0 (the pre-v34 behaviour) and counted
//               here, and this counter is the ONLY signal that a caller's
//               scheduling horizon is shorter than the engine's own latency.
//   dropped   : events the queue or the audio thread's parked set could not
//               hold. A swallowed note-off is a stuck note, so it is counted
//               rather than ignored.
DCA_API dca_status DCA_CALL dca_engine_get_midi_timing_stats(
    dca_engine_t engine,
    int64_t*     out_scheduled,
    int64_t*     out_late,
    int64_t*     out_dropped);

DCA_API dca_status DCA_CALL dca_engine_reset_midi_timing_stats(dca_engine_t engine);

// ---- MIDI hardware OUTPUT -------------------------------------------------
// The mirror of the ABI v4 MIDI INPUT surface. Enumerate with the two-call
// pattern (pass NULL/0 to size the buffer); names are UTF-8, newline-separated,
// and the index of a name is the `device_index` to open.
//
// DISPATCH: every message goes through ONE dedicated high-priority thread per
// open device, never the caller's. `juce::MidiOutput::sendBlockOfMessages`
// would have been the obvious answer and is not usable here — it takes the
// output's CriticalSection on the CALLING thread and takes its deadlines in
// Time::getMillisecondCounter() MILLISECONDS. The dispatcher sleeps while its
// earliest deadline is far away and BUSY-SPINS the last ~1.5 ms on the
// high-resolution counter, because a `wait(1)` is 1 ms only while some process
// on the box holds timeBeginPeriod(1) and ~15.6 ms otherwise.
//
// NONE OF THIS IS AUDIO-THREAD SAFE and it is not meant to be: `_send` and
// `_send_at` are message / scheduler-thread entry points.
typedef struct dca_midi_out* dca_midi_out_t;

DCA_API dca_status DCA_CALL dca_midi_output_enumerate(char* buffer, int32_t capacity,
                                                      int32_t* out_size, int32_t* out_count);
DCA_API dca_status DCA_CALL dca_midi_output_open(int32_t device_index, dca_midi_out_t* out_handle);

// Close the device. The dispatcher thread is joined with a BOUNDED wait and, if
// it does not exit, the device is LEAKED and DCA_ERR_DEVICE is returned (the
// close still consumes the handle — do not call again, and do not free it).
// Terminating a thread parked inside the OS MIDI API and then destroying the
// device under it is process-wide corruption; a leak beats a use-after-free.
DCA_API dca_status DCA_CALL dca_midi_output_close(dca_midi_out_t handle);

// Send now (asynchronously — the dispatcher owns the driver call). `length` is
// 1..3 bytes.
//
// DCA_ERR_DEVICE (v35) means the dispatcher queue was FULL and the message was
// DROPPED — see dca_midi_output_get_counts. Before v35 that surfaced as
// DCA_ERR_INVALID_ARG, i.e. a saturated queue told the caller its ARGUMENTS
// were wrong while the note-off it lost became a stuck note.
DCA_API dca_status DCA_CALL dca_midi_output_send(dca_midi_out_t handle,
                                                 const uint8_t* data, int32_t length);

// Send when output frame `render_frame` reaches the EAR.
//
// `engine` is a parameter and not something the handle caches, deliberately:
// the frame domain belongs to the engine, and an output holding an
// `AudioEngine*` would outlive it the moment a caller destroyed the engine
// first. The frame -> deadline conversion runs here, on the calling thread,
// against the render clock PLUS the engine's output latency (master limiter
// included) — so a hardware synth's note lines up with what the user hears and
// not with what the callback has just rendered. A negative `render_frame`, or
// an engine with no device open, degrades to `_send`.
DCA_API dca_status DCA_CALL dca_midi_output_send_at(dca_midi_out_t handle,
                                                    dca_engine_t   engine,
                                                    const uint8_t* data,
                                                    int32_t        length,
                                                    int64_t        render_frame);

// How many messages the dispatcher has actually put on the wire. A scheduler is
// trivially "working" if nothing ever checks that anything came out the far end.
DCA_API dca_status DCA_CALL dca_midi_output_get_sent_count(dca_midi_out_t handle,
                                                           int64_t* out_count);

// Both dispatcher counters in one call (v35). `out_dropped` is what
// `_get_sent_count` could never say: how many messages were THROWN AWAY because
// the 4,096-entry queue was saturated (a dense automation pass, or a wedged
// driver backing the dispatcher up). Either out-pointer may be NULL; both NULL
// is DCA_ERR_INVALID_ARG.
DCA_API dca_status DCA_CALL dca_midi_output_get_counts(dca_midi_out_t handle,
                                                       int64_t* out_sent,
                                                       int64_t* out_dropped);

// All THREE dispatcher counters (v36). `out_failed` is the one v35 could not
// report: messages the DRIVER threw on inside sendMessageNow. It is a different
// fault from `out_dropped` — dropped means "we never handed it over" (our queue
// was full), failed means "it reached the driver and did not come out the other
// side" — and before v35 counted it separately, `sent` moved on a throw too, so
// a wedged interface read as working. `sent + failed` is what `sent` alone used
// to report.
//
// A THIRD export rather than a third out-param on the one above: an existing
// caller passes two pointers and must keep compiling, and a NULL-terminated
// "pass one more" convention does not exist in this ABI. Any out-pointer may be
// NULL; all three NULL is DCA_ERR_INVALID_ARG.
DCA_API dca_status DCA_CALL dca_midi_output_get_counts3(dca_midi_out_t handle,
                                                        int64_t* out_sent,
                                                        int64_t* out_dropped,
                                                        int64_t* out_failed);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DOTCHARTER_AUDIO_H
