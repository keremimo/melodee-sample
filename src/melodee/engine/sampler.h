// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/sample_data.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/engine/voice_tap.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/sampler.h"
#include <array>

// SAMPLER: a kit of 128 pads, each a region of a sample (up to four, its
// layers, picked by velocity, in turn or at random), written for
// sample-base after the melodee-ios sampler's behaviour and the MPC
// Sample's Sample Mode: gate (Note On) or one shot, loop and reverse, mute
// groups (pads in a group cut each other), mono or poly, a linked pad,
// offset, velocity sensitivity, its own pitch, level, pan, envelope, filter
// and filter envelope, warp, and one filter on the whole kit. Notes 36..163
// play pads 1..128 (bank A first). A muted pad is not heard. Stereo. Audio thread only; the sample
// table belongs to the engine host. A pad may hold a multisampled instrument
// (engine/multisample.h): its note and velocity pick zones, read through the
// stream pager, everything else about the pad staying the pad's.
namespace melodee {

namespace sampler {
enum Param : int8_t {
    Tune, Filter, Resonance, Decay, Level,      // the kit
    FilterType, Follow,                         // Filter, Follow pages
    Cloud, Position, Size, Density, Scan, Spread, // Cloud: pads as clouds of grains
    FollowHow, PitchHow,                        // Follow: Beats / Tones / Tape; a pitch change moves speed or is stretched
    Count
};
}

class SamplerEngine final : public Engine {
public:
    static constexpr unsigned max_voices = 32;
    // Past max_voices a new voice takes the oldest's place, which fades out (a choke's quick fade) in one of
    // these spare voices instead of stopping dead (a click).
    static constexpr unsigned spare_voices = 8;
    using Table = std::array<const SampleData*, sample_slots + 1>; // the last: preview_slot

    SamplerEngine();
    void set_samples(const Table* table) { samples = table; }
    void set_pager(StreamPager* streams) { pager = streams; } // nullptr: instruments' frames are all safe to read (tests)
    // A pad's sound. What the pad plays now hears it too (at the next render, gliding there: refresh), while it
    // plays the same sample: its level, pan, tune, envelopes, filter, speed, play mode, region and loop.
    void set_pad(unsigned pad, const PadSound& sound) { if (pad < sampler_pads) { pads[pad] = base[pad] = sound; mark(pad); } }
    bool pad_used(unsigned pad) const { return pad < sampler_pads && pads[pad].used(); } // a sample on it
    // Round 3 "instrument C": the track's zone trims (model/sampler.h), as a whole or one; a zone of pad `pad`
    // at UINT32_MAX lets every trim of the pad go. A zone sounding hears its trim at once.
    void set_trims(const ZoneTrims& all) { trims = all; retune_all = true; }
    void set_trim(unsigned pad, uint32_t zone, unsigned slot, int tune, int level) {
        if (zone == UINT32_MAX) clear_trims(trims, pad);
        else melodee::set_trim(trims, pad, zone, slot, tune, level);
        mark(pad);
    }
    // Motion on a pad's own values (model/pad_params.h): the value plays
    // until the pad's lane moves again or restore_pads() gives every pad its
    // own values back (the pattern's start, Stop). What the pad plays hears it.
    void move_pad(unsigned pad, PadParam which, int value) {
        if (pad >= sampler_pads) return;
        set_pad_param(pads[pad], which, value);
        moved = true;
        mark(pad);
    }
    void restore_pads() { if (moved) { pads = base; moved = false; retune_all = true; } }
    // One of a pad's own values back to the kit's (a Motion lock ending, roadmap P1).
    void restore_pad(unsigned pad, PadParam which) {
        if (pad < sampler_pads) { set_pad_param(pads[pad], which, pad_param_value(base[pad], which)); mark(pad); }
    }
    // Mute: a muted pad does not start and what it plays fades out now.
    void set_pad_mute(unsigned pad, bool on);
    void set_pad_mutes(const std::array<uint64_t, (sampler_pads + 63) / 64>& bits);
    bool pad_muted(unsigned pad) const { return pad < sampler_pads && ((muted[pad / 64] >> (pad % 64)) & 1); }
    // A region of a sample on no pad, for Chop's audition (plays one shot).
    void audition(uint16_t sample, uint32_t start, uint32_t end, int velocity);
    // A sample is about to change: whatever plays it stops now.
    void sample_gone(uint16_t sample);
    // A kit pad shifted by `semitones`, its filter at `cutoff` (0..100; -1 its
    // own): 16 Levels. note_off for the pad's note releases it.
    void play_pad(unsigned pad, int velocity, int semitones, int cutoff = -1);

