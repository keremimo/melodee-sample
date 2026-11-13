// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/onsets.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// Hits for Chop (find_hits): where a new sound starts, found from the change
// in the spectrum rather than in the overall level, so a chord change at the
// same loudness or a hi-hat over a held pad counts too. The SuperFlux method
// (Böck and Widmer, 2013): a log spectrum in bands a semitone wide, 100 frames
// a second; each band's rise over the frame before, that frame first widened
// by one band either side so a vibrato's slide is not counted as a rise. A hit
// is a peak of the summed rises, the highest within 30 ms and above the
// average around it. Its cut is then moved back to the quiet point just before
// the attack, in 1 ms steps, so the slice holds the whole attack; Zero Snap
// (shell/chop.h) puts it on a crossing after that.
namespace melodee {
namespace {

using Complex = std::complex<float>;
constexpr unsigned fft_size = 2048, hop = 441; // 46 ms windows, 100 frames a second (at 44.1 kHz)
constexpr unsigned fine = 44;                  // the attack search's steps: 1 ms
constexpr unsigned spacing = 3;                // frames: hits at least 30 ms apart
constexpr float delta = 1.0f;                  // how far a peak stands above the average around it

// In-place radix-2 FFT of a.size() (a power of two) points; twiddle[k] = e^(-2πik/n), k < n/2.
void fft(std::vector<Complex>& a, const std::vector<Complex>& twiddle) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2, step = n / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < half; ++k) {
                Complex u = a[i + k], v = a[i + k + half] * twiddle[k * step];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
}

// Semitone bands from 30 Hz to 16 kHz as triangles over the FFT's bins; low
// bands that would share a bin merge, so each band has a bin of its own.
struct Band { unsigned from, peak, to; };
std::vector<Band> make_bands() {
    std::vector<unsigned> centres;
    for (double f = 30; f <= 16000; f *= std::pow(2.0, 1.0 / 12)) {
        unsigned bin = unsigned(std::lround(f * fft_size / 44100));
        if (centres.empty() || bin > centres.back()) centres.push_back(bin);
    }
    std::vector<Band> bands;
    for (size_t n = 1; n + 1 < centres.size(); ++n) bands.push_back({centres[n - 1], centres[n], centres[n + 1]});
    return bands;
}

float band_sum(const std::vector<float>& magnitude, const Band& b) {
    float sum = 0;
    for (unsigned k = b.from; k < b.to; ++k)
        sum += magnitude[k] * (k < b.peak ? float(k - b.from) / float(b.peak - b.from) : float(b.to - k) / float(b.to - b.peak));
    return sum;
}

// The hits in time order, every one found (the first frame 0), and the flux they were found in.
void analyse(const SampleData& s, std::vector<Hit>& found, std::vector<float>& flux) {
    const uint32_t frames = std::min<uint32_t>(s.frames, uint32_t(std::min(s.left.size(), s.right.size())));
    found.assign(1, Hit {0, 255});
    flux.clear();
    if (frames > fine * 4) {
        std::vector<float> mono(frames); // the two sides summed
        for (uint32_t n = 0; n < frames; ++n) mono[n] = s.left[n] + s.right[n];

        // The bands of frames centred hop apart, two frames an FFT: one as the real part, one as the
        // imaginary, parted again by the spectrum's symmetry.
        // Frames whose window runs past the end are left out: the sound cut off there would count as a hit.
        const unsigned count = frames > fft_size / 2 ? (frames - fft_size / 2) / hop + 1 : 0;
        std::vector<Complex> twiddle(fft_size / 2), buffer(fft_size);
        for (unsigned k = 0; k < fft_size / 2; ++k) twiddle[k] = std::polar(1.0f, float(-2 * M_PI * k / fft_size));
        std::vector<float> window(fft_size);
        for (unsigned n = 0; n < fft_size; ++n) window[n] = float(0.5 - 0.5 * std::cos(2 * M_PI * n / fft_size));
        const auto bands = make_bands();
        const size_t width = bands.size();
        std::vector<float> magnitude[2], sums(size_t(count) * width);
        flux.assign(count, 0.0f);
        magnitude[0].resize(fft_size / 2 + 1);
        magnitude[1].resize(fft_size / 2 + 1);
        auto sample = [&](unsigned frame, unsigned n) {
            int64_t at = int64_t(frame) * hop - fft_size / 2 + n;
            return at >= 0 ? mono[size_t(at)] * window[n] : 0.0f;
        };
        for (unsigned frame = 0; frame < count; frame += 2) {
            for (unsigned n = 0; n < fft_size; ++n) buffer[n] = {sample(frame, n), frame + 1 < count ? sample(frame + 1, n) : 0.0f};
            fft(buffer, twiddle);
            for (unsigned k = 0; k <= fft_size / 2; ++k) {
                Complex z = buffer[k], mirror = std::conj(buffer[(fft_size - k) % fft_size]);
                magnitude[0][k] = std::abs(z + mirror) / 2;
                magnitude[1][k] = std::abs(z - mirror) / 2;
            }
            for (unsigned side = 0; side < 2 && frame + side < count; ++side)
                for (size_t b = 0; b < width; ++b) sums[(frame + side) * width + b] = band_sum(magnitude[side], bands[b]);
        }

        // The log spectrum, 0 at 60 dB under the sample's loudest band, so a quiet sample finds the hits a loud one
        // does and the faint leakage of a clean tone (it changes with the tone's phase) is not heard as a rise. The
        // flux: each band's rise over the frame before, that frame widened by one band either side.
        const float floor = std::max(1e-9f, sums.empty() ? 0.0f : *std::max_element(sums.begin(), sums.end()) * 1e-3f);
        for (float& v : sums) v = std::log10(1.0f + v / floor);
        for (unsigned t = 1; t < count; ++t) {
            const float *level = &sums[t * width], *before = &sums[(t - 1) * width];
            float rise = 0;
            for (size_t b = 0; b < width; ++b) {
                float widened = std::max({before[b], before[b ? b - 1 : b], before[b + 1 < width ? b + 1 : b]});
                rise += std::max(0.0f, level[b] - widened);
            }
            flux[t] = rise;
        }

        // Peaks: the highest within 30 ms either side, above the average from 100 ms before to 70 ms after. Strength
        // is the height above that average, so a busy texture's bumps stay weak beside a clear hit.
        float loudest = 0;
        std::vector<unsigned> peaks;
        std::vector<float> height; // how far each stands above the average: its strength, against the highest
        for (unsigned t = 1; t < count; ++t) {
            unsigned lo = t > spacing ? t - spacing : 0, hi = std::min(count - 1, t + spacing);
            if (flux[t] <= 0 || *std::max_element(flux.begin() + lo, flux.begin() + hi + 1) > flux[t]) continue;
            if (t > lo && flux[t - 1] == flux[t]) continue; // a flat top counts once
            unsigned from = t > 10 ? t - 10 : 0, to = std::min(count - 1, t + 7);
            float average = 0;
            for (unsigned u = from; u <= to; ++u) average += flux[u];
            average /= float(to - from + 1);
            if (flux[t] < average + delta) continue;
            peaks.push_back(t);
            height.push_back(flux[t] - average);
            loudest = std::max(loudest, flux[t] - average);
        }

        // Backtrack: in 1 ms steps of the sound's edges (its first difference, so a hat over a held pad shows), the
        // steepest rise from 25 ms before the frame's centre to 25 ms after (a 46 ms frame peaks anywhere from as
        // an attack enters its window to half a window after). It counts when the attack (the loudest edge in the
        // 5 ms from it) is twice the loudest edge from 30 ms to 8 ms before; the cut then goes back while the edges
        // stay above twice the background (their middle level there), at most 20 ms, and lands where the sound
        // rises out of it: an attack is loud and ragged, so it is not walked back step by falling step. A peak with
        // no clear rise (a chord change at the same loudness, a beating chord) keeps the frame's centre: a cut a
        // little early keeps the attack, a late one leaves it at the end of the slice before.
        const uint32_t steps = frames / fine;
        std::vector<float> edge(steps);
        for (uint32_t j = 0; j < steps; ++j) {
            float sum = 0;
            for (uint32_t n = j * fine; n < (j + 1) * fine; ++n) {
                float d = n ? mono[n] - mono[n - 1] : mono[n];
                sum += d * d;
            }
            edge[j] = std::sqrt(sum / fine);
        }
        std::vector<float> level(steps); // the sound's level in the same steps
        for (uint32_t j = 0; j < steps; ++j) {
            float sum = 0;
            for (uint32_t n = j * fine; n < (j + 1) * fine; ++n) sum += mono[n] * mono[n];
            level[j] = std::sqrt(sum / fine);
        }
        auto level_at = [&](int64_t j) { return j >= 0 && j < int64_t(steps) ? level[size_t(j)] : 0.0f; };
        auto edge_at = [&](int64_t j) { return j >= 0 && j < int64_t(steps) ? edge[size_t(j)] : 0.0f; };
        std::vector<float> around;
        for (size_t p = 0; p < peaks.size(); ++p) {
            const unsigned t = peaks[p];
            uint32_t centre = t * hop, cut = centre;
            int64_t lo = std::max<int64_t>(1, int64_t(centre / fine) - 25), hi = std::min<int64_t>(int64_t(steps) - 1, centre / fine + 25);
            int64_t steepest = -1;
            float most = 0;
            for (int64_t j = lo; j <= hi; ++j)
                if (edge[size_t(j)] - edge[size_t(j - 1)] > most) { most = edge[size_t(j)] - edge[size_t(j - 1)]; steepest = j; }
            if (steepest > 0) {
                around.assign(edge.begin() + std::max<int64_t>(0, steepest - 30), edge.begin() + std::max<int64_t>(0, steepest - 8));
                float background = 0, loudest_before = 0, attack = 0;
                if (!around.empty()) {
                    loudest_before = *std::max_element(around.begin(), around.end());
                    std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
                    background = around[around.size() / 2];
                }
                for (int64_t j = steepest; j <= std::min<int64_t>(int64_t(steps) - 1, steepest + 5); ++j) attack = std::max(attack, edge[size_t(j)]);
                if (attack >= 2 * loudest_before) {
                    int64_t j = steepest - 1, floor = std::max<int64_t>(0, steepest - 21);
                    while (j > floor && edge[size_t(j)] > 2 * background) --j;
                    cut = uint32_t(j + 1) * fine;
                }
            }
            // Then the trough: the quietest 1 ms (by level) up to 20 ms before the cut. Where both the level and the
            // edges between it and the cut are 12 dB (4 times) above it (the edges too: a held low note's level
            // swings with its wave within a millisecond, its edges do not), the attack rose out of it, from a soft first stroke (a flam,
            // a grace note) or past a tail the edges missed: the cut moves to where it starts rising, the trough's
            // last step within 3 dB of its quietest (in a silence, the end nearest the attack).
            if (cut > fine * 2) {
                int64_t c = cut / fine, quietest = c, from = std::max<int64_t>(0, c - 20);
                for (int64_t j = c - 1; j >= from; --j) if (level_at(j) < level_at(quietest)) quietest = j;
                float between = 0, edges = 0;
                for (int64_t j = quietest + 1; j < c; ++j) { between = std::max(between, level_at(j)); edges = std::max(edges, edge_at(j)); }
                if (quietest < c && between >= 4 * level_at(quietest) && edges >= 4 * edge_at(quietest)) {
                    int64_t end = c - 1; // the trough's last step as quiet as it (within 3 dB): where the rise starts
                    while (end > quietest && level_at(end) > 1.4f * level_at(quietest)) --end;
                    cut = uint32_t(end + 1) * fine;
                }
            }
            if (cut <= found.back().frame + spacing * hop) continue; // too near the hit before (or the start)
            found.push_back({cut, uint8_t(std::max(1L, std::lround(255 * height[p] / loudest)))});
        }
    }
}

// The beats (Ellis, "Beat tracking by dynamic programming", 2007, laid out as librosa does it, held tighter to
// the tempo: loops keep time). The tempo: the flux's autocorrelation from 40 to 240 BPM with its multiples',
// weighted toward 120 BPM (an octave away counts 0.6 as much) and toward a tempo the sample is 2, 4 ... 256
// beats long at (a loop cut to its bars, 1.3 times). The beats: for each frame the best chain of beats ending
// on it, each about a period after the one before (the further off, the more it costs) and on strong flux; the
// chain from the last strong frame back, without weak beats at its ends. Each beat then moves onto the hit nearest it within 30 ms (the hits sit where the attacks
// start; a frame peaks about 10 ms before); one with no hit there goes between the nearest beats that have one.
// The tempo shown is the beats' average spacing, or exact where the sample is 2, 4, 8, 16, 32 or 64 such beats
// long within 4% (a loop cut to its bars). None for under a second, under four hits or no steady pulse.
BeatGrid track_beats(const std::vector<float>& flux, const std::vector<Hit>& hits, uint32_t frames, bool starts) {
    BeatGrid grid;
    constexpr double fps = 44100.0 / hop;
    const size_t n = flux.size();
    if (n < size_t(fps) || hits.size() < 4) return grid;
    double mean = 0, spread = 0;
    for (float v : flux) mean += v;
    mean /= double(n);
    for (float v : flux) spread += (v - mean) * (v - mean);
    spread = std::sqrt(spread / double(n));
    if (spread <= 0) return grid;

    // The tempo: the lag (frames a beat) whose autocorrelation, with its multiples' (a beat's pulse comes back a
    // beat, two, three and four later; a lag of three quarters of a beat, which swung or triplet hats bring, does
    // not every time), weighted, is highest; refined between frames.
    const size_t shortest = size_t(fps * 60 / 240), longest = std::min(size_t(fps * 60 / 40), n * 2 / 3);
    if (longest < shortest + 2) return grid;
    std::vector<double> ac(std::min(n * 2 / 3, 4 * longest) + 2, 0.0);
    for (size_t lag = 0; lag < ac.size() && lag < n; ++lag) {
        double sum = 0;
        for (size_t t = 0; t + lag < n; ++t) sum += (flux[t] - mean) * (flux[t + lag] - mean);
        ac[lag] = sum / double(n - lag) / (spread * spread);
    }
    size_t lag = 0;
    double best = 0;
    for (size_t l = shortest; l <= longest; ++l) {
        if (ac[l] <= 0 || ac[l] < ac[l - 1] || ac[l] < ac[l + 1]) continue;
        double sum = 0, weight = 0;
        for (size_t k = 1; k <= 4 && k * l + k < ac.size(); ++k) {
            double most = ac[k * l];
            for (size_t m = k * l - (k - 1); m <= k * l + (k - 1); ++m) most = std::max(most, ac[m]);
            sum += most / double(k);
            weight += 1.0 / double(k);
        }
        double bpm_l = 60 * fps / double(l), octaves = std::log2(bpm_l / 120), prior = std::exp(-0.5 * octaves * octaves);
        for (unsigned k = 2; k <= 256; k *= 2)
            if (std::fabs(k * 60.0 * 44100 / frames / bpm_l - 1) < 0.02) { prior *= 1.3; break; }
        double weighted = sum / weight * prior;
        if (weighted > best) { best = weighted; lag = l; }
    }
    if (!lag || ac[lag] < 0.1) return grid;
    double period = double(lag), bend = ac[lag - 1] - 2 * ac[lag] + ac[lag + 1];
    if (bend < 0) period += 0.5 * (ac[lag - 1] - ac[lag + 1]) / bend;

    // The flux (over its spread) smoothed over a 32nd of a beat, and the best chain ending on each frame.
    const int64_t reach = int64_t(std::ceil(period / 8));
    std::vector<double> local(n, 0.0), chain(n, 0.0);
    std::vector<int64_t> back(n, -1);
    for (size_t i = 0; i < n; ++i)
        for (int64_t k = -reach; k <= reach; ++k)
            if (int64_t(i) + k >= 0 && int64_t(i) + k < int64_t(n)) {
                double x = double(k) * 32 / period;
                local[i] += flux[size_t(int64_t(i) + k)] / spread * std::exp(-0.5 * x * x);
            }
    const double top = *std::max_element(local.begin(), local.end());
    const int64_t nearest = std::llround(period / 2), furthest = std::llround(2 * period);
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        double most = -1e300;
        int64_t from = -1;
        for (int64_t d = furthest; d >= nearest; --d) {
            double off = std::log(double(d) / period), score = -400 * off * off;
            int64_t at = int64_t(i) - d;
            if (at >= 0) score += chain[size_t(at)];
            if (score > most) { most = score; from = at; }
        }
        chain[i] = local[i] + most;
        if (first && local[i] < 0.01 * top) continue;
        back[i] = from;
        first = false;
    }
    std::vector<double> peaks;
    for (size_t i = 1; i + 1 < n; ++i)
        if (chain[i] > chain[i - 1] && chain[i] >= chain[i + 1]) peaks.push_back(chain[i]);
    if (peaks.empty()) return grid;
    std::nth_element(peaks.begin(), peaks.begin() + peaks.size() / 2, peaks.end());
    const double enough = 0.5 * peaks[peaks.size() / 2];
    int64_t last = -1;
    for (size_t i = 1; i + 1 < n; ++i)
        if (chain[i] > chain[i - 1] && chain[i] >= chain[i + 1] && chain[i] >= enough) last = int64_t(i);
    std::vector<size_t> beats;
    for (int64_t i = last; i >= 0; i = back[size_t(i)]) beats.insert(beats.begin(), size_t(i));
    double power = 0;
    for (size_t b : beats) power += local[b] * local[b];
    const double weak = beats.empty() ? 0 : 0.5 * std::sqrt(power / double(beats.size()));
    while (!beats.empty() && local[beats.front()] < weak) beats.erase(beats.begin());
    while (!beats.empty() && local[beats.back()] < weak) beats.pop_back();
    if (beats.size() < 2) return grid;

