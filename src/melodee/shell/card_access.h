// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <atomic>
#include <cstdint>

// SD card access (parity phase 10b), as the MPC's Project > SD Card Access:
// the card goes to a computer over USB-C as a drive, and while it is there
// nothing else in Melodee can be used. B1 leaves, after the drive was
// ejected on the computer; if the computer still holds it, B1 again asks
// whether to take it back anyway. The base does the lending
// (drivers/card_share.h); USB audio, MIDI and the network keep working.
//
// Threads: the MIDI thread asks (CardLink::request), the service thread
// lends, takes back and copies what happened into the link, and the MIDI
// thread copies the link into UiState::card (card_follow()).
namespace melodee {

enum class CardRequest : uint8_t { None, Lend, Return, Force };

struct CardLink {
    std::atomic<uint8_t> request {0}; // CardRequest: MIDI thread asks, service thread takes
    std::atomic<uint8_t> state {0};   // CardShareState, service thread
    std::atomic<bool> connected {false}, ejected {false}, locked {false};
    std::atomic<uint8_t> note {0};    // CardNote, service thread
    std::atomic<uint64_t> returns {0};
};

// True while the card is not Melodee's: lending, lent or coming back.
inline bool card_away(const CardView& card) { return card.state != 0; }
// True while the page or the card's absence owns the controls.
inline bool card_owns_controls(const UiState& ui) { return ui.page == PageId::CardAccess || card_away(ui.card); }

// A button pressed while card_owns_controls(): what it asks of the service
// thread, whether Melodee should leave the page (B1 with the card home), and
// whether the press is taken (everything but Play and Stop).
struct CardPress { bool taken = true, leave = false; CardRequest request = CardRequest::None; };
CardPress card_press(UiState& ui, SurfaceButton button);

// What changed when the service thread's view (`now`) replaced ui.card:
// the card left (open the page), or came back (leave it, rescan done).
struct CardChange { bool left = false, back = false; };
CardChange card_follow(UiState& ui, const CardView& now);

// The page's lines: what is happening and what to do.
const char* card_title(const CardView& card);
const char* card_detail(const CardView& card);

} // namespace melodee
