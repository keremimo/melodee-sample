// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/features.h"
#include "melodee/shell/ui_state.h"
#include <array>

// The mixer's channels (docs/MIXER_PLAN.md, Controls): Pad Bank steps between two rows. The first is the sixteen
// tracks, a pad each. The second holds the submixes on pads 1-8, the buses A-D on 9-12, the input on 15 and the
// master on 16 (13 and 14 hold nothing); Basic leaves the submixes out (decision 3). A channel is named by its strip
// number (model/project.h: tracks 0-15, buses 16-19, the master 20, the input 21, the submixes 22-29). Pure, for the
// page, the shell, the pad lights and the tests.
namespace melodee {

enum class ChannelKind : uint8_t { Track, Submix, Bus, Input, Master };

constexpr ChannelKind channel_kind(unsigned strip) {
    return strip < track_count ? ChannelKind::Track : strip < master_chain ? ChannelKind::Bus : strip == master_chain ? ChannelKind::Master
         : strip == strip_input ? ChannelKind::Input : ChannelKind::Submix;
}
constexpr unsigned channel_bus(unsigned strip) { return (strip - bus_chain(0)) % bus_count; }
constexpr unsigned channel_sub(unsigned strip) { return (strip - strip_sub(0)) % sub_count; }

// The second row, left to right: S1-S8, A-D, IN, MST (as the pads hold them).
constexpr unsigned other_count = sub_count + bus_count + 2;
constexpr std::array<uint8_t, other_count> other_row {
    uint8_t(strip_sub(0)), uint8_t(strip_sub(1)), uint8_t(strip_sub(2)), uint8_t(strip_sub(3)),
    uint8_t(strip_sub(4)), uint8_t(strip_sub(5)), uint8_t(strip_sub(6)), uint8_t(strip_sub(7)),
    uint8_t(bus_chain(0)), uint8_t(bus_chain(1)), uint8_t(bus_chain(2)), uint8_t(bus_chain(3)),
    uint8_t(strip_input), uint8_t(master_chain)};
static_assert(sub_count == 8 && bus_count == 4, "the second row's pads");
// Where Basic's second row starts: no submixes.
inline unsigned other_first(const UiState& ui) { return full_features(ui) ? 0 : sub_count; }

// The pad (0-15) of a channel in the second row, or -1 (a track).
constexpr int other_pad(unsigned strip) {
    switch (channel_kind(strip)) {
    case ChannelKind::Submix: return int(channel_sub(strip));
    case ChannelKind::Bus: return int(sub_count + channel_bus(strip));
    case ChannelKind::Input: return 14;
    case ChannelKind::Master: return 15;
    default: return -1;
    }
}
// The channel a pad holds in the second row, or -1 (pads 13 and 14; the submixes in Basic).
inline int pad_other(const UiState& ui, unsigned pad) {
    pad %= 16;
    if (pad < sub_count) return full_features(ui) ? int(strip_sub(pad)) : -1;
    if (pad < sub_count + bus_count) return int(bus_chain(pad - sub_count));
    if (pad == 14) return int(strip_input);
    if (pad == 15) return int(master_chain);
    return -1;
}
// The place in other_row of a strip (0 when it is none of them).
constexpr unsigned other_place(unsigned strip) {
    for (unsigned n = 0; n < other_count; ++n) if (other_row[n] == strip) return n;
    return 0;
}

// The channel in hand: the selected track, or the second row's.
inline unsigned mixer_strip(const UiState& ui) {
    if (ui.mixer_bank == 0) return ui.track % track_count;
    return other_row[std::max<unsigned>(ui.mixer_other % other_count, other_first(ui))];
}
// How many channels the row in view holds, and the place of the one in hand in it.
inline unsigned mixer_row_count(const UiState& ui) { return ui.mixer_bank == 0 ? track_count : other_count - other_first(ui); }
inline unsigned mixer_row_at(const UiState& ui) {
    return ui.mixer_bank == 0 ? ui.track % track_count : other_place(mixer_strip(ui)) - other_first(ui);
}
// The strip at place `n` of the row in view.
inline unsigned mixer_row_strip(const UiState& ui, unsigned n) {
    return ui.mixer_bank == 0 ? n % track_count : other_row[(other_first(ui) + n) % other_count];
}
// The second row's channel `strip` in hand (the first row's go through select_track: the selected track).
inline void pick_other(UiState& ui, unsigned strip) { ui.mixer_bank = 1; ui.mixer_other = uint8_t(other_place(strip)); }

// What a channel has (the plan's table): sends, pan and an output (tracks; submixes play to the master), mute, solo.
constexpr bool channel_sends(ChannelKind k) { return k == ChannelKind::Track || k == ChannelKind::Submix; }
constexpr bool channel_mutes(ChannelKind k) { return k == ChannelKind::Track || k == ChannelKind::Submix || k == ChannelKind::Bus; }
constexpr bool channel_solos(ChannelKind k) { return k == ChannelKind::Track || k == ChannelKind::Submix; }
// Every channel has a COMP; the master's is the printed Compressor (Shift + Pad 5), never its strip's.
constexpr bool channel_comp(ChannelKind) { return true; }
// A channel's effect chain by its number (model/project.h), or -1: the input has none.
constexpr int channel_chain(unsigned strip) {
    switch (channel_kind(strip)) {
    case ChannelKind::Track: return int(strip % track_count);
    case ChannelKind::Bus: return int(bus_chain(channel_bus(strip)));
    case ChannelKind::Master: return int(master_chain);
    case ChannelKind::Submix: return int(sub_chain(channel_sub(strip)));
    default: return -1;
    }
}
// B1 on a channel: a track or submix has two pages (SENDS, FX); a bus or the master only FX; the input INPUT (it has
// no chain).
constexpr bool channel_fx(ChannelKind k) { return k != ChannelKind::Input; }

// The mixer's blocks: what the card shows and K1-K3 turn.
enum : unsigned { block_sends = 0, block_eq = 1, block_comp = 2, block_fx = 3, block_input = 4 };
// The block for the channel in hand: B1 lit is SENDS or FX (as `mixer_b1` left it; a bus or the master FX alone, the
// input INPUT), B2 EQ, B3 COMP. Basic: SENDS only.
inline unsigned mixer_block_of(const UiState& ui, ChannelKind k) {
    if (!full_features(ui)) return block_sends;
    const unsigned b = ui.mixer_block % 3;
    if (b == 0) {
        if (channel_sends(k)) return ui.mixer_b1 ? block_fx : block_sends;
        return channel_fx(k) ? block_fx : block_input;
    }
    return b;
}
// The master's COMP lit: the printed Compressor's cards and picture.
inline bool mixer_master_comp(const UiState& ui) {
    return ui.page == PageId::Mixer && mixer_strip(ui) == master_chain && mixer_block_of(ui, ChannelKind::Master) == block_comp;
}
// The FX block lit, on the channel in hand.
inline bool mixer_fx_lit(const UiState& ui) { return ui.page == PageId::Mixer && mixer_block_of(ui, channel_kind(mixer_strip(ui))) == block_fx; }

// The mixer's Minus / Plus: a tap moves through the lit block as it is let go; held mixer_view_hold_ms, Plus shows the
// channel in hand's card and Minus every channel of the row as strips (UiState::mixer_card); the release does no more.
constexpr uint32_t mixer_view_hold_ms = 400;
struct MixerViewHold {
    bool down = false, fired = false;
    int way = 0; // −1 Minus, +1 Plus
    uint32_t at = 0;
};
inline void mixer_view_press(MixerViewHold& h, int way, uint32_t now) { h = MixerViewHold {true, false, way, now}; }
// How far the mixer is drawn toward the card at `now` (0 the strips, 1 the card): on the way, an ease-out over
// mixer_view_ms from where it stood when Plus or Minus turned it.
constexpr uint32_t mixer_view_ms = 180;
inline float mixer_card_shown(const UiState& ui, uint32_t now) {
    const float to = ui.mixer_card ? 1.0f : 0.0f;
    const uint32_t gone = now - ui.mixer_card_at;
    if (!ui.mixer_card_at || gone >= mixer_view_ms) return to;
    const float t = 1 - float(gone) / float(mixer_view_ms);
    return to + (ui.mixer_card_from - to) * t * t * t; // 1 - (1 - x)^3 of the way
}
// Each pass while it is down: the card or the strips once it has been held long enough. Off the mixer (a page opened
// while it was down), it is let go.
inline void mixer_view_tick(MixerViewHold& h, UiState& ui, uint32_t now) {
    if (h.down && ui.page != PageId::Mixer) h.down = false;
    if (!h.down || h.fired || now - h.at < mixer_view_hold_ms) return;
    h.fired = true;
    if (const bool card = h.way > 0; card != ui.mixer_card) {
        ui.mixer_card_from = mixer_card_shown(ui, now);
        ui.mixer_card = card;
        ui.mixer_card_at = now ? now : 1;
    }
}
// Its button let go: the way to move when it was a tap, else 0.
inline int mixer_view_release(MixerViewHold& h) {
    const bool tap = h.down && !h.fired;
    h.down = false;
    return tap ? h.way : 0;
}

} // namespace melodee
