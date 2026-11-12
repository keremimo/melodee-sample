// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sfz.h"
#include <map>
#include <string>
#include <vector>

// A DecentSampler preset (.dspreset, roadmap P3) as the sampler's zones, read as
// an SFZ's are, after melodee-ios's reader and its maker's public developer
// guide: its <sample>s under <group>s under <groups>, each level's attributes
// over the one above. What has no place in the sampler is named and not played;
// a sample waiting on a tag turned off or a controller where it does not start
// is left out. A release sample plays on the key let go. The file's XML is read
// elsewhere (shell, with JUCE's XmlDocument) into the plain tree below.
namespace melodee {

struct DsElement {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::vector<DsElement> children;
};

// Its zones; `root` the document's top element (<DecentSampler>). `error` set when it is no preset.
SfzRead read_dspreset(const DsElement& root, std::string* error = nullptr);

// A level as the format writes one: linear, or dB with "dB" after it; in dB.
std::optional<double> dspreset_decibels(const std::string& text);

} // namespace melodee
