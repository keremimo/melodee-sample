// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/input_route.h"
#include "drivers/mixer.h"
#include "runtime/app.h"
#include <algorithm>

namespace melodee {

void InputRoute::service(Hardware& hardware, bool plugged) {
    uint8_t want_now = wanted.load(std::memory_order_relaxed);
    auto source = InputSource((want_now & 0x7f) % unsigned(InputSource::Count));
    bool mute = want_now & 0x80;
    if (int(source) != routed) {
        // USB-C first off (the audio thread stops taking blocks), then the ADC route, then USB-C on.
        if (!from_usb(source)) { usb_live.store(false, std::memory_order_release); usb.stop(); }
        if (hardware.mixer.available() && !hardware.mixer.input(input_route(source))) hardware.mixer.input("off");
        if (from_usb(source)) {
            if (usb.state() == UsbStreamState::Stopped) usb.start("auto");
            usb_live.store(true, std::memory_order_release);
        }
        routed = int(source);
    }
    // The speaker: off with headphones or line out in, and while the mic is heard through it.
    int on = !plugged && !mute ? 1 : 0;
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
