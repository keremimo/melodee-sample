// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// The MPC's Input Configuration (Shift + Sample), as Akai's guide lists it:
// where Sample Record and Audio Recall take their sound from, whether it is
// heard meanwhile, how loud it must get before a take starts, how long a
// take runs and whether Knob FX are recorded with it. Melodee adds a gain.
// Plain data, kept with Melodee's settings (not the project), as the MPC
// keeps it with its preferences.
namespace melodee {

enum class InputSource : uint8_t {
    Mic,      // the built-in microphone (mono: both ADC sides, averaged)
    Rear,     // the rear line inputs, L and R
    RearL, RearR,
    Resample, // Melodee's own mix, before main volume
    Usb,      // what a computer plays over USB-C, L and R
    UsbL, UsbR,
    Track,    // one track's engine on its own (InputConfig::track), before or after its effects
    Count
};
enum class Monitor : uint8_t {
    Off,  // never heard
    Auto, // heard while the Record page is in view
    On,   // always heard
    Count
};

inline const char* input_source_name(InputSource s) {
    static const char* const names[] {"Mic", "Rear", "Rear L", "Rear R", "Resample", "USB", "USB L", "USB R", "Track"};
    return names[unsigned(s) % unsigned(InputSource::Count)];
}
inline const char* monitor_name(Monitor m) {
    static const char* const names[] {"Off", "Auto", "On"};
    return names[unsigned(m) % unsigned(Monitor::Count)];
}
inline bool from_rear(InputSource s) { return s == InputSource::Rear || s == InputSource::RearL || s == InputSource::RearR; }
inline bool from_usb(InputSource s) { return s == InputSource::Usb || s == InputSource::UsbL || s == InputSource::UsbR; }
// Melodee's own sound (the mix, a track): true silence between notes, nothing to pick up.
inline bool internal_source(InputSource s) { return s == InputSource::Resample || s == InputSource::Track; }
// The hardware route a source needs: "mic", "line" or "off" (the mix, a track and USB-C use neither ADC input).
inline const char* input_route(InputSource s) { return s == InputSource::Mic ? "mic" : from_rear(s) ? "line" : "off"; }

// Rec length: Free (until stopped), Seq (the selected track's pattern: while
// playing, the take starts where the pattern comes round), or Melodee's 1-16 bars.
constexpr unsigned rec_free = 0, rec_seq = 1, rec_lengths = 18; // 2.. = 1..16 bars
inline unsigned rec_bars(unsigned length) { return length >= 2 ? length - 1 : 0; }

struct InputConfig {
    InputSource source = InputSource::Rear;
    Monitor monitor = Monitor::Auto;
    int8_t threshold = -30; // dB, -96..0; -96 starts at once
    uint8_t length = rec_free;
    bool fx = false;        // Rec Input FX: Knob FX recorded with the take (Knob FX: parity phase 7)
    int8_t gain = 0;        // dB, -12..24 (Melodee's)
    uint8_t track = 0;      // Track: which track (0..15)
    bool post = true;       // Track: after its chain, level and pan (else the engine alone)
    Monitor before_mic = Monitor::Auto; // Monitor as it was before the mic was picked
    static constexpr int threshold_min = -96;
    bool at_once() const { return threshold <= threshold_min; }
    // Whether the source is heard now: Auto only while the Record page is in view; the mix never (it is heard already).
    bool monitoring(bool record_page) const {
        if (source == InputSource::Resample) return false;
        return monitor == Monitor::On || (monitor == Monitor::Auto && record_page);
    }
    // The built-in speaker goes quiet while the mic is heard (as on the MPC: no howl).
    bool mutes_speaker(bool record_page) const { return source == InputSource::Mic && monitoring(record_page); }
    // A new source. Picking the mic turns Monitor off (the speaker stays on); leaving it puts Monitor back as it was.
    void set_source(InputSource s) {
        if (s == InputSource::Mic && source != InputSource::Mic) { before_mic = monitor; monitor = Monitor::Off; }
        else if (s != InputSource::Mic && source == InputSource::Mic) monitor = before_mic;
        source = s;
    }
    void clamp() {
        source = InputSource(std::min<unsigned>(unsigned(source), unsigned(InputSource::Count) - 1));
        monitor = Monitor(std::min<unsigned>(unsigned(monitor), unsigned(Monitor::Count) - 1));
        before_mic = Monitor(std::min<unsigned>(unsigned(before_mic), unsigned(Monitor::Count) - 1));
        threshold = int8_t(std::clamp<int>(threshold, threshold_min, 0));
        length = uint8_t(std::min<unsigned>(length, rec_lengths - 1));
        gain = int8_t(std::clamp<int>(gain, -12, 24));
        track = uint8_t(track % 16);
    }
};

// How Audio Recall cuts the ring and when a Free take ends on its own
// (Settings page). Plain indices into the tables below, kept with the settings.
enum class RecallGrab : uint8_t {
    LastPhrase, // after the last quiet gap of recall_gap, to the last sound
    FirstSound, // from the first sound in the ring
    Whole,      // the whole 25 s, untouched
    Count
};
enum class AutoEnd : uint8_t {
    Off,
    Internal, // the mix and a track (true silence)
    All,      // the mic, line and USB-C too
    Count
};
inline const char* recall_grab_name(RecallGrab g) {
    static const char* const names[] {"Last Phrase", "First Sound", "Whole 25 s"};
    return names[unsigned(g) % unsigned(RecallGrab::Count)];
}
inline const char* auto_end_name(AutoEnd a) {
    static const char* const names[] {"Off", "Synths & Resample", "All Sources"};
    return names[unsigned(a) % unsigned(AutoEnd::Count)];
}

struct SamplingPrefs {
    static constexpr unsigned gap_count = 4, end_count = 4, silence_count = 3;
    static constexpr float gap_seconds[gap_count] {0.5f, 1.0f, 2.0f, 4.0f};
    static constexpr float end_seconds[end_count] {0.5f, 1.0f, 1.5f, 3.0f};
    static constexpr int silence_dbs[silence_count] {-48, -60, -72};
    RecallGrab grab = RecallGrab::LastPhrase;
    uint8_t gap = 1;            // gap_seconds: 1 s
    bool trim_end = true;       // Last Phrase and First Sound: the silence after the last sound cut
    AutoEnd auto_end = AutoEnd::Internal;
    uint8_t end_after = 2;      // end_seconds: 1.5 s
    uint8_t silence = 1;        // silence_dbs: -60 dB
    // Whether a Free take from `source` ends on its own.
    bool auto_ends(InputSource source) const {
        return auto_end == AutoEnd::All || (auto_end == AutoEnd::Internal && internal_source(source));
    }
    int silence_db() const { return silence_dbs[silence % silence_count]; }
    // The entry nearest a level in dB (the settings file keeps the dB).
    static uint8_t silence_index(int db) {
        unsigned best = 0;
        for (unsigned n = 1; n < silence_count; ++n) if (std::abs(silence_dbs[n] - db) < std::abs(silence_dbs[best] - db)) best = n;
        return uint8_t(best);
    }
    float silence_level() const { return std::pow(10.0f, float(silence_db()) / 20.0f); }
    void clamp() {
        grab = RecallGrab(std::min<unsigned>(unsigned(grab), unsigned(RecallGrab::Count) - 1));
        auto_end = AutoEnd(std::min<unsigned>(unsigned(auto_end), unsigned(AutoEnd::Count) - 1));
        gap = uint8_t(std::min<unsigned>(gap, gap_count - 1));
        end_after = uint8_t(std::min<unsigned>(end_after, end_count - 1));
        silence = uint8_t(std::min<unsigned>(silence, silence_count - 1));
    }
};

} // namespace melodee
