// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/dspreset_file.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cctype>

namespace melodee {
namespace {
DsElement tree_of(const juce::XmlElement& element, unsigned depth) {
    DsElement made;
    made.name = element.getTagName().toStdString();
    for (int n = 0; n < element.getNumAttributes(); ++n)
        made.attributes[element.getAttributeName(n).toStdString()] = element.getAttributeValue(n).toStdString();
    if (depth < 16)
        for (auto* child : element.getChildIterator())
            if (!child->isTextElement()) made.children.push_back(tree_of(*child, depth + 1));
    return made;
}
} // namespace

bool is_dspreset(const std::string& path) {
    static const std::string tail = ".dspreset";
    if (path.size() < tail.size()) return false;
    return std::equal(tail.rbegin(), tail.rend(), path.rbegin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

SfzRead read_dspreset_text(const std::string& text, std::string* error) {
    juce::XmlDocument document(juce::String::fromUTF8(text.data(), int(text.size())));
    auto xml = document.getDocumentElement();
    if (!xml) { if (error) *error = "Unreadable"; return {}; }
    return read_dspreset(tree_of(*xml, 0), error);
}

} // namespace melodee
