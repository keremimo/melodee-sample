// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// lo-fly for Melodee: the values the host calls, the audio thread and the screen share. Every parameter is an atomic
// plain value; the audio thread hands changed ones to lofly::Synth at the start of each block.
#pragma once

#include "lofly/params.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace lofly_melodee {

// CLAP parameter ID: FNV-1a of the lo-fly key, so IDs stay the same when lo-fly adds parameters in between.
constexpr std::uint32_t clap_id(std::string_view key)
{
    std::uint32_t h = 2166136261u;
    for (char c : key) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    return h & 0x7fffffffu;
}

inline const lofly::ParamSpec& spec_of(lofly::ParamId id) { return lofly::kParamSpecs[static_cast<std::size_t>(id)]; }

struct Store {
    std::array<std::atomic<float>, lofly::kNumParams> values;
    std::atomic<float> peak { 0.0f };       // the last block's output peak
    std::atomic<std::uint32_t> voices { 0 }; // bit n: voice n sounds
    std::atomic<bool> playing { false };    // the host's transport plays
    std::atomic<int> page { 0 };            // the screen's page, for the audio thread's redraw requests

    Store()
    {
        for (const auto& s : lofly::kParamSpecs)
            values[static_cast<std::size_t>(s.id)].store(s.def, std::memory_order_relaxed);
    }

    float get(lofly::ParamId id) const { return values[static_cast<std::size_t>(id)].load(std::memory_order_relaxed); }

    // Clamped to the range, whole numbers for stepped kinds; returns the value stored.
    float set(lofly::ParamId id, float v)
    {
        const auto& s = spec_of(id);
        if (!std::isfinite(v))
            v = s.def;
        v = std::clamp(v, s.min, s.max);
        if (lofly::isStepped(s.kind))
            v = std::round(v);
        values[static_cast<std::size_t>(id)].store(v, std::memory_order_relaxed);
        return v;
    }

    // The name of the preset loaded last ("Init" at first). Main and screen threads only.
    std::string preset() const
    {
        std::lock_guard<std::mutex> held(preset_lock_);
        return preset_;
    }
    void set_preset(std::string name)
    {
        std::lock_guard<std::mutex> held(preset_lock_);
        preset_ = std::move(name);
    }

    float normalized(lofly::ParamId id) const { return lofly::toNormalized(spec_of(id), get(id)); }
    void set_normalized(lofly::ParamId id, float n) { set(id, lofly::fromNormalized(spec_of(id), std::clamp(n, 0.0f, 1.0f))); }
    bool on(lofly::ParamId id) const { return get(id) >= 0.5f; }
    int index(lofly::ParamId id) const { return static_cast<int>(std::lround(get(id))); }

private:
    mutable std::mutex preset_lock_;
    std::string preset_ = "Init";
};

} // namespace lofly_melodee
