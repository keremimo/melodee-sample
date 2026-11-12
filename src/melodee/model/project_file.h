// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/print.h"
#include "melodee/model/project.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A project as a file: "MELODEE1", then tagged sections (four letters, a
// length, the contents). A reader skips sections it does not know and fills
// fields a section leaves out with their defaults, so files stay readable as
// the format grows. Only what differs from empty is written: empty steps and
// patterns take no room.
namespace melodee {

// `states`: each plug-in's state (plugin/plugin_rack.h), saved with its entry in PLUG; none: the paths alone.
// `prints`: the project's prints (model/print.h), a PRNT section each.
std::vector<uint8_t> save_project(const Project& project, const PatternBank& patterns,
                                  const std::array<std::vector<uint8_t>, max_plugins>* states = nullptr, const PrintTable* prints = nullptr);
// Empty on success; on failure the targets are left as they were.
// `version`: the file's params_version (limits.h), 0 when it predates it. `states`: the plug-ins' states, when wanted.
// `prints`: the project's prints, when wanted (none in a file from before them).
std::string load_project(const std::vector<uint8_t>& data, Project& project, PatternBank& patterns, unsigned* version = nullptr,
                         std::array<std::vector<uint8_t>, max_plugins>* states = nullptr, PrintTable* prints = nullptr);

// A kit bank as a file (".mldb", in the card's Samples/Banks): sixteen pads,
// their chains and the files they play, so one bank of a kit loads into any
// bank of another. The files are named by path; nothing is copied.
constexpr unsigned bank_pads = 16;
struct BankFile {
    char name[name_length] {};
    std::array<PadSound, bank_pads> pads {};      // .sample: an entry of `samples`, or no_sample
    std::array<Chain, bank_pads> chains;          // each pad's own (PadFX)
    // The files: the pads' own first (at most one a pad), then their layers' (roadmap P3, BLSM).
    std::array<SampleRef, bank_pads * pad_layers> samples {};
    BankFile() { for (auto& c : chains) c.size = pad_slots; }
    unsigned used() const;                        // pads with a sound
};
std::vector<uint8_t> save_bank(const BankFile& bank);
bool load_bank(const std::vector<uint8_t>& data, BankFile& out); // false: not a bank file, or damaged
// Bank `bank` (0-7, A-H) of track `track`'s kit as a bank file named `name`.
BankFile bank_of(const Project& project, unsigned track, unsigned bank, const char* name);

} // namespace melodee