    // Onto the hits; the rest between their neighbours that moved.
    constexpr uint32_t near = 1323; // 30 ms
    std::vector<uint32_t> at(beats.size());
    std::vector<bool> on(beats.size(), false);
    for (size_t b = 0; b < beats.size(); ++b) {
        uint32_t frame = uint32_t(beats[b]) * hop, best_gap = near + 1;
        at[b] = frame;
        auto it = std::lower_bound(hits.begin(), hits.end(), frame, [](const Hit& h, uint32_t f) { return h.frame < f; });
        for (auto h : {it, it == hits.begin() ? it : it - 1})
            if (h != hits.end()) {
                uint32_t gap = h->frame > frame ? h->frame - frame : frame - h->frame;
                if (gap < best_gap && (!b || h->frame > at[b - 1])) { best_gap = gap; at[b] = h->frame; on[b] = true; }
            }
    }
    for (size_t b = 0; b < beats.size(); ++b) {
        if (on[b]) continue;
        size_t before = b, after = b;
        while (before > 0 && !on[before]) --before;
        while (after + 1 < beats.size() && !on[after]) ++after;
        if (on[before] && on[after] && before < b && after > b)
            at[b] = at[before] + uint32_t(uint64_t(at[after] - at[before]) * (b - before) / (after - before));
    }

