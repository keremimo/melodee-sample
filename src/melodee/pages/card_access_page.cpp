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

// The state in two words at most, for the header chip, in its colour.
std::pair<juce::String, juce::Colour> state_word(const CardView& card) {
    switch (card.state) {
    case 1: return {"LENDING", colour::mid};
    case 3: return {"TAKING BACK", colour::mid};
    case 2:
        if (card.ejected) return {"EJECTED", colour::play};
        if (card.locked) return {"STILL HELD", colour::amber};
        return {card.connected ? "CARD LENT" : "NO COMPUTER", card.connected ? colour::amber : colour::mid};
    default: break;
    }
    switch (card.note) {
    case CardNote::NoCard: return {"NO CARD", colour::amber};
    case CardNote::InUse: return {"IN USE", colour::amber};
    case CardNote::Camera: return {"CAMERA IN USE", colour::amber};
    case CardNote::Writing: return {"WRITING", colour::amber};
    case CardNote::Empty: return {"EMPTY", colour::amber};
    case CardNote::Failed: return {"FAILED", colour::rec};
    default: return {"SD CARD", colour::mid};
    }
}

// A step of lending the card, on the link: done (a check), the one to do now (amber, the only one with a word), or
// still to come (dim).
enum class Step { Done, Now, Later };
void step(Paint& p, float cx, float cy, Step state, Icon glyph, const juce::String& key, const juce::String& word) {
    auto c = state == Step::Done ? colour::play : state == Step::Now ? colour::amber : colour::dim;
    p.circle(cx, cy, 13, colour::bg);
    if (state == Step::Now) p.circle(cx, cy, 13, colour::amber.withAlpha(0.18f));
    p.g.setColour(c);
    p.g.drawEllipse(cx - 13, cy - 13, 26, 26, 1.6f);
    if (state == Step::Done) icon(p, Icon::Check, cx - 7, cy - 7, 14, c, 2);
    else if (key.isNotEmpty()) p.text(key, cx - 13, cy - 8, 26, 16, plain(c, type::small, Weight::Bold), juce::Justification::centred);
    else icon(p, glyph, cx - 7, cy - 7, 14, c, 1.7f);
    if (state == Step::Now) p.text(word, cx - 50, cy + 22, 100, 14, caps_text(colour::text), juce::Justification::centred);
}

void draw(Paint& p, const Snapshot& s) {
    const auto& card = s.ui.card;
    bool lent = card.state == 2, busy = card.state == 1 || card.state == 3, away = card.state != 0;
    SoftKeys keys {{"", "", ""}};
    if (lent) { // B1 takes it back: lit once the computer has let it go
        keys = SoftKeys {{"TAKE BACK", "", ""}, card.ejected ? 0 : -1, colour::play};
        keys.glyphs[0] = Icon::Card;
    } else if (!busy) {
        keys = SoftKeys {{"BACK", "", ""}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    }
    Header h = header_of(s);
    auto [word, tone] = state_word(card);
    h.left.push_back(pill(lent && card.ejected ? Icon::Check : lent ? Icon::Laptop : Icon::Card, word, tone));
    if (lent && card.connected) h.right.push_back(pill(Icon::Usb, "USB-C", colour::sec));
    frame(p, s, keys, h);

    // Where the card is: the MPC on the left, the computer on the right, the link between with the steps on it.
    constexpr float ly = zone::picture_y + 56, from = 70, to = 244;
    bool none = !away && card.note == CardNote::NoCard;
    juce::Colour here = away || none ? colour::dim : colour::text;
    juce::Colour there = lent && !card.ejected ? colour::text : colour::dim;
    auto mpc = lent && card.ejected ? colour::text : here;
    icon(p, Icon::Card, 20, ly - 22, 40, mpc, 1.3f);
    if (none) icon(p, Icon::Cross, 33, ly - 8, 14, colour::amber, 2); // no card in the slot
    p.text("MPC", 0, ly + 26, 80, 14, caps_text(mpc), juce::Justification::centred);
    icon(p, Icon::Laptop, 252, ly - 26, 50, there, 1.25f);
    p.text("COMPUTER", 237, ly + 26, 80, 14, caps_text(there), juce::Justification::centred);
    auto link = lent && card.connected && !card.ejected ? colour::amber : colour::dim;
    if (lent || busy) for (float x = from; x <= to; x += 8) p.circle(x, ly, 1.2f, link);
    if (lent) { // what to do, as three steps on the link: plug in, eject on the computer, B1
        Step plug = card.connected ? Step::Done : Step::Now;
        Step eject = card.ejected ? Step::Done : card.connected ? Step::Now : Step::Later;
        Step back = card.ejected ? Step::Now : Step::Later;
        step(p, 108, ly, plug, Icon::Usb, {}, "PLUG IN");
        step(p, 157, ly, eject, card.locked && !card.ejected ? Icon::Warn : Icon::Eject, {}, "EJECT");
        step(p, 206, ly, back, Icon::None, "B1", "TAKE BACK");
    } else if (!busy && card.note != CardNote::None && !none) { // why it did not go: one glyph, on the link
        Icon why = card.note == CardNote::NoCard ? Icon::Card : card.note == CardNote::InUse ? Icon::Lock : card.note == CardNote::Camera ? Icon::Laptop
                 : card.note == CardNote::Writing ? Icon::Rec : card.note == CardNote::Failed ? Icon::Warn : Icon::Folder;
        icon(p, why, 157 - 12, ly - 12, 24, tone, 1.6f);
    }

    // While the card is away: what still answers, as four dim glyphs in the corner.
    if (away) {
        float x = zone::width - zone::pad - 12, cy = zone::picture_y + zone::picture_h - 12;
        icon(p, Icon::Knob, x, cy - 6, 12, colour::mid, 1.5f); x -= 10 + 8;
        icon(p, Icon::Stop, x, cy - 4.5f, 9, colour::mid); x -= 10 + 8;
        icon(p, Icon::Play, x, cy - 5, 10, colour::mid); x -= 12 + 8;
        icon(p, Icon::Pads, x, cy - 6, 12, colour::mid, 1.4f);
    }
}
} // namespace

// Soft keys, the wheel and Push are the shell's while the card owns the controls (shell/card_access.h).
const PageSpec card_access_page {draw, play_pad, no_wheel, no_push, [](Context&, int) {}, [](Context&, int) {}};

} // namespace melodee
