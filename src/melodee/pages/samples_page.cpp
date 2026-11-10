// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SAMPLES (Sample Select on a sampler track): the WAV files in the card's
// Samples folder. The wheel scrolls, Push loads the file onto the last pad
// hit (the whole file; Trim or Chop shape it), Shift + Push goes back. K2
// jumps through the list. B1-B3 filter: all, recorded, recent (the files
// this project already holds).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/sample_library.h"
#include "melodee/ui/widgets.h"
#include <filesystem>
#include <vector>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = 7;

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    juce::String folder = s.sample_root.empty() ? juce::String("NO CARD")
        : juce::String(std::filesystem::path(s.sample_root).filename().string()).toUpperCase();
    h.left.push_back(pill(folder + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(s.files.size()), c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"ALL", "RECORDED", "RECENT"}, s.ui.files_filter, c}, h);
    std::vector<const SampleFile*> files;
    for (const auto& f : s.files) if (sample_file_listed(s.project, s.ui.files_filter, f.path)) files.push_back(&f);
    if (files.empty()) {
        juce::String why = s.sample_root.empty() ? "No Samples folder on the card"
            : s.ui.files_filter == 1 ? "Nothing recorded yet" : s.ui.files_filter == 2 ? "No samples in this project yet" : "No WAV files in Samples";
        p.text(why, zone::pad, zone::picture_y, zone::width - 2 * zone::pad, 40, plain(colour::dim, 11, Weight::Medium), juce::Justification::centred);
        return;
    }
    unsigned count = unsigned(files.size()), cursor = std::min<unsigned>(s.ui.files_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        const auto& file = *files[first + row];
        ListRow r;
        r.name = file.name; r.accent = c; r.current = first + row == cursor;
        // Which pads of this kit play it.
        for (unsigned n = 0; n < sample_slots && r.tag.isEmpty(); ++n)
            if (file.path == s.project.samples[n].path)
                for (unsigned k = 0; k < sampler_pads; ++k)
                    if (track.pads[k].sample == n) { char label[4]; pad_label(k, label); r.tag = juce::String("ON ") + juce::String(label); break; }
        p.g.saveState();
        list_row(p, zone::pad, zone::picture_y + 0.5f + row * (list_row_h + list_row_gap), zone::width - 2 * zone::pad, r);
        p.g.restoreState();
    }
}

void wheel(Context& c, int delta) {
    c.ui.files_cursor = uint16_t(std::clamp<int>(c.ui.files_cursor + delta, 0, std::max(int(c.ui.files_count) - 1, 0)));
}

void push(Context& c) {
    if (!c.library) return;
    std::vector<SampleFile> files;
    for (const auto& f : c.library->files()) if (c.files_match(f.path)) files.push_back(f);
    if (c.ui.files_cursor >= files.size()) return;
    const auto& file = files[c.ui.files_cursor];
    int slot = c.sample_slot(file.path, file.name);
    if (slot < 0) return; // the project's sample slots are full
    auto& pad = c.pad();
    pad = PadSound {};
    pad.sample = uint16_t(slot);
    c.send_pad(c.ui.track, c.track().last_pad);
}

void soft(Context& c, int key) {
    c.ui.files_filter = uint8_t(key); // RECENT: the files this project already uses
    c.ui.files_cursor = 0;
    unsigned listed = 0;
    if (c.library) for (const auto& f : c.library->files()) listed += c.files_match(f.path);
    c.ui.files_count = uint16_t(listed);
}
} // namespace

const PageSpec samples_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
