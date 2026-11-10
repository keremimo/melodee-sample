// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// INPUT CONFIG (Shift + Sample, as printed; B2 on Record): where Record
// takes its sound from and how loud it must get before a take starts.
// B1-B3 pick the source: the built-in mic, the line inputs, or the mix
// itself (resample). K1 threshold (-60 dB: at once). The input is heard
// while this page is open, so the meter moves. Monitor, record length and
// input effects join in phase 6 (MELODEE_PARITY.md).
#include "melodee/pages/common.h"
#include "melodee/pages/input_meter.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(pill("INPUT CONFIG"));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"MIC", "LINE", "RESAMPLE"}, s.ui.rec_source, c}, h);

    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y;
    static const char* const titles[] {"Built-in mic", "Line inputs", "Resample"};
    static const char* const details[] {
        "The microphone on the back of the MPC.",
        "The rear line inputs, left and right.",
        "Melodee's own mix, before main volume.",
    };
    unsigned source = s.ui.rec_source % 3;
    p.text(titles[source], x, y, w, 22, plain(colour::text, type::title, Weight::SemiBold));
    p.text(details[source], x, y + 24, w, 14, plain(colour::sec, 10));

    // Settings in rows, with what is still to come dim.
    struct Row { const char* name; juce::String value; bool here; };
    const Row rows[] {
        {"SOURCE", input_source_name(source), true},
        {"THRESHOLD", s.ui.rec_threshold > -60 ? db_text(float(s.ui.rec_threshold)) : juce::String("AT ONCE"), true},
        {"MONITOR", "LATER", false},
        {"REC LENGTH", "ON RECORD", false},
    };
    float ry = y + 46;
    for (const auto& r : rows) {
        p.text(r.name, x, ry, 100, 11, caps_text(r.here ? colour::mid : colour::dim, 7));
        p.text(r.value, x + 100, ry, w - 100, 11, caps_text(r.here ? colour::text : colour::dim, 7), juce::Justification::centredRight);
        ry += 14;
    }
    input_meter(p, s, c, x, ry + 6, w);
}

void soft(Context& c, int key) {
    if (c.host_recording()) { c.notice("Source stays while recording"); return; }
    c.set_rec_source(unsigned(key));
}
} // namespace

const PageSpec input_config_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
