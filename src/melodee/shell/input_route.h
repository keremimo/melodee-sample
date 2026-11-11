// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "drivers/usb_audio.h"
#include "melodee/model/input_config.h"
#include <atomic>
#include <cstdint>

struct Hardware;

// The hardware behind Input Config's source (parity phase 6), and the
// Looper's loops that take a hardware input of their own, on the service
// thread: the ADC route (the mic or the rear line inputs, never both; a loop's
// pick wins over Input Config's), USB-C audio from a computer (opened only
// while a USB source is chosen, by Input Config or a loop; nothing streams
// until the computer plays) and the built-in
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

    // What the loops' own hardware inputs need (loop_needs()).
    static constexpr unsigned need_line = 1, need_mic = 2, need_usb = 4;
    // MIDI thread: the source, whether monitoring should quiet the speaker, and what the loops need.
    void want(InputSource source, bool mute_speaker, unsigned loops = 0) {
        wanted.store(uint16_t(unsigned(source) | (mute_speaker ? 0x80u : 0u) | (loops & 7u) << 8), std::memory_order_relaxed);
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
    std::atomic<uint16_t> wanted {uint16_t(InputSource::Rear)};
    std::atomic<bool> usb_live {false}, quieted {false};
    std::atomic<int> speaker {-1}; // as last set: 1 on, 0 off, -1 not touched
    int routed = -1;               // the InputSource and loop needs routed, -1 none yet
    float block[2 * 128] {};
};

} // namespace melodee
