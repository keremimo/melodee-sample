// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstddef>
#include <string>

// The web page's files, carried in the binary (build/gen/web_bundle.bin,
// made by tools/web_bundle.py from src/melodee/web/static/ and the user
// docs): the device page, the quickstart and the user guide as HTML, the
// images they show, the style sheet and the script. Rubik comes from the
// font the screen already uses (ui/rubik_data.h).
namespace melodee::web {

struct File {
    const char* data = nullptr;
    size_t size = 0;
    std::string type; // "text/html; charset=utf-8"
    explicit operator bool() const { return data != nullptr; }
};

// The file served at `path` ("/", "/docs/manual/basics", "/img/screens/stage.png",
// "/font/rubik.ttf"); empty when there is none.
File bundled(const std::string& path);

} // namespace melodee::web
