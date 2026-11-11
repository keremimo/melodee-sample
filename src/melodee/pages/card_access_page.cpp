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
    case CardNote::Writing: return {"WRITING", colour::amber};
    case CardNote::Empty: return {"EMPTY", colour::amber};
    case CardNote::Failed: return {"FAILED", colour::rec};
    default: return {"SD CARD", colour::mid};
    }
}

// A step of lending the card: done (a check), the one to do now (amber), or still to come (dim).
enum class Step { Done, Now, Later };
void step(Paint& p, float cx, float cy, Step state, Icon glyph, const juce::String& key, const juce::String& word) {
    auto c = state == Step::Done ? colour::play : state == Step::Now ? colour::amber : colour::dim;
    if (state == Step::Now) p.circle(cx, cy, 13, colour::amber.withAlpha(0.16f));
    p.g.setColour(c);
    p.g.drawEllipse(cx - 13, cy - 13, 26, 26, 1.6f);
    if (state == Step::Done) icon(p, Icon::Check, cx - 7, cy - 7, 14, c, 2);
    else if (key.isNotEmpty()) p.text(key, cx - 13, cy - 8, 26, 16, plain(c, type::small, Weight::Bold), juce::Justification::centred);
    else icon(p, glyph, cx - 7, cy - 7, 14, c, 1.7f);
    auto label = caps_text(state == Step::Now ? colour::text : state == Step::Done ? colour::sec : colour::mid);
    p.text(word, cx - 50, cy + 16, 100, 14, label, juce::Justification::centred);
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

    // Where the card is: the MPC on the left, the computer on the right, the link between.
    float y = zone::picture_y + 10, mid = zone::width / 2.0f;
    bool none = !away && card.note == CardNote::NoCard;
    juce::Colour here = away || none ? colour::dim : colour::text;
    juce::Colour there = lent && !card.ejected ? colour::text : colour::dim;
    icon(p, Icon::Card, mid - 116, y + 2, 38, lent && card.ejected ? colour::text : here, 1.3f);
    if (none) icon(p, Icon::Cross, mid - 104, y + 16, 14, colour::amber, 2); // no card in the slot
    p.text("MPC", mid - 147, y + 46, 100, 14, caps_text(lent && card.ejected ? colour::text : here), juce::Justification::centred);
    icon(p, Icon::Laptop, mid + 69, y - 6, 54, there, 1.25f);
    p.text("COMPUTER", mid + 46, y + 46, 100, 14, caps_text(there), juce::Justification::centred);
    float ly = y + 22, from = mid - 64, to = mid + 54;
    if (lent && card.ejected) { // let go: the link broken in the middle
        p.fill(from, ly - 1, (to - from) / 2 - 16, 2, colour::dim, 1);
        p.fill(mid - 5 + 16, ly - 1, (to - from) / 2 - 16 - 5, 2, colour::dim, 1);
        icon(p, Icon::Cross, mid - 11, ly - 6, 12, colour::dim, 2);
    } else if (lent || busy) { // held by the computer (amber), or on its way (dim)
        auto c = lent && card.connected ? colour::amber : colour::dim;
        for (float x = from; x < to; x += 8) p.circle(x, ly, 1.2f, c);
    }

    float sy = zone::picture_y + 104;
    if (lent) { // what to do, as three steps: plug in, eject on the computer, B1
        Step plug = card.connected ? Step::Done : Step::Now;
        Step eject = card.ejected ? Step::Done : card.connected ? Step::Now : Step::Later;
        Step back = card.ejected ? Step::Now : Step::Later;
        p.fill(mid - 96 + 15, sy - 0.75f, 96 - 30, 1.5f, colour::line);
        p.fill(mid + 15, sy - 0.75f, 96 - 30, 1.5f, colour::line);
        step(p, mid - 96, sy, plug, Icon::Usb, {}, "PLUG");
        step(p, mid, sy, eject, card.locked && !card.ejected ? Icon::Warn : Icon::Eject, {}, "EJECT");
        step(p, mid + 96, sy, back, Icon::None, "B1", "TAKE BACK");
    } else if (!busy && card.note != CardNote::None && !none) { // why it did not go: one glyph
        Icon why = card.note == CardNote::NoCard ? Icon::Card : card.note == CardNote::InUse ? Icon::Lock
                 : card.note == CardNote::Writing ? Icon::Rec : card.note == CardNote::Failed ? Icon::Warn : Icon::Folder;
        icon(p, why, mid - 12, sy - 12, 24, tone, 1.6f);
    }

    // The knobs' zone, while the card is away: what still answers, as glyphs.
    if (away) {
        auto still = caps_text(colour::dim);
        float w = p.width("STILL ON", still) + 9 + 13 + 9 + 11 + 9 + 10 + 9 + 13, x = mid - w / 2, cy = zone::knobs_y + 26;
        p.text("STILL ON", x, cy - 7, 80, 14, still);
        x += p.width("STILL ON", still) + 9;
        icon(p, Icon::Pads, x, cy - 6.5f, 13, colour::sec, 1.5f); x += 13 + 9;
        icon(p, Icon::Play, x, cy - 5.5f, 11, colour::sec); x += 11 + 9;
        icon(p, Icon::Stop, x, cy - 5, 10, colour::sec); x += 10 + 9;
        icon(p, Icon::Knob, x, cy - 6.5f, 13, colour::sec, 1.6f);
    }
}
} // namespace

// Soft keys, the wheel and Push are the shell's while the card owns the controls (shell/card_access.h).
const PageSpec card_access_page {draw, play_pad, no_wheel, no_push, [](Context&, int) {}, [](Context&, int) {}};

} // namespace melodee
