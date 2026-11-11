// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// INPUT CONFIG (Shift + Sample, as printed; B2 on Record): what Sample
// Record and Audio Recall hear, as Akai's input_configuration page lists it.
// K1 source (Mic, Rear, Rear L / R, Resample, USB, USB L / R, then each
// track with a sound: shell/input_choices.h; Shift + K1 its tap), K2 rec length
// (Free, Seq, Melodee's 1-16 bars), K3 threshold (-96 dB: at once; Shift +
// K3 Melodee's gain); B1 back, as on every sheet, B2 monitor (Off, Auto while
// Record is in view, On; its LED amber on Auto, green on On), B3 Rec Input FX
// (LED). Record's cards are the same ones, so the two pages always agree. The
// picture is the back panel: the sources as the sockets they are (the mic,
// the rear jacks, USB-C, then Melodee's own sound: Resample and a track),
// the one chosen glowing with its lead running into the stereo meter and
// its threshold. The source is always routed, so the meter moves.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/input_meter.h"
#include "melodee/pages/pages.h"
#include "drivers/usb_audio.h"

namespace melodee {

// The glyph of an input source (also Record's header).
ui::Icon source_icon(InputSource source) {
    switch (source) {
    case InputSource::Mic: return ui::Icon::Mic;
    case InputSource::Rear: case InputSource::RearL: case InputSource::RearR: return ui::Icon::Jack;
    case InputSource::Resample: return ui::Icon::Mpc;
    case InputSource::Usb: case InputSource::UsbL: case InputSource::UsbR: return ui::Icon::Usb;
    case InputSource::Track: return ui::Icon::Note;
    default: return ui::Icon::Jack;
    }
}

namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    const auto& in = s.ui.settings.input;
    Header h = header_of(s);
    h.left.push_back(pill("INPUT"));
    if (s.speaker_quieted) h.left.push_back(pill(Icon::SpeakerOff, "SPEAKER", colour::amber));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"BACK", "MONITOR", "INPUT FX"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.toggles = 2 | 4;
    keys.lit = uint8_t((in.monitor != Monitor::Off ? 2 : 0) | (in.fx ? 4 : 0));
    keys.leds[1] = in.monitor == Monitor::Auto ? colour::amber : colour::play;
    frame(p, s, keys, h);

    // The sockets, the one chosen glowing; which sides of it (L + R, L, R) over its lead into the meter.
    struct Socket { Icon glyph; const char* name; };
    static const Socket sockets[] {{Icon::Mic, "MIC"}, {Icon::Jack, "REAR"}, {Icon::Usb, "USB"}, {Icon::Loop, "RESAMPLE"}, {Icon::Note, "TRACK"}};
    int chosen = in.source == InputSource::Mic ? 0 : from_rear(in.source) ? 1 : from_usb(in.source) ? 2 : in.source == InputSource::Resample ? 3 : 4;
    float gy = zone::picture_y + 13, step = (zone::width - 2 * zone::pad) / 5.0f;
    for (int n = 0; n < 5; ++n) {
        float cx = zone::pad + step * (n + 0.5f);
        bool on = n == chosen;
        if (on) p.circle(cx, gy + 11, 18, c.withAlpha(0.2f));
        icon(p, sockets[n].glyph, cx - 11, gy, 22, on ? c : colour::sec, 1.4f);
        juce::String name = sockets[n].name;
        if (on && n == 4) name = "T" + juce::String(in.track % track_count + 1) + (in.post ? " POST" : " PRE"); // Shift + K1
        p.text(name, cx - step / 2 - 8, gy + 34, step + 16, 14, caps_text(on ? colour::text : colour::mid), juce::Justification::centred); // neighbours are short
        if (on && n == 2) { // USB-C's stream: its LED
            auto usb = UsbStreamState(s.usb_state);
            auto led = usb == UsbStreamState::Running ? colour::play : usb == UsbStreamState::Failed || usb == UsbStreamState::Missing ? colour::rec : colour::amber;
            p.circle(cx + 16, gy + 2, 3, led);
        }
    }
    float cx = zone::pad + step * (chosen + 0.5f), my = zone::picture_y + 97;
    juce::String sides = in.source == InputSource::RearL || in.source == InputSource::UsbL ? "L"
                       : in.source == InputSource::RearR || in.source == InputSource::UsbR ? "R"
                       : in.source == InputSource::Mic ? "MONO" : "L + R";
    p.text(sides, cx - 30, gy + 50, 60, 14, plain(colour::sec, type::small, Weight::SemiBold), juce::Justification::centred);
    juce::Path lead; // from the socket down into the meter's middle
    float mx = zone::width / 2.0f;
    lead.startNewSubPath(cx, gy + 66);
    lead.cubicTo(cx, gy + 80, mx, my - 18, mx, my);
    p.g.setColour(c);
    p.g.strokePath(lead, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    input_meter(p, s, c, zone::pad, my, zone::width - 2 * zone::pad);
}

void soft(Context& c, int key) {
    if (key == 0) c.back();
    if (key == 1) c.cycle_monitor();
    if (key == 2) c.toggle_rec_fx();
}
} // namespace

const PageSpec input_config_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
