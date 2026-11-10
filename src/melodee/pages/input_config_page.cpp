// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// INPUT CONFIG (Shift + Sample, as printed; B2 on Record): what Sample
// Record and Audio Recall hear, as Akai's input_configuration page lists it.
// K1 source (Mic, Rear, Rear L / R, Resample, USB, USB L / R), K2 rec length
// (Free, Seq, Melodee's 1-16 bars), K3 threshold (-96 dB: at once; Shift +
// K3 Melodee's gain); B1 monitor (Off, Auto while Record is in view, On),
// B2 Rec Input FX, B3 back. Record's cards are the same ones, so the two
// pages always agree. The source is always routed, so the meter moves.
#include "melodee/pages/common.h"
#include "melodee/pages/input_meter.h"
#include "melodee/pages/pages.h"
#include "drivers/usb_audio.h"

namespace melodee {
namespace {
using namespace ui;

const char* detail_of(InputSource source) {
    switch (source) {
    case InputSource::Mic: return "The microphone by the wheel, in mono.";
    case InputSource::Rear: return "The rear inputs 1 / L and 2 / R, in stereo.";
    case InputSource::RearL: return "The rear input 1 / L, on both sides.";
    case InputSource::RearR: return "The rear input 2 / R, on both sides.";
    case InputSource::Resample: return "Melodee's own mix, before main volume.";
    case InputSource::Usb: return "What a computer plays over USB-C, in stereo.";
    case InputSource::UsbL: return "USB-C's left channel, on both sides.";
    case InputSource::UsbR: return "USB-C's right channel, on both sides.";
    default: return "";
    }
}

juce::String length_text(const Snapshot& s) {
    unsigned length = s.ui.settings.input.length;
    if (length == rec_free) return "FREE";
    if (length == rec_seq) return "SEQ";
    unsigned bars = rec_bars(length);
    return juce::String(bars) + (bars == 1 ? " BAR" : " BARS");
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    const auto& in = s.ui.settings.input;
    Header h = header_of(s);
    h.left.push_back(pill("INPUT CONFIG"));
    if (s.speaker_quieted) h.left.push_back(pill("SPEAKER OFF", colour::amber));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{juce::String("MON ") + juce::String(monitor_name(in.monitor)).toUpperCase(), in.fx ? "INPUT FX ON" : "INPUT FX OFF", "BACK"},
                 -1, c, {KeyIcon::None, KeyIcon::None, KeyIcon::Back}}, h);

    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y;
    p.text(input_source_name(in.source), x, y, w, 22, plain(colour::text, type::title, Weight::SemiBold));
    juce::String detail = detail_of(in.source);
    if (from_usb(in.source)) {
        auto usb = UsbStreamState(s.usb_state);
        detail = usb == UsbStreamState::Running ? juce::String("USB-C: the computer is playing.")
               : usb == UsbStreamState::Missing ? juce::String("USB-C: no audio gadget found.")
               : usb == UsbStreamState::Failed ? juce::String("USB-C: could not be opened.")
               : juce::String("USB-C: waiting for the computer to play.");
    }
    p.text(detail, x, y + 24, w, 14, plain(colour::sec, 10));

    struct Row { const char* name; juce::String value; };
    juce::String monitor = juce::String(monitor_name(in.monitor)).toUpperCase();
    if (in.source == InputSource::Resample) monitor += " (THE MIX IS HEARD)";
    else if (s.monitoring) monitor += juce::String::fromUTF8(" \xc2\xb7 HEARD NOW");
    const Row rows[] {
        {"SOURCE", juce::String(input_source_name(in.source)).toUpperCase()},
        {"MONITOR", monitor},
        {"THRESHOLD", in.at_once() ? juce::String("AT ONCE") : db_text(float(in.threshold))},
        {"REC LENGTH", length_text(s)},
        {"REC INPUT FX", in.fx ? juce::String::fromUTF8("ON \xc2\xb7 KNOB FX: PHASE 7") : juce::String("OFF")},
        {"GAIN", (in.gain > 0 ? "+" : "") + db_text(float(in.gain))},
    };
    float ry = y + 44;
    for (const auto& r : rows) {
        p.text(r.name, x, ry, 100, 11, caps_text(colour::mid, 7));
        p.text(r.value, x + 100, ry, w - 100, 11, caps_text(colour::text, 7), juce::Justification::centredRight);
        ry += 12;
    }
    input_meter(p, s, c, x, ry + 2, w);
}

void soft(Context& c, int key) {
    if (key == 0) c.cycle_monitor();
    if (key == 1) c.toggle_rec_fx();
    if (key == 2) c.back();
}
} // namespace

const PageSpec input_config_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
