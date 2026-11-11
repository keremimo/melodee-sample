// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/multisample.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

// What keeps a multisampled voice off the card's time. A zone's frames are
// mapped from the card and read as memory, which is a page fault, and so a
// wait on the SD card, for any page not in RAM: the audio thread must never
// do that. This thread pins the chunks ahead of each voice (mlock: a wait
// here, not there) and publishes how far is safe; the audio thread reads
// only below it. mlock answers ENOMEM where a read would be SIGBUS, so a card
// pulled out stops a voice instead of the app.
namespace melodee {

class StreamPager {
public:
    static constexpr unsigned stream_count = 96;
    using Pin = bool (*)(const void* address, size_t bytes, bool lock);
    explicit StreamPager(Pin pin = nullptr);
    ~StreamPager();
    StreamPager(const StreamPager&) = delete;
    StreamPager& operator=(const StreamPager&) = delete;
    void start();
    void stop();

    // Audio thread. open() gives a stream, or -1 with none free.
    int open(ZonePart& part, uint32_t frame, bool reverse, float speed);
    void restart(int stream, uint32_t frame); // a loop came round to here
    void place(int stream, uint32_t frame) { slots[size_t(stream)].at.store(frame, std::memory_order_relaxed); }
    // Reading forwards: frames below this are safe. Reversed: frames at or above it.
    uint32_t limit(int stream) const;
    bool dead(int stream) const { return slots[size_t(stream)].dead.load(std::memory_order_relaxed); }
    void close(int stream);
    void block();                             // closes the queue had no room for
    void underrun() { under.fetch_add(1, std::memory_order_relaxed); }

    // Before a part's mapping goes (any thread but audio and the pager's):
    // returns once the pager has seen everything the audio thread sent.
    void flush();
    unsigned underruns() const { return under.load(std::memory_order_relaxed); }

    // The landing thread, before the part is published: frames that stay in
    // memory for as long as the part does. False when they could not be pinned.
    bool hold(ZonePart& part, uint32_t frame, uint32_t frames);

    bool pump(); // one pass: messages, then a chunk for each stream that wants one; true if it did anything

private:
    struct Slot {
        std::atomic<uint32_t> at {0};
        std::atomic<uint64_t> ready {0}; // serial << 32 | limit
        std::atomic<bool> dead {false};
        uint32_t serial = 0, head = 0;   // audio thread's
        bool reverse = false;
        bool used = false, closing = false;
    };
    struct Work {
        ZonePart* part = nullptr;
        uint32_t serial = 0;
        bool reverse = false, dead = false;
        float speed = 1;
        uint32_t lo = 0, hi = 0;          // the chunks this stream has pinned: [lo, hi)
    };
    struct Message {
        enum Kind : uint8_t { Open, Restart, Close } kind = Open;
        int16_t stream = 0;
        bool reverse = false;
        ZonePart* part = nullptr;
        uint32_t frame = 0, serial = 0;
        float speed = 1;
    };
    void begin(Work& work, const Message& message);
    void release(Work& work);
    bool step(unsigned stream);
    bool pin_chunk(ZonePart& part, uint32_t chunk);
    void unpin_chunk(ZonePart& part, uint32_t chunk);
    void run();

    Pin pin_fn;
    std::array<Slot, stream_count> slots {};
    std::array<Work, stream_count> work {};
    MidiQueue<4096, Message> messages;
    std::atomic<unsigned> under {0};
    std::atomic<uint64_t> flush_asked {0}, flush_done {0};
    std::atomic<bool> running {false};
    std::thread thread;
};

} // namespace melodee
