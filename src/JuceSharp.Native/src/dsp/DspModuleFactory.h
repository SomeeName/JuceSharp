#pragma once

#include "DspModule.h"

#include <memory>
#include <vector>

// Factory + static catalogue for the in-house tone-rig modules (0.16 Phase 0).
// The catalogue drives the managed pedalboard UI (generic param sliders), and
// createModule() instantiates a module by its type id for the DspChain.
namespace dcrig
{

// All available module types, in a stable display order. Built once.
const std::vector<ModuleTypeInfo>& moduleCatalogue();

// Create a module instance for a type id, or nullptr if the id is unknown.
std::unique_ptr<DspModule> createModule(const char* typeId);

// Capability bits for one catalogue entry, by INDEX into moduleCatalogue()
// (ABI v34, behind dca_dsp_module_type_flags). Bit 0 = the module accepts an
// external sidechain key. Returns 0 for an index the catalogue does not have.
//
// This is a separate export rather than a field on ModuleTypeInfo because
// dca_dsp_module_type_info cannot change signature — and a separate one is
// where the NEXT capability bit goes too, instead of a second signature break.
uint32_t moduleTypeFlags(int index);

// Bit 0: the module accepts an external sidechain key.
constexpr uint32_t kModuleFlagSidechain = 1u;

} // namespace dcrig