    void note_on(int key, int velocity) override;
    void set_accent(float gain) override { accent = gain; }
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void render(float* out, unsigned frames) override; // mono fold, for callers that want one channel
    bool relieve(bool steal) override; // the overload guard: released pads choked, with `steal` the oldest voice too
    bool stereo() const override { return true; }
    void render_stereo(float* left, float* right, unsigned frames) override;
    bool sounding() const override;
    // What the voices show the screen (engine/voice_tap.h): the newest
    // TrackTaps::voices of them, then the cloud's grains. Audio thread, after render.
    void taps(TrackTaps& out) const;
    // The project's tempo, for Follow (tenths of a BPM).
    void set_tempo(unsigned tenths) { if (tenths != tempo) { tempo = tenths; tempo_moved = true; } }
    // Pad Punch: the voices of `pad` go through `punch` on their own before
    // they join the kit (nullptr: none). The punch belongs to the host.
    void punch_pad(int pad, Punch* punch) { aside_pad = pad; aside = punch; }
    // PadFX: each pad's own chain (the host's). A pad with a chain plays
    // through it on a lane of its own, up to `lanes` pads at once; a lane
    // stays with its pad three seconds after the pad falls silent, for tails.
    using PadEffects = std::array<std::array<Effect, pad_slots>, sampler_pads>;
    void set_pad_effects(PadEffects* effects) { pad_effects = effects; }
    static constexpr unsigned lanes = 8;
    // Knob FX (parity phase 7): the voices of these pads (a bit a pad) play
    // into the host's l / r instead, after their own chain and punch, added to
    // what is there, for the host to put through Knob FX. No bits: none. Like
    // the Sidechain's key pad, the host hands this sub-block's place in its own
    // buffers before each render; nullptr: none.
    void set_knob_pads(uint64_t low, uint64_t high) { knob_mask[0] = low; knob_mask[1] = high; }
    void set_knob_out(float* l, float* r) { knob_l = l; knob_r = r; }
    bool knob_fed() const { return knob_used; } // the last render put something there
    // Sidechain: this kit pad's voices (after its own chain and punch) play into l / r instead of the kit,
    // added to what is there; the host hands this sub-block's place in its own buffers before each render
    // (a render can be part of the host's block). -1: none.
    void set_key_pad(int pad, float* l, float* r) { key_pad = l && r ? pad : -1; key_l = l; key_r = r; }

private:
    struct Voice {
        int pad = -1; // -1 free; sampler_pads: the audition voice's region
        int trigger = -1; // the pad whose hit started it: its own, or one linked to it (its note off releases it)
        const SampleData* sample = nullptr;
        const float* l = nullptr;
        const float* r = nullptr;
        uint16_t slot = no_sample;
        double position = 0, step = 1;
        uint32_t start = 0, end = 0;
        bool reverse = false, held = false, gate = false, choked = false, cloud = false;
        double next_grain = 0, scan = 0; // Cloud: samples to the next grain; how far the place has moved
        double life = 0; // Cloud, a one shot: output frames left of the time the pad would play for (0: until let go or hit again)
        // Stretch (Follow by Beats or Tones, or a pitch kept to length): `place`
        // moves through the sample at `advance`; taps read from where they
        // began at `step` (the pitch), each faded over `window`, a new one every `hop`.
        bool stretch = false, beats = false;
        double place = 0, advance = 1;
        std::array<double, 2> tap {};
        std::array<uint32_t, 2> tap_age {};
        std::array<bool, 2> tap_on {};
        uint32_t window = 1, hop = 1, since_hop = 0;
        unsigned next_tap = 0;
        uint8_t group = 0;
        float gain = 0, left = 0.7071f, right = 0.7071f, fade = 1;
        uint32_t age = 0;
        double scale = 1; // its frames a 44.1 kHz frame: above 1 when it plays a copy at the device's rate (roadmap P2)
        uint32_t sounded = 0;   // output frames it has played (Offset's wait not counted), for its tap
        bool wrapped = false;   // went round its loop in this block
        dsp::Envelope env;
        dsp::Envelope::Shape shape;
        // Loop: after the end (or, reversed, the loop point) it goes on from the loop point (or the end).
        bool loop = false;
        uint32_t loop_at = 0;
        // Roadmap P3, a kit pad's loop seam crossfaded over `xfade` frames of what it reads: 1 with what lies before
        // the loop point (reversed: past the end), at the seam's near side; 2 after the turn, with what follows the
        // end (reversed: before the loop point); 3, with neither in the file, a dip either side. `turned` once it has
        // gone round; `total` the frames it may read.
        uint8_t xfade_kind = 0;
        bool turned = false;
        uint32_t xfade = 0, total = 0;
        // A one shot's region edges inside the file (a chop's slices, a trimmed
        // pad): faded over slice_edge so a cut mid-wave does not click.
        bool edge_in = false, edge_out = false;
        // A zone of an instrument: its note, what cuts it, its loop's ends, and its stream.
        bool zone = false, loop_held = false, capped = false, starved = false, played = false;
        int key = 0, semis = 0, stream = -1;
        uint32_t off_by = 0, loop_hi = 0, head_end = 0;
        float duck = 1, duck_to = 1;        // a gap's short fades
        uint32_t wait = 0;                  // Offset: frames before it starts
        float tail = 0, filter_tail = 0;    // Decay from the end: frames of output it fades (and the filter closes) over
        // The pad's filter: two state-variable stages a side, and its envelope.
        PadFilter filter = PadFilter::Off;
        float cutoff = 0, depth = 0;        // octaves above 20 Hz; how far the envelope opens it
        float k1 = 1.4f, k2 = 1.4f, fg = 0.1f;
        float fs[2][2][2] {};               // [stage][side][s1, s2]
        unsigned since_tune = 0;
        dsp::Envelope filter_env;
        dsp::Envelope::Shape filter_shape;
        // What its pad and the kit ask of it now (aim): a turned knob, Motion, a modulator or the tempo moves these
        // while it plays, and the values above glide there over `gliding` more frames (glide), so the change is
        // heard at once, without a step. What it was started with stays: how hard, the note, 16 Levels' cutoff,
        // the sequencer's accent, its layer and zone, a release sample's fall.
        struct Aim {
            double step = 1, advance = 1;
            float gain = 0, left = 0.7071f, right = 0.7071f;
            float cutoff = 0, depth = 0, k1 = 1.4f, k2 = 1.4f, tail = 0, filter_tail = 0;
            bool beats = false; // stretched, it would play Beats' whole segments (else Tones' windows)
        } aim;
        uint32_t gliding = 0;
        uint8_t velocity = 0;
        int8_t layer = -1;                  // the pad's layer it plays (layers_on), -1 the pad itself
        int16_t levels_cutoff = -1;         // 16 Levels' cutoff it was hit with, -1 the pad's own
        float accent = 1, fall = 1;         // the sequencer's accent; a release sample's rt_decay
        bool native = false;                // reads the sample's copy at the device's rate (roadmap P2)
        const SampleGrowth* grow = nullptr; // its sample is still being read: it reads only what landed (admit_grown)
        bool follows = false;               // its speed comes from the tempo (Warp's Seq, Follow)
        Multisample* multi = nullptr;       // a zone's instrument, and the zone's place in it
        uint32_t zone_index = 0;
    };
    // Each pad whose sound changed since the last render (a bit a pad); `retune_all` for the kit's values.
    std::array<uint64_t, (sampler_pads + 63) / 64> retune {};
    bool retune_all = false, tempo_moved = false;
    void mark(unsigned pad) { if (pad < sampler_pads) retune[pad / 64] |= uint64_t(1) << (pad % 64); }
    bool marked(int pad) const { return retune_all || ((retune[unsigned(pad) / 64] >> (unsigned(pad) % 64)) & 1); }
    // The glide: the share of the way left it goes a frame, and how many frames it takes, at the device's rate now.
    float glide_rate = 0, glide_step = 0;
    uint32_t glide_frames = 0;
    void glide_setup();
    void aim(Voice& v, const PadSound& sound, const SampleZone* zone, Multisample* multi, int key);
    void settle(Voice& v);
    void glide(Voice& v);
    void reach(Voice& v, const PadSound& sound);      // a kit pad's region as the pad has it now, round where it plays
    void loop_setup(Voice& v, const PadSound& sound); // its loop over its region, the edges' fades, the seam's crossfade
    void begin_stretch(Voice& v, bool live);
    void refresh(Voice& v);
    void retune_voices();
    void hit(int pad, int velocity, int semitones, int cutoff);
    void start(Voice& v, const PadSound& sound, int pad, int velocity, int semitones = 0, int cutoff = -1, int trigger = -1,
               const SampleZone* zone = nullptr, Multisample* multi = nullptr, int key = 60);
    void play(const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger);
    void zones_on(Multisample& multi, const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger);
    void layers_on(const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger);
    void release_zones(int pad, int semitones); // roadmap P3: an instrument's release samples for the key let go
    Multisample* multi_of(const PadSound& sound) const;
    void end_stream(Voice& v);
    void came_round(Voice& v);          // a zone's loop went back to its start
    bool admit(Voice& v, unsigned frames); // whether its next frames are safe to read
    bool admit_grown(Voice& v, unsigned frames); // the same for a sample still being read (SampleGrowth)
    void play_zone(Voice& v, float* to_l, float* to_r, unsigned from, unsigned frames);
    float frames_left(const Voice& v, double at, double speed) const; // output frames to the end, for the fades
    static float edge_gain(const Voice& v, double at);                // the region edges' fades, 0..1
    static void seam(const Voice& v, const float* l, const float* r, double at, float& out_l, float& out_r); // a loop's crossfade
    void voice_out(Voice& v, float l, float r, float a, float left_frames, float* to_l, float* to_r, unsigned f);
    Voice& free_voice();

