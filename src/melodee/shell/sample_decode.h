// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include <string>

namespace melodee {

// An instrument's audio file, whatever its kind, at the project's rate; empty on success.
std::string read_audio(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info, float play_at = 0); // play_at: read_wav's

} // namespace melodee
