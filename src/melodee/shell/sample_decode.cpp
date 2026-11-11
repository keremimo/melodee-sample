// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_decode.h"

namespace melodee {

std::string read_audio(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info) {
    return read_wav(path, out, max_seconds, info);
}

} // namespace melodee