    // The tempo: the least-squares spacing of the beats, or the loop's own.
    double count = double(at.size()), mid = (count - 1) / 2, sum_at = 0, slope = 0, norm = 0;
    for (uint32_t f : at) sum_at += f;
    for (size_t b = 0; b < at.size(); ++b) {
        slope += (double(b) - mid) * (double(at[b]) - sum_at / count);
        norm += (double(b) - mid) * (double(b) - mid);
    }
    if (slope <= 0) return grid;
    double bpm = 60.0 * 44100 / (slope / norm);
    for (unsigned k : {2u, 4u, 8u, 16u, 32u, 64u}) {
        double loop = k * 60.0 * 44100 / frames;
        if (std::fabs(loop / bpm - 1) < 0.04) bpm = loop;
    }
    if (bpm < 30 || bpm > 300) return grid;
    grid.tempo = uint16_t(std::lround(bpm * 10));
    grid.at = std::move(at);

    // The beats' phase: within one beat from the start, where the flux (over its spread), looked up beat after
    // beat as its highest within 20 ms (swing, a take's drift), is highest on average. Kicks and snares fall on
    // the beat; hats between beats count less often. A loop cut to whole bars (within 3%) that `starts` with an
    // attack starts on a beat (its 1). Then onto the hit at the steady beat's start. Which beat is a bar's 1 is not guessed:
    // kicks and snares alone did not tell it apart (about 4 loops in 10); the region's start says it.
    auto level = [&](double x) {
        int64_t c = std::llround(x);
        if (c < 0 || c >= int64_t(n)) return std::nan("");
        float most = flux[size_t(c)];
        for (int64_t i = std::max<int64_t>(0, c - 2); i <= std::min<int64_t>(int64_t(n) - 1, c + 2); ++i) most = std::max(most, flux[size_t(i)]);
        return (double(most) - mean) / spread;
    };
    const double step = 60 * fps / bpm, bars = frames / (4 * step * hop);
    int64_t phase = 0;
    const bool loop = starts && std::fabs(bars - std::round(bars)) < 0.03 * std::round(bars) && bars >= 0.97;
    // Where the steady beat runs: from the first tracked beat to the last, each stretched a beat at a time while a
    // hit sits there (within 30 ms), so a first or last beat the tracker let go still counts, while a free intro or
    // outro (hits off the beat) stays outside it. A loop cut to bars runs end to end.
    const double beat_frames = step * hop;
    auto hit_near = [&](double frame, uint32_t& at_hit) {
        auto it = std::lower_bound(hits.begin(), hits.end(), uint32_t(std::max(0.0, frame - 1323)),
                                   [](const Hit& h, uint32_t f) { return h.frame < f; });
        if (it == hits.end() || double(it->frame) > frame + 1323) return false;
        at_hit = it->frame;
        return true;
    };
    double from_beat = grid.at.front(), to_beat = grid.at.back();
    for (uint32_t h = 0; from_beat - beat_frames > -1323 && hit_near(from_beat - beat_frames, h);) from_beat = h;
    for (uint32_t h = 0; to_beat + beat_frames < frames && hit_near(to_beat + beat_frames, h);) to_beat = h;
    grid.pulse_from = loop ? 0 : uint32_t(from_beat); // a loop's beat runs end to end
    grid.pulse_to = loop ? frames : uint32_t(to_beat);
    // The phase is voted on only where the steady beat runs: a free intro's hits do not count.
    const double vote_from = grid.pulse_from / double(hop) - 2, vote_to = grid.pulse_to / double(hop) + 2;
    if (!loop) {
        double best_phase = -1e300;
        for (int64_t t = 0; t < std::min<int64_t>(int64_t(n), std::llround(step)); ++t) {
            double sum = 0;
            unsigned seen = 0;
            for (double at_beat = double(t); at_beat < double(n); at_beat += step) {
                if (at_beat < vote_from || at_beat > vote_to) continue;
                double v = level(at_beat);
                if (std::isnan(v)) continue;
                sum += v;
                ++seen;
            }
                    if (seen && sum / seen > best_phase + 1e-9) { best_phase = sum / seen; phase = t; }
        }
    }
    // On a real hit where the steady beat starts (the frame-0 hit find_hits() always keeps is not one, unless the
    // sample starts with an attack), then back by whole beats to the first beat in the sample.
    double anchor = phase * double(hop);
    anchor += std::round((grid.pulse_from - anchor) / beat_frames) * beat_frames;
    uint32_t on_hit = 0;
    if (hit_near(anchor, on_hit) && (on_hit || starts)) anchor = on_hit;
    grid.first = uint32_t(std::max(0.0, anchor - std::floor(anchor / beat_frames) * beat_frames));