    struct Grain {
        const SampleData* sample = nullptr;
        double position = 0, step = 1;
        uint32_t age = 0, length = 0;
        float gain = 0, left = 0.7071f, right = 0.7071f;
        const float* l = nullptr;
        const float* r = nullptr;
        uint32_t low = 0, top = 1;      // the frames it may read, [low, top): its pad's region, or what an instrument holds
        uint32_t serial = 0;            // for the screen (taps): which grain, where it began, whose
        double from = 0;
        int16_t pad = -1;
        uint16_t slot = 0xffff;
        double scale = 1; // its voice's (Voice::scale)
    };
    void spawn(Voice& v);
    std::array<Grain, 48> grains {};
    uint32_t grain_serial = 0;
    unsigned tempo = 1200;
    uint32_t seed = 99;
    // A choke's fade and an instrument zone's duck move this far a frame: 64 44.1 kHz frames (1.5 ms) at any rate.
    float edge_step = 1.0f / 64;
    const Table* samples = nullptr;
    StreamPager* pager = nullptr;
    std::array<PadSound, sampler_pads> pads {}, base {}; // as they play (motion), and as the kit holds them
    ZoneTrims trims {};
    std::array<uint8_t, sampler_pads> layer_turn {};    // Cycle: the layer each pad plays next
    // Roadmap P3, an instrument's release samples: each key's last press, how hard and when (frames played).
    std::array<uint8_t, 128> key_velocity {};
    std::array<uint64_t, 128> key_since {};
    uint64_t played_frames = 0;
    bool moved = false;
    std::array<uint64_t, (sampler_pads + 63) / 64> muted {};
    std::array<Voice, max_voices + spare_voices> voices {};
    std::array<int, sampler::Count> p {};
    uint32_t clock = 0;
    float accent = 1; // the sequencer's, for the voices started next (set_accent)
    float g = 1, k = 2, s1[2] {}, s2[2] {};
    bool filtered = false;
    int aside_pad = -1;
    Punch* aside = nullptr;
    float aside_l[128] {}, aside_r[128] {};
    // A pad's chain plays on in its lane after the pad falls silent, until its own tail has been quiet for
    // lane_quiet_seconds (longer than any echo's time, so a late repeat is not lost) or lane_ring_seconds have gone.
    static constexpr float lane_quiet_seconds = 1.5f, lane_ring_seconds = 30.0f, lane_floor = 1e-5f;
    struct Lane { int pad = -1; uint32_t quiet = 0, ringing = 0; bool fed = false; float l[128] {}, r[128] {}; };
    PadEffects* pad_effects = nullptr;
    uint64_t knob_mask[2] {};
    bool knob_used = false;
    float* knob_l = nullptr;
    float* knob_r = nullptr;
    int key_pad = -1;
    float* key_l = nullptr;
    float* key_r = nullptr;
    bool knobbed(int pad) const { return pad >= 0 && pad < 128 && ((knob_mask[pad / 64] >> (pad % 64)) & 1); }
    std::array<Lane, lanes> lane {};
    Lane* lane_for(int pad); // the pad's lane, a free one, or nullptr
    void stretch(Voice& v, const float* l, const float* r, float* to_l, float* to_r, unsigned from, unsigned frames);
};

} // namespace melodee
