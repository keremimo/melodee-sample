// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>

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
    Count
};
enum class Monitor : uint8_t {
    Off,  // never heard
    Auto, // heard while the Record page is in view
    On,   // always heard
    Count
};

inline const char* input_source_name(InputSource s) {
    static const char* const names[] {"Mic", "Rear", "Rear L", "Rear R", "Resample", "USB", "USB L", "USB R"};
    return names[unsigned(s) % unsigned(InputSource::Count)];
}
inline const char* monitor_name(Monitor m) {
    static const char* const names[] {"Off", "Auto", "On"};
    return names[unsigned(m) % unsigned(Monitor::Count)];
}
inline bool from_rear(InputSource s) { return s == InputSource::Rear || s == InputSource::RearL || s == InputSource::RearR; }
inline bool from_usb(InputSource s) { return s == InputSource::Usb || s == InputSource::UsbL || s == InputSource::UsbR; }
// The hardware route a source needs: "mic", "line" or "off" (the mix and USB-C use neither ADC input).
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
    static constexpr int threshold_min = -96;
    bool at_once() const { return threshold <= threshold_min; }
    // Whether the source is heard now: Auto only while the Record page is in view; the mix never (it is heard already).
    bool monitoring(bool record_page) const {
        if (source == InputSource::Resample) return false;
        return monitor == Monitor::On || (monitor == Monitor::Auto && record_page);
    }
    // The built-in speaker goes quiet while the mic is heard (as on the MPC: no howl).
    bool mutes_speaker(bool record_page) const { return source == InputSource::Mic && monitoring(record_page); }
    void clamp() {
        source = InputSource(std::min<unsigned>(unsigned(source), unsigned(InputSource::Count) - 1));
        monitor = Monitor(std::min<unsigned>(unsigned(monitor), unsigned(Monitor::Count) - 1));
        threshold = int8_t(std::clamp<int>(threshold, threshold_min, 0));
        length = uint8_t(std::min<unsigned>(length, rec_lengths - 1));
        gain = int8_t(std::clamp<int>(gain, -12, 24));
    }
};

} // namespace melodee
