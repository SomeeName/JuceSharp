# C ABI Reference

The public C header is `src/JuceSharp.Native/include/dotcharter_audio.h`.
Every function uses C linkage and the cdecl calling convention. The current
`DCA_ABI_VERSION` is **46**, and the header exports **224** functions
(`grep -c '^DCA_API'` — re-measure rather than carrying this number forward;
it read 148 at v32, still said so after v33 shipped, and said 162 through the
whole v34 engine wave that added 14. `AbiExceptionGuardLintTests` now asserts
the count it states, so at least one copy of this number cannot rot silently).

The `dca_` prefix and the header filename are historical — this engine was
extracted from DotCharter and the symbols were kept so existing callers did
not have to be rewritten.

## Conventions

| Topic                | Convention                                                      |
| -------------------- | --------------------------------------------------------------- |
| Calling convention   | cdecl (`DCA_CALL` macro).                                        |
| String encoding      | **UTF-8, everywhere** — paths, device ids and names, module ids, log lines. Managed declarations use `[MarshalAs(UnmanagedType.LPUTF8Str)]`; `LPStr` is a bug. |
| Fixed out buffers    | Truncate at a **code point**, never a byte (`dcabi::utf8TruncatedLength`). A byte-wise cut would decode to U+FFFD and corrupt the tail. |
| Return values        | Most functions return `dca_status` (0 = `DCA_OK`). A handful return `int32_t` or `float` directly — the count/id/step getters below. |
| Handles              | Opaque struct pointers: `dca_engine_t`, `dca_source_t`, `dca_plugin_t`, `dca_midi_in_t`, `dca_midi_out_t`. Never deref them. |
| Memory ownership     | The engine owns every handle it returns. The only caller-frees export is `dca_decode_free`. |
| Two-call sizing      | Enumerations and buffer fills take `capacity` and an `out_count`; call once with capacity 0 to size, then again to fill. |
| Threading            | Every export is safe to *call* from any thread — plugin calls are proxied onto the message thread via `JuceHost::callSync`. Thread-safe is not state-independent: the offline rig renders (`dca_engine_dsp_process_buffer*`) answer `DCA_ERR_DEVICE` while a device is OPEN, and must not be driven from two tooling threads at once before the rig has ever been prepared. "Offline means offline." |

## Status codes

```c
typedef enum dca_status {
    DCA_OK                  = 0,
    DCA_ERR_INVALID_ARG     = 1,
    DCA_ERR_NOT_INITIALIZED = 2,
    DCA_ERR_DEVICE          = 3,
    DCA_ERR_DECODE          = 4,
    DCA_ERR_PLUGIN          = 5,
    DCA_ERR_INTERNAL        = 99
} dca_status;
```

Ordinals never change once shipped; new codes are appended.

## Seats and node ids

```c
#define DCA_MAX_PLAYERS           4
#define DCA_PLAYER_NODE_ID_STRIDE 1000000
```

The engine hosts up to four independent live-input rigs ("seats"), each with
its own DSP chain, VST3 input chain, capture ring and hardware input channel,
all sharing **one** open device. `dca_engine_set_player_count` decides how many
are live; it defaults to 1, and at 1 the callback runs the pre-v28 path
byte-for-byte.

Node ids are unique per **engine**: seat `p` hands out
`p * DCA_PLAYER_NODE_ID_STRIDE + local`. That is why every NODE-scoped export
kept its pre-v28 signature and resolves the seat from the id, while only
CHAIN-scoped ones gained a `*_for_player` variant. A node id naming a seat
≥ `DCA_MAX_PLAYERS` is **rejected**, never clamped onto another seat.

---

## Library lifecycle

| Export | Since | Purpose |
|---|:--:|---|
| `dca_abi_version` | v1 | The ABI integer the DLL implements. |
| `dca_version_string` | v1 | Human-readable engine version, for logs only. |
| `dca_set_log_callback` | v1 | Install the log sink; `NULL` unhooks it. |

## Device enumeration

| Export | Since | Purpose |
|---|:--:|---|
| `dca_enumerate_output_devices` | v1 | Output devices, no engine required. |
| `dca_enumerate_input_devices` | v1 | Input devices, no engine required. |
| `dca_enumerate_host_devices` | v13 | Devices for one named host type, without opening anything. |
| `dca_enumerate_host_types` | v13 | Every `AudioIODeviceType` JUCE recognises here. |
| `dca_engine_enumerate_output_devices` | v1 | Output devices as seen by a live engine. |
| `dca_engine_enumerate_input_devices` | v1 | Input devices as seen by a live engine. |

