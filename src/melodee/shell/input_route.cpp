// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/input_route.h"
#include "drivers/mixer.h"
#include "runtime/app.h"
#include <algorithm>

namespace melodee {

void InputRoute::service(Hardware& hardware, bool plugged) {
    uint16_t want_now = wanted.load(std::memory_order_relaxed);
    auto source = InputSource((want_now & 0x7f) % unsigned(InputSource::Count));
    bool mute = want_now & 0x80;
    unsigned loops = want_now >> 8 & 7u;
    int key = int(source) | int(loops) << 8;
    if (key != routed) {
        bool usb_wanted = from_usb(source) || (loops & need_usb);
        // A loop on the rear inputs or the mic routes the ADC to it (the loops never ask for both).
        const char* adc = loops & need_line ? "line" : loops & need_mic ? "mic" : input_route(source);
        // USB-C first off (the audio thread stops taking blocks), then the ADC route, then USB-C on.
        if (!usb_wanted) { usb_live.store(false, std::memory_order_release); usb.stop(); }
        if (hardware.mixer.available() && !hardware.mixer.input(adc)) hardware.mixer.input("off");
        if (usb_wanted) {
            if (usb.state() == UsbStreamState::Stopped) usb.start("auto");
            usb_live.store(true, std::memory_order_release);
        }
        routed = key;
    }
    // The speaker: off with headphones or line out in, and while the mic is heard through it.
    int on = !plugged && !mute ? 1 : 0;
    plugged_now.store(plugged, std::memory_order_relaxed);
    quieted.store(!plugged && mute, std::memory_order_relaxed);
    if (on != speaker.load(std::memory_order_relaxed) && hardware.mixer.available() && hardware.mixer.speaker(on != 0))
        speaker.store(on, std::memory_order_relaxed);
}

const float* InputRoute::usb_block(unsigned frames) {
    if (!usb_live.load(std::memory_order_acquire)) return nullptr;
    frames = std::min(frames, 128u);
    if (!usb.pull(block, frames)) std::fill(block, block + 2 * frames, 0.0f); // silent until enough is buffered
    return block;
}

} // namespace melodee
