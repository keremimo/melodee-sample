// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/undo_scope.h"
#include "melodee/shell/stage_map.h"

namespace melodee {
using Kind = Binding::Kind;

namespace {
UndoScope pad_scope(const UiState& ui, const Project& project) {
    const auto& t = project.tracks[ui.track % track_count];
    if (t.engine != EngineKind::Sampler) return {};
    return {UndoScope::Kind::Pad, uint8_t(ui.track % track_count), uint16_t(t.last_pad % sampler_pads)};
}
UndoScope of(UndoScope::Kind kind, const UiState& ui, unsigned index = 0) { return {kind, uint8_t(ui.track % track_count), uint16_t(index)}; }

} // namespace

MixState mix_of(const Project& p) {
    MixState m {};
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& x = p.tracks[t].mix;
        m.level[t] = x.level; m.pan[t] = x.pan; m.sends[t] = x.sends;
    }
    for (unsigned b = 0; b < bus_count; ++b) m.returns[b] = p.buses[b].level;
    return m;
}
SoundState sound_of(const Track& t) {
    SoundState s {};
    s.params = t.params;
    s.mods = t.mods;
    return s;
}
MasterState master_of(const Project& p) {
    MasterState m {};
    m.knob_choice = p.knob_fx.choice; m.knob_kind = p.knob_fx.kind; m.knob_bypass = p.knob_fx.bypass; m.knob_params = p.knob_fx.params;
    m.knob_a = p.knob_fx.a; m.knob_b = p.knob_fx.b; m.knob_morph = p.knob_fx.morph;
    m.compressor = p.compressor; m.pad_fx = p.pad_fx_params; m.flex = p.flex;
    m.punch_depth = p.punch_depth; m.punch_snap = p.punch_snap; m.punch_tail = p.punch_tail; m.punch_quantize = p.punch_quantize;
    m.modulators = p.modulators; m.macros = p.macros; // roadmap P4
    return m;
}
ChopState chop_of(const UiState& ui) {
    ChopState c {};
    c.slices = ui.slices; c.mode = uint8_t(ui.chop_mode); c.slice = ui.chop_slice; c.amount = ui.chop_amount;
    c.sample = ui.chop_sample; c.from = ui.chop_from; c.to = ui.chop_to;
    return c;
}
UndoScope binding_scope(const Project& project, const UiState& ui, const Binding& b) {
    using S = UndoScope::Kind;
    switch (b.kind) {
    case Kind::Param: case Kind::ModSource: case Kind::ModDest: case Kind::ModAmount: return of(S::Sound, ui);
    case Kind::Level: case Kind::Pan: case Kind::Send: case Kind::SendBus: case Kind::BusReturn: return of(S::Mix, ui);
    case Kind::FxParam: case Kind::FxKey: return of(S::Chain, ui, unsigned(b.id) / 64);
    // Roadmap P4: a modulator's settings and a macro's targets, with the master's; a macro's turn and the picks are not kept.
    case Kind::ModKind: case Kind::ModShape: case Kind::ModSync: case Kind::ModRate: case Kind::ModInput: case Kind::ModAttack:
    case Kind::ModRelease: case Kind::ModDepth: case Kind::MacroLow: case Kind::MacroHigh: return of(S::Master, ui);
    case Kind::PadPitch: case Kind::PadLevel: case Kind::PadPan: case Kind::PadStart: case Kind::PadEnd: case Kind::PadFine:
    case Kind::PadPlayMode: case Kind::PadGroup: case Kind::PadReverse: case Kind::PadAttack: case Kind::PadDecay: case Kind::PadRelease:
    case Kind::PadSustain: case Kind::PadLoopPoint:
    case Kind::PadLoop: case Kind::PadMono: case Kind::PadLink: case Kind::PadOffset: case Kind::PadVelSense: case Kind::PadDecayFrom:
    case Kind::PadFilterType: case Kind::PadCutoff: case Kind::PadResonance: case Kind::PadFAttack: case Kind::PadFDecay:
    case Kind::PadFSustain: case Kind::PadFRelease: case Kind::PadFAmount: case Kind::PadWarp: case Kind::PadBeats:
    case Kind::LayerLow: case Kind::LayerHigh: case Kind::LayerPitch: case Kind::LayerGain: case Kind::LayerPlayMode: case Kind::PadLoopXfade:
    case Kind::PadColour: case Kind::PadSend:
    case Kind::ZoneTune: case Kind::ZoneLevel: // round 3 "instrument C": the pad's state holds its trims
        return pad_scope(ui, project);
    case Kind::ChopMode: case Kind::ChopAmount: case Kind::ChopStart: case Kind::ChopEnd: return of(S::Chop, ui);
    case Kind::KnobParam: case Kind::CompAttack: case Kind::CompRelease: case Kind::CompAmount: case Kind::CompBoost:
    case Kind::PadFxParam: case Kind::FlexMode: case Kind::FlexMix: case Kind::PunchDepth: case Kind::PunchSnap: case Kind::PunchTail:
        return of(S::Master, ui);
    case Kind::SongPattern: case Kind::SongLength: return b.id >= 0 ? of(S::Song, ui) : UndoScope {};
    case Kind::SongTempo: case Kind::SongTempoShape: return of(S::Song, ui); // roadmap P5: the song's tempo lane
    case Kind::StepLength: case Kind::StepVelocity: case Kind::StepChance: case Kind::EventVelocity: case Kind::EventLength:
    case Kind::EventChance: case Kind::StepRatchet: case Kind::StepRamp: case Kind::StepPitchRamp: case Kind::EventRatchet:
    case Kind::EventRamp: case Kind::EventPitchRamp: case Kind::StepCondition: case Kind::EventCondition: case Kind::Lock:
    case Kind::LaneLength: case Kind::FeelStrength: case Kind::FeelSwing: case Kind::FeelHumanize: case Kind::FeelVelocity:
    case Kind::GrooveFrom: case Kind::GrooveAmount: case Kind::EuclidHits: case Kind::EuclidSteps: case Kind::EuclidRotate: case Kind::EventNudge: case Kind::MarksRamp:
    case Kind::FollowAction: case Kind::FollowOther: case Kind::FollowAfter: case Kind::FollowChance: // roadmap P5: a pattern's follow action
    case Kind::PatternProgram: // a MIDI track's pattern's own program
        return pattern_scope(project, ui);
    default: return {};
    }
}

