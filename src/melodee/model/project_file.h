// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>
#include <string>
#include <vector>

// A project as a file: "MELODEE1", then tagged sections (four letters, a
// length, the contents). A reader skips sections it does not know and fills
// fields a section leaves out with their defaults, so files stay readable as
// the format grows. Only what differs from empty is written: empty steps and
// patterns take no room.
namespace melodee {

std::vector<uint8_t> save_project(const Project& project, const PatternBank& patterns);
// Empty on success; on failure the targets are left as they were.
std::string load_project(const std::vector<uint8_t>& data, Project& project, PatternBank& patterns);

} // namespace melodee
