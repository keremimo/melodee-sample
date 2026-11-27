// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SAMPLES (Sample Select on a sampler track): the MPC's sample browser.
// MPC drum programs (KIT) first, then instruments (XPM, XTY, SFZ), then folders, as categories, then the WAV files. The wheel and Minus /
// Plus move, Push opens a folder or loads the file onto the last pad hit
// (keeping the pad's settings; the whole file, Trim or Chop shape it), or a
// bank file (BANK, Samples/Banks) into the kit's bank in view, or an MPC drum
// program (KIT) into the kit: the LOAD KIT sheet asks which of its filled
// banks go into which of the track's banks (LOAD), or all of it as the
// track's kit, as Load Kit does (WHOLE KIT); or installs an MPC
// expansion (.xpn, INSTALL) onto the card and opens its folder; Shift +
// Sample Select here saves that bank as a bank file (Save Bank). B1
// goes back out, B2 picks the drive (the card, or the internal one: the
// factory categories and the stock app's samples, read only), B3 hears each
// file as the cursor lands on it. Shift + pad loads the file onto the
// kit's next empty pad (here instead of the printed Shift + pad jobs). K1
// jumps. On an audio file K2 is its START and K3 its END: the first turn
// raises the trim sheet over the list (shell/browse_trim.h), the whole file
// and a closer view round the end turned last; the wheel moves that end
// finer, Minus / Plus go on to the next file, Push and Shift + pad load it
// with START and END on the pad (the file on the card is not changed), B1
// lowers it, Erase puts both ends back (Erase + K2 / K3: that one). The pads
// play the kit. Shift + B3 stars the file under the cursor;
// the card's Samples folder and its top lead with STARRED and RECENT, which
// Push opens as folders (shell/sample_lists.h). A file loaded here goes first
// on RECENT.
#include "melodee/shell/smf_jobs.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/engine/dsp.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sample_lists.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/browse_trim.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/waveform.h"
#include "melodee/ui/widgets.h"
#include <cstdio>
#include <filesystem>
#include <vector>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned visible = list_rows;

juce::String folder_title(const Snapshot& s) {
    if (s.files_folder == starred_list) return "STARRED";
    if (s.files_folder == recent_list) return "RECENT";
    if (s.ui.files_drive == unsigned(Drive::Internal) && s.files_top) return "CATEGORIES";
    if (s.files_folder.empty()) return "NO CARD";
    return juce::String(std::filesystem::path(s.files_folder).filename().string());
}

// What Shift + B3 can star: a file (a sample, an instrument, a kit, a bank, a MIDI file), not a folder, a list, an
// expansion or a preset inside an .sf2. A '#' in a file's own name ("KeysPh G#m90.wav") is no preset (shell/sample_lists.h).
bool starrable(const SampleFile& f) { return !f.folder && !f.list && !f.expansion && !part_of_file(f.path); }

