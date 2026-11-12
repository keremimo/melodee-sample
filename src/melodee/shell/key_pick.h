// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// Screens whose choices sit as tiles under B1-B3 (an empty track's three doors, the power sheet, a question,
// Project's SAVE, OPEN and NEW): the wheel moves the highlight from key to key, a detent a key, held at the
// ends, and Push presses the key highlighted. The keys still press their own tile at once.
namespace melodee {

// The key `delta` detents from `at`. `keys`: the keys that hold a choice, a bit each (B1 1, B2 2, B3 4);
// the others are stepped over.
constexpr uint8_t key_pick_turn(unsigned at, int delta, unsigned keys = 7) {
    int n = int(at % 3);
    for (int left = delta < 0 ? -delta : delta; left > 0; --left) {
        int next = n + (delta < 0 ? -1 : 1);
        while (next >= 0 && next < 3 && !(keys >> next & 1)) next += delta < 0 ? -1 : 1;
        if (next < 0 || next > 2) break;
        n = next;
    }
    return uint8_t(n);
}

} // namespace melodee