`dca_device_info` carries `type_name[64]` (ABI v7): the driver enum collapses
every WASAPI variant into `DCA_DRIVER_WASAPI`, so the type name is the only way
to tell shared / exclusive / low-latency apart.

## Engine lifecycle

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_create` | v1 | Create an engine from a `dca_engine_config`. |
| `dca_engine_create2` | v41 | `dca_engine_create` plus per-engine `DCA_ENGINE_CREATE_*` flags: `NULL_DEVICE` registers the headless "Null" device type for THIS engine, `NULL_DEVICE_MANUAL` also puts it in manual-pump mode. OR-ed with the `DCA_ENABLE_NULL_DEVICE` / `DCA_NULL_DEVICE_MANUAL` env vars; an unknown bit is `DCA_ERR_INVALID_ARG`. |
| `dca_engine_destroy` | v1 | Destroy it; force-tears any orphaned plugins first. |
| `dca_engine_start` | v1 | Open the device and start streaming. |
| `dca_engine_stop` | v1 | Stop the device. |
| `dca_engine_get_sample_rate` | v1 | Active rate in Hz. |
| `dca_engine_get_buffer_frames` | v1 | Active block size in frames. |
| `dca_engine_get_output_latency_samples` | v7 | Device + driver output latency **plus the master limiter's look-ahead** (v34); 0 with no device open. |
| `dca_engine_set_master_limiter` | v34 | Master-bus ceiling: enable, ceiling dB `[-24, 0]`, release ms `[1, 1000]`. Non-finite is REJECTED, never clamped. **DEFAULT OFF since v35** (it was ON in v34): the stage sits on the LIVE MONITOR path, so default-ON cost every player 1 ms of latency on their own playing and moved every consumer's click alignment by 48 samples at 48 kHz. The host opts in. |
| `dca_engine_get_master_limiter` | v34 | Read back what the engine holds, post-clamp. Any out pointer may be NULL. |
| `dca_engine_get_master_limiter_reduction_db` | v34 | Poll-only decaying hold (0.25 s) of the deepest gain reduction, in POSITIVE dB. |
| `dca_engine_get_master_limiter_latency_samples` | v34 | The limiter's own share of the output latency. 0 while it is OFF — switched off it is not in the path. |
| `dca_engine_reconfigure` | v8 | In-place device reconfigure (`AudioDeviceManager::setAudioDeviceSetup`). |
| `dca_engine_set_device_type` | v12 | In-place host swap to another `AudioIODeviceType`. |

> ⚠️ **Warning**
> `stop()` + `start()` on one engine does **not** restore playback — sources
> are detached from the mixer for good. Recreate or reconfigure instead. A
> device rebuild only replays what `prepareMasterBus` knows about.

## Live input monitoring

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_monitor_gain` | v1 | Direct-monitor gain, ramped across the block. |
| `dca_engine_set_input_gain` | v1 | Pre-chain input trim, ramped. |
| `dca_engine_get_input_peak` | v1 | Read-and-reset the input peak (linear). |
| `dca_engine_set_input_pitch_shift` | v17 | Drop-tune amount in semitones (seat 0). |
| `dca_engine_set_input_pitch_shift_mode` | v18 (meaning changed at v25) | Drop-tune quality/lag mode. |
| `dca_engine_set_player_input_pitch_shift` | v31 | The same knob, per seat. |
| `dca_engine_set_player_input_pitch_shift_mode` | v31 | The same mode, per seat. |
| `dca_engine_show_asio_panel` | v1 | Open the vendor's ASIO control panel (modal). |

## Live input capture (ABI v11 — dry, pre-gain, pre-chain)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_input_capture_channel` | v11 | Which input channel feeds the mono ring; `-1` mixes all. |
| `dca_engine_read_input_capture` | v11 | Latest window from the rotating ring. Must **not** be drain-polled. |
| `dca_engine_read_input_capture_stream` | v15 | Streaming read with its own cursor; only unconsumed samples. |
| `dca_engine_read_input_capture_stream2` | v16 | As above, plus the absolute stream position. |
| `dca_engine_get_input_capture_sync` | v16 | Atomically sample {captured samples, host clock ticks}. |

