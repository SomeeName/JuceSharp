// dcdsp_rsamp_models.h — the ROWS of the `rsamp` model list: which Rocksmith
// 2014 amp each persisted `model` int names. Public names only: each amp's gear
// key and the display name the game's store and every community gear catalogue
// print (DotCharter embeds one, Rocksmith2014.NET's tone_gear_data.json, MIT), in
// ORDINAL order of the key. No number of any amp's chain is here: the chains are
// read on the user's machine from the user's own install and handed in at run
// time (dcdsp_rsamp_table.h, ABI v45 dca_rsamp_set_table). Order is APPEND-ONLY:
// a saved preset names a row by its index, and a loaded table names each of its
// rows by key, refused when the key is not that row's (rsamp::parseTable).
#pragma once

namespace dcdsp {
namespace rsamp {

struct ModelRow {
    const char* key;
    const char* name;   // UTF-8; non-ASCII as byte escapes, each closing its literal
};

inline constexpr int kNumModels = 63;

inline constexpr ModelRow kModelRows[kNumModels] = {
    { "Amp_AT120",                    "AT-120" },                                        // 0
    { "Amp_AT20",                     "AT-20" },                                         // 1
    { "Amp_BT15",                     "BTQ-15" },                                        // 2
    { "Amp_BT30",                     "BTQ-30" },                                        // 3
    { "Amp_BT45",                     "BTQ-45" },                                        // 4
    { "Amp_CA100",                    "Cali-100" },                                      // 5
    { "Amp_CA38",                     "Cali-38" },                                       // 6
    { "Amp_CA85",                     "Cali-85" },                                       // 7
    { "Amp_CS100",                    "CL-SS 100" },                                     // 8
    { "Amp_CS120",                    "CL-SS 120" },                                     // 9
    { "Amp_CS90",                     "CL-SS 90" },                                      // 10
    { "Amp_EN30",                     "EN-30" },                                         // 11
    { "Amp_EN50",                     "EN-50" },                                         // 12
    { "Amp_EpiphoneElectarCentury",   "Epiphone\xC2" "\xAE" " Electar Century" },        // 13
    { "Amp_EpiphoneElectarM",         "Epiphone\xC2" "\xAE" " Electar Model M" },        // 14
    { "Amp_EpiphoneElectricHawaiian", "Epiphone\xC2" "\xAE" " Electric Hawaiian" },      // 15
    { "Amp_EpiphoneZephyr",           "Epiphone\xC2" "\xAE" " Zephyr" },                 // 16
    { "Amp_GB100",                    "GBC-1H" },                                        // 17
    { "Amp_GB38",                     "GBC-38" },                                        // 18
    { "Amp_GB50",                     "GBC-50" },                                        // 19
    { "Amp_GibsonGA79",               "Gibson\xC2" "\xAE" " GA-79" },                    // 20
    { "Amp_GibsonGA8",                "Gibson\xC2" "\xAE" " GA-8" },                     // 21
    { "Amp_GibsonGA88",               "Gibson\xC2" "\xAE" " GA-88" },                    // 22
    { "Amp_HG100",                    "HG-100" },                                        // 23
    { "Amp_HG180",                    "HG-180" },                                        // 24
    { "Amp_HG500",                    "HG-500" },                                        // 25
    { "Amp_Marshall1962Bluesbreaker", "Marshall 1962 \"Bluesbreaker\"" },                // 26
    { "Amp_MarshallDSL100H",          "Marshall DSL100H" },                              // 27
    { "Amp_MarshallDSL15H",           "Marshall DSL15H" },                               // 28
    { "Amp_MarshallJCM800",           "Marshall JCM800" },                               // 29
    { "Amp_MarshallJTM45",            "Marshall JTM45" },                                // 30
    { "Amp_MarshallJVM410H",          "Marshall JVM410H" },                              // 31
    { "Amp_MarshallPlexi",            "Marshall Plexi" },                                // 32
    { "Amp_OrangeAD50",               "Orange AD50" },                                   // 33
    { "Amp_OrangeJimmyBean",          "Orange 'Jimmy Bean' Amp" },                       // 34
    { "Amp_OrangeOR100",              "Orange OR100" },                                  // 35
    { "Amp_OrangeOR50H",              "Orange OR50H" },                                  // 36
    { "Amp_OrangeRockerverb",         "Orange Rockerverb 50 MkII" },                     // 37
    { "Amp_OrangeTinyTerror",         "Orange Tiny Terror" },                            // 38
    { "Amp_TW22",                     "AMTW-22" },                                       // 39
    { "Amp_TW26",                     "AMTW-26" },                                       // 40
    { "Amp_TW40",                     "AMTW-40" },                                       // 41
    { "Bass_Amp_BT600B",              "BH-600B" },                                       // 42
    { "Bass_Amp_BT880B",              "BH-880B" },                                       // 43
    { "Bass_Amp_BT975B",              "BH-975B" },                                       // 44
    { "Bass_Amp_CH300B",              "CLH-300B" },                                      // 45
    { "Bass_Amp_CH350B",              "CLH-350B" },                                      // 46
    { "Bass_Amp_CH600B",              "CLH-600B" },                                      // 47
    { "Bass_Amp_CS240B",              "CSS-240B" },                                      // 48
    { "Bass_Amp_CS300B",              "CSS-300B" },                                      // 49
    { "Bass_Amp_CS75B",               "CSS-75B" },                                       // 50
    { "Bass_Amp_EdenE300",            "Eden E300" },                                     // 51
    { "Bass_Amp_EdenWT550",           "Eden WT550" },                                    // 52
    { "Bass_Amp_EdenWT800",           "Eden WT800" },                                    // 53
    { "Bass_Amp_HT100B",              "HW100B" },                                        // 54
    { "Bass_Amp_HT300B",              "HW-300B" },                                       // 55
    { "Bass_Amp_HT400B",              "HW-400B" },                                       // 56
    { "Bass_Amp_LT25B",               "LW-25B" },                                        // 57
    { "Bass_Amp_LT85B",               "LW-85B" },                                        // 58
    { "Bass_Amp_OrangeAD200B",        "Orange AD200B" },                                 // 59
    { "DI_Amp_BassDriver",            "DI. Bass Driver" },                               // 60
    { "DI_Amp_MixerPre",              "DI. Mixer" },                                     // 61
    { "DI_Amp_TubePre",               "DI. Pre Tube" },                                  // 62
};

} // namespace rsamp
} // namespace dcdsp
