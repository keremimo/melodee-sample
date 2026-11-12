// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sf2.h"
#include <memory>
#include <string>

// An .sf2 on the card mapped read only (roadmap P3), its lists read
// (engine/sf2.h): the browser lists its presets, a preset's zones are read and
// its sounds taken out as they land, all from one mapping kept while anything
// holds it. The last few opened are kept, so a kit of presets reads the file once.
namespace melodee {

struct Sf2Mapped {
    Sf2Mapped() = default;
    Sf2Mapped(const Sf2Mapped&) = delete;
    Sf2Mapped& operator=(const Sf2Mapped&) = delete;
    ~Sf2Mapped();
    void* map = nullptr;
    size_t size = 0;
    Sf2 sf2;
    std::string error; // why it could not be read; empty when it was
    // The preset at its place in the file; nullptr when there is none.
    const Sf2Preset* preset(unsigned index) const;
};

// The file mapped and read, or what went wrong in `error`. Any thread but audio.
std::shared_ptr<Sf2Mapped> open_sf2(const std::string& path);
// Lets go of the files kept (SD card access: nothing may hold the card); what still holds one keeps it.
void forget_sf2_files();

} // namespace melodee