// The trim sheet's rise (0 down .. 1 up), eased out over sheet_rise_ms from when it opened.
constexpr uint32_t sheet_rise_ms = 180;
float sheet_rise(const Snapshot& s) {
    if (!s.ui.browse.up()) return 0;
    const int32_t gone = int32_t(s.now - s.ui.browse.opened_at);
    const float t = std::clamp(float(gone) / float(sheet_rise_ms), 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

void draw_list(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    Drive drive = Drive(s.ui.files_drive);
    auto drive_chip = chip(drive_name(drive), c);
    drive_chip.spare = true; // a long folder name keeps its room: B2 names the other drive
    h.left.push_back(drive_chip);
    h.left.push_back(pill(folder_title(s) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(s.files.size()), c));
    if (track.engine == EngineKind::Sampler) { // a kit: the pad Push loads onto, the last one hit
        char label[8];
        pad_label(track.last_pad % sampler_pads, label);
        h.right.push_back(pill(Icon::ArrowRight, juce::String(label), c)); // an arrow: the rows' pad glyph marks files already on a pad
    }
    h.right.push_back(tag_of(s, s.ui.track));
    juce::String other = drive == Drive::Card ? "INTERNAL" : "CARD";
    // The file under the cursor as it previews (mock 7): its row fills as it plays; B3 lit while there is sound.
    PreviewPlace heard = s.ui.settings.preview ? preview_place(s.heads, int(sampler_pads), uint16_t(preview_slot), dsp::model_rate) : PreviewPlace {};
    SoftKeys keys {{s.files_top ? "CANCEL" : "BACK", other, "PREVIEW"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.toggles = 4; // PREVIEW names a thing that is on or off: its LED, never ON / OFF (the row fills while it sounds)
    keys.lit = s.ui.settings.preview ? 4 : 0;
    if (s.ui.browse.up()) keys.labels[0] = "BACK", keys.tints[1] = colour::dim; // the trim sheet: B1 lowers it, B2 waits
    if (s.ui.sheet_open) keys = {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}; // LOAD KIT's cards
    else if (s.ui.shift) { // Shift + B3 stars the file under the cursor: its LED says whether it is starred
        const SampleFile* at = s.files.empty() ? nullptr : &s.files[std::min<size_t>(s.ui.files_cursor, s.files.size() - 1)];
        keys.labels[2] = at && starrable(*at) ? "STAR" : "";
        keys.glyphs[2] = keys.labels[2].isEmpty() ? Icon::None : Icon::Star;
        keys.toggles = 4;
        keys.lit = at && at->starred ? 4 : 0;
    }
    frame(p, s, keys, h);
    if (s.files.empty()) { // a glyph and two words: no card, or nothing here
        bool no_card = drive == Drive::Card && s.sample_root.empty();
        icon(p, no_card ? Icon::Card : Icon::Folder, zone::width / 2.0f - 14, zone::picture_y + 40, 28, colour::dim, 1.4f);
        p.text(no_card ? "NO CARD" : "EMPTY", 0, zone::picture_y + 76, zone::width, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    const bool in_list = list_folder(s.files_folder);
    unsigned count = unsigned(s.files.size()), cursor = std::min<unsigned>(s.ui.files_cursor, count - 1);
    unsigned first = cursor < 2 ? 0 : std::min(cursor - 2, count > visible ? count - visible : 0);
    // The trim sheet rising: the list goes up with it until the file's row is the top one.
    const float lift = sheet_rise(s) * float(cursor - first) * (list_row_h + list_row_gap);
    p.g.saveState();
    p.g.reduceClipRegion(0, zone::picture_y, zone::width, zone::picture_h);
    for (unsigned row = 0; row < visible + 6 && first + row < count; ++row) {
        if (row >= visible && lift <= 0) break;
        const auto& file = s.files[first + row];
        ListRow r;
        r.name = name_case(juce::String(file.name)); r.accent = c; r.current = first + row == cursor;
        const juce::String path(file.path);
        juce::String format = path.endsWithIgnoreCase(".xpm") ? "XPM" : path.endsWithIgnoreCase(".xty") ? "XTY" : path.endsWithIgnoreCase(".exs") ? "EXS"
                            : path.endsWithIgnoreCase(".xpn") ? "XPN" : path.endsWithIgnoreCase(".sf2") || path.contains(".sf2#") || path.contains(".SF2#") ? "SF2"
                            : path.endsWithIgnoreCase(".dspreset") ? "DS" : "SFZ";
        r.starred = file.starred && s.files_folder != starred_list; // on STARRED every row is
        if (file.list) { r.icon = file.list == 1 ? Icon::Star : Icon::Clock; r.fact = juce::String(file.where); }
        else if (file.folder) { r.icon = Icon::Folder; r.chevron = true; }
        else if (file.bank) r.icon = Icon::Layers;
        else if (file.kit) { r.icon = Icon::Pads; r.fact = format; }
        else if (file.expansion) { r.icon = Icon::ArrowDown; r.fact = format; } // Push installs it
        else if (file.instrument) { r.icon = Icon::Keyboard; r.fact = format; }
        else if (file.midi) { r.icon = Icon::Note; r.fact = "MIDI"; } // roadmap P4: Push lays it on the pattern
        else {
            r.icon = Icon::Wav;
            if (path.endsWithIgnoreCase(".mp3")) r.fact = "MP3";
            else if (path.endsWithIgnoreCase(".flac")) r.fact = "FLAC";
            else if (path.endsWithIgnoreCase(".ogg") || path.endsWithIgnoreCase(".oga")) r.fact = "OGG";
            else if (path.endsWithIgnoreCase(".aif") || path.endsWithIgnoreCase(".aiff")) r.fact = "AIFF";
            for (unsigned k = 0; k < sampler_pads && r.pad.isEmpty(); ++k) // which pad of this kit plays it, on any layer
                for (unsigned l = 0; l < pad_layers; ++l) {
                    const PadLayer layer = track.pads[k].layer(l);
                    if (!layer.used() || file.path != s.project.samples[layer.sample].path) continue;
                    char label[4]; pad_label(k, label); r.pad = juce::String(label);
                    break;
                }
        }
        if (in_list && !file.where.empty()) r.fact = name_case(juce::String(file.where)); // the folder it is in
        if (r.current && heard.shown && !file.folder && !file.bank) { // the time heard of its length
            char now[16], of[24];
            std::snprintf(now, sizeof(now), "%.1f", heard.heard);
            std::snprintf(of, sizeof(of), " / %.1f s", heard.length);
            r.tag_lead = now; r.tag = of; r.fact = {};
            r.played = heard.through; r.played_wake = heard.wake; r.played_head = heard.strength;
        }
        p.g.saveState();
        list_row(p, zone::pad, zone::picture_y + 0.5f + row * (list_row_h + list_row_gap) - lift, zone::width - 2 * zone::pad, r);
        p.g.restoreState();
    }
    p.g.restoreState();
}

// The trim sheet (shell/browse_trim.h), under the file's row: the whole file with the part kept tinted and a box on
// the closer view; the closer view round the end turned last, its ends flagged in their cards' colours (K2, K3); the
// zoom, the time kept (or heard of it) and the file's length. It rises from the cards as it opens.
void draw_sheet(Paint& p, const Snapshot& s) {
    const float rise = sheet_rise(s);
    if (rise <= 0) return;
    const float top = zone::picture_y + 0.5f + list_row_h + list_row_gap + 2, bottom = float(zone::knobs_y);
    const float y = std::round(bottom - (bottom - top) * rise);
    const auto c = colour_of(s), sc = knob_shade(s.ui.track, 1), ec = knob_shade(s.ui.track, 2);
    p.g.saveState();
    p.g.reduceClipRegion(0, zone::picture_y, zone::width, int(bottom) - zone::picture_y);
    for (int n = 1; n <= 4; ++n) p.fill(0, y - float(n), float(zone::width), 1, colour::bg.withAlpha(0.5f - 0.1f * float(n))); // its shadow
    p.fill(0, y, float(zone::width), bottom - y + 10, colour::panel, 8);
    p.fill(8, y, float(zone::width) - 16, 1, colour::line);
    const float x = float(zone::pad) + 6, w = float(zone::width) - 2 * x;
    const float oy = y + 7, oh = 19;      // the whole file
    const float wy = y + 46, wh = 52;     // the closer view, its flags above it
    const float iy = y + 102;             // the row of facts
    const auto h = browse_handles(s.project, s.ui.browse);
    const uint32_t frames = h.frames && s.sample.ready && s.sample.frames == h.frames ? h.frames : 0;
    if (!frames) { // still reading, or it could not be read
        const bool failed = !s.sample.error.empty();
        const juce::String why = failed ? juce::String(s.sample.error) : juce::String("LOADING");
        p.text(why, x, y + 48, w, 16, p.fit(why, failed ? plain(colour::amber, type::body, Weight::Medium) : caps_text(colour::mid, type::body), w, type::floor),
               juce::Justification::centred);
        p.g.restoreState();
        return;
    }
    const FrameView view = browse_view(s.project, s.ui.browse);
    const float kf = float(h.start) / float(frames), kt = float(h.end) / float(frames);
    PadSound probe; // the sheet's audition voice (Context::browse_tick), as Stage finds a pad's
    probe.sample = uint16_t(s.ui.browse.slot);
    WaveHead head;
    const PlayHead* heard = pad_head(s, sampler_pads, probe, frames, head);
    const unsigned heads = heard ? 1 : 0;

    p.fill(x + kf * w, oy, (kt - kf) * w, oh, c.withAlpha(0.12f));
    waveform(p, x, oy, w, oh, s.sample.overview, 0, 1, kf, kt, c, &head, heads, 1.0f);
    p.fill(x + kf * w - 0.75f, oy, 1.5f, oh, sc);
    p.fill(x + kt * w - 0.75f, oy, 1.5f, oh, ec);
    const float bx0 = x + float(view.from) / float(frames) * w, bx1 = x + float(view.to) / float(frames) * w;
    p.ring(bx0 - 2, oy - 2, std::max(4.0f, bx1 - bx0 + 4), oh + 4, colour::text.withAlpha(0.85f), 2, 1);

    const float from = float(view.from) / float(frames), to = std::max(float(view.to) / float(frames), from + 1e-6f);
    WaveDetail detail;
    if (s.detail_to > s.detail_from) detail = {&s.detail, float(s.detail_from) / float(frames), float(s.detail_to) / float(frames)};
    auto x_of = [&](float at) { return x + (at - from) / (to - from) * w; };
    const float sx = x_of(kf), ex = x_of(kt);
    p.g.saveState();
    p.g.reduceClipRegion(int(x), int(wy) - 1, int(w) + 1, int(wh) + 2);
    p.fill(sx, wy, ex - sx, wh, c.withAlpha(0.12f));
    waveform(p, x, wy, w, wh, s.sample.overview, from, to, kf, kt, c, &head, heads, detail);
    p.fill(sx - 0.75f, wy, 1.5f, wh, sc);
    p.fill(ex - 0.75f, wy, 1.5f, wh, ec);
    p.g.restoreState();
    draw_heads(p, x, wy, w, wh, from, to, &head, heads);
    if (sx >= x - 1 && sx <= x + w + 1) icon(p, Icon::Flag, sx - 2, wy - 14, 12, sc, 1.5f);
    if (ex >= x - 1 && ex <= x + w + 1) icon(p, Icon::Flag, ex - 2, wy - 14, 12, ec, 1.5f);

    const double times = double(frames) / std::max(1u, view.width());
    if (times >= 1.05) p.text(juce::String::fromUTF8("\xc3\x97") + (times < 10 ? juce::String(times, 1) : juce::String(int(std::lround(times)))),
                              x, iy, 60, 14, caps_text(c));
    auto secs = [](uint32_t f) { char text[24]; std::snprintf(text, sizeof(text), "%.3f s", f / 44100.0); return juce::String(text); };
    const auto lead = plain(colour::text, type::small, Weight::SemiBold), rest = plain(colour::mid, type::small, Weight::SemiBold),
               dim = plain(colour::dim, type::small, Weight::SemiBold);
    const juce::String whole = secs(frames), kept = secs(h.end - h.start);
    juce::String now, of;
    if (heard && heard->at >= h.start) { now = secs(uint32_t(std::min<double>(heard->at - h.start, h.end - h.start))); of = " OF " + kept; }
    else now = kept;
    float rx = x + w - p.width(whole, dim);
    p.text(whole, rx, iy, p.width(whole, dim) + 1, 14, dim);
    rx -= 10;
    if (of.isNotEmpty()) { rx -= p.width(of, rest); p.text(of, rx, iy, p.width(of, rest) + 1, 14, rest); }
    rx -= p.width(now, lead);
    p.text(now, rx, iy, p.width(now, lead) + 1, 14, lead);
    p.g.restoreState();
}

// LOAD KIT's cards: LOAD (the banks picked), WHOLE KIT, then the track's banks A-H, each with the kit's bank going in.
constexpr unsigned kit_cards = 2 + sampler_banks;

juce::String pads_text(unsigned n) { return juce::String(n) + (n == 1 ? " pad" : " pads"); }

void draw_kit_sheet(Paint& p, const Snapshot& s) {
    const auto& sheet = s.ui.kit_sheet;
    const auto& track = s.track();
    auto c = colour_of(s);
    const auto have = kit_bank_pads(track);
    CardSheet m;
    m.chip = "LOAD KIT"; m.chip_colour = c; m.accent = c;
    m.title = name_case(juce::String(sheet.name));
    unsigned kit_pads = 0;
    juce::String banks;
    for (unsigned b = 0; b < sampler_banks; ++b) {
        if (!sheet.pads[b]) continue;
        kit_pads += sheet.pads[b];
        banks += juce::String::fromUTF8(" \xc2\xb7 ") + juce::String::charToString(juce::juce_wchar('A' + b)) + " " + juce::String(sheet.pads[b]);
    }
    m.detail = pads_text(kit_pads) + banks;
    unsigned picked = 0;
    for (unsigned b = 0; b < sampler_banks; ++b) if (sheet.into[b] >= 0) ++picked;
    m.items[0] = card_item(Icon::ArrowDown, "LOAD", picked == 1 ? "1 bank" : juce::String(picked) + " banks", false, false, !picked);
    bool any = false;
    for (auto n : have) any |= n > 0;
    m.items[1] = card_item(Icon::Pads, "WHOLE KIT", "all banks", false, any);
    for (unsigned b = 0; b < sampler_banks; ++b) {
        const juce::String bank = juce::String::charToString(juce::juce_wchar('A' + b));
        const int from = sheet.into[b];
        juce::String sub = from >= 0 ? "from " + juce::String::charToString(juce::juce_wchar('A' + from)) + juce::String::fromUTF8(" \xc2\xb7 ") + pads_text(sheet.pads[unsigned(from)])
                         : have[b] ? pads_text(have[b]) : juce::String("empty");
        m.items[2 + b] = card_item(Icon::Pads, "BANK " + bank, sub, from >= 0, from >= 0 && have[b] > 0);
    }
    m.count = kit_cards; m.current = s.ui.sheet_row % kit_cards;
    card_sheet(p, m);
}

void draw(Paint& p, const Snapshot& s) {
    draw_list(p, s);
    if (s.ui.browse.up() && !s.ui.sheet_open) draw_sheet(p, s);
    if (s.ui.sheet_open) draw_kit_sheet(p, s);
}

// Minus / Plus: the list; on the trim sheet the next file (the sheet goes down).
void nudge(Context& c, int delta) {
    if (c.ui.sheet_open) { c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(kit_cards) - 1)); return; }
    int was = c.ui.files_cursor;
    c.ui.files_cursor = uint16_t(std::clamp<int>(c.ui.files_cursor + delta, 0, std::max(int(c.ui.files_count) - 1, 0)));
    if (c.ui.files_cursor != was) c.preview_cursor();
}

void wheel(Context& c, int delta) {
    if (c.ui.browse.up() && !c.ui.sheet_open) { // the trim sheet: the end turned last, finer (as on Trim)
        const auto cards = browse_cards(c.project, c.ui.browse);
        c.fine_edit(cards[c.ui.browse.focus % 2], delta);
        return;
    }
    if (c.ui.sheet_open) { c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(kit_cards) - 1)); return; }
    int was = c.ui.files_cursor;
    c.ui.files_cursor = uint16_t(std::clamp<int>(c.ui.files_cursor + delta, 0, std::max(int(c.ui.files_count) - 1, 0)));
    if (c.ui.files_cursor != was) c.preview_cursor();
}

void push(Context& c) {
    if (c.ui.sheet_open) { // LOAD KIT's cards
        unsigned card = c.ui.sheet_row % kit_cards;
        if (card == 0) c.load_kit_banks();
        else if (card == 1) {
            c.ui.sheet_open = false;
            if (auto from = std::move(c.kit_held)) c.load_kit(*from, c.ui.kit_sheet.name);
        } else c.kit_sheet_step(card - 2);
        return;
    }
    if (!c.library) return;
    auto files = c.library->files();
    if (c.ui.files_cursor >= files.size()) return;
    const auto& file = files[c.ui.files_cursor];
    if (file.list) c.library->browse(Drive::Card, file.path); // STARRED or RECENT
    else if (file.folder) c.library->browse(Drive(c.ui.files_drive), file.path);
    else if (file.bank) c.load_bank_file(file.path, file.name);
    else if (file.kit) c.load_kit_file(file.path, file.name, c.track().engine == EngineKind::Sampler); // a kit: LOAD KIT asks which banks
    else if (file.expansion) c.library->install(file.path);
    else if (file.midi) import_midi(c, file.path); // roadmap P4: its notes onto the track's pattern
    else if (c.ui.browse.up() && c.browse_path == file.path) { // the trim sheet's START and END onto the pad
        const auto h = browse_handles(c.project, c.ui.browse);
        c.load_file(file.path, file.name, false, h.start, h.end < h.frames ? h.end : 0);
        c.browse_close();
    }
    else c.load_file(file.path, file.name, false);
    if (!file.folder && !file.expansion && !file.midi) c.library->used(file.path); // first on RECENT
}

// B3 PREVIEW: its LED says which; kept with the settings, as it was left.
void soft_preview(Context& c) {
    c.ui.settings.preview = !c.ui.settings.preview;
    if (c.store) c.store->save_settings(c.ui.settings.text());
    if (!c.ui.browse.up()) c.preview_cursor();
    else if (!c.ui.settings.preview) c.stop_preview(); // the sheet's audition stops; turned on, the next move is heard
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.sheet_open) { if (key == 0) { c.kit_held.reset(); c.back(); } return; } // LOAD KIT's cards: B1 BACK
    if (!c.library) return;
    if (ui.browse.up() && !(ui.shift && key == 2)) { // the trim sheet: B1 lowers it, B2 waits, B3 PREVIEW (Shift + B3: STAR)
        if (!ui.shift && key == 0) c.browse_close();
        if (!ui.shift && key == 2) soft_preview(c);
        return;
    }
    if (key == 0) { if (c.library->top()) samples_cancel(c); else c.library->browse_up(); } // at the top: CANCEL
    if (key == 1) { // the other drive, where it was left
        unsigned from = ui.files_drive % unsigned(Drive::Count), to = (from + 1) % unsigned(Drive::Count);
        const unsigned kind = c.files_kind >= 0 ? unsigned(c.files_kind) : c.browse_kind();
        c.files_places[kind][from] = {c.library->folder(), ui.files_cursor, true};
        c.files_drives[kind] = uint8_t(to);
        const auto& back = c.files_places[kind][to];
        c.files_row_wanted = back.set ? back.row : -1;
        ui.files_drive = uint8_t(to);
        c.library->browse(Drive(to), back.set ? back.folder : std::string());
    }
    if (key == 2 && ui.shift) { // Shift + B3: the file under the cursor starred, or no longer
        auto files = c.library->files();
        if (ui.files_cursor < files.size() && starrable(files[ui.files_cursor])) c.library->star(files[ui.files_cursor].path);
        else c.notice("Star: a file");
        return;
    }
    if (key == 2) soft_preview(c);
}
} // namespace