bool print_covers(const std::array<TrackFreeze, track_count>& freeze, const UndoScope& s) {
    using S = UndoScope::Kind;
    const unsigned t = s.track % track_count;
    switch (s.kind) {
    case S::Sound: case S::Pad: return freeze[t].frozen();
    case S::Pattern: return freeze[t].frozen() && freeze[t].plays(s.index);
    case S::Chain:
        if (s.index < track_count) return freeze[s.index].frozen() && freeze[s.index].tap == PrintTap::PostFx; // Pre FX prints hold no chain
        if (s.index >= pad_chain_base) return freeze[(s.index - pad_chain_base) / sampler_pads % track_count].frozen();
        return false;
    default: return false;
    }
}

UndoScope freeze_scope(const Project& project, const UiState& ui, const Binding& b) {
    switch (b.kind) {
    case Kind::PadColour: return {}; // a pad's colour is not heard: no print holds it
    case Kind::PatternBars: case Kind::PatternLength: case Kind::Division: case Kind::Swing: case Kind::TcShift: case Kind::TcSwing:
        return pattern_scope(project, ui);
    case Kind::TrackSpeed: case Kind::StageSound: case Kind::PluginParam: case Kind::ArpMode: case Kind::ArpRate: case Kind::ArpOctaves:
    case Kind::ArpGate: case Kind::ArpHold: case Kind::ChordType: case Kind::ChordVoicing: case Kind::ChordInvert: case Kind::ChordOn:
        return of(UndoScope::Kind::Sound, ui); // the plug-in's values, the arp and chords play into the print
    default: return binding_scope(project, ui, b);
    }
}

