// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SAMPLES (Sample Select on a sampler track): the MPC's sample browser.
// Folders first, as categories, then the WAV files. The wheel and Minus /
// Plus move, Push opens a folder or loads the file onto the last pad hit
// (keeping the pad's settings; the whole file, Trim or Chop shape it), B1
// goes back out, B2 picks the drive (the card, or the internal one: the
// factory categories and the stock app's samples, read only), B3 hears each
// file as the cursor lands on it. Shift + pad loads the file onto the
// kit's next empty pad (here instead of the printed Shift + pad jobs). K2
// jumps. The pads play the kit.
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

juce::String folder_title(const Snapshot& s) {
    if (s.ui.files_drive == unsigned(Drive::Internal) && s.files_top) return "CATEGORIES";
    if (s.files_folder.empty()) return "NO CARD";
    return juce::String(std::filesystem::path(s.files_folder).filename().string()).toUpperCase();
}

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    Drive drive = Drive(s.ui.files_drive);
    h.left.push_back(chip(drive_name(drive), c));
    h.left.push_back(pill(folder_title(s) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(s.files.size()), c));
    h.right.push_back(tag_of(s, s.ui.track));
    juce::String other = drive == Drive::Card ? "INTERNAL" : "CARD";
    frame(p, s, {{s.files_top ? "" : "BACK", other, s.ui.preview ? "PREVIEW ON" : "PREVIEW"}, s.ui.preview ? 2 : -1, c,
                 {s.files_top ? KeyIcon::None : KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    if (s.files.empty()) {
        juce::String why = drive == Drive::Card && s.sample_root.empty() ? "No Samples folder on the card"
                         : drive == Drive::Internal && s.files_top ? "No internal samples" : "No folders or WAV files here";
        p.text(why, zone::pad, zone::picture_y, zone::width - 2 * zone::pad, 40, plain(colour::dim, 11, Weight::Medium), juce::Justification::centred);
        return;
    }
    unsigned count = unsigned(s.files.size()), cursor = std::min<unsigned>(s.ui.files_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    for (unsigned row = 0; row < visible && first + row < count; ++row) {
        const auto& file = s.files[first + row];
        ListRow r;
        r.name = file.name; r.accent = c; r.current = first + row == cursor;
        if (file.folder) r.tag = juce::String::fromUTF8("FOLDER \xe2\x80\xba");
        else // which pads of this kit play it
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
    int was = c.ui.files_cursor;
    c.ui.files_cursor = uint16_t(std::clamp<int>(c.ui.files_cursor + delta, 0, std::max(int(c.ui.files_count) - 1, 0)));
    if (c.ui.files_cursor != was) c.preview_cursor();
}

void push(Context& c) {
    if (!c.library) return;
    auto files = c.library->files();
    if (c.ui.files_cursor >= files.size()) return;
    const auto& file = files[c.ui.files_cursor];
    if (file.folder) c.library->browse(Drive(c.ui.files_drive), file.path);
    else c.load_file(file.path, file.name, false);
}

void soft(Context& c, int key) {
    if (!c.library) return;
    auto& ui = c.ui;
    if (key == 0) c.library->browse_up();
    if (key == 1) { ui.files_drive = uint8_t((ui.files_drive + 1) % unsigned(Drive::Count)); c.library->browse(Drive(ui.files_drive), ""); }
    if (key == 2) { ui.preview = !ui.preview; c.preview_cursor(); c.notice(ui.preview ? "Preview on" : "Preview off"); }
}
} // namespace

// Shift + pad on the browser: the file under the cursor onto the next empty pad.
bool samples_shift_pad(Context& c) {
    if (c.ui.page != PageId::Samples || !c.library || c.track().engine != EngineKind::Sampler) return false;
    auto files = c.library->files();
    if (c.ui.files_cursor >= files.size() || files[c.ui.files_cursor].folder) { c.notice("Shift + pad: a file first"); return true; }
    const auto& file = files[c.ui.files_cursor];
    c.load_file(file.path, file.name, true);
    return true;
}

const PageSpec samples_page {draw, play_pad, wheel, push, soft, wheel}; // Minus / Plus move the list

} // namespace melodee
