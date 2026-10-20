#pragma once

#include "DspModule.h"
#include "DspModuleFactory.h"

#include <cstdint>
#include <memory>
#include <vector>

// One catalogue row: the metadata the managed catalogue reads, the factory
// function the chain calls, the palette category and the capability flags.
// Shared between DspModuleFactory.cpp (the original 57 rows) and the
// per-category gear files under src/dsp/gear/ (everything added since
// 2026-09-11), so a new module is one core header + one adapter + one row in
// ITS category's file, and nine agents can add gear without touching the
// same translation unit.
namespace dcrig
{

// ABI v33: the fixed palette categories. A category is NOT free text — the
// managed catalogue mirror and any pedalboard UI drawer both key off it, so a
// new one has to be added HERE (and to the native gate's allow-list in
// tests/dsp/tone_regressions.cpp) rather than typed at a call site.
constexpr const char* kCategoryDynamics   = "dynamics";
constexpr const char* kCategoryDrive      = "drive";
constexpr const char* kCategoryAmp        = "amp";
constexpr const char* kCategoryCab        = "cab";
constexpr const char* kCategoryModulation = "modulation";
constexpr const char* kCategoryDelay      = "delay";
constexpr const char* kCategoryReverb     = "reverb";
constexpr const char* kCategoryPitch      = "pitch";
constexpr const char* kCategoryFilter     = "filter";
constexpr const char* kCategoryEq         = "eq";
constexpr const char* kCategoryUtility    = "utility";
constexpr const char* kCategoryRouting    = "routing";
constexpr const char* kCategorySynth      = "synth";

struct Entry
{
    ModuleTypeInfo info;
    std::unique_ptr<DspModule> (*make)();
    // Every entry must name one. It is a separate member rather than the last
    // field of `info` because `info` is brace-initialised inline in the table
    // and most modules omit the stringParams list — appending there would mean
    // spelling out an empty `{}` on every row.
    const char* category;
    // ABI v34 capability bits (dca_dsp_module_type_flags). Bit 0 = the module
    // accepts an external sidechain key. Defaulted, so a row that does not opt
    // in keeps its meaning; the RT gate's
    // RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly asserts the table
    // agrees with what a constructed module actually reports.
    uint32_t flags = 0;
};

// Table-side spelling of the ONE definition in DspModuleFactory.h, so the bit
// is not written down twice.
constexpr uint32_t kFlagSidechain = kModuleFlagSidechain;

// Per-category gear files (src/dsp/gear/<Category>Gear.cpp) append their rows
// here. Called once, in this order, after the original table is built, so a NEW
// module is never inserted before an existing one.
//
// "Stable within a build" is the exact claim, and it is materially weaker than
// it sounds: the LAST row of the original table is `namamp`, immediately before
// these appends and behind `#if DOTCHARTER_DSP_NEURAL`. **A
// -DDOTCHARTER_DSP_NEURAL=OFF build therefore shifts EVERY gear index down by
// one and the catalogue is 99 rows, not 100** -- and three test executables
// build exactly that way (`juce_sharp_tone_regressions`, `juce_sharp_dualcab_test`
// and `juce_sharp_delay_sync_trails` all set DOTCHARTER_DSP_NEURAL=0 while
// compiling the same JUCESHARP_RIG_FACTORY_SOURCES). So the gate binaries and
// the shipped DLL disagree about every gear index.
//
// Nothing PERSISTS an index and that is the reassuring half: the index-keyed
// exports (`dca_dsp_module_type_info` / `_category` / `_flags`) are read only
// inside JuceDspModuleCatalog.Load(), and everything downstream -- presets
// included -- keys by type ID. Never compare a flags or category table across
// builds by index, and never write a test whose floor is a bare count.
void appendDynamicsGear(std::vector<Entry>& out);
void appendDriveGear(std::vector<Entry>& out);
void appendAmpCabGear(std::vector<Entry>& out);
void appendFilterGear(std::vector<Entry>& out);
void appendModulationGear(std::vector<Entry>& out);
void appendPitchGear(std::vector<Entry>& out);
void appendDelayGear(std::vector<Entry>& out);
void appendReverbGear(std::vector<Entry>& out);
void appendUtilityGear(std::vector<Entry>& out);
void appendRsAmpGear(std::vector<Entry>& out);   // Rocksmith 2014's own amp chains (rsamp)

} // namespace dcrig
