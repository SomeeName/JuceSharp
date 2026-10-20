#include "ChartVoiceFactory.h"

#include "GuitarChartVoice.h"
#include "PlaceholderPluckVoice.h"
#include "../../include/dotcharter_audio.h"

namespace dcsynth {

int voiceAvailability(int kind) noexcept
{
    switch (kind)
    {
        case DCA_CHART_VOICE_PLACEHOLDER: return 1;
        // The built-in guitar and bass (#39 step 1 + the bass step): real
        // recordings when the sample pack is installed beside the DLL, the
        // waveguide string model when it is not — so both are always available.
        case DCA_CHART_VOICE_GUITAR:      return 1;
        case DCA_CHART_VOICE_BASS:        return 1;
        default:                          return -1;
    }
}

std::unique_ptr<ChartVoice> makeVoice(int kind, const dcvoice::InstrumentConfig& config)
{
    switch (kind)
    {
        case DCA_CHART_VOICE_GUITAR: return std::make_unique<GuitarChartVoice>(false, config);
        case DCA_CHART_VOICE_BASS:   return std::make_unique<GuitarChartVoice>(true, config);
        default:                     return makeVoice(kind);
    }
}

int64_t livePackBytes()
{
    return dcvoice::livePackBytes();
}

bool voicePackInstalled(const char* name)
{
    return dcvoice::voicePackInstalled(name);
}

std::unique_ptr<ChartVoice> makeVoice(int kind)
{
    switch (kind)
    {
        case DCA_CHART_VOICE_PLACEHOLDER: return std::make_unique<PlaceholderPluckVoice>();
        case DCA_CHART_VOICE_GUITAR:      return std::make_unique<GuitarChartVoice>(false);
        case DCA_CHART_VOICE_BASS:        return std::make_unique<GuitarChartVoice>(true);
        default:                          return nullptr;
    }
}

} // namespace dcsynth