## Per-player seats: routing + capture (ABI v28)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_player_count` | v28 | How many seats are live (1…`DCA_MAX_PLAYERS`). |
| `dca_engine_get_player_count` | v28 | Current seat count. |
| `dca_engine_set_player_input_channel` | v28 | Hardware input channel for one seat. |
| `dca_engine_get_player_input_channel` | v28 | Read it back. |
| `dca_engine_set_player_monitor_gain` | v28 | Per-seat monitor gain, ramped. |
| `dca_engine_get_player_input_peak` | v28 | Read-and-reset one seat's input peak. |
| `dca_engine_read_input_capture_for_player` | v28 | Windowed capture read for one seat. |
| `dca_engine_read_input_capture_stream_for_player` | v28 | Streaming capture read for one seat. |
| `dca_engine_read_input_capture_stream2_for_player` | v28 | …with the absolute stream position. |
| `dca_engine_get_input_capture_sync_for_player` | v28 | Per-seat capture/host-clock sample. |

## Master / filter bus

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_master_volume` | v1 | Master gain, ramped across the block. |
| `dca_engine_set_high_pass` | v1 | Master high-pass; `freq <= 0` disables. |
| `dca_engine_set_low_pass` | v1 | Master low-pass. |
| `dca_engine_set_band_pass` | v1 | Master band-pass. |

## Parametric EQ (ABI v29)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_eq_band` | v29 | Configure one of six bands (type, freq, Q, gain, enabled). |
| `dca_engine_set_eq_enabled` | v29 | Master enable for the whole bank. |
| `dca_engine_set_eq_solo` | v29 | Solo one band; negative clears. |
| `dca_engine_set_eq_route` | v29 | Which bus the bank sits on. |
| `dca_engine_set_eq_trim` | v29 | Post-EQ make-up trim in dB. |
| `dca_engine_get_eq_active_count` | v29 | How many bands are actually **audible**. |
| `dca_engine_read_eq_spectrum` | v29 | Sampled magnitude response, in dB, for a UI curve. |

A band is published only when it is audible, decided once on the message
thread — so `get_eq_active_count` and the audio callback are one predicate
rather than two copies of one. Everything is OFF at engine start.

## Streaming mirror

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_mirror_device` | v1 | Mirror the final master mix to a second device; `NULL` or `""` disables. |

## Click bus

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_play_click` | v1 | Fire a click immediately (beat / downbeat / clap). |

## Sample-accurate click scheduling (ABI v26)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_get_render_clock` | v26 | `{frames rendered, host ticks}` at a block boundary. |
| `dca_engine_schedule_click` | v26 | Queue a click at an absolute render frame. |
| `dca_engine_clear_scheduled_clicks` | v26 | Drop every queued click. |

```
frame(T) = render_frames + (T - audible_position) / speed * sample_rate
```

Output latency is deliberately absent: the click lands in the same output
samples as the note's own audio, so both cross the identical device buffer.
The render clock is published **immediately after the transport pull**, which
is what makes it and `dca_source_get_audible_position_seconds` describe the
same output sample. Clear the queue on seek / pause / speed change — the render
clock keeps running while the transport does not.

## Dropout / xrun counters + device health (ABI v30)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_get_xrun_stats` | v30 | Snapshot the callback's own dropout counters. |
| `dca_engine_reset_xrun_stats` | v30 | Zero them. |
| `dca_engine_get_callback_load` | v46 | The callback's load as a fraction of the buffer period: an average (0.5 s EMA) and a decaying peak (1.5 s), not reset by reading. |

Three counters, three different faults: `late_callbacks` (the callback arrived
late — the audible one, and the only signal WASAPI/CoreAudio give at all),
`deadline_overruns` (we were the cause), `near_misses` (a load warning).
`device_xruns` is **−1** on a driver that cannot report; never surface that as
zero. The first four callbacks of a device session are ignored.

## In-house DSP tone rig

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_add_node` | v21 | Append a module by type id; returns the node id (seat 0). |
| `dca_engine_dsp_remove_node` | v21 | Remove a node after its fade-out. |
| `dca_engine_dsp_move_node` | v21 | Reorder a node; never clears its delay buffers. |
| `dca_engine_dsp_set_bypass` | v21 | Crossfade a node out or in over 10 ms. |
| `dca_engine_dsp_set_param` | v21 | Set a float param. NaN is rejected and reported. |
| `dca_engine_dsp_get_param` | v21 | Read a float param back. |
| `dca_engine_dsp_node_count` | v21 | Node count (seat 0). |
| `dca_engine_dsp_get_node_ids` | v21 | Node ids in chain order (seat 0). |
| `dca_engine_dsp_clear` | v21 | Empty the chain (seat 0). |

### Per-player chain ops (ABI v28)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_add_node_for_player` | v28 | Append a module to one seat's chain. |
| `dca_engine_dsp_add_node_bypassed_for_player` | v32 | Insert a **parked** node at zero wet, for live preset construction. |
| `dca_engine_dsp_node_count_for_player` | v28 | Node count for one seat. |
| `dca_engine_dsp_get_node_ids_for_player` | v28 | Node ids for one seat. |
| `dca_engine_dsp_clear_for_player` | v28 | Empty one seat's chain. |

