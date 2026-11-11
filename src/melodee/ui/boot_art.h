// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// The boot animation's art as Base64 PNG, 320 x 240 each (boot_logo.cpp and boot_backdrop.cpp,
// written by tools/boot_art.py).
namespace melodee::ui {
extern const char boot_logo_png[];     // the logo, on transparency
extern const char boot_backdrop_png[]; // the backdrop that fades in behind it
}
