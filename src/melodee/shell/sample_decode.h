// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include <memory>
#include <string>

namespace melodee {

// An instrument's audio file, whatever its kind, at the project's rate; empty on success.
std::string read_audio(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info, float play_at = 0); // play_at: read_wav's

// A sound file a piece at a time, as read_audio() reads it whole (it is read_audio()'s reader): WAV by WavReader,
// FLAC, Ogg, AIFF and MP3 by JUCE's (an MP3's encoder delay and padding left out). open() finds its rate and its
// frames, at most `max_seconds` of them (empty on success, else read_audio()'s words); read() hands on the next ones at
// the file's own rate (a mono file's on both sides), fewer when the file turns out shorter than it said, which
// ends it (failed(): it could not be read there, not an MP3 whose length was a guess). Service thread only.
class AudioReader {
public:
    AudioReader();
    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;
    std::string open(const std::string& path, unsigned max_seconds, WavInfo* info = nullptr);
    unsigned rate() const { return hz; }
    uint32_t frames() const { return count; }
    uint32_t read(float* left, float* right, uint32_t frames);
    bool ended() const { return done >= count; }
    bool failed() const { return broken; }
private:
    struct Juce;
    std::unique_ptr<Juce> juce; // none: a WAV
    WavReader wav;
    unsigned hz = 0;
    uint32_t count = 0, done = 0;
    bool broken = false;
};

} // namespace melodee