### String-valued node parameters (ABI v22)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_set_node_string` | v22 | Set a string param (IR path, model path, lyric slot…). |
| `dca_engine_dsp_get_node_string` | v22 | Read it back. |

### Split / parallel chains (ABI v23)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_set_node_branch` | v23 | Tag a node into a branch of a split region. |
| `dca_engine_dsp_get_node_branch` | v23 | Read the branch tag. |

### Node introspection (ABI v33)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_get_node_type` | v33 | The node's catalogue type id (size-then-copy UTF-8). |
| `dca_engine_dsp_get_bypass` | v33 | The node's bypass FLAG — message-thread intent, not the fade position. |

Both were things the chain knew and never told anybody: before v33 a caller
could only know a node's type and bypass state by having created the node
itself, so a chain enumerated through `dca_engine_dsp_get_node_ids*` came back
as bare integers a UI could not look a parameter list up for. A node id naming
a seat past `DCA_MAX_PLAYERS` is REJECTED, never clamped.


### Edit scopes (ABI v34)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_begin_edit_for_player` | v34 | Open a scope: publication is SUPPRESSED for this seat until commit. |
| `dca_engine_dsp_commit_edit_for_player` | v34 | Publish everything the scope accumulated, as one change. |
| `dca_engine_dsp_abort_edit_for_player` | v34 | Throw the scope away; the pre-edit structure comes back. |
| `dca_engine_dsp_get_editing_for_player` | v34 | Non-zero while a scope is open on that seat. |
| `dca_engine_dsp_begin_edit` / `_commit_edit` / `_abort_edit` / `_get_editing` | v35 | Seat-0 aliases. Every pre-v28 export is one; these four broke that shape for a release. |

A preset change is ONE gesture and this ABI had no way to say so. Every
add / remove / move / retag / bypass publishes an ordering snapshot and takes
the engine's one-block barrier, so a twelve-pedal preset was twelve
publications plus ~thirty parameter writes, each individually visible to the
audio callback — the board assembled itself pedal by pedal, and v32's parked
insertion only ever covered ONE node.

Inside a scope, structure and parameter writes accumulate against a SHADOW
chain. Reads (`_get_node_ids*`, `_node_count*`, `_get_param`, `_get_bypass`,
`_get_node_branch`, `_get_chain_latency*`) report that shadow, so a caller can
build a preset and read it back before anybody hears it. Nodes ADDED inside a
scope start parked at zero wet whatever their bypass flag and fade in together;
nodes REMOVED inside a scope keep playing out of the live chain and fade out
together, retiring after the barrier. **Commit costs at most TWO publications
whatever the preset size** — one transitional (only when the scope removed
something audible) and one final — and every ramp lands in the same ~10 ms
window.

Abort restores the pre-edit STRUCTURE — membership, order, bypass flags, branch
tags — and destroys the nodes the scope added. Parameter VALUES are not
journalled and are not rolled back: a scope is a structure transaction, not an
undo stack.