UndoScope pattern_scope(const Project& project, const UiState& ui) {
    return of(UndoScope::Kind::Pattern, ui, project.tracks[ui.track % track_count].pattern % pattern_count);
}

UndoScope press_scope(const Project& project, const UiState& ui, SurfaceButton button, bool pad) {
    using S = UndoScope::Kind;
    using B = SurfaceButton;
    if (ui.seq_held) return {}; // the Sequence layer: launches and pages; a copy keeps its own edit (shell/seq_layer.cpp)
    if (!pad && ui.page == PageId::Song && !ui.shift && !ui.song_export && (button == B::Loop || button == B::NoteRepeat)) return of(S::Song, ui); // the brace, the clip's repeats
    if (!pad && (button == B::Loop || (button == B::Chop && ui.shift))) return pad_scope(ui, project);
    if (!pad && (button == B::KnobFx || button == B::Mute || button == B::NoteRepeat || button == B::PadBank || button == B::Play
                 || button == B::Stop || button == B::SeqRecord)) return {}; // modes and the transport, not edits
    switch (ui.page) {
    case PageId::Song: return of(S::Song, ui);
    case PageId::Tracks: case PageId::Sends: return of(S::Mix, ui);
    case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: return of(S::Chain, ui, page_chain(project, ui));
    case PageId::KnobFx: case PageId::Compressor: case PageId::FlexBeat: case PageId::Punch: return of(S::Master, ui);
    case PageId::Chop: return ui.chop_armed && pad ? UndoScope {} : of(S::Chop, ui); // Manual's taps are one take, not a step each
    case PageId::Trim: case PageId::PadMix: case PageId::EnvelopePage: case PageId::PadPage: case PageId::PlayPage:
    case PageId::PadFilterPage: case PageId::FilterEnvPage: case PageId::Pads: case PageId::LayersPage:
        return pad_scope(ui, project);
    case PageId::Edit: return of(S::Sound, ui);
    case PageId::Roll: case PageId::Steps: case PageId::StepEdit: return pattern_scope(project, ui); // notes written, toggled, moved
    case PageId::Stage: { // a kit's TRIM: Shift + B1 Loop Lock and Shift + B2 Normalize, as on the old Trim
        const auto& t = project.tracks[ui.track % track_count];
        bool kit_page = stage_kit_page(stage_page_now(ui, t).kind) != PageId::Count;
        return kit_page && ui.shift && !pad ? pad_scope(ui, project) : UndoScope {};
    }
    default: return {};
    }
}

std::string scope_name(const Project& project, const UiState& ui, const UndoScope& s) {
    using S = UndoScope::Kind;
    switch (s.kind) {
    case S::Pad: { char label[4]; pad_label(s.index, label); return label; }
    case S::Mix: return "Mix";
    case S::Chain:
        if (s.index >= pad_chain_base) { char label[4]; pad_label((s.index - pad_chain_base) % sampler_pads, label); return std::string(label) + " FX"; }
        if (s.index == master_chain) return "Master FX";
        if (is_loop_chain(s.index)) return "L" + std::to_string(s.index - loop_chain(0) + 1) + " FX";
        if (s.index >= track_count) return std::string("Bus ") + char('A' + (s.index - track_count) % bus_count) + " FX";
        return "T" + std::to_string(s.index + 1) + " FX";
    case S::Master:
        switch (ui.page) {
        case PageId::Compressor: return "Compressor";
        case PageId::FlexBeat: return "Flex Beat";
        case PageId::Punch: return "Pad FX";
        default: return "Knob FX";
        }
    case S::Sound: return "T" + std::to_string(s.track + 1) + " sound";
    case S::Song: return "Song";
    case S::Chop: return "Chop";
    case S::Pattern: return "T" + std::to_string(s.track + 1) + " P" + std::to_string(s.index + 1);
    default: (void)project; return {};
    }
}

} // namespace melodee
