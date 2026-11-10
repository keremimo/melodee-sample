// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/note_repeat.h"
#include "melodee/engine/arp.h"
#include "melodee/shell/context.h"

namespace melodee {

void repeat_button(Context& c, bool pressed) {
    auto& ui = c.ui;
    auto& t = c.track();
    if (pressed) {
        if (ui.shift) ui.shift_used = true; // nothing printed for Shift + Note Repeat: it acts as Note Repeat
        if (ui.repeat_tap_at && c.now - ui.repeat_tap_at <= repeat_double_ms) { // a double tap: on, and its page
            ui.repeat_tap_at = 0; ui.repeat_held = false;
            if (!t.arp) c.set_arp(true);
            c.open(PageId::Arp);
            return;
        }
        bool was = t.arp;
        if (!was) c.set_arp(true);
        ui.repeat_held = true; ui.repeat_turned_on = !was; ui.repeat_down = c.now;
        return;
    }
    if (!ui.repeat_held) return; // the second press of a double tap
    ui.repeat_held = false;
    if (c.now - ui.repeat_down > repeat_hold_ms) { // held: only for as long
        if (ui.repeat_turned_on) c.set_arp(false);
        ui.repeat_tap_at = 0;
        return;
    }
    ui.repeat_tap_at = ui.repeat_down; // a tap: it stays as the press left it, or a tap while on turns it off
    if (!ui.repeat_turned_on) c.set_arp(false);
}

bool repeat_soft(Context& c, int key) {
    if (!repeat_keys(c.project, c.ui)) return false;
    auto& t = c.track();
    t.arp_rate = uint8_t(repeat_rate(key, c.ui.shift));
    c.send({Command::Type::Arp, c.ui.track, 0, 2, int16_t(t.arp_rate)});
    c.notice(std::string("Repeat ") + arp_rate_name(t.arp_rate));
    return true;
}

} // namespace melodee
