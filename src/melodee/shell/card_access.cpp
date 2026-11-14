// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/card_access.h"

namespace melodee {

namespace {
constexpr uint8_t home = 0, lending = 1, lent = 2, returning = 3; // CardShareState
} // namespace

CardPress card_press(UiState& ui, SurfaceButton button) {
    using B = SurfaceButton;
    CardPress press;
    if (button == B::Play || button == B::Stop) { press.taken = false; return press; } // the transport still answers
    if (ui.ask == UiState::Ask::TakeCardBack) {
        if (button == B::WheelPush) button = ui.ask_pick == 2 ? B::TopRight : B::TopLeft; // the key the wheel highlights
        if (button == B::TopLeft) ui.ask = UiState::Ask::None;                          // Cancel: it stays lent
        else if (button == B::TopRight) { ui.ask = UiState::Ask::None; press.request = CardRequest::Force; }
        return press;
    }
    if (button != B::TopLeft) return press; // every other mode is locked
    switch (ui.card.state) {
    case home: press.leave = true; break;   // nothing lent (no card, or it would not go): back to Project
    case lent:
        // The computer has not let go: B1 again asks whether to take it anyway.
        if (ui.card.locked && !ui.card.ejected) { ui.ask = UiState::Ask::TakeCardBack; ui.ask_pick = 0; }
        else press.request = CardRequest::Return;
        break;
    default: break;                          // lending or returning: wait
    }
    return press;
}

CardChange card_follow(UiState& ui, const CardView& now) {
    CardChange change;
    change.left = !card_away(ui.card) && card_away(now);
    change.back = now.returns != ui.card.returns && !card_away(now);
    if (!card_away(now) && ui.ask == UiState::Ask::TakeCardBack) ui.ask = UiState::Ask::None;
    ui.card = now;
    return change;
}

const char* card_title(const CardView& card) {
    switch (card.state) {
    case lending: return "Mounting the card";
    case returning: return "Ejecting the card";
    case lent:
        if (card.ejected) return "Ejected on the computer";
        return card.connected ? "Mounted on the computer" : "Connect USB-C to a computer";
    default: break;
    }
    switch (card.note) {
    case CardNote::NoCard: return "No card";
    case CardNote::InUse: return "The card is in use";
    case CardNote::Writing: return "A file is being written";
    case CardNote::Empty: return "Card ejected, nothing on it to open";
    case CardNote::Failed: return "The card could not mount";
    default: return "SD card access";
    }
}

const char* card_detail(const CardView& card) {
    switch (card.state) {
    case lending: return "Projects and samples wait until it is ejected.";
    case returning: return "Projects and samples are read again.";
    case lent:
        if (card.ejected) return "Press B1 to eject.";
        if (card.locked) return "Still mounted. Eject it there, or press B1 again.";
        return card.connected ? "Eject it on the computer, then press B1." : "The card mounts when a computer connects.";
    default: break;
    }
    switch (card.note) {
    case CardNote::NoCard: return "Put a microSD card in the slot.";
    case CardNote::InUse: return "Something has a file open on it; try again.";
    case CardNote::Writing: return "Wait for the recording or render to finish.";
    case CardNote::Empty: return "The computer may have formatted it.";
    case CardNote::Failed: return "The USB gadget would not mount it.";
    default: return "";
    }
}

} // namespace melodee
