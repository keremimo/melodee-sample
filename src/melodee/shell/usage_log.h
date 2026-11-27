// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include <cstdint>
#include <cstdio>
#include <string>

// The usage log (Settings' doors, K2 USAGE LOG; off until turned on): what the hands did, a line each, to find
// where people get stuck. It goes to the card's Melodee/Usage folder, a file for each start with the log on
// (usage-<unix time>.txt), written every 10 s by the project store's thread. Pages, controls, undo, the MENU and
// lessons only: no project, sample, plug-in or file names. tools/usage_report.py reads the files.
//
// Each line starts with the seconds since the log started, to the millisecond:
//   0.000 start 1.0.0 Full            the version, and Basic or Full
//   4.210 page Chop Trim 2310         a page opened: the page left and the ms spent on it
//   5.002 nojob B2 Stage              a press (with its release), a knob or the wheel that changed nothing there
//   6.120 undo 850 Clear Stage        undo (or redo): ms since the last edit was made, the edit's first word
//                                     (Clear, Delete, Chop...; never a name) and the page
//   7.400 menu GO 4                   a MENU tile picked (GO, its pad 1-16), or a card (HERE CAPTURE)
//   9.800 lesson 1 3 4200             a lesson step done: the lesson, the step, the ms it took
namespace melodee {

inline const char* usage_page_name(PageId page) {
    static const char* const names[] {
        "Stage", "Keys", "Sounds", "Edit", "Roll", "Motion", "Patterns", "Song", "InputConfig", "Rack", "Sends", "Bus",
        "Master", "Pads", "Samples", "Trim", "Chop", "Record", "Project", "Tempo", "Pad", "Play", "Envelope", "Filter",
        "Follow", "Cloud", "Punch", "Settings", "System", "PadFx", "TimeCorrect", "StepEdit", "PadMix", "PadFilter",
        "FilterEnv", "KnobFx", "Compressor", "FlexBeat", "Fader", "MidiConfig", "CardAccess", "Tracks", "Looper",
        "Plugins", "PluginPresets", "Layers", "Mod", "Print", "Scenes", "AutoSample", "Stems", "Mixer"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(PageId::Count), "a name for every page");
    return unsigned(page) < unsigned(PageId::Count) ? names[unsigned(page)] : "None";
}

class UsageLog {
public:
    static constexpr uint32_t flush_ms = 10000; // the card gets the lines every 10 s
    static constexpr size_t flush_bytes = 4096; // or sooner, once this many wait
    static constexpr uint32_t repeat_ms = 2000; // the same nojob again within this: one line (a knob turns in steps)

    // Turned on (or Melodee started with it on): a new file named from `unix_time`.
    void start(uint32_t ms, long long unix_time, const char* version, bool full) {
        if (on_) return;
        on_ = true;
        started_ = ms; flushed_ = ms; page_ = PageId::Count; lesson_ = 0; nojob_.clear();
        file_ = "usage-" + std::to_string(unix_time) + ".txt";
        line(ms, std::string("start ") + version + (full ? " Full" : " Basic"));
    }
    void stop() { on_ = false; }
    bool on() const { return on_; }
    const std::string& file() const { return file_; }

    // An edit was kept (History::pushes moved): an undo soon after says it was not what was wanted.
    void edited(uint32_t ms) { edited_at_ = ms; }

    // The page in view now (after each input and on the lights' pass): a line when it changed.
    void page(uint32_t ms, PageId now) {
        if (!on_ || now == page_) return;
        if (page_ != PageId::Count)
            line(ms, std::string("page ") + usage_page_name(now) + " " + usage_page_name(page_) + " " + std::to_string(ms - page_at_));
        page_ = now; page_at_ = ms;
    }
    // A control that changed nothing on `page`; once for a run of the same one (a knob's many steps).
    void nojob(uint32_t ms, const char* control, PageId page) {
        if (!on_) return;
        std::string what = std::string(control) + " " + usage_page_name(page);
        if (what == nojob_ && ms - nojob_at_ < repeat_ms) { nojob_at_ = ms; return; }
        nojob_ = what; nojob_at_ = ms;
        line(ms, "nojob " + what);
    }
    void undo(uint32_t ms, bool redo, const std::string& what, PageId page) {
        if (!on_) return;
        std::string word = what.substr(0, what.find(' '));
        bool plain = !word.empty();
        for (char ch : word) plain = plain && ch >= 'A' && ch <= 'z' && (ch <= 'Z' || ch >= 'a'); // a word, not a name or number
        line(ms, std::string(redo ? "redo " : "undo ") + std::to_string(ms - edited_at_) + " " + (plain ? word : "Edit") + " "
                     + usage_page_name(page));
    }
    void menu_go(uint32_t ms, unsigned tile) { if (on_) line(ms, "menu GO " + std::to_string(tile % 16 + 1)); }
    void menu_here(uint32_t ms, const char* card) { if (on_) line(ms, std::string("menu HERE ") + card); }
    // The lesson in hand (UiState::lesson, lesson_step): a line for each step done.
    void lesson(uint32_t ms, unsigned lesson, unsigned step) {
        if (!on_) return;
        if (lesson && lesson == lesson_ && step > step_)
            line(ms, "lesson " + std::to_string(lesson) + " " + std::to_string(step_ + 1) + " " + std::to_string(ms - step_at_));
        if (lesson != lesson_ || step != step_) step_at_ = ms;
        lesson_ = lesson; step_ = step;
    }

    // The writer's side: whether to hand the lines over now, and the lines (emptied).
    bool due(uint32_t ms) const { return !text_.empty() && (text_.size() >= flush_bytes || ms - flushed_ >= flush_ms); }
    std::string take(uint32_t ms) { flushed_ = ms; std::string out; out.swap(text_); return out; }

    void line(uint32_t ms, const std::string& words) {
        if (!on_) return;
        const uint32_t t = ms - started_;
        char stamp[24];
        std::snprintf(stamp, sizeof stamp, "%u.%03u ", unsigned(t / 1000), unsigned(t % 1000));
        text_ += stamp;
        text_ += words;
        text_ += '\n';
    }

private:
    bool on_ = false;
    uint32_t started_ = 0, flushed_ = 0, edited_at_ = 0, page_at_ = 0, nojob_at_ = 0, step_at_ = 0;
    PageId page_ = PageId::Count;
    unsigned lesson_ = 0, step_ = 0;
    std::string file_, text_, nojob_;
};

} // namespace melodee
