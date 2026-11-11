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
// picture is the signal path as glyphs (source, gain, FX, monitor) over the
// level and its threshold. The source is always routed, so the meter moves.
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

    // The path, left to right: where it comes from, the gain, the effect on the way in, and whether it is heard.
    struct Node { Icon glyph; juce::String value, under; juce::Colour colour; };
    juce::String source = in.source == InputSource::Track ? "Track " + juce::String(in.track % track_count + 1) : juce::String(input_source_name(in.source));
    juce::String under, gain = db_text(float(in.gain));
    juce::Colour under_colour = colour::mid;
    if (in.source == InputSource::Track) under = in.post ? "POST FX" : "PRE FX"; // Shift + K1
    if (from_usb(in.source)) {
        auto usb = UsbStreamState(s.usb_state);
        under = usb == UsbStreamState::Running ? "PLAYING" : usb == UsbStreamState::Missing ? "NO GADGET" : usb == UsbStreamState::Failed ? "FAILED" : "WAITING";
        under_colour = usb == UsbStreamState::Running ? colour::play : usb == UsbStreamState::Failed || usb == UsbStreamState::Missing ? colour::rec : colour::amber;
    }
    juce::String fx = in.fx && s.project.knob_fx.kind ? juce::String(knob_fx_entry(s.project.knob_fx.choice).name) : juce::String("Off");
    bool heard = s.monitoring || in.source == InputSource::Resample;
    const Node nodes[] {
        {source_icon(in.source), source, under, colour::text},
        {Icon::Knob, gain, {}, colour::text},
        {Icon::Modulation, fx, {}, in.fx ? colour::text : colour::dim},
        {in.monitor == Monitor::Off ? Icon::SpeakerOff : Icon::Speaker, monitor_name(in.monitor), heard ? "HEARD" : "", heard ? colour::amber : colour::text},
    };
    float y = zone::picture_y + 8, step = (zone::width - 2 * zone::pad) / 4.0f;
    for (int n = 0; n < 4; ++n) {
        const auto& node = nodes[n];
        float cx = zone::pad + step * (n + 0.5f);
        icon(p, node.glyph, cx - 12, y, 24, node.colour, 1.4f);
        auto value = plain(node.colour, type::body, Weight::SemiBold);
        p.text(node.value, cx - step / 2 + 2, y + 30, step - 4, 16, p.fit(node.value, value, step - 4, type::floor), juce::Justification::centred);
        if (node.under.isNotEmpty())
            p.text(node.under, cx - step / 2, y + 47, step, 14, caps_text(n == 0 ? under_colour : node.colour), juce::Justification::centred);
        if (n < 3) { // the arrow on to the next node
            juce::Path arrow;
            float ax = cx + 18, bx = cx + step - 18, ay = y + 12;
            arrow.startNewSubPath(ax, ay); arrow.lineTo(bx, ay);
            arrow.startNewSubPath(bx - 4, ay - 4); arrow.lineTo(bx, ay); arrow.lineTo(bx - 4, ay + 4);
            p.g.setColour(colour::dim);
            p.g.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
    input_meter(p, s, c, zone::pad, zone::picture_y + 88, zone::width - 2 * zone::pad);
}

void soft(Context& c, int key) {
    if (key == 0) c.back();
    if (key == 1) c.cycle_monitor();
    if (key == 2) c.toggle_rec_fx();
}
} // namespace

const PageSpec input_config_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
