// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/multisample.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// A sample in memory at 44.1 kHz, and what the screen needs about it. Read,
// converted and analysed on the service thread (it allocates and reads
// files); the audio thread only reads `left`, `right` and `frames` through a
// pointer handed over by the engine host.
namespace melodee {

struct SampleData {
    std::vector<float> left, right;
    uint32_t frames = 0;
    std::shared_ptr<Multisample> multi; // a multisampled instrument: no frames of its own, its zones' instead
    // Roadmap P2: the same sound at the device's rate (prepare_playback()), made once before the audio thread has
    // it; the sampler plays it a frame a frame, its 44.1 kHz positions scaled. None at 44.1 kHz, for an instrument,
    // or past the library's budget: the 44.1 kHz frames are then read that much further each device frame.
    std::vector<float> play_left, play_right;
    uint32_t play_frames = 0;
    float play_rate = 0;
    bool plays_at(float rate) const { return play_frames && play_rate == rate; }
    size_t play_bytes() const { return (play_left.capacity() + play_right.capacity()) * sizeof(float); }
};

// Band-limited rate conversion (the base's dsp::SincResampler: flat to 0.907 of the lower Nyquist, images 90 dB
// down): `frames` of left and right at `from` Hz to `to` Hz, frame n of the result at n * from / to of the input
// (no delay), as many as the ratio gives. Allocates: not for the audio thread.
void resample(const float* left, const float* right, uint32_t frames, unsigned from, unsigned to,
              std::vector<float>& out_left, std::vector<float>& out_right);
// The device-rate copy of `sample` for `rate`, from its 44.1 kHz frames (dropped at 44.1 kHz, never for an
// instrument). Before the audio thread has the sample.
void prepare_playback(SampleData& sample, float rate);
// The same straight from the file's own `frames` at `rate` Hz (read_wav, read_audio): the file's frames when they are
// at `play_at` already.
void playback_from(SampleData& sample, const float* left, const float* right, uint32_t frames, unsigned rate, float play_at);

// What a file says of itself, beyond its frames: its rate and sides, and the
// first loop of its smpl chunk (frames at its own rate, end past the last; -1 none).
struct WavInfo {
    unsigned channels = 0, rate = 0;
    int64_t loop_start = -1, loop_end = -1;
};

// The waveform as the pages draw it: the peak of each of 512 columns, 0..255.
using Overview = std::array<uint8_t, 512>;
// Where the hits are, for chopping: a frame and how strong the rise was, 0..255.
struct Hit { uint32_t frame = 0; uint8_t strength = 0; };
constexpr unsigned max_hits = 128;

// Reads a WAV file (PCM 16, 24 or 32 bit, or 32-bit float; mono or stereo;
// any rate, converted to 44.1 kHz) of at most `max_seconds`. Returns an
// empty message on success, else what went wrong.
// `play_at` (roadmap P2): the device's rate, for the copy it plays made straight from the file (its own frames when
// the rates agree, else one conversion); 0 or 44.1 kHz none.
std::string read_wav(const std::string& path, SampleData& out, unsigned max_seconds = 60, WavInfo* info = nullptr, float play_at = 0);
// The loop a FLAC file keeps from the WAV it was made of (flac --keep-foreign-metadata: an
// APPLICATION block "riff" holding the smpl chunk), into `info`; false when it has none.
bool read_flac_loop(const std::string& path, WavInfo& info);
// Writes stereo at 44.1 kHz: 24-bit PCM, or 32-bit float when the sound goes
// past full scale (so the file never clips what plays); empty on success.
std::string write_wav(const std::string& path, const SampleData& sample);
// Stereo 32-bit float at 44.1 kHz written as it comes (the song's mixdown,
// kept whole however loud): open, add interleaved stereo frames, then close,
// which puts the lengths in the header.
class WavStream {
public:
    ~WavStream() { close(); }
    bool open(const std::string& path);
    bool add(const float* interleaved, unsigned frames);
    bool close(); // false when anything failed
    uint32_t frames() const { return written; }
private:
    std::FILE* file = nullptr;
    uint32_t written = 0;
    bool failed = false;
};
void make_overview(const SampleData& sample, Overview& out);
// The hits, strongest kept, sorted by frame; the first is always frame 0.
unsigned find_hits(const SampleData& sample, std::array<Hit, max_hits>& out);

} // namespace melodee
