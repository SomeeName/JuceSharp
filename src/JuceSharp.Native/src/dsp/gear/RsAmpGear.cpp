// RsAmp gear — Rocksmith 2014's own amps (DotCharter #88): one module, `rsamp`,
// that runs the soundbank FX chain of the amp its `model` names. The core and
// what is inferred in it: ../cores/dcdsp_rsamp.h. The chains are the game's
// data and are NOT in this library: the host reads them from the user's own
// install and loads them at run time (../cores/dcdsp_rsamp_table.h, ABI v45
// dca_rsamp_set_table); until it does, the module passes audio through. The
// `model` rows (which amp each int names) are public: ../cores/dcdsp_rsamp_models.h.
//
// The ten knobs are POSITIONS, 0..1 of the model's own knob between its
// manifest min and max, in the manifest's knob order: what each one does is
// the model's (Gain on one amp, a 7.5 kHz graphic band on another), so the
// labels here are generic and DotCharter names them per model.
#include "../GearEntry.h"

#include <cmath>
#include <cstring>
#include <string>

#include "../cores/dcdsp_rsamp.h"

namespace dcrig
{
namespace
{

// A NAMED discrete param reaches an int core field, so the adapter lrounds it
// here and getParam hands the INT back; a NaN never reaches std::lround (see
// AmpCabGear.cpp, whose copy GearFilterTests lints with this one).
int quantise(float v, int fallback)
{
    return std::isfinite(v) ? (int) std::lround(v) : fallback;
}

// "k1".."k10" -> 0..9, anything else -> -1.
int knobIndex(const char* id)
{
    if (id[0] != 'k') return -1;
    if (id[1] >= '1' && id[1] <= '9' && id[2] == '\0') return id[1] - '1';
    if (id[1] == '1' && id[2] == '0' && id[3] == '\0') return 9;
    return -1;
}

class RsAmpModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "model")) p_.model = quantise(v, p_.model);
        else if (!std::strcmp(id, "level")) p_.levelDb = v;
        else if (const int k = knobIndex(id); k >= 0) p_.knob[k] = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "model")) out = (float) p_.model;
        else if (!std::strcmp(id, "level")) out = p_.levelDb;
        else if (const int k = knobIndex(id); k >= 0) out = p_.knob[k];
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "rsamp"; }
private:
    dcdsp::RsAmpCore core_; dcdsp::RsAmpParams p_{}; ParamMailbox<dcdsp::RsAmpParams> mail_;
};

std::unique_ptr<DspModule> makeRsAmp() { return std::make_unique<RsAmpModule>(); }

} // namespace

void appendRsAmpGear(std::vector<Entry>& out)
{
    std::vector<std::string> names;
    names.reserve(dcdsp::rsamp::kNumModels);
    for (const auto& m : dcdsp::rsamp::kModelRows) names.emplace_back(m.name);

    std::vector<ParamInfo> params;
    params.push_back({ "model", "Amp", 0.0f, (float) (dcdsp::rsamp::kNumModels - 1), 0.0f, std::move(names) });
    for (int i = 1; i <= dcdsp::rsamp::kMaxKnobs; ++i)
        params.push_back({ "k" + std::to_string(i), "Knob " + std::to_string(i), 0.0f, 1.0f, 0.5f });
    // An output trim. 0 dB is the chain at the game's own levels; DotCharter's
    // loudness levelling spills into it past its trailing gain's ±24 dB.
    params.push_back({ "level", "Level (dB)", -24.0f, 24.0f, 0.0f });

    out.push_back({ { "rsamp", "Rocksmith Amp", std::move(params) }, &makeRsAmp, kCategoryAmp });
}

} // namespace dcrig
