// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/effects.h"
#include "melodee/engine/engine_info.h"
#include <array>
#include <cstdint>

// A PLUGIN track's three performance knobs (docs/MELODEE.md, Plug-ins): Stage's PRESETS K1-K3, the same on every
// plug-in, run on its output inside its engine (engine/plugin_engine.h). EDIT keeps the plug-in's own parameters.
//
// Each of the three slots holds a macro kind and one value, value 0 always off. A new macro is a MacroKind, its
// ParamInfo in macro_param() and its sound in MacroStage (macros.cpp); a slot takes any kind, so letting people
// choose a slot's kind is only a matter of setting plugin_track::Kind1 + slot. The track keeps the slots in
// track.params (plugin_track below), so they are saved, recorded as motion and undone as any engine parameter.
namespace melodee {

enum class MacroKind : uint8_t {
    Filter = 1, // -100..100: low pass to the left, high pass to the right, off in the middle (a DJ mixer's filter)
    Echo,       // 0..100: how much goes into a tempo-synced dotted-eighth echo; its repeats ring on when turned back
    Space,      // 0..100: how much goes into a reverb; its tail rings on when turned back
    End,
};
constexpr unsigned macro_slots = 3;
constexpr std::array<MacroKind, macro_slots> default_macros {MacroKind::Filter, MacroKind::Echo, MacroKind::Space};

namespace plugin_track {
// A PLUGIN track's engine parameters: each slot's value, then each slot's kind as stored (macro_kind()).
enum : uint8_t { Macro1, Macro2, Macro3, Kind1, Kind2, Kind3, Count };
} // namespace plugin_track

// A slot's kind from what the track stores: 0 (every project before macros, and a new track) the slot's default.
inline MacroKind macro_kind(int stored, unsigned slot) {
    if (stored > 0 && stored < int(MacroKind::End)) return MacroKind(stored);
    return default_macros[slot % macro_slots];
}
// Its card: label, range (0 off), format.
const ParamInfo& macro_param(MacroKind kind);
// Engine parameter `id` of a track playing `engine` with `params` as its card and lane show it: a PLUGIN track's knob
// values as their slot's kind, the rest as engine_info() states them (id below its param_count).
template <typename Params>
const ParamInfo& track_param_info(EngineKind engine, const Params& params, unsigned id) {
    if (engine == EngineKind::Plugin && id < macro_slots) return macro_param(macro_kind(params[plugin_track::Kind1 + id], id));
    return engine_info(engine).params[id];
}

// The three slots' sound, on the audio thread. Echo and Space are sends: the knob sets how much goes in, so their
// tails go on after it is turned back. They take delay memory from the host's pool only while they sound, and give it
// back a second after they fall silent; with the pool used up they stay silent.
class MacroStage {
public:
    void set_pool(DelayPool* p) { pool = p; }
    void set(unsigned slot, MacroKind kind, int value);
    void process(float* left, float* right, unsigned frames);
    void release(); // the track is no longer a plug-in's: delay memory back to the pool, tails gone
    bool sounding() const;

private:
    struct Slot {
        MacroKind kind = MacroKind::Filter;
        int value = 0;
        Effect fx;
        bool set_up = false;   // fx holds this kind's sound
        float send = 0;        // Echo and Space: the gain into the effect, as last heard
        uint32_t quiet = 0;    // frames its effect has been silent with nothing going in
    };
    void tune(Slot& s);
    void send_into(Slot& s, float* left, float* right, unsigned frames);
    DelayPool* pool = nullptr;
    std::array<Slot, macro_slots> slots {};
};

} // namespace melodee
