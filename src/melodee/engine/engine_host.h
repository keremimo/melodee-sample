// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/arp.h"
#include "melodee/engine/command.h"
#include "melodee/engine/cz.h"
#include "melodee/engine/fm6.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/looper.h"
#include "melodee/engine/master_comp.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/plugin_engine.h"
#include "melodee/engine/sid.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/prophet.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/render_pool.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/engine/bounce.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/midi_out.h"
#include "melodee/model/input_config.h"
#include "melodee/model/project.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// The audio side of Melodee: sixteen track slots, each with its engine and
// mix strip, a mirror of every pattern, and the sequencer clock. The shell
// sends Commands from the MIDI thread; render() runs on the audio thread and
// never allocates, locks or waits. What the screen needs back (playhead,
// meters) is published as atomics.
namespace melodee {

// One track's sixteen patterns as the audio thread plays them: copies made
// on the MIDI thread, swapped in by pointer and handed back to be freed
// there, so the audio thread never allocates or frees.
using PlayPatterns = std::array<Pattern*, pattern_count>;
PlayPatterns copy_patterns(const std::array<Pattern, pattern_count>& patterns);
void free_patterns(PlayPatterns& patterns);

// A whole project for the audio thread to switch to at once (a load or a new
// project). The MIDI thread makes it, the audio thread swaps its patterns
// for the ones it played and hands it back, to be freed with those.
struct ProjectPack {
    ProjectPack(const Project& project, const PatternBank& bank);
    ~ProjectPack();
    ProjectPack(const ProjectPack&) = delete;
    ProjectPack& operator=(const ProjectPack&) = delete;
    Project project;
    std::array<PlayPatterns, track_count> patterns {};
};

// One track for the audio thread to take whole, without stopping (a track
// copy, or its undo): the track and its sixteen patterns, their motion in
// them. Made and freed like a ProjectPack.
struct TrackPack {
    TrackPack(unsigned track, const Track& value, const std::array<Pattern, pattern_count>& patterns);
    ~TrackPack();
    TrackPack(const TrackPack&) = delete;
    TrackPack& operator=(const TrackPack&) = delete;
    uint8_t track = 0;
    Track value;
    PlayPatterns patterns {};
};

class EngineHost {
public:
    EngineHost();
    ~EngineHost();
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;
    // Before the audio thread starts: the whole project and its patterns.
    void load(const Project& project, const PatternBank& patterns);

