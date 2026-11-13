// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/multisample.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

template <unsigned Channels> class SincResampler; // dsp/frame_ring.h

// A sample in memory at 44.1 kHz, and what the screen needs about it. Read,
// converted and analysed on the service thread (it allocates and reads
// files); the audio thread only reads `left`, `right` and `frames` through a
// pointer handed over by the engine host. Its 44.1 kHz frames are what the
// project's positions count (a region, a slice, the overview, the hits);
// what was read or recorded at another rate also keeps its own frames as the
// copy the device plays when the rates agree (play_*, source_rate).
namespace melodee {

// A sample still being read (the browser's preview, SampleFill): its frames are laid out at their full length, silent,
// before the audio thread has it, and filled in from the start while it plays. How far each copy is filled in,
// published as it lands; the audio thread reads below that only. `cut`: no more will come (the file ran out early,
// or it is no longer wanted), so a voice plays to there and ends; `whole`: everything is there.
struct SampleGrowth {
    std::atomic<uint32_t> frames {0};      // of `left` and `right`
    std::atomic<uint32_t> play_frames {0}; // of `play_left` and `play_right`
    std::atomic<bool> cut {false}, whole {false};
};

struct SampleData {
    std::vector<float> left, right;
    uint32_t frames = 0;
    std::shared_ptr<Multisample> multi; // a multisampled instrument: no frames of its own, its zones' instead
    std::shared_ptr<SampleGrowth> growth; // still being read (SampleFill); none: all of it is there
    // Roadmap P2: the same sound at the device's rate (prepare_playback()), made once before the audio thread has
    // it; the sampler plays it a frame a frame, its 44.1 kHz positions scaled. None at 44.1 kHz, for an instrument,
    // or past the library's budget: the 44.1 kHz frames are then read that much further each device frame.
    std::vector<float> play_left, play_right;
    uint32_t play_frames = 0;
    float play_rate = 0;
    unsigned source_rate = 44100; // the rate of the file or capture it was made of (adopt())
    bool plays_at(float rate) const { return play_frames && play_rate == rate; }
    // The play copy is the source's own frames, not a conversion of them.
    bool plays_own() const { return play_frames && unsigned(std::lround(play_rate)) == source_rate; }
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
// Frames read or recorded at `rate` Hz as `sample`: its 44.1 kHz frames (one band-limited conversion, none at 44.1 kHz)
// and the copy the device plays at `play_at` (these frames themselves when the rates agree, else one conversion; none
// at 44.1 kHz). Takes the vectors (by value: the sample's own may be moved in); source_rate is `rate`.
void adopt(SampleData& sample, std::vector<float> left, std::vector<float> right, unsigned rate, float play_at);

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
// Enough for a minute of busy playing (about 8 a second), so a long take keeps the hits early in it that Beats
// lands its cuts on, not only its strongest 128.
constexpr unsigned max_hits = 512;

// Reads a WAV file (PCM 16, 24 or 32 bit, or 32-bit float; mono or stereo;
// any rate, converted to 44.1 kHz) of at most `max_seconds`. Returns an
// empty message on success, else what went wrong.
// `play_at` (roadmap P2): the device's rate, for the copy it plays made straight from the file (its own frames when
// the rates agree, else one conversion); 0 or 44.1 kHz none.
std::string read_wav(const std::string& path, SampleData& out, unsigned max_seconds = 60, WavInfo* info = nullptr, float play_at = 0);

// A WAV file's frames a piece at a time, as read_wav() reads them whole (it is read_wav()'s reader): open() reads
// the head (and, with `info`, the smpl chunk wherever it lies) and finds the frames, at most `max_seconds` of them;
// read() hands on the next ones as floats at the file's own rate (a mono file's on both sides), fewer at the end of
// the file. open() is empty on success, else what went wrong (read_wav()'s words).
class WavReader {
public:
    std::string open(const std::string& path, unsigned max_seconds = 60, WavInfo* info = nullptr);
    unsigned rate() const { return hz; }
    uint32_t frames() const { return count; }
    uint32_t read(float* left, float* right, uint32_t frames);
private:
    std::ifstream file;
    unsigned format = 0, channels = 0, hz = 0, bits = 0;
    uint32_t count = 0, done = 0;
    std::vector<uint8_t> bytes;
};

// A sample laid out at its full length before its frames are there, then filled in a piece at a time from the start
// (the browser's preview: it plays while the rest is read). The frames are adopt()'s, to the bit: its 44.1 kHz frames
// and the device's copy, each the file's own frames or one conversion of them; each copy's progress is published
// through the sample's growth as it lands (SampleGrowth). Service thread: allocates and converts.
class SampleFill {
public:
    SampleFill();
    ~SampleFill();
    SampleFill(const SampleFill&) = delete;
    SampleFill& operator=(const SampleFill&) = delete;
    // `frames` to come at `rate` Hz: `sample` laid out for them, silent, nothing landed. The device's copy at
    // `play_at` as adopt() makes it, none when it would take more than `play_room` bytes (the library's budget).
    void begin(SampleData& sample, uint32_t frames, unsigned rate, float play_at, size_t play_room = std::numeric_limits<size_t>::max());
    // The next frames from the file; any past the `frames` begun with are left out.
    void add(const float* left, const float* right, uint32_t frames);
    // The end: each conversion lets out its last frames, and the sample is whole, or, short of the frames begun with
    // (the file ran out early, or it is no longer wanted), cut there.
    void finish();
    bool filling() const { return sample != nullptr; }
    uint32_t to_come() const { return left_to_add; }
private:
    struct Copy {
        std::vector<float>* left = nullptr;
        std::vector<float>* right = nullptr;
        std::atomic<uint32_t>* landed = nullptr;
        uint32_t length = 0, written = 0;
        std::unique_ptr<SincResampler<2>> converter; // none: the file's own frames
    };
    SampleData* sample = nullptr;
    std::shared_ptr<SampleGrowth> growth;
    std::array<Copy, 2> copies;                    // the 44.1 kHz frames, the device's copy
    uint32_t left_to_add = 0;
};
// The loop a FLAC file keeps from the WAV it was made of (flac --keep-foreign-metadata: an
// APPLICATION block "riff" holding the smpl chunk), into `info`; false when it has none.
bool read_flac_loop(const std::string& path, WavInfo& info);
// An MP3's first frame: the frames each of its frames holds (1152 for MPEG 1, 576 for MPEG 2 and 2.5) and, when it
// carries a LAME tag (LAME, and FFmpeg's "Lavc"/"Lavf", in the Xing or Info frame), the encoder delay and padding
// it records. False when the file does not start with a Layer III frame.
struct Mp3Head { unsigned frame_samples = 0, delay = 0, padding = 0; bool gapless = false; };
bool read_mp3_head(const std::string& path, Mp3Head& head);
// Writes the sample's `left` and `right` as stereo at `rate` Hz (what they are at: 44.1 kHz for a sample's own
// frames, the device's for a capture before adopt()): 24-bit PCM, or 32-bit float when the sound goes past full
// scale (so the file never clips what plays); empty on success.
std::string write_wav(const std::string& path, const SampleData& sample, unsigned rate = 44100);
// The sample's `left` and `right` as write_wav keeps them: on 24-bit steps (read back by read_wav to the same floats),
// unchanged when it writes 32-bit float. What plays is then what the file holds, to the bit.
void as_written(SampleData& sample);
// A capture (a take, Audio Recall, a render), its `left` and `right` at `rate` Hz, as a sample: kept as a file holds it
// (as_written), written to `path` at that rate (not when empty), then adopt()ed: its own frames play at `play_at` when
// the rates agree, its 44.1 kHz frames are what the project counts. write_wav's error, if any.
std::string keep_capture(SampleData& sample, unsigned rate, const std::string& path, float play_at);
// Stereo 32-bit float at `rate` Hz written as it comes (the song's mixdown,
// kept whole however loud): open, add interleaved stereo frames, then close,
// which puts the lengths in the header. Roadmap P4: or integer PCM, 24 or 16
// bits, TPDF dithered (only there: a float file needs none) and clipped.
class WavStream {
public:
    ~WavStream() { close(); }
    // `data_at` (freeze plan F2b's prints): the frames start that many bytes in (a JUNK chunk before them), a page's
    // boundary, so a mapping of the file pins and lets go of whole pages of them; 0: right after the header.
    // `channels` interleaved (2: stereo; a kit's print with pad sends has a pair more for each bus it sends into).
    static constexpr unsigned max_wav_channels = 10;
    bool open(const std::string& path, unsigned rate = 44100, unsigned bits = 32, unsigned data_at = 0, unsigned channels = 2);
    bool add(const float* interleaved, unsigned frames);
    bool close(); // false when anything failed
    uint32_t frames() const { return written; }
private:
    std::FILE* file = nullptr;
    uint32_t written = 0;
    unsigned rate = 44100, bits = 32, data_at = 0, channels = 2;
    bool failed = false;
    std::vector<unsigned char> header(uint32_t frames) const;
    uint32_t noise = 0x9e3779b9u; // the dither's
};
// `out` the louder side of each column; with `sides`, the left and right apart. True when the sides differ (a
// stereo file; a mono one is read onto both sides alike).
bool make_overview(const SampleData& sample, Overview& out, std::array<Overview, 2>* sides = nullptr);
// The hits (engine/onsets.cpp: changes in the spectrum, each cut moved back to
// the quiet point before its attack), strongest kept, sorted by frame; the
// first is always frame 0. Strength is relative to the strongest hit.
unsigned find_hits(const SampleData& sample, std::array<Hit, max_hits>& out);

} // namespace melodee