// Shift + pad on the browser: the file under the cursor onto the next empty pad.
bool samples_shift_pad(Context& c) {
    if (c.ui.page != PageId::Samples || !c.library || c.track().engine != EngineKind::Sampler) return false;
    auto files = c.library->files();
    if (c.ui.files_cursor >= files.size() || files[c.ui.files_cursor].folder) { c.notice("Shift + pad: a file first"); return true; }
    const auto& file = files[c.ui.files_cursor];
    if (file.bank) { c.notice("Push loads a bank"); return true; }
    if (file.kit) { c.notice("Push loads a kit"); return true; }
    if (file.expansion) { c.notice("Push installs it"); return true; }
    if (file.midi) { c.notice("Push lays it on the pattern"); return true; }
    if (c.ui.browse.up() && c.browse_path == file.path) { // the trim sheet's START and END onto the next empty pad
        const auto h = browse_handles(c.project, c.ui.browse);
        c.load_file(file.path, file.name, true, h.start, h.end < h.frames ? h.end : 0);
        c.browse_close();
    } else c.load_file(file.path, file.name, true);
    c.library->used(file.path); // first on RECENT
    return true;
}

const PageSpec samples_page {draw, play_pad, wheel, push, soft, nudge}; // Minus / Plus move the list

} // namespace melodee
