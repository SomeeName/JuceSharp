#pragma once

// The chart source's voice catalogue (ABI v38). The ONE place a voice kind
// (DCA_CHART_VOICE_*) becomes an object: dca_chart_voice_is_available and
// dca_engine_chart_source_set_voice_for_player both answer from here, so a
// build can never advertise a voice it cannot construct. Adding the real guitar
// or bass voice (#39 step 1) is a change to ChartVoiceFactory.cpp only — no ABI
// change, no engine change.

#include "ChartVoice.h"
#include "../dsp/voice/dcvoice_models.h"

#include <cstdint>
#include <memory>

namespace dcsynth {

/// 1 = this build contains `kind`, 0 = a known kind it does not contain,
/// -1 = not a DCA_CHART_VOICE_* value at all.
int voiceAvailability(int kind) noexcept;

/// A new, UNPREPARED voice of `kind`, or null when voiceAvailability(kind) != 1.
/// Control thread (allocates).
std::unique_ptr<ChartVoice> makeVoice(int kind);

/// The same, with the INSTRUMENT MODEL selection (#50) the guitar / bass voice
/// is built for (ignored by the placeholder). An invalid selection for the
/// kind's family falls back to that family's legacy selection.
std::unique_ptr<ChartVoice> makeVoice(int kind, const dcvoice::InstrumentConfig& config);

/// Bytes of sample packs alive in this process (every voice's, shared ones
/// once). Control thread.
int64_t livePackBytes();

/// True when sample pack `name` is installed beside the module. Control thread (I/O).
bool voicePackInstalled(const char* name);

} // namespace dcsynth
