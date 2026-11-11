// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SD CARD ACCESS (Project, at the top; parity phase 10b), as the MPC's: the
// card on a computer over USB-C as a drive. The picture shows where the card
// is (the MPC or the computer) and what to do; B1 leaves once the computer
// has ejected it, and asks first if the computer still holds it. Meanwhile
// every other mode waits; the pads play the selected track and Play / Stop
// still work (shell/card_access.h).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/card_access.h"

namespace melodee {
namespace {
using namespace ui;

// The card: a rounded body with the cut corner and its contacts.
void card_glyph(Paint& p, float x, float y, float w, float h, juce::Colour c, bool solid) {
    juce::Path body;
    float cut = w * 0.28f;
    body.startNewSubPath(x + cut, y);
    body.lineTo(x + w, y); body.lineTo(x + w, y + h); body.lineTo(x, y + h); body.lineTo(x, y + cut);
    body.closeSubPath();
    p.g.setColour(solid ? c.withAlpha(0.18f) : colour::surf); p.g.fillPath(body);
    p.g.setColour(c); p.g.strokePath(body, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved));
    for (int k = 0; k < 4; ++k) p.fill(x + cut + 1 + k * (w - cut - 4) / 4, y + 4, 2, 7, c, 1);
}

// A laptop: the screen and its base.
void computer_glyph(Paint& p, float cx, float y, juce::Colour c, bool solid) {
    float w = 48, h = 32;
    p.fill(cx - w / 2, y, w, h, solid ? c.withAlpha(0.18f) : colour::surf, 3);
    p.ring(cx - w / 2, y, w, h, c, 3, 1.5f);
    p.fill(cx - w / 2 - 6, y + h + 3, w + 12, 3, c, 1.5f);
}

void draw(Paint& p, const Snapshot& s) {
    const auto& card = s.ui.card;
    bool lent = card.state == 2, busy = card.state == 1 || card.state == 3, away = card.state != 0;
    SoftKeys keys = lent ? SoftKeys {{"LEAVE", "", ""}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}
                  : busy ? SoftKeys {{"", "", ""}}
                         : SoftKeys {{"BACK", "", ""}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    Header h = header_of(s);
    h.left.push_back(pill("SD CARD ACCESS"));
    if (lent) h.right.push_back(pill(card.connected ? "USB-C" : "NO COMPUTER", card.connected ? colour::text : colour::mid));
    frame(p, s, keys, h);

    // Where the card is: on the MPC (left) or on the computer (right), and the cable between.
    float y = zone::picture_y + 14, mid = zone::width / 2.0f;
    bool none = !away && card.note == CardNote::NoCard;
    juce::Colour here = away || none ? colour::dim : colour::text, there = lent ? colour::text : colour::dim;
    p.text("MPC", mid - 120, y + 46, 80, 14, caps_text(here), juce::Justification::centred);
    card_glyph(p, mid - 96, y, 32, 40, here, !away && !none);
    computer_glyph(p, mid + 80, y + 2, there, lent);
    p.text("COMPUTER", mid + 40, y + 46, 80, 14, caps_text(there), juce::Justification::centred);
    // The cable: dotted while it is moving, solid once lent.
    float cy = y + 20;
    for (float x = mid - 54; x < mid + 46; x += busy ? 7 : 1) p.fill(x, cy - 0.75f, busy ? 3.5f : 1.5f, 1.5f, lent ? colour::text : colour::dim);
    if (lent && card.ejected) p.fill(mid - 8, cy - 7, 16, 14, colour::bg); // ejected: the cable broken in the middle

    // What is happening and what to do.
    float ty = zone::picture_y + 80, x = zone::pad + 10, w = zone::width - 2 * zone::pad - 20;
    p.text(card_title(card), x, ty, w, 20, plain(colour::text, 15, Weight::Medium), juce::Justification::centred);
    juce::Colour detail = card.locked && !card.ejected ? colour::amber : colour::sec;
    auto detail_style = plain(detail, type::small);
    auto lines = p.wrap(card_detail(card), detail_style, w, 3);
    for (int l = 0; l < lines.size(); ++l) p.text(lines[l], x, ty + 22 + l * 14, w, 14, detail_style, juce::Justification::centred);

    // The knobs' zone: why nothing else answers.
    if (away) {
        auto note = plain(colour::dim, type::small);
        auto note_lines = p.wrap("Other modes wait while the card is away. The pads still play.", note, zone::width - 2 * zone::pad, 2);
        for (int l = 0; l < note_lines.size(); ++l)
            p.text(note_lines[l], zone::pad, zone::knobs_y + 12 + l * 14, zone::width - 2 * zone::pad, 14, note, juce::Justification::centred);
    }
}
} // namespace

// Soft keys, the wheel and Push are the shell's while the card owns the controls (shell/card_access.h).
const PageSpec card_access_page {draw, play_pad, no_wheel, no_push, [](Context&, int) {}, [](Context&, int) {}};

} // namespace melodee
