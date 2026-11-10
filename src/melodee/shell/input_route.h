// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "drivers/usb_audio.h"
#include "melodee/model/input_config.h"
#include <atomic>
#include <cstdint>

struct Hardware;

// The hardware behind Input Config's source (parity phase 6), on the
// service thread: the ADC route (the mic or the rear line inputs, never
// both), USB-C audio from a computer (opened only while a USB source is
// chosen; nothing streams until the computer plays) and the built-in
// speaker, which plays while neither headphones nor line out is plugged in
// and, as on the MPC, goes quiet while the mic is monitored. The source is
// always routed, so Audio Recall's ring always hears it.
//
// The MIDI thread says what it wants (want()); the audio thread takes the
// USB-C block (usb_block()); the service thread does the rest (service()).
namespace melodee {

class InputRoute {
public:
    InputRoute() = default;
    InputRoute(const InputRoute&) = delete;
    InputRoute& operator=(const InputRoute&) = delete;

    // MIDI thread: the source, and whether monitoring it should quiet the speaker.
    void want(InputSource source, bool mute_speaker) {
        wanted.store(uint8_t(unsigned(source) | (mute_speaker ? 0x80u : 0u)), std::memory_order_relaxed);
    }
    // Service thread, every pass (and on a jack change): routes, USB-C, the speaker.
    void service(Hardware& hardware, bool plugged);
    // Audio thread: this block from USB-C (interleaved stereo, `frames` <= 128),
    // or nullptr while no USB source is chosen.
    const float* usb_block(unsigned frames);
    // Audio thread: the device's rate, which USB-C in is converted to.
    void set_device_rate(unsigned rate) { usb.set_device_rate(rate); }

    // Any thread.
    bool speaker_on() const { return speaker.load(std::memory_order_relaxed) == 1; }
    bool speaker_quieted() const { return quieted.load(std::memory_order_relaxed); } // by the monitored mic
    UsbStreamState usb_state() const { return usb.state(); }

private:
    UsbAudioInput usb;
    std::atomic<uint8_t> wanted {uint8_t(InputSource::Rear)};
    std::atomic<bool> usb_live {false}, quieted {false};
    std::atomic<int> speaker {-1}; // as last set: 1 on, 0 off, -1 not touched
    int routed = -1;               // the InputSource routed, -1 none yet
    float block[2 * 128] {};
};

} // namespace melodee
