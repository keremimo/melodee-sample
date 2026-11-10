// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/model/project_file.h"
#include <functional>
#include <string>

// Projects made in code (parity phase 10), the MPC's Demos: the one Melodee
// starts with and a few more, listed in the Project page's DEMOS folder and
// opened unsaved (Save asks a name). Load Kit takes a project's kit onto the
// selected track: its pads, their samples and effects, no patterns; a bank
// file takes one bank of it. Pure.
namespace melodee {

// The project Melodee starts with: four tracks with sounds and a bass line (demo 0).
void starting_project(Project& project, PatternBank& patterns);

constexpr unsigned demo_count = 3;
const char* demo_name(unsigned n);
void make_demo(unsigned n, Project& project, PatternBank& patterns);

// Load Kit: the track of `from` whose kit is taken (the first kit with a sample on a pad), -1 none.
int kit_track(const Project& from);
// That kit onto `into`, which becomes a kit track with its pads, their effects and the kit's own values
// (its filter, Follow, Cloud); its patterns, mix and track chain are its own still. `slot_for` gives this
// project's sample slot for a file (-1: none free, the pad stays empty).
struct KitImport { unsigned pads = 0, missing = 0; };
KitImport import_kit(const Project& from, unsigned track, Track& into, const std::function<int(const SampleRef&)>& slot_for);
// A bank file onto bank `bank` (0-7) of the kit `into`: its sixteen pads and their chains replace that
// bank's, their mutes cleared; the other banks, the kit's own values and its patterns stay.
KitImport import_bank(const BankFile& from, unsigned bank, Track& into, const std::function<int(const SampleRef&)>& slot_for);

} // namespace melodee