    // MIDI thread only. False when the queue is full (the change is lost).
    bool send(const Command& command) {
        if (queue.push(command)) return true;
        lost.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // MIDI thread: a note played at `ns` (CLOCK_MONOTONIC, the pad's or the MIDI message's receive stamp). It plays
    // a block after the frame being rendered then, at that frame (Command::timed), so two hits keep their spacing
    // to the frame instead of moving to block starts. Untimed (the next block's start) while the clock is unknown.
    bool send_at(Command command, uint64_t ns) {
        if (ns && clock.known()) { command.timed = true; command.at = uint32_t(clock.frame_at(int64_t(ns)) + block); }
        return send(command);
    }
    // Any thread: where `track` was in its pattern when a hit at `ns` was heard (its stamp less the output latency),
    // to the nearest tick, counted back from position_of() (so it may come out below 0: wrap it by the pattern's
    // length), and ticks() then. False while no tick of this run is known for it. place_at: the same for the frame
    // rendered then.
    bool heard_place(unsigned track, uint64_t ns, int& position, uint32_t& ticks) const;
    bool place_at(unsigned track, uint64_t frame, int& position, uint32_t& ticks) const;
    // Any thread: timed notes played on their frame, those that came after it (played at once), and the latest by.
    uint32_t timed_kept() const { return timed_on_time.load(std::memory_order_relaxed); }
    // Any thread (roadmap P1): ratchets' hits played after their first; notes a trig condition let through and held back.
    uint32_t ratchet_hits() const { return hits_played.load(std::memory_order_relaxed); }
    uint32_t trigs_kept() const { return trig_kept.load(std::memory_order_relaxed); }
    uint32_t trigs_dropped() const { return trig_dropped.load(std::memory_order_relaxed); }
    bool filling() const { return fill_now.load(std::memory_order_relaxed); }
    uint32_t timed_missed() const { return timed_late.load(std::memory_order_relaxed); }
    uint32_t timed_worst_frames() const { return timed_worst.load(std::memory_order_relaxed); }

    // Audio thread: `frames` interleaved stereo S32 frames, after `volume`;
    // `input` the capture block (silent when there is none), for Record.
    // `stems`, when given, gets each track's stem: `track_count` rows of
    // `block` floats, the track after its chain and fader as you hear it,
    // summed to mono with its pan law taken back out, before the buses, the
    // master chain and main volume. Muted (or soloed-out) tracks are silent.
    // `usb`, when given, is what a computer plays over USB-C: `frames`
    // interleaved stereo floats, for the USB sources.
    void render(int32_t* out, unsigned frames, float volume, const int32_t* input = nullptr, float* stems = nullptr,
                const float* usb = nullptr);
    // Before the audio thread starts: `count` render workers beside it (0: it renders alone), at its real-time
    // priority. Each block's tracks, their engines and then their chains, are shared out among them once the
    // last block's took more than `from_ns` (all cores together; 0 always, for tests).
    void set_render_threads(unsigned count, uint64_t from_ns = 150000);
    unsigned render_threads() const { return workers.workers(); }
    // Any thread: the workers used (true) or left asleep, from the next block; for comparing the two.
    void set_render_parallel(bool on) { parallel_allowed.store(on, std::memory_order_relaxed); }
    bool render_parallel() const { return parallel_allowed.load(std::memory_order_relaxed); }
    // Any thread: the share of its time the latest blocks took (the peak, falling slowly), and how many blocks the
    // workers have helped with.
    float render_load() const { return block_load.load(std::memory_order_relaxed); }
    uint64_t render_parallel_blocks() const { return parallel_blocks.load(std::memory_order_relaxed); }
    // The Looper (docs/LOOPER.md): its loops play into the tracks L1-L4, after the sixteen.
    Looper& looper() { return loops; }
    const Looper& looper() const { return loops; }
    float loop_meter(unsigned loop) const { return loop_meters[loop % loop_tracks].load(std::memory_order_relaxed); }
    TakeRecorder& recorder() { return take; }
    const TakeRecorder& recorder() const { return take; }
    RecallRing& recall() { return recall_ring; }
    const RecallRing& recall() const { return recall_ring; }
    Bounce& bounce() { return render_out; }
    const Bounce& bounce() const { return render_out; }
    unsigned record_source() const { return source_now.load(std::memory_order_relaxed); } // InputSource
    unsigned record_track() const { return source_track.load(std::memory_order_relaxed); } // InputSource::Track: which
    bool record_post() const { return source_post.load(std::memory_order_relaxed); }      // and after its chain
    bool monitoring() const { return monitor_on.load(std::memory_order_relaxed); }

    // Feedback, any thread.
    bool playing() const { return is_playing.load(std::memory_order_relaxed); }
    uint32_t ticks() const { return tick_count.load(std::memory_order_relaxed); } // clock ticks since Play, 96 a quarter
    // Where a track is in its pattern, in ticks; -1 while it plays none.
    int position_of(unsigned track) const { return positions_now[track % track_count].load(std::memory_order_relaxed); }
    unsigned count_in() const { return count_beat.load(std::memory_order_relaxed); } // the count-in's beat now, 1..; 0 none
    unsigned tempo_now() const { return tempo_shown.load(std::memory_order_relaxed); } // tenths of a BPM, as it plays
    unsigned pattern_of(unsigned track) const { return patterns_now[track % track_count].load(std::memory_order_relaxed); }
    bool punching() const { return punch_on.load(std::memory_order_relaxed); }
    float compression() const { return comp.reduction_db(); } // dB the master compressor takes off now
    void compressor_activity(float& level_db, float& reduce_db) { comp.take_activity(level_db, reduce_db); } // the page's picture
    unsigned flex_playing() const { return flex_now.load(std::memory_order_relaxed); } // Flex Beat's pad playing, 0 Empty
    unsigned live_punches() const { return live_on.load(std::memory_order_relaxed); }  // Pad FX: a bit a live slot sounding
    bool song_playing() const { return in_song.load(std::memory_order_relaxed); }
    unsigned song_bar() const { return bar_now.load(std::memory_order_relaxed); }
    float meter(unsigned track) const { return meters[track % track_count].load(std::memory_order_relaxed); }
    // The voices of the track the screen watches (engine/voice_tap.h), as the last block left them; false while a
    // write was under way each time it looked. watch_taps(-1): none taken.
    void watch_taps(int track) { tap_track.store(track, std::memory_order_relaxed); }
    bool taps(TrackTaps& out) const { return tap_board.read(out); }
    float peak() const { return output_peak.load(std::memory_order_relaxed); }
    // The meter lights beside the screen: the mix (before main volume, the listening level) and the input source,
    // side 0 left, 1 right, as peaks falling meter_fall_db a second, so the 5 ms light pass misses none.
    static constexpr float meter_fall_db = 30;
    float out_level(unsigned side) const { return out_levels[side & 1].load(std::memory_order_relaxed); }
    float in_level(unsigned side) const { return in_levels[side & 1].load(std::memory_order_relaxed); }
    unsigned dropped() const { return lost.load(std::memory_order_relaxed); }

    // Service thread: a loaded sample for slot `slot` (nullptr empties it). The
    // audio thread swaps it in at its next block and hands the one it replaced
    // back through take_retired(), to be freed there.
    bool offer_sample(unsigned slot, const SampleData* sample) { return offers.push({uint16_t(slot), sample}); }
    StreamPager& streams() { return pager; } // multisampled instruments' reading from the card
    bool take_retired(const SampleData*& sample) { return retired.pop(sample); }
    // The rack's thread (plugin/plugin_rack.h): the plug-in for entry `entry` of Project::plugins (nullptr: none). The
    // audio thread swaps it in at its next block, after the commands sent before it, and hands the one it replaced
    // back through take_retired_plugin(), to be freed there.
    bool offer_plugin(unsigned entry, PluginRunner* runner) { return plugin_offers.push({uint8_t(entry), runner}); }
    bool take_retired_plugin(PluginRunner*& runner) { return retired_plugins.pop(runner); }
    // MIDI thread: a ProjectPack the audio thread has finished with, to free.
    bool take_done_pack(const ProjectPack*& pack) { return done_packs.pop(pack); }
    bool take_done_track(const TrackPack*& pack) { return done_tracks.pop(pack); }
    // MIDI thread (parity phase 9): what goes out, each at its frame, and the clock that says when that frame is heard.
    bool take_midi(MidiOutEvent& event) { return midi_events.pop(event); }
    const AudioClock& audio_clock() const { return clock; }
    // Audio thread, before the first block at each open of the device (and before it starts): its frames a second and
    // the frames it holds queued (MIDI out lines up with them). A new rate silences render() until the project comes
    // again (a Reload: every engine and effect worked out anew at it); format_changes() tells the shell to send it.
    void set_audio_format(double frames_a_second, uint64_t latency_frames);
    unsigned format_changes() const { return format_serial.load(std::memory_order_acquire); }
    unsigned midi_dropped() const { return midi_lost.load(std::memory_order_relaxed); }
    bool following() const { return ext_now.load(std::memory_order_relaxed); } // the sequencer waits for MIDI clock in
    // MTC in (parity phase 9b): the song placed and kept on time by it; how far off it is (filtered) and the pace trim.
    bool mtc_following() const { return mtc_now.load(std::memory_order_relaxed); }
    int mtc_error_us() const { return mtc_error_now.load(std::memory_order_relaxed); }
    int mtc_trim_ppm() const { return mtc_ppm.load(std::memory_order_relaxed); }
    unsigned mtc_places() const { return mtc_placed.load(std::memory_order_relaxed); } // locates done (Start, jumps, catch-ups)
    // MIDI thread: a pattern copy the audio thread played until a newer one came (PatternData), to free.
    bool take_retired_pattern(Pattern*& pattern) { return retired_patterns.pop(pattern); }

    static constexpr unsigned block = 128;

private:
    struct PendingOff { int32_t key = -1; int32_t samples = 0; }; // a pad's key, with an instrument's semitones over it
    struct PendingOn { int16_t key = -1, velocity = 0, from = -1; int32_t samples = 0, length = 0; }; // an arp note swung late
    // A ratchet's hit still to come (roadmap P1): `samples` frames on it plays `key` (as the engine plays it; a kit
    // pad's semitones over it) and `midi` out, for `length` frames.
    struct PendingHit { int32_t key = -1; int16_t midi = -1; uint8_t velocity = 0; bool accent = false; int32_t samples = 0, length = 0; };
    struct Slot {
        EngineKind kind = EngineKind::None;
        ProphetEngine prophet;
        SamplerEngine sampler;
        Kit909Engine kit;
        Fm6Engine fm;
        SidEngine sid;
        CzEngine cz;
        MidiEngine midi; // silent: a MIDI track (docs/MIDI_TRACKS.md)
        PluginEngine plugin;             // a PLUGIN track: the rack's plug-in for `plugin_entry`
        uint8_t plugin_entry = no_plugin;
        Engine* engine = nullptr;
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f;
        std::array<float, bus_count> sends {}; // into each bus A-D (round 2: four sends a track)
        bool mute = false, solo = false;
        uint8_t pattern = 0, queued = 255; // queued: launches on the next bar
        uint32_t start = 0;                // the tick its pattern started on
        int clip = -1;                     // in the song: the clip playing
        bool silent = false;               // in the song: no clip here
        std::array<PendingOff, 32> offs {};
        // A note with Slide or Tie, held into the next note of its pattern, which takes it over (key: as the
        // engine plays it; midi: as it went out). Its off, a sixteenth past that note, lets it go if none comes.
        struct Held { int16_t key = -1, midi = -1; bool glide = false; } held;
        std::array<int16_t, max_params> base {}; // the track's own values; motion only borrows them
        std::array<std::array<int16_t, effect_params>, max_slots> fx_base {}; // and its effects'
        bool fx_moved = false;
        bool moved = false;                       // motion changed a value since Play
        float wheel = 0;                          // the mod wheel as the fader left it, 0..1
        bool wheel_moved = false;                 // and motion moved it since (parity phase 8)
        // Motion's locks playing (roadmap P1): a lane, and the ticks before it goes back.
        struct Lock { int16_t param = -1; uint8_t pad = 0; uint32_t left = 0; };
        std::array<Lock, 16> locks {};
        unsigned locks_on = 0;
        float buffer[block] {};
        float left[block] {}, right[block] {};
        float knob_pad_l[block] {}, knob_pad_r[block] {}; // a kit's pads under Knob FX, as the sampler left them
        bool knob_pad_fed = false;                         // and some sub-block of this block put something there
        Arp arp;
        PendingOn arp_late;
        std::array<PendingHit, 32> hits {}; // ratchets' hits to come, each on its frame
        unsigned hits_waiting = 0;
        // Trig conditions (model/trig.h): how the last weighed note went, on the track and on each key (a kit's
        // lanes), and whether one has been weighed since Play (none yet: counted as played, for NEI).
        bool weighed = false, went = false;
        std::array<uint64_t, 4> keys_weighed {}, keys_went {};
        int arp_played = -1, arp_from = -1; // the arp's last note and the held key it came from
        // Notes Seq Record just wrote ahead of the playhead (Rec Quantize), already heard live: skipped once.
        struct Skip { int32_t key = -1; uint8_t pattern = 0; uint32_t tick = 0, until = 0; };
        std::array<Skip, 8> skips {};
    };
    void load_track(unsigned t, const Track& track); // a whole track's sound, kit, mix and effects
    void load_state(const Project& project, bool keep_loops = false); // all but the patterns (keep_loops: the same project at a new rate)
    void run_late(Slot& slot, unsigned frames);       // a swung arp note, when its time comes
    // Ratchets: the hits due now (at block_at) play, the rest come `frames` nearer; the frames to the next one due
    // (the render loop splits there, so each lands on its frame); and every one dropped (Stop, Play).
    void run_hits(unsigned t, Slot& slot, unsigned frames);
    unsigned next_hit() const;
    void drop_hits();
    void play_hit(unsigned t, Slot& slot, const PendingHit& hit, uint64_t at);
    // Trig conditions: whether `e` plays on pass `pass` of track `t` (its chance too), and how it went kept for PRE and NEI.
    bool trig_plays(unsigned t, Slot& slot, const Event& e, uint32_t pass);
    bool fill = false; // Fill held (Command::Fill)
    void arp_note(Slot& slot, int key, int velocity, int from, int32_t length); // with its pad's pressure
    void apply(const Command& command);
    void select_engine(Slot& slot, EngineKind kind);
    void tick();       // plays whatever falls on this clock tick, then moves on
    void arp_tick();   // the arps' own clock, running whether the song plays or not
    void bar_start();  // launches queued patterns, or follows the song
    double tick_samples(uint32_t at) const;
    void play_motion(unsigned track, Slot& slot, const Pattern& pattern, uint32_t at);
    void move_lane(unsigned track, Slot& slot, unsigned param, unsigned pad, int value); // a Motion lane to `value`
    void restore_lane(unsigned track, Slot& slot, unsigned param, unsigned pad);        // and back to the track's own
    void restore(Slot& slot);
    void schedule_off(Slot& slot, int key, int32_t samples);
    void run_offs(Slot& slot, unsigned frames);
    void cancel_off(Slot& slot, int key);
    double tick_length() const { return dsp::sample_rate() * 60.0 / (tempo / 10.0) / ticks_per_quarter; } // device frames a clock tick
    double arp_tick_length() const { return tick_length() * (ticks_per_quarter / arp_ticks_per_quarter); }
    // MIDI out and sync (parity phase 9).
    void emit(const MidiOutEvent& e) { if (!midi_events.push(e)) midi_lost.fetch_add(1, std::memory_order_relaxed); }
    bool out_wanted(unsigned t, int key) const;                              // this note of this track goes out
    bool heard(unsigned t) const;                                            // not muted, nor silenced by another's solo
    bool is_midi(unsigned t) const { return slots[t % track_count].kind == EngineKind::Midi; }
    void midi_silenced();                                                    // MIDI tracks muted or soloed out: their notes end
    void midi_note(unsigned t, int key, int velocity, uint64_t at, int64_t length); // and its off after `length` frames (-1 none)
    void midi_note_off(unsigned t, int key, uint64_t at);
    bool midi_hold(unsigned t, int key, int64_t length, uint64_t at); // a tie: the note sounding goes on `length` frames from `at`
    void midi_offs_due(uint64_t until);                                      // offs before `until`, at their frames
    void midi_all_off(unsigned track, uint64_t at);                         // 255: every track's
    // A MIDI track's own parameter beyond port and channel: a controller value goes out as a CC; bank and
    // program (any of the three) as the bank selects that are on, then the program, when it is on.
    void midi_param(unsigned t, int id, int value, uint64_t at);
    void midi_program(unsigned t, uint64_t at);
    void sync_stop(uint64_t at);
    void ext_release(uint64_t at);                                           // clock pulses whose frame has come
    std::atomic<unsigned> format_serial {0};
    bool reformatting = false; // audio thread: the rate changed and the Reload has not come yet
    MidiQueue<1024, MidiOutEvent> midi_events;
    std::atomic<unsigned> midi_lost {0};
    AudioClock clock;
    uint64_t frame_now = 0;                                                  // frames rendered before this block
    std::array<uint8_t, track_count> out_channel {}, out_mode {};
    struct MidiOff { bool on = false; uint8_t channel = 1, key = 0, track = 0; uint64_t at = 0; MidiDest dest = MidiDest::Config; };
    static MidiOutEvent off_event(const MidiOff& off, uint64_t at) {
        MidiOutEvent e {MidiOutEvent::Kind::NoteOff, off.channel, off.key, 0, 0, at};
        e.dest = off.dest;
        if (off.dest != MidiDest::Config) e.track = off.track;
        return e;
    }
    std::array<MidiOff, 128> midi_offs {};
    SyncOut sync;
    bool sync_started = false;                                                // the song's clocks are going (after any count-in)
    bool sync_clock = false, sync_mtc = false, sync_cv = false;
    unsigned sync_ppqn = 2;
    bool ext_on = false, ext_gate = false;                                    // MIDI clock in: followed; the sequencer waits for it
    uint32_t ext_clocks = 0, ext_base = 0;                                   // pulses let through since Start; the tick it began on
    uint16_t ext_tempo = 0;
    std::array<uint32_t, 64> ext_ring {};                                     // pulses not let through yet: their frames
    unsigned ext_head = 0, ext_tail = 0;
    std::atomic<bool> ext_now {false};
    // MTC in (parity phase 9b). Time code says where the song should be, not its tempo: the sequencer runs on its own
    // tempo, a hair faster or slower (`speed`, within 0.5 %) to stay on the master's time; far off, it is placed again.
    bool mtc_on = false;
    double mtc_at = 0;    // the song's seconds at frame_now, as the sequencer has run them
    double mtc_error = 0; // seconds the master is ahead of the song, filtered against jitter
    double speed = 1;     // the sequencer's pace against the audio clock
    unsigned mtc_misses = 0;
    std::atomic<bool> mtc_now {false};
    std::atomic<int> mtc_error_now {0}, mtc_ppm {0};
    std::atomic<unsigned> mtc_placed {0};
    void mtc_place(double seconds); // the song (or the patterns playing) to where `seconds` falls at the tempo playing now
    void mtc_off();
    void clips_at_bar(bool force);    // the song's clips for song_bar_now (a clip from where it stands in the bar)
    double quarter_samples() const { return tick_length() * ticks_per_quarter; }
    void follow_tempo();  // the tempo now: the global one, or the lead pattern's own (BPM per pattern)
    bool skipped(Slot& slot, unsigned key, uint32_t tick);

    MidiQueue<4096, Command> queue;
    // The first command not applied yet: a timed note before its frame, or one sent after it, waiting behind it.
    Command waiting {};
    bool has_waiting = false;
    unsigned take_commands(unsigned offset, unsigned frames); // those due by `offset`; the offset the next one waits for
    static bool mid_block(Command::Type type);                // applied inside a block too (notes and values)
    static constexpr int32_t timed_reach = 3 * 128;           // further ahead than a hit can be (three blocks): at once
    // Each tick of a pattern playing: its frame (low 32 bits) and tick_now, for place_at(). The audio thread writes
    // the ring, any thread reads it.
    static constexpr unsigned tick_ring_size = 256;
    std::array<std::atomic<uint64_t>, tick_ring_size> tick_ring {};
    std::atomic<uint32_t> tick_head {0};  // ticks written
    std::atomic<float> tick_frames {0};   // frames the newest tick lasts (swing and MTC's pace in)
    std::atomic<uint32_t> timed_on_time {0}, timed_late {0}, timed_worst {0};
    std::atomic<uint32_t> hits_played {0}, trig_kept {0}, trig_dropped {0};
    std::atomic<bool> fill_now {false};
    // Plug-ins: each entry's as the rack offered it; the ones replaced wait in the queue to be freed.
    struct PluginOffer { uint8_t entry = 0; PluginRunner* runner = nullptr; };
    MidiQueue<64, PluginOffer> plugin_offers;
    MidiQueue<64, PluginRunner*> retired_plugins;
    std::array<PluginRunner*, max_plugins> runners {};
    std::array<PluginRunner*, 16> plugin_limbo {}; // replaced ones the queue had no room for yet
    PluginRunner* runner_of(unsigned entry) const { return entry < max_plugins ? runners[entry] : nullptr; }
    void take_plugins(); // the offers waiting, at the start of a block
    struct Offer { uint16_t slot = 0; const SampleData* sample = nullptr; };
    MidiQueue<64, Offer> offers;
    MidiQueue<128, const SampleData*> retired;
    MidiQueue<8, const ProjectPack*> done_packs;
    MidiQueue<8, const TrackPack*> done_tracks;
    MidiQueue<1024, Pattern*> retired_patterns;
    std::array<Pattern*, 64> limbo {}; // retired ones the queue had no room for yet
    void retire(Pattern* pattern);
    SamplerEngine::Table samples {};
    StreamPager pager;
    TakeRecorder take;
    RecallRing recall_ring; // Audio Recall: the source, always
    Bounce render_out;      // Resample and the song's mixdown
    std::atomic<unsigned> source_now {1};
    std::atomic<unsigned> source_track {0};
    std::atomic<bool> source_post {true};
    uint32_t auto_quiet = 0;       // RecordAutoEnd: a Free take ends after this long quiet (frames at the model rate; 0 never)
    float auto_silence_db = -60;   // and what quiet is
    std::atomic<bool> monitor_on {false};
    std::atomic<uint8_t> loop_monitor {uint8_t(Monitor::Auto)}; // Input Config's Monitor, for loops on a hardware input
    float record_gain = 1;  // linear
    int cue_track = -1;     // a take cued for Rec length Seq starts where this track's pattern comes round
    uint32_t bounce_ticks = 0; // a pattern render ends after this many ticks; 0 with bounce_song
    bool bounce_song = false;  // the song's render: to its end, then its tail
    unsigned block_at = 0;  // frames of this block rendered when tick() runs
    void select_input(const int32_t* input, const float* usb, unsigned frames); // into record_l / record_r
    double until_arp = 0;
    uint32_t arp_ticks = 0;
    bool click = false;
    float click_level = 0.4f, click_env = 0, click_phase = 0, click_step = 0;
    float record_l[block] {}, record_r[block] {};
    float tap_l[block] {}, tap_r[block] {}; // the Track source this block
    // Punch: one on the master or a track, one on a single sampler pad. A
    // press while playing waits for the next step of its snap grid.
    struct PendingPunch { bool waiting = false; uint8_t target = 255; PunchKind kind {}; float depth = 1; unsigned snap = 2, tail = 0; };
    Punch punch, pad_punch; // `punch`: the song's punches
    uint8_t punch_target = 255;
    // Pad FX (parity phase 7b): four punches held at once, in series on their target, each with its own grid wait.
    static constexpr unsigned live_slots = 4;
    struct Live { Punch punch; PendingPunch pending; uint8_t target = 255; };
    std::array<Live, live_slots> live;
    std::array<std::array<int16_t, punch_params>, 16> mpc_params {}; // the MPC's Pad FX: K1-K3 each
    const int16_t* params_of(PunchKind kind) const { return punch_is_mpc(kind) ? mpc_params[unsigned(kind) - punch_mpc_first].data() : nullptr; }
    void start_live(unsigned slot, const PendingPunch& p);
    void run_live(uint8_t target, float* left, float* right, unsigned frames);
    // Flex Beat: a time effect over the master for a bar, then Empty again (One Shot) or again (Loop).
    Punch flex;
    uint8_t flex_pad = 0, flex_mode = 0, flex_mix = 100;
    bool flex_quantize = true, flex_waiting = false;
    double flex_left = 0; // samples of its bar left
    void start_flex();
    std::atomic<unsigned> flex_now {0};
    int pad_punch_track = -1, pad_punch_pad = -1;
    PendingPunch pending;
    bool song_recording = false, punch_from_song = false, live_punch = false;
    uint32_t song_punch_until = 0; // the tick a song punch lets go
    void play_song_punches();      // on each sixteenth of the song: punches written there
    void start_punch(const PendingPunch& p);
    double bar_length() const { return tick_length() * bar; }
    std::array<Slot, track_count> slots;
    // Rendering on several cores (render_pool.h): the workers; the tracks heaviest first by what each took last
    // block, the ones with an engine in that order; each track's time this block (engine and chain), and the total.
    static constexpr int render_priority = 30;            // the audio thread's (base drivers/audio.cpp)
    uint64_t parallel_from_ns = 150000;                   // less than this a block: the audio thread alone
    RenderPool workers;
    std::array<uint8_t, track_count> order = [] { std::array<uint8_t, track_count> o {}; for (unsigned t = 0; t < track_count; ++t) o[t] = uint8_t(t); return o; }();
    std::array<uint8_t, track_count> sounding_tracks {};
    std::array<uint32_t, track_count> track_ns {};
    uint64_t track_work_ns = 0;
    std::atomic<float> block_load {0};
    std::atomic<bool> parallel_allowed {true};
    std::atomic<uint64_t> parallel_blocks {0};
    bool plugin_in_two_places() const;
    DelayPool pool;
    std::array<std::array<Effect, max_slots>, chain_count> fx;
    std::unique_ptr<std::array<SamplerEngine::PadEffects, track_count>> pad_fx; // each sampler pad's chain
    std::array<float, bus_count> returns {0.7f, 0.7f, 0.7f, 0.7f};
    float bus_l[bus_count][block] {}, bus_r[bus_count][block] {}, master_l[block] {}, master_r[block] {};
    void run_chain(unsigned chain, float* left, float* right, unsigned frames);
    // Knob FX (parity phase 7): one effect the chosen tracks and kit pads go
    // through on their way to the master (`knob_in`, its twin, on the input
    // during a take with Rec Input FX); it rings on a while after it stops.
    Effect knob, knob_in;
    bool knob_heard = false, knob_rec = false;
    uint16_t knob_tracks = 0;
    std::array<std::array<uint64_t, 2>, track_count> knob_pads {};
    uint32_t knob_tail = 0;
    float knob_l[block] {}, knob_r[block] {};
    float key_l[block] {}, key_r[block] {}, aside_l[block] {}, aside_r[block] {};
    float key_pad_l[block] {}, key_pad_r[block] {}; // Sidechain on one kit pad: that pad as the sampler left it // Sidechain: the key heard by the detector, and as it joins the master
    void knob_targets(); // hands each kit its pads (none while Knob FX is not heard)
    void knob_command(const Command& c);
    MasterCompressor comp;
    MasterComp comp_settings;
    // The Looper and its four tracks' strips; what each source played the block before, for its loops to record.
    struct LoopStrip {
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f, send = 0;
        bool mute = false, solo = false;
        uint8_t bus = 0;
        float left[block] {}, right[block] {};
    };
    Looper loops;
    std::array<LoopStrip, loop_tracks> loop_strips {};
    std::array<std::atomic<float>, loop_tracks> loop_meters {};
    float source_l[looper::source_count][block] {}, source_r[looper::source_count][block] {};
    unsigned source_frames = 0;
    bool loop_restart = false; // Play or a jump: the loops line up when the sequencer next runs
    bool loop_running = false, loop_counting = false;
    double loop_tick = 0;      // the sequencer's place at this block's first frame (the tick it plays next, less what is left)
    bool loop_strip_command(const Command& c); // Level, Pan, Send, Mute, Solo, SendBus for L1-L4 (track 16-19)
    void run_loops(unsigned frames, bool solo); // the loops into their tracks, and those into the master and buses
    void keep_sources(const int32_t* input, const float* usb, unsigned frames); // this block's sources for the loops' next one
    void publish_taps(unsigned frames);         // the watched track's voices, for the screen
    std::array<PlayPatterns, track_count> patterns {};
    Song song;
    bool song_mode = false;
    unsigned song_bar_now = 0;
    uint16_t tempo = 1200, global_tempo = 1200;
    // BPM per pattern: the tempo of the pattern launched last (`lead`), when set.
    bool seq_tempo = false;
    std::array<uint16_t, pattern_count> pattern_tempo {};
    uint8_t lead = 0;
    unsigned bar = 4 * ticks_per_quarter, beat = ticks_per_quarter; // the time signature, in ticks
    uint32_t count_left = 0; // Count-In: ticks of the bar counted before the song starts
    uint8_t swing = 50;
    bool running = false;
    uint32_t tick_now = 0;
    double until_tick = 0; // samples until the next tick
    uint32_t random = 0x2545F491u;

    std::atomic<bool> is_playing {false};
    std::atomic<uint32_t> tick_count {0};
    std::array<std::atomic<int>, track_count> positions_now {};
    std::atomic<unsigned> count_beat {0};
    std::atomic<unsigned> tempo_shown {1200};
    std::array<std::atomic<unsigned>, track_count> patterns_now {};
    std::atomic<bool> in_song {false};
    std::atomic<unsigned> bar_now {0};
    std::array<std::atomic<float>, track_count> meters {};
    std::atomic<int> tap_track {-1};
    TapBoard tap_board;
    std::atomic<float> output_peak {0};
    std::array<std::atomic<float>, 2> out_levels {}, in_levels {};
    std::atomic<unsigned> lost {0};
    std::atomic<bool> punch_on {false};
    std::atomic<unsigned> live_on {0};
};

} // namespace melodee
