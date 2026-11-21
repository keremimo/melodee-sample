// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace melodee {

// Recent processed output, one signed min/max pair per channel per DSP block.
using CustomGuiTrace = std::array<uint32_t, 64>;

class CustomGuiScope {
public:
    using Time = std::chrono::steady_clock::time_point;

    // A handle has one render producer. Both stereo channels share one tagged
    // atomic slot, so the reader can discard any slot overwritten during its copy.
    void write(const float* left, const float* right, unsigned frames, Time now) {
        const auto ticks = milliseconds(now);
        if (ticks - previous >= timeout) available = 0;
        previous = ticks;
        const auto packed = peaks(left, frames) | (uint32_t(peaks(right, frames)) << 16);
        const uint32_t seq = next++;
        slots[seq % slots.size()].store(uint64_t(seq) << 32 | packed, std::memory_order_relaxed);
        available = std::min(available + 1, unsigned(slots.size()));
        last.store(ticks, std::memory_order_relaxed);
        head.store(uint64_t(available) << 32 | next, std::memory_order_release);
    }

    CustomGuiTrace read(Time now) const {
        CustomGuiTrace trace {};
        const auto state = head.load(std::memory_order_acquire);
        if (milliseconds(now) - last.load(std::memory_order_relaxed) >= timeout) return trace;
        const auto count = unsigned(state >> 32);
        for (unsigned n = 0; n < count; ++n) {
            const uint32_t seq = uint32_t(state) - count + n;
            const auto slot = slots[seq % slots.size()].load(std::memory_order_relaxed);
            if (uint32_t(slot >> 32) == seq) trace[trace.size() - count + n] = uint32_t(slot);
        }
        return trace;
    }

private:
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static constexpr int64_t timeout = 250;
    static int64_t milliseconds(Time time) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }
    static uint16_t peaks(const float* samples, unsigned frames) {
        float low = 0, high = 0;
        for (unsigned n = 0; n < frames; ++n) {
            const float value = samples[n];
            if (!std::isfinite(value)) continue;
            low = std::min(low, value);
            high = std::max(high, value);
        }
        auto quantize = [](float v) { return uint8_t(int(std::clamp(v, -1.0f, 1.0f) * 127)); };
        return uint16_t(quantize(low) | (unsigned(quantize(high)) << 8));
    }

    std::array<std::atomic<uint64_t>, CustomGuiTrace{}.size()> slots {};
    std::atomic<uint64_t> head {0};
    std::atomic<int64_t> last {0};
    uint32_t next = 0;
    unsigned available = 0;
    int64_t previous = 0;
};

} // namespace melodee
