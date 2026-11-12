// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_player.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/sample_library.h"
#include <filesystem>

// Freeze plan F2a: prints asked for (for now by `melodee-print`, before any page has them) and their records kept.
namespace melodee {

void Context::print(const PrintAsk& ask) {
    if (ask.mode == PrintAsk::Freeze) { freeze(ask); return; } // freeze plan F2b, `melodee-freeze` and `melodee-unfreeze`
    if (ask.mode == PrintAsk::Unfreeze) { unfreeze(ask.track); return; }
    if (!printer) return;
    if (project_path.empty()) { printer->note("save the project first: its prints go beside its file"); return; }
    const unsigned t = ask.track % track_count;
    PrintJob job;
    job.project_path = project_path;
    job.track = t;
    job.tap = ask.tap;
    job.tail = ask.tail;
    for (unsigned p = 0; p < pattern_count; ++p) {
        bool asked = (ask.patterns[p / 64] >> (p % 64)) & 1;
        if (ask.all ? !patterns.at[t][p].empty() : asked) job.patterns.push_back(uint8_t(p));
    }
    bool none = !ask.all;
    for (auto bits : ask.patterns) none &= bits == 0;
    if (none) job.patterns.push_back(project.tracks[t].pattern); // the one it plays
    if (job.patterns.empty()) { printer->note("track " + std::to_string(t + 1) + " has no pattern with notes"); return; }
    job.project = std::make_unique<Project>(project);
    job.bank = std::make_unique<PatternBank>(patterns);
    if (plugins) job.states = plugins->states();
    if (library) {
        auto root = library->root();
        job.card_top = root.empty() ? std::string() : std::filesystem::path(root).parent_path().string();
    }
    job.known = prints;
    if (!printer->start(std::move(job))) printer->note("busy: a print is under way");
}

void Context::freeze(const PrintAsk& ask) {
    if (!printer) return;
    const unsigned t = ask.track % track_count;
    const auto& tr = project.tracks[t];
    if (project_path.empty()) { printer->note("save the project first: its prints go beside its file"); return; }
    if (!tr.used() || tr.engine == EngineKind::Midi) { printer->note("track " + std::to_string(t + 1) + " has no sound to freeze"); return; }
    TrackFreeze f;
    f.state = ask.deep ? FreezeState::Deep : FreezeState::Frozen;
    f.tap = ask.deep ? PrintTap::PostFx : ask.tap; // freeze plan F2d: the chain's plug-ins go too, so the print holds them
    f.tail = ask.tail;
    bool none = !ask.all;
    for (auto bits : ask.patterns) none &= bits == 0;
    for (unsigned p = 0; p < pattern_count; ++p) // a deep freeze: the whole track, as nothing of it plays live
        if (!patterns.at[t][p].empty() && (none || ask.all || ask.deep || ((ask.patterns[p / 64] >> (p % 64)) & 1))) f.set(p, true);
    bool any = false;
    for (auto bits : f.patterns) any |= bits != 0;
    if (!any) { printer->note("track " + std::to_string(t + 1) + " has no pattern with notes to freeze"); return; }
    const uint32_t bit = 1u << t;
    if (ask.deep && !deep(t)) { deep_hold |= bit; deep_sent[t] = 0; } // its plug-ins stay until the print plays
    thawing &= ~bit;
    prints.freeze[t] = f;
    dirty = true;
    refreeze(t);
}

uint32_t Context::plugins_asleep() const {
    uint32_t out = 0;
    for (unsigned t = 0; t < track_count; ++t)
        if (deep(t) && !((deep_hold >> t) & 1) && !((thawing >> t) & 1)) out |= 1u << t;
    return out;
}

void Context::refreeze(unsigned track) {
    const unsigned t = track % track_count;
    if (!frozen(t)) return;
    ++freeze_serial[t];        // what an earlier ask is still making is dropped
    freeze_waiting |= 1u << t; // started from print_tick() once the print engine is free
}

void Context::unfreeze(unsigned track) {
    const unsigned t = track % track_count;
    const uint32_t bit = 1u << t;
    // Freeze plan F2d: a deep freeze thaws first. Its plug-ins load again beside the prints, which play on until
    // every one is open (thaw_tick()); one held still (its print not yet playing) goes live at once.
    if (deep(t) && !(deep_hold & bit)) {
        if (!(thawing & bit)) { thawing |= bit; thaw_since[t] = now ? now : 1; thaw_ready[t] = 0; }
        return;
    }
    go_live(t);
}

void Context::go_live(unsigned track) {
    const unsigned t = track % track_count;
    const uint32_t bit = 1u << t;
    deep_hold &= ~bit;
    thawing &= ~bit;
    deep_sent[t] = 0;
    ++freeze_serial[t];
    freeze_waiting &= ~(1u << t);
    bool was = frozen(t);
    prints.freeze[t] = TrackFreeze {};
    Command c {Command::Type::Freeze, uint8_t(t)}; // none: live again at once
    host.send(c);
    if (was) dirty = true;
}

void Context::print_tick() {
    if (!printer) return;
    // Freeze plan F2b: packs the audio thread is done with, freed on the print thread; prints ready to play, sent to
    // switch on the next bar; and the next track waiting for its prints, started once the print engine is free.
    FreezePack* done = nullptr;
    while (host.take_done_freeze(done)) printer->discard(done);
    FreezeReady ready;
    while (printer->take_ready(ready)) {
        const unsigned t = ready.track % track_count;
        bool wanted = ready.project_path == project_path && ready.serial == freeze_serial[t] && frozen(t);
        // Freeze plan F2c: the PRINT sheet waiting for this freeze closes once it is sent (or says why not).
        auto& sheet = ui.print_sheet;
        bool sheet_waits = wanted && sheet.printing && sheet.track == t && sheet.serial == ready.serial;
        if (sheet_waits) {
            sheet.printing = false;
            if (ui.page == PageId::Print) back();
        }
        if (!wanted || !ready.pack) {
            printer->discard(ready.pack);
            if (wanted) notice("Track " + std::to_string(t + 1) + " not frozen: " + ready.error);
            continue;
        }
        print_peaks[t] = std::move(ready.peaks); // freeze plan F2c: Stage's picture of each print
        Command c {Command::Type::Freeze, uint8_t(t), 0, 0, 1}; // on the next bar while playing
        c.freeze = ready.pack;
        const bool deep_pack = ready.pack->deep;
        if (!host.send(c)) printer->discard(ready.pack);
        else {
            if (deep_pack) deep_sent[t] = now ? now : 1; // freeze plan F2d: its plug-ins go once the engine sleeps
            if (sheet_waits) notice("Track " + std::to_string(t + 1) + (deep_pack ? " deep frozen" : " frozen"));
        }
    }
    // Freeze plan F2d: a deep freeze's plug-ins let go once its print plays and the engine has gone to sleep (a
    // second below the floor), or 8 s after its pack went; a thaw goes live once its plug-ins are open.
    for (unsigned t = 0; t < track_count; ++t) {
        const uint32_t bit = 1u << t;
        if (!(deep_hold & bit)) continue;
        if (!deep(t)) { deep_hold &= ~bit; continue; }
        if (!deep_sent[t]) continue;
        uint32_t since = now - deep_sent[t];
        bool sleeps = (host.sleeping_tracks() & bit) && (host.frozen_tracks() & bit);
        if ((sleeps && since >= 200) || since >= 8000) deep_hold &= ~bit;
    }
    thaw_tick();
    uint32_t deep_bits = 0;
    for (unsigned t = 0; t < track_count; ++t) deep_bits |= uint32_t(deep(t)) << t;
    deep_now.store(deep_bits, std::memory_order_relaxed);
    thawing_now.store(thawing, std::memory_order_relaxed);
    thaw_took_now.store(thaw_took, std::memory_order_relaxed);
    if (freeze_waiting && !printer->busy() && !project_path.empty()) {
        unsigned t = 0;
        while (!((freeze_waiting >> t) & 1)) ++t;
        const auto& f = prints.freeze[t];
        PrintJob job;
        job.project_path = project_path;
        job.track = t;
        job.tap = f.tap;
        job.tail = f.tail;
        job.freeze = true;
        job.deep = f.state == FreezeState::Deep; // freeze plan F2d
        job.serial = freeze_serial[t];
        for (unsigned p = 0; p < pattern_count; ++p)
            if (f.plays(p) && !patterns.at[t][p].empty()) job.patterns.push_back(uint8_t(p));
        job.project = std::make_unique<Project>(project);
        job.bank = std::make_unique<PatternBank>(patterns);
        if (plugins) job.states = plugins->states();
        if (library) {
            auto root = library->root();
            job.card_top = root.empty() ? std::string() : std::filesystem::path(root).parent_path().string();
        }
        job.known = prints;
        bool empty = job.patterns.empty();
        if (empty || printer->start(std::move(job))) freeze_waiting &= ~(1u << t);
        if (empty) {
            unfreeze(t); // its patterns have no notes left
            if (ui.print_sheet.printing && ui.print_sheet.track == t) { ui.print_sheet.printing = false; if (ui.page == PageId::Print) back(); }
        }
    }
    PrintResult r;
    while (printer->take(r)) {
        if (r.project_path != project_path || !r.error.empty()) continue; // another project's, or not made
        const auto* known = prints.find(r.record.track, r.record.pattern);
        if (known && *known == r.record) continue;
        prints.put(r.record);
        dirty = true; // its record goes with the project
    }
}

// Freeze plan F2d: each thawing track goes live once every plug-in it uses (its own and its chain's) has been open
// for 1.5 s (the audio thread has taken it, and work a plug-in does in the background after loading its state, such
// as TONE3000 preparing its models, is done before it plays: done while it played, it cost xruns on the MPC); one
// that fails to open, or 30 s without, goes live anyway and says so.
void Context::thaw_tick() {
    for (unsigned t = 0; t < track_count; ++t) {
        const uint32_t bit = 1u << t;
        if (!(thawing & bit)) continue;
        if (!deep(t)) { thawing &= ~bit; continue; }
        bool ready = true, failed = false;
        if (plugins) {
            auto check = [&](unsigned entry) {
                if (entry >= max_plugins || !project.plugins[entry].used()) return;
                auto st = plugins->status(entry);
                if (st == PluginStatus::Failed || st == PluginStatus::Crashed) failed = true;
                else if (st != PluginStatus::Ready || plugins->asleep(entry)) ready = false;
            };
            const auto& tr = project.tracks[t];
            if (tr.engine == EngineKind::Plugin) check(tr.plugin);
            for (const auto& slot : tr.fx.slots) if (slot.kind == plugin_effect_kind) check(unsigned(slot.params[0]));
        }
        const std::string name = "Track " + std::to_string(t + 1);
        if (failed || now - thaw_since[t] > 30000) {
            go_live(t);
            notice(name + " live: a plug-in did not open");
            continue;
        }
        if (!ready) { thaw_ready[t] = 0; continue; }
        if (!thaw_ready[t]) { thaw_ready[t] = now ? now : 1; continue; }
        if (now - thaw_ready[t] < 1500) continue;
        thaw_took = now - thaw_since[t];
        go_live(t);
        notice(name + " live");
    }
}

} // namespace melodee