    return grid;
}

} // namespace

unsigned find_hits(const SampleData& s, std::array<Hit, max_hits>& out, BeatGrid& beats) {
    std::vector<Hit> found;
    std::vector<float> flux;
    analyse(s, found, flux);
    // Whether it starts with an attack: a peak in its first 10 ms at least a quarter of its highest.
    const uint32_t frames = std::min<uint32_t>(s.frames, uint32_t(std::min(s.left.size(), s.right.size())));
    float peak = 0, opening = 0;
    for (uint32_t n = 0; n < frames; ++n) {
        float v = std::fabs(s.left[n] + s.right[n]);
        peak = std::max(peak, v);
        if (n < 441) opening = std::max(opening, v);
    }
    beats = track_beats(flux, found, s.frames, peak > 0 && opening >= 0.25f * peak);
    if (found.size() > out.size()) { // keep the strongest, then put them back in time order
        std::sort(found.begin() + 1, found.end(), [](const Hit& a, const Hit& b) { return a.strength > b.strength; });
        found.resize(out.size());
        std::sort(found.begin(), found.end(), [](const Hit& a, const Hit& b) { return a.frame < b.frame; });
    }
    std::copy(found.begin(), found.end(), out.begin());
    return unsigned(found.size());
}

unsigned find_hits(const SampleData& s, std::array<Hit, max_hits>& out) {
    BeatGrid beats;
    return find_hits(s, out, beats);
}

} // namespace melodee
