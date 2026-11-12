// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dspreset.h"
#include <string>

// A DecentSampler preset's XML read with JUCE's XmlDocument into the plain tree
// engine/dspreset.h reads (roadmap P3).
namespace melodee {

bool is_dspreset(const std::string& path);
// Its zones from the file's text; `error` set when it is no preset.
SfzRead read_dspreset_text(const std::string& text, std::string* error);

} // namespace melodee
