// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// The card's files as the web page's Files tab sees them (web/web_app.h).
// A computer names a place by a path from the card's top ("" the top,
// "Samples/Kicks"); these turn it into a place on the card and refuse any
// that would leave it. Plain C++, no JUCE: the tests reach it.
namespace melodee::web {

// `rel` as a place under `card`: false with `error` when a part is empty,
// ".", "..", hidden (a leading dot) or holds a backslash, or when the place,
// its links followed, is not on the card or is in autorun/ (what the MPC
// starts) or sample-base/ (the base's own files), in any case.
bool card_place(const std::string& card, const std::string& rel, std::filesystem::path& out, std::string& error);
// Whether the page may change things at `rel`: not in autorun/ or sample-base/.
bool card_writable(const std::string& rel);
// A folder's entries as JSON: {"name","dir","size","time"} each, folders
// first, then by name ignoring case; hidden ones and System Volume
// Information left out, and at the card's top (`top`) autorun and sample-base.
std::string folder_json(const std::filesystem::path& folder, bool top = false);
// `name` in `folder`, else "<stem> 2<ext>", "<stem> 3<ext>"..., so nothing there is written over.
std::filesystem::path free_path(const std::filesystem::path& folder, const std::string& name);
// The type a file is sent as, by its extension: audio/wav, audio/mpeg..., else application/octet-stream.
std::string file_type(const std::string& name);

} // namespace melodee::web