A scope is per SEAT. **A nested begin ABORTS the stale scope, logs a Warning and
opens a fresh one — it SUCCEEDS.** It is never a reference
count (two owners editing one rig would have the inner commit publish the outer
one's half-built state), but refusing left the dead scope OPEN, and an open scope
publishes nothing to the audio callback: a caller that dropped its scope froze
that seat's rig for the life of the engine while every later add/remove/move/
bypass accumulated in the shadow and returned success. Discarding one leaked
scope's structure edits is recoverable; a permanently dead rig is not.
A commit or abort with nothing open is still rejected.
`dca_engine_destroy` ends an open scope rather than leaking the nodes it is
holding for retirement.

### Branch delay compensation (ABI v34)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_set_branch_pdc_for_player` | v34 | Align a split region's branches at the merger. **On by default.** |
| `dca_engine_dsp_get_branch_pdc_for_player` | v34 | Read the flag back. |
| `dca_engine_dsp_set_branch_pdc` / `_get_branch_pdc` | v35 | Seat-0 aliases. |

A parallel region whose branches carry different latencies is a **comb filter**,
not a blend: the merger summed `x[n]` from an empty branch with `x[n-L]` from
one holding a pitch shifter or an oversampled drive. v23 accepted that; the
chain had already computed the per-branch totals for
`dca_engine_dsp_get_chain_latency` and threw them away. The merger now delays
each branch by (max − branch), so **that export describes a region that is
aligned** rather than merely the latest of several that are not.

Off is the pre-v34 behaviour byte for byte. The compensation follows structure
AND parameter changes (a param can move a module's latency) and is re-derived on
a device change. Ceiling 8,191 samples per branch — more than the largest
single-module latency in the catalogue at any supported rate — and a branch that
stacks past it CLAMPS. An **unmatched** splitter auto-merges inside the chain
rather than at a merger node and is not compensated.

### Node sidechain (ABI v34)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_set_node_sidechain` | v34 | Key a node's detector from another signal. |
| `dca_engine_dsp_get_node_sidechain` | v34 | Read the key source back. |
| `dca_dsp_module_type_flags` | v34 | Capability bits per catalogue entry; bit 0 = accepts a key. |

Until v34 every module in the rig was single-buffer, and the compressor's
"sidechain" was an internal 100 Hz high-pass on its OWN input — a detector
shape, not a sidechain. A keyed node's audio path, crossfade and scrub are
unchanged; only its DETECTOR reads somewhere else.

| `source` | Meaning |
|---|---|
| `DCA_DSP_SIDECHAIN_NONE` (0) | The module's own input. Default, and byte-for-byte the pre-v34 behaviour. |
| `DCA_DSP_SIDECHAIN_INPUT` (1) | The seat's DRY input — this chain's input block — so a pedal late in the chain follows the pick, not what the drive left of it produced. |
| `DCA_DSP_SIDECHAIN_SONG` (2) | The song / master mix as it stands when this seat is processed (the transport output after the master bus, plus the monitor of any seat processed earlier in the same callback). Ducks the rig under the backing track. |

REJECTED with `DCA_ERR_INVALID_ARG` for an unknown node, a source outside 0..2,
or a module whose `dca_dsp_module_type_flags` does not carry
`DCA_DSP_MODULE_FLAG_SIDECHAIN` — a source a module silently ignored would read
as a live feature that does nothing. If the requested source is UNAVAILABLE when
the block runs (no song mix on the offline render path), the module keys off its
own input: **never off silence**, because "keyed from silence" on a gate means
closed. `compressor` and `gate` accept a key today.

The capability is a separate export because `dca_dsp_module_type_info` cannot
change signature — and because the next capability bit goes there too, instead
of a second signature break.

### Module catalogue

| Export | Since | Purpose |
|---|:--:|---|
| `dca_dsp_module_type_count` | v21 | How many module types the DLL was built with. |
| `dca_dsp_module_type_info` | v21 | Id + display name for one type. |
| `dca_dsp_module_type_category` | v33 | Which palette drawer the type belongs in. |
| `dca_dsp_module_param_count` | v21 | Float-param count for a module type. |
| `dca_dsp_module_param_info` | v21 | Name, range and default for one float param. |
| `dca_dsp_module_string_param_count` | v22 | String-param count for a module type. |
| `dca_dsp_module_string_param_info` | v22 | Describe one string param. |

### Enum value labels (ABI v24)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_dsp_module_param_label_count` | v24 | How many named values a discrete param has. |
| `dca_dsp_module_param_label` | v24 | One label. |

### Discrete quantization step (ABI v27)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_dsp_module_param_step` | v27 | Step size for an ordinal param; > 0 means discrete. |

Every discrete param declares itself — NAMED (value labels) or ORDINAL
(step > 0). A param the adapter rounds is never a plain knob, or the UI parks
it at "6.5 stages" while the core runs 6.

### Rocksmith amp chain table (ABI v45)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_rsamp_set_table` | v45 | Load the `rsamp` chain table (the blob of `src/dsp/cores/dcdsp_rsamp_table.h`), or unload it with `NULL, 0`. Process-wide, control thread; the bytes are copied. A malformed blob is `DCA_ERR_INVALID_ARG` (reason logged) and the table in force stays. |
| `dca_rsamp_table_row_count` | v45 | How many rows the loaded table has a chain for; 0 when none. Any thread. |

The library ships no table: the numbers are Rocksmith's, built by the host from
the user's own install. Without one every `rsamp` module passes audio through.

### Per-node meters + latency (ABI v30)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_dsp_set_metering` | v30 | Turn metering on (seat 0). Off until asked for. |
| `dca_engine_dsp_set_metering_for_player` | v30 | Turn metering on for one seat. |
| `dca_engine_dsp_get_metering` | v30 | Is metering on for this seat? |
| `dca_engine_dsp_get_node_meter` | v30 | Poll one node's in/out peak and RMS. |
| `dca_engine_dsp_read_node_meters` | v30 | Poll a whole seat's meters in one call. |
| `dca_engine_dsp_get_node_latency` | v30 | One node's reported latency in samples. |
| `dca_engine_dsp_get_chain_latency` | v30 | Chain latency (seat 0). |
| `dca_engine_dsp_get_chain_latency_for_player` | v30 | Chain latency for one seat. |

Meters are **poll-only**. Chain latency is the **max** over a split region's
branches, not the sum — and it is not a claim that they are aligned.

## Offline waveform peak generation

| Export | Since | Purpose |
|---|:--:|---|
| `dca_generate_waveform` | v1 | One peak per `block_ms` window from a file. Two-call sizing. |
| `dca_generate_waveform_from_memory` | v20 | The same from an in-memory blob. |

`block_ms` is a **double** on purpose: an integer samples-per-block is exact at
48 kHz and drifts +408 ms over three minutes at 44.1 kHz.

## Offline decode-to-PCM

| Export | Since | Purpose |
|---|:--:|---|
| `dca_decode_audio_file` | — | Decode a whole file to interleaved float32. |
| `dca_decode_free` | — | Free the buffer the call above allocated. One of the two caller-frees exports (the other is `dca_wem_encode_free`, v44). |

## Wwise Vorbis (.wem) decoding (ABI v42)

A native ww2ogg-style header rebuild that feeds libvorbis directly. A
`dca_wem_t` is independent of any engine and single-threaded, and is never
called from an audio callback. Since v42 every engine's format manager also
opens `.wem`, so `dca_source_open_file` / `_open_memory` (hint `"wem"`) play one
directly.

| Export | Since | Purpose |
|---|:--:|---|
| `dca_wem_open_file` | v42 | Open a decoder over a `.wem` file (read whole into memory). `variant` is a `DCA_WEM_VARIANT_*`; AUTO tries aoTuV, default, inline, default-mod, default-standard. |
| `dca_wem_open_memory` | v42 | The same over a caller buffer, which is COPIED. |
| `dca_wem_get_info` | v42 | Channels, rate, output frames, header sample count, decoded frames, variant, mod packets, granules, packet count (`dca_wem_info`, 48 bytes). |
| `dca_wem_read_f32` | v42 | Next frames as interleaved float (not clipped); 0 frames at the end. |
| `dca_wem_read_s16` | v42 | Next frames as interleaved 16-bit with DotCharter's WAV formula (clamp, -1 → -32768, round-half-even of `x * 32767`). |
| `dca_wem_seek` | v42 | Exact seek (restarts libvorbis one packet early). |
| `dca_wem_destroy` | v42 | Free the decoder; NULL is a no-op. |

## Wwise Vorbis (.wem) encoding (ABI v44)

Writes a `.wem` the way Wwise 2021 writes it for Rocksmith, with no Wwise
installed: libvorbis at quality 0.4 with an 18 kHz lowpass, framed the way Wwise
frames it. Every field and where its rule came from:
[`docs/agents/wem-encoder.md`](../agents/wem-encoder.md). Independent of any
engine; never called from an audio callback.

| Export | Since | Purpose |
|---|:--:|---|
| `dca_wem_encode_vorbis` | v44 | Encode 48 kHz stereo interleaved float to a natively allocated `.wem`. `DCA_ERR_INVALID_ARG` for any other rate or channel count, NULL pointers or `frames <= 0`; `DCA_ERR_INTERNAL` (reason logged) when the encode fails or the setup header is not the pinned Wwise one. Both outs are zeroed on failure. |
| `dca_wem_encode_free` | v44 | Free the buffer the call above returned; NULL is a no-op. |

## Playback sources

| Export | Since | Purpose |
|---|:--:|---|
| `dca_source_open_file` | v1 | Open a decodable file as a source. |
| `dca_source_open_memory` | — | Open an in-memory blob, with an optional format hint. |
| `dca_source_destroy` | v1 | Destroy a source. |
| `dca_source_play` | v1 | Start streaming. |
| `dca_source_pause` | v1 | Freeze position. |
| `dca_source_stop` | v1 | Freeze and rewind. |
| `dca_source_seek_seconds` | v1 | Jump to an offset. |
| `dca_source_get_position_seconds` | v1 | Transport position. |
| `dca_source_get_length_seconds` | v1 | Total length. |
| `dca_source_get_preload_digest` | v43 | Test seam: FNV-1a 64 of a PRELOADED source's int16 samples, plus its step, frames, held rate and whether the open converted. `DCA_ERR_INVALID_ARG` for a streaming source. |
| `dca_convert_audio_for_device` | v43 | Engine-less: write `src` at a device rate exactly as the preload holds it -- 16-bit samples plus the preload step in a `dcps` chunk every open honours -- so opening it plays bit-identical samples at 16 bits; `*out_bits` 0 and no file when the load would not convert. `dca_source_open_*` refuse a damaged device-rate file (`DCA_ERR_DECODE`). |
| `dca_source_set_volume` | v1 | Linear gain, clamped to `[0, +inf)`. **No upper bound** — boost above unity ships. |
| `dca_source_set_pan` | v1 | Stereo position, clamped to `[-1, +1]`. |
| `dca_source_set_tempo` | v1 | Rubber Band time-stretch factor (1 = normal). |
| `dca_source_set_pitch_semitones` | v1 | Rubber Band pitch shift. |
| `dca_engine_set_stretch_quality` | — | 0 = R3 (Finer), 1 = R2 (Faster). Only affects sources opened **after** the call. |
| `dca_source_set_stem_grouped` | — | Join / leave the single shared stem-group stretcher. |
| `dca_engine_set_stem_group_tempo` | — | Tempo on the shared group stretcher. |
| `dca_engine_set_stem_group_pitch` | — | Pitch on the shared group stretcher. |

Grouped sources bypass their own stretcher and are summed into one bus driven
by a single stretcher — that is what avoids the comb-filter artefacts
independent per-stem stretchers create. Per-source tempo/pitch are no-ops
while grouped.

## Playback position compensation

| Export | Since | Purpose |
|---|:--:|---|
| `dca_source_get_audible_position_seconds` | v1 | What the listener is hearing now, accounting for stretcher read-ahead. |

## VST3 plugin host

| Export | Since | Purpose |
|---|:--:|---|
| `dca_plugin_load_vst3` | v1 | Load component 0 of a VST3 bundle on the message thread. |
| `dca_plugin_count_vst3_components` | v33 | How many plugins the bundle declares. |
| `dca_plugin_load_vst3_component` | v33 | Load ONE component by index; an index the bundle lacks is rejected, never clamped. |
| `dca_plugin_unload` | v1 | Editor-safe teardown, then destroy. |
| `dca_plugin_attach_to_input` | v1 | Insert into the input chain (seat 0). |
| `dca_plugin_attach_to_input_for_player` | v28 | Insert into **one seat's** input chain. |
| `dca_plugin_detach_from_input` | v1 | Remove from the input chain. |
| `dca_plugin_get_input_player` | v28 | Which seat this plugin is attached to, or −1. |
| `dca_plugin_show_editor` | v1 | Open the editor on a JUCE-owned window. |
| `dca_plugin_hide_editor` | v1 | Close it. |
| `dca_plugin_save_state` | — | Copy the plugin's opaque state blob out. Two-call sizing. |
| `dca_plugin_load_state` | — | Push a saved blob back. |
| `dca_plugin_set_editor_closed_callback` | v14 | Fired on the message thread when the **user** closes the editor. |

## Internal instrument bus (MIDI-driven VSTi)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_plugin_attach_to_instrument` | v1 | Wire a plugin up as the engine's instrument. |
| `dca_plugin_detach_from_instrument` | v1 | Detach it (not unload). |
| `dca_engine_send_midi_short` | v1 | Lock-free short-message push to the instrument. Stamped at sample 0 of the block that drains it. |
| `dca_engine_send_midi_at` | v34 | The same queue with an ABSOLUTE output frame. Parked until the block containing it, then placed on its exact sample. A negative frame = ASAP. |
| `dca_engine_get_midi_timing_stats` | v34 | Poll-only `scheduled` / `late` / `dropped`. `late` is the only signal that a caller's scheduling horizon is too short. |
| `dca_engine_reset_midi_timing_stats` | v34 | Zero them. |
| `dca_engine_set_instrument_gain` | v1 | Instrument-bus gain. |

## Out-of-process VST bridge (ABI v19)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_engine_set_remote_bridge` | v19 | Attach / detach the shared-memory bridge to `juce_vst_host.exe`. |
| `dca_engine_get_remote_bridge_stats` | v19 | Blocks processed, timeouts, heartbeat. |

## VST3 plugin parameters (ABI v4)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_plugin_get_parameter_count` | v4 | How many automatable parameters. |
| `dca_plugin_get_parameter_info` | v4 | Name, default and step count for one. |
| `dca_plugin_get_parameter` | v4 | Read a normalised 0…1 value. |
| `dca_plugin_set_parameter` | v4 | Write a normalised 0…1 value. |

## Plugin latency reporting (ABI v10)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_plugin_get_latency_samples` | v10 | The plugin's reported processing latency. |

## MIDI hardware input (ABI v4)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_midi_input_enumerate` | v4 | Packed list of device names. Two-call sizing. |
| `dca_midi_input_open` | v4 | Open a device; the callback fires on the JUCE MIDI thread. |
| `dca_midi_input_close` | v4 | Close it. |
| `dca_midi_output_enumerate` | v34 | Packed list of MIDI OUTPUT device names. Two-call sizing. |
| `dca_midi_output_open` | v34 | Open one. Starts a dedicated high-priority dispatcher thread. |
| `dca_midi_output_send` | v34 | Send 1..3 bytes now, asynchronously (never on the caller's thread). |
| `dca_midi_output_send_at` | v34 | Send when an output frame reaches the EAR (render clock + output latency). Takes the engine, because the frame domain belongs to it. |
| `dca_midi_output_get_sent_count` | v34 | Messages the dispatcher actually put on the wire. |
| `dca_midi_output_get_counts` | v35 | Both counters in one call. `dropped` is what the sent count could never say: messages thrown away because the 4,096-entry queue was full. Before v35 that surfaced to the caller as `DCA_ERR_INVALID_ARG`, i.e. as a bad ARGUMENT, while the note-off it lost became a stuck note; `_send` / `_send_at` now answer `DCA_ERR_DEVICE`. |
| `dca_midi_output_close` | v34 | Join the dispatcher (BOUNDED) and close the device. On expiry the device is LEAKED and `DCA_ERR_DEVICE` returned — terminating a thread parked in the OS MIDI API and then freeing the device under it is process-wide corruption. |

## Offline render (ABI v4)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_render_to_wav` | v4 | Faster-than-realtime decode of a file to a 16-bit PCM WAV. |

## AI stem separation (ABI v5)

| Export | Since | Purpose |
|---|:--:|---|
| `dca_stems_is_supported` | v5 | 1 when a native backend was compiled in. |
| `dca_stems_describe_backend` | v5 | Human-readable backend description. |
| `dca_stems_separate_file` | v5 | Run native htdemucs; progress on the CALLING thread. |

Compiled in only when `DOTCHARTER_STEMS_DEMUCS_CPP=ON`, which is **off by
default** — so `dca_stems_is_supported` returns 0 in a stock build.

## Offline test / tooling utilities

Additive exports that run engine DSP off the device path so a managed test gate
can exercise it deterministically. They are the same code the live callback
runs, so they double as offline preview/bounce hooks — but their contracts are
tooling-grade.

| Export | Since | Purpose |
|---|:--:|---|
| `dca_droptune_process_offline` | — | Mono CSVD drop-tune over a segment list, against a primed ring. |
| `dca_engine_dsp_process_buffer` | — | Render a stereo buffer through seat 0's rig. |
| `dca_engine_dsp_process_buffer_for_player` | v28 | The same for any seat. |
| `dca_engine_dsp_process_buffer_automated_for_player` | v34 | The same, INTERLEAVED and in place, applying a sorted `dca_dsp_automation_event` list at their frames. Rejects by INDEX via an out param. `"bypass"` is a reserved param id. Since v35 the pre-pass RESOLVES every event (seat, node, param id) as well, so a rejection leaves the RIG untouched and not only the buffer; an id naming ANOTHER SEAT is rejected; `event_count` is capped at 262,144. |
| `dca_engine_dsp_process_buffer_automated` | v35 | Seat-0 alias — the non-automated sibling has had one since v28. |

> ⚠️ **Warning**
> `dsp_process_buffer` takes the chain's process barrier once per 512 frames.
> Rendering three minutes against a **running** device contends the audio
> callback ~16,000 times. Stop the engine — or use a device-less one — first;
> an open device returns `DCA_ERR_DEVICE`.

## Calling-thread guarantees

Every export is safe to call from any thread. Plugin-related calls are
internally proxied onto the message thread via `JuceHost::callSync`, so
even concurrent loads from many threads serialise safely.
