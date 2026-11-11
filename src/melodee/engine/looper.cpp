// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/looper.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using looper::Do;

// The stretch's window: a squared sine over LoopStretch::window frames, worked out once.
const std::array<float, LoopStretch::window>& window_table() {
    static const std::array<float, LoopStretch::window> table = [] {
        std::array<float, LoopStretch::window> t {};
        for (unsigned n = 0; n < t.size(); ++n) { float s = std::sin(dsp::pi * float(n) / float(t.size())); t[n] = s * s; }
        return t;
    }();
    return table;
}

inline double wrap(double x, double length) {
    x = std::fmod(x, length);
    return x < 0 ? x + length : x;
}

// A loop's frame at a fractional place, read between its two neighbours (the last joins the first).
inline void read(const float* l, const float* r, uint32_t length, double at, float& out_l, float& out_r) {
    auto i = uint32_t(at);
    if (i >= length) i = length - 1;
    float frac = float(at - double(i));
    uint32_t j = i + 1 == length ? 0 : i + 1;
    out_l = l[i] + (l[j] - l[i]) * frac;
    out_r = r[i] + (r[j] - r[i]) * frac;
}

constexpr float tenths_of(double ticks_per_model_frame) { return float(ticks_per_model_frame * dsp::model_rate * 60.0 / ticks_per_quarter * 10.0); }
} // namespace

// ---- LoopStretch ------------------------------------------------------------

double LoopStretch::best(const float* l, const float* r, uint32_t length, double from, double around, double step) const {
    // The other tap's continuation, as mono, every second frame.
    std::array<float, match> want {};
    double len = length;
    for (unsigned k = 0; k < match; ++k) {
        auto i = uint32_t(wrap(from + double(k) * 2 * step, len));
        want[k] = l[i] + r[i];
    }
    double best_at = around;
    float best_score = -1e30f;
    for (int d = -int(search); d <= int(search); d += 2) {
        double s = around + d;
        float dot = 0, energy = 1e-9f;
        for (unsigned k = 0; k < match; ++k) {
            auto i = uint32_t(wrap(s + double(k) * 2 * step, len));
            float x = l[i] + r[i];
            dot += x * want[k];
            energy += x * x;
        }
        float score = dot / std::sqrt(energy);
        if (score > best_score) { best_score = score; best_at = s; }
    }
    return wrap(best_at, len);
}

void LoopStretch::frame(const float* l, const float* r, uint32_t length, double at, double step, float& out_l, float& out_r) {
    const auto& w = window_table();
    if (since >= hop) {
        since = 0;
        unsigned t = next;
        next ^= 1;
        double from = wrap(at, length);
        if (!first && on[t ^ 1]) from = best(l, r, length, tap[t ^ 1], at, step);
        tap[t] = from;
        age[t] = first ? hop : 0; // the first starts whole, so nothing dips while it comes in
        on[t] = true;
        first = false;
    }
    ++since;
    float sl = 0, sr = 0;
    for (unsigned t = 0; t < 2; ++t) {
        if (!on[t]) continue;
        if (age[t] >= window) { on[t] = false; continue; }
        float a, b;
        read(l, r, length, tap[t], a, b);
        sl += a * w[age[t]]; sr += b * w[age[t]];
        tap[t] += step;
        if (tap[t] >= length) tap[t] -= length;
        ++age[t];
    }
    out_l = sl; out_r = sr;
}

// ---- Looper -----------------------------------------------------------------

Looper::Looper() {
    (void)window_table();
    for (auto& b : held_buffer) b.store(nullptr);
}

Looper::~Looper() {
    for (auto& l : loops) { delete l.audio; delete l.undo; }
    for (auto* b : spare) delete b;
    LoopBuffer* b = nullptr;
    while (pool.pop(b)) delete b;
    while (retired.pop(b)) delete b;
    LoopLoad item;
    while (loads.pop(item)) delete item.buffer;
    for (auto& w : waiting) delete w.buffer;
}

uint32_t Looper::sources_used() const {
    uint32_t bits = 0;
    if (!enabled) return 0;
    for (auto& l : loops) bits |= uint32_t(1) << (l.source % looper::source_count);
    return bits;
}

double Looper::boundary(double tick) const {
    double q = quantize_mode == uint8_t(looper::Quantize::Bar) ? last_bar : quantize_mode == uint8_t(looper::Quantize::Beat) ? last_beat : 0;
    if (q <= 0) return tick;
    return std::ceil((tick - 1e-6) / q) * q;
}

// An armed take starts where the longest loop playing starts again, so it never begins halfway through what is
// heard; with none playing (or Quantize Off) on the next boundary.
double Looper::take_start(const Loop& self, double now) const {
    if (quantize_mode == uint8_t(looper::Quantize::Off)) return now;
    const Loop* lead = nullptr;
    for (auto& x : loops)
        if (&x != &self && x.ticks > 0 && x.length && (x.state == State::Playing || x.state == State::Overdub) && (!lead || x.ticks > lead->ticks)) lead = &x;
    if (!lead) return boundary(now);
    return lead->start + std::ceil((now - lead->start - 1e-6) / lead->ticks) * lead->ticks;
}

void Looper::queue(Loop& l, Pending p, double now) {
    l.pending = p;
    l.target = halted ? -1 : p == Pending::Start ? take_start(l, now) : boundary(now);
}

unsigned Looper::lag_of(const Loop& l, unsigned frames) const {
    unsigned lag = frames; // every source comes from the block before
    if (looper::source_from_outside(l.source)) lag += unsigned(input_latency >= 0 ? input_latency : int(default_lag));
    return lag;
}

double Looper::index_at(const Loop& l, double tick) const {
    if (l.ticks <= 0 || !l.length) return 0;
    return wrap(tick - l.start, l.ticks) / l.ticks * double(l.length);
}

void Looper::reset(Loop& l, bool keep_memory) {
    l.state = State::Empty; l.pending = Pending::None; l.target = -1;
    l.start = 0; l.ticks = 0; l.length = 0; l.take_at = 0; l.tempo = 0; l.layers = 0;
    l.write = Write::Off; l.write_next = Write::Off; l.write_in = -1; l.wx_known = false; l.dub = 0;
    l.dub_start = 0; l.dub_count = 0; l.undo_state = looper::Undo::None; l.undo_wanted = false;
    l.swapping = false; l.swap_at = 0; l.clearing = false; l.gain = 0; l.xfade = 0; l.was_stretched = false;
    l.stretch.reset();
    l.span = 0; l.peaks_next = looper::peak_columns;
    l.peaks.fill(0);
    if (!keep_memory && !l.held) { retire(l.audio); retire(l.undo); }
}

void Looper::configure(const LooperSettings& s, bool keep) {
    quantize_mode = s.quantize % uint8_t(looper::Quantize::Count);
    then_mode = s.then % uint8_t(looper::Then::Count);
    transport_mode = s.transport % uint8_t(looper::Transport::Count);
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto& l = loops[n];
        if (!keep) {
            if (l.fit.load() != FitState::None) l.fit_cancel.store(true);
            reset(l, true);
            l.waiting_load = s.on && s.loops[n].frames > 0; // its file comes from the service thread
        }
        l.bars = s.loops[n].bars;
        l.feedback = std::clamp<int>(s.loops[n].feedback, 0, 100) / 100.0f;
        l.source = s.loops[n].source < looper::source_count && s.loops[n].source != looper::source_loop(n) ? s.loops[n].source : looper::source_input;
    }
    if (s.on && !enabled) ask(Do::Enable, 0, 1);
    if (!s.on && enabled) ask(Do::Enable, 0, 0);
}

void Looper::ask(Do what, unsigned n, int value, uint32_t position) {
    n %= loop_tracks;
    auto& l = loops[n];
    bool per_loop = what == Do::Primary || what == Do::Toggle || what == Do::Halt || what == Do::Now || what == Do::Undo || what == Do::Clear;
    bool any_held = false;
    for (auto& x : loops) any_held |= x.held;
    if ((per_loop && l.held) || (what == Do::Enable && !value && any_held)) {
        if (deferred_count < deferred.size()) deferred[deferred_count++] = {what, uint8_t(n), value, position};
        return;
    }
    switch (what) {
    case Do::Enable:
        if (value) { enabled = true; disable_wanted = false; want_memory.store(true, std::memory_order_release); }
        else if (enabled) disable_wanted = true; // once no fit runs (run())
        break;
    case Do::Primary: if (enabled) primary(l, n, own_tick); break;
    case Do::Toggle: if (enabled) toggle(l, own_tick); break;
    case Do::Halt: if (enabled) halt(l, own_tick); break;
    case Do::Now:
        if (!enabled) break;
        if (l.state == State::Playing || l.state == State::Overdub) { if (l.state == State::Overdub) punch(l, false, lag_of(l, block_frames)); l.state = State::Stopped; l.pending = Pending::None; }
        else if (l.state == State::Stopped) { l.state = State::Playing; l.pending = Pending::None; }
        else toggle(l, own_tick);
        break;
    case Do::Undo:
        if (!enabled || !l.length) break;
        if (l.fit.load() != FitState::None || l.swapping) { refuse(l, looper::Error::Busy); break; }
        if (l.state == State::Overdub) { punch(l, false, lag_of(l, block_frames)); l.state = State::Playing; l.undo_wanted = true; break; }
        if (l.write != Write::Off || l.write_in >= 0) { l.undo_wanted = true; break; }
        if (l.undo_state == looper::Undo::None) { refuse(l, looper::Error::Locked); break; }
        l.swapping = true; l.swap_at = 0;
        break;
    case Do::Clear:
        if (l.fit.load() != FitState::None) l.fit_cancel.store(true);
        if (l.state != State::Empty) l.clearing = true;
        l.pending = Pending::None;
        break;
    case Do::Length:
        if (l.state != State::Empty && l.state != State::Armed) { refuse(l, looper::Error::Locked); break; }
        l.bars = uint8_t(std::clamp(value, 0, 16));
        break;
    case Do::Feedback: l.feedback = std::clamp(value, 0, 100) / 100.0f; break;
    case Do::Quantize: quantize_mode = uint8_t(std::clamp(value, 0, int(looper::Quantize::Count) - 1)); break;
    case Do::Then: then_mode = uint8_t(std::clamp(value, 0, int(looper::Then::Count) - 1)); break;
    case Do::Transport: transport_mode = uint8_t(std::clamp(value, 0, int(looper::Transport::Count) - 1)); break;
    case Do::Latency: input_latency = value < 0 ? -1 : int(position); break;
    case Do::Source:
        if (value < 0 || value >= int(looper::source_count) || value == looper::source_loop(n)) { refuse(l, looper::Error::Locked); break; }
        if (l.state == State::Recording || l.state == State::Overdub || l.write != Write::Off) { refuse(l, looper::Error::Locked); break; }
        l.source = uint8_t(value);
        break;
    }
}

void Looper::primary(Loop& l, unsigned n, double now) {
    (void)n;
    if (l.fit.load() != FitState::None) { l.fit_cancel.store(true); return; } // Push again: no fit
    switch (l.state) {
    case State::Empty: {
        if (!l.audio || !l.undo) { refuse(l, looper::Error::NoMemory); return; }
        if (l.waiting_load) { refuse(l, looper::Error::Busy); return; }
        if (l.bars && last_per_frame > 0) {
            double frames = l.bars * last_bar / last_per_frame * (dsp::model_rate / rate);
            if (frames > looper::capacity) { refuse(l, looper::Error::TooLong); return; }
        }
        l.state = State::Armed;
        queue(l, Pending::Start, now);
        break;
    }
    case State::Armed: l.state = State::Empty; l.pending = Pending::None; break;
    case State::Recording: close_on_bar(l, now, true); break;
    case State::Playing: {
        if (l.swapping) { refuse(l, looper::Error::Busy); return; }
        if (halted) { refuse(l, looper::Error::Locked); return; } // no overdub while the transport is stopped
        if (stretched(l)) { fit_start(l, true); return; }
        punch(l, true, lag_of(l, block_frames));
        break;
    }
    case State::Overdub: punch(l, false, lag_of(l, block_frames)); l.state = State::Playing; break;
    case State::Stopped: toggle(l, now); break;
    }
}

void Looper::toggle(Loop& l, double now) {
    switch (l.state) {
    case State::Playing: case State::Overdub:
        if (l.pending == Pending::Stop) l.pending = Pending::None; else queue(l, Pending::Stop, now);
        break;
    case State::Stopped:
        if (l.pending == Pending::Play) l.pending = Pending::None; else queue(l, Pending::Play, now);
        break;
    case State::Armed: l.state = State::Empty; l.pending = Pending::None; break;
    case State::Recording: close_on_bar(l, now, true); break;
    case State::Empty: break;
    }
}

void Looper::halt(Loop& l, double now) {
    if (l.state == State::Recording) close_on_bar(l, now, false);
    if (l.state == State::Overdub) punch(l, false, 0);
    if (l.state == State::Playing || l.state == State::Overdub) l.state = State::Stopped;
    if (l.state == State::Armed) l.state = State::Empty;
    l.pending = Pending::None;
}

bool Looper::stretched(const Loop& l) const {
    if (!l.length || l.ticks <= 0 || last_per_frame <= 0) return false;
    double speed = double(l.length) / l.ticks * last_per_frame; // source frames a device frame, to stay on the beat
    double r = speed / (dsp::model_rate / rate);
    return std::fabs(r - 1) > 2e-4;
}

void Looper::set_write(Loop& l, Write mode) {
    if (mode == Write::DubIn && (l.write == Write::Off)) { // a new pass: its frames' old layers go to the undo
        l.dub_count = 0;
        l.undo_state = looper::Undo::None;
        l.dub = 0;
        ++l.layers;
    }
    if (mode == Write::Take) { l.wx = -double(dsp::model_rate / rate); l.wx_known = true; }
    else if (mode != Write::Off && (l.write == Write::Off || l.write == Write::Take)) l.wx_known = false;
    if (mode == Write::Off && (l.write == Write::Dub || l.write == Write::DubIn || l.write == Write::DubOut)) end_pass(l);
    l.write = mode;
}

void Looper::end_pass(Loop& l) {
    l.dub = 0;
    if (l.dub_count) { l.undo_state = looper::Undo::Undo; l.peaks_next = 0; }
    else if (l.layers > 1) --l.layers; // punched in and out with nothing written
    if (l.undo_wanted) { l.undo_wanted = false; if (l.undo_state != looper::Undo::None) { l.swapping = true; l.swap_at = 0; } }
}

void Looper::schedule_write(Loop& l, Write mode, unsigned lag) {
    if (l.write_in >= 0) { set_write(l, l.write_next); l.write_in = -1; } // one waits at a time
    if (!lag) { set_write(l, mode); return; }
    l.write_next = mode;
    l.write_in = int(lag);
}

void Looper::punch(Loop& l, bool in, unsigned lag) {
    if (in) {
        l.state = State::Overdub;
        if (l.write == Write::DubOut) { l.write = Write::DubIn; return; }
        schedule_write(l, Write::DubIn, lag);
    } else {
        if (l.write == Write::Take) { l.after_take = Write::Off; return; }
        if (!lag) {
            if (l.write_in >= 0 && l.write_next == Write::DubIn) { l.write_in = -1; } // never began
            if (l.write != Write::Off && l.write != Write::Take) set_write(l, Write::Off);
            return;
        }
        if (l.write_in >= 0 && l.write_next == Write::DubIn) { l.write_in = -1; return; } // punched out before it began
        schedule_write(l, Write::DubOut, lag);
    }
}

void Looper::begin_take(Loop& l, double at, unsigned lag) {
    l.state = State::Recording;
    l.start = at; l.ticks = 0; l.length = 0; l.take_at = 0; l.tempo = 0; l.layers = 1;
    l.undo_state = looper::Undo::None; l.dub_count = 0;
    l.span = l.bars ? uint32_t(std::min<double>(looper::capacity, l.bars * last_bar / last_per_frame * (dsp::model_rate / rate)))
                    : uint32_t(std::min<double>(looper::capacity, 4 * last_bar / std::max(last_per_frame, 1e-9) * (dsp::model_rate / rate)));
    l.span = std::max<uint32_t>(l.span, looper::peak_columns);
    l.peaks.fill(0);
    l.peaks_next = looper::peak_columns;
    l.after_take = Write::Off;
    l.stretch.reset();
    schedule_write(l, Write::Take, lag);
    if (l.bars) { l.pending = Pending::Close; l.target = at + l.bars * last_bar; }
    else l.pending = Pending::None;
}

// A take closes on whole bars from its start, so the loop is always whole bars long. A set length plays out to its
// last bar (pressing does not cut it short). Free, pressed (`nearest`): the nearest bar, at least the first; one
// still ahead closes on its tick, one just passed closes now and drops what ran past it. Stopped (not `nearest`):
// nothing more comes, so the last bar passed; under a bar it keeps what it has.
void Looper::close_on_bar(Loop& l, double now, bool nearest) {
    if (l.state != State::Recording) return;
    if (nearest && l.bars) return; // its Close waits on its last bar already
    double bar = std::max(last_bar, 1.0), per = std::max(last_per_frame, 1e-9), ratio = dsp::model_rate / rate;
    double ran = l.take_at / ratio * per; // the ticks the take has run, at this pace
    double whole = nearest ? std::max(1.0, std::round(ran / bar)) : std::floor(ran / bar + 1e-6);
    if (whole < 1) { close_take(l, now); return; }
    double past = l.take_at - whole * bar / per * ratio; // frames the take ran past that bar
    if (past < 0 && nearest) { l.pending = Pending::Close; l.target = l.start + whole * bar; return; }
    close_take(l, l.start + whole * bar, std::max(past, 0.0));
}

void Looper::close_take(Loop& l, double at, double past) {
    if (l.state != State::Recording) return;
    l.length = uint32_t(std::clamp<double>(std::round(l.take_at - past), 1, looper::capacity));
    l.ticks = at - l.start;
    if (l.ticks <= 1e-6) l.ticks = l.length / (dsp::model_rate / rate) * std::max(last_per_frame, 1e-9);
    l.tempo = uint16_t(std::clamp<float>(std::round(tenths_of(l.ticks / l.length)), 1, 65535));
    l.pending = Pending::None;
    auto then = looper::Then(then_mode);
    l.state = then == looper::Then::Overdub ? State::Overdub : then == looper::Then::Stop ? State::Stopped : State::Playing;
    l.after_take = then == looper::Then::Overdub ? Write::Dub : Write::Off;
    if (l.write == Write::Off && l.write_in < 0) take_written(l); // nothing more to come
}

// The first take is all written: its ends faded, the waveform drawn again over its length, then on as it says.
void Looper::take_written(Loop& l) {
    seam(l);
    l.undo_wanted = false; // a take has nothing to undo
    l.span = l.length;
    l.peaks_next = 0;
    Write next = l.state == State::Overdub ? l.after_take : Write::Off;
    l.write = Write::Off;
    if (next == Write::Dub) { set_write(l, Write::DubIn); l.dub = 1; l.write = Write::Dub; }
}

void Looper::seam(Loop& l) {
    if (!l.audio || l.length < 4 * looper::fade) return;
    float* a[2] {l.audio->left.get(), l.audio->right.get()};
    for (unsigned k = 0; k < looper::fade; ++k) {
        float g = float(k) / looper::fade;
        for (auto* c : a) { c[k] *= g; c[l.length - 1 - k] *= g; }
    }
}

void Looper::act(Loop& l, Pending p, double at, unsigned lag) {
    l.pending = Pending::None;
    switch (p) {
    case Pending::Start: if (l.state == State::Armed) begin_take(l, at, lag); break;
    case Pending::Close: close_take(l, at); break;
    case Pending::Play: if (l.state == State::Stopped) l.state = State::Playing; break;
    case Pending::Stop:
        if (l.state == State::Overdub) punch(l, false, lag);
        if (l.state == State::Playing || l.state == State::Overdub) l.state = State::Stopped;
        break;
    case Pending::None: break;
    }
}

void Looper::write_at(Loop& l, unsigned n, uint32_t k, float in_l, float in_r) {
    float* al = l.audio->left.get();
    float* ar = l.audio->right.get();
    if (l.write == Write::Take) {
        al[k] = in_l; ar[k] = in_r;
    } else {
        uint32_t len = l.length;
        uint32_t from = (k + len - l.dub_start) % len;
        if (!l.dub_count) { l.dub_start = k; from = 0; }
        if (from >= l.dub_count) { // not written this pass yet: its old layers kept for undo
            l.undo->left[k] = al[k]; l.undo->right[k] = ar[k];
            l.dub_count = std::min(len, from + 1);
        }
        float g = l.dub;
        if (l.source == looper::source_mix) { // the mix holds the loop itself: it replaces
            al[k] = al[k] * (1 - g) + in_l * g; ar[k] = ar[k] * (1 - g) + in_r * g;
        } else {
            float keep = 1 - g * (1 - l.feedback);
            al[k] = al[k] * keep + in_l * g; ar[k] = ar[k] * keep + in_r * g;
        }
    }
    // The waveform: a column's loudest frame.
    uint32_t span = std::max<uint32_t>(l.span, 1);
    if (k < span) {
        unsigned c = unsigned(uint64_t(k) * looper::peak_columns / span);
        auto v = uint8_t(std::min(1.0f, std::max(std::fabs(al[k]), std::fabs(ar[k]))) * 255);
        if (v > l.peaks[c]) l.peaks[c] = v;
    }
    (void)n;
}

void Looper::write_frame(Loop& l, unsigned n, double x, float in_l, float in_r) {
    double ratio = dsp::model_rate / rate;
    if (l.write == Write::Take) {
        double prev = l.wx;
        x = prev + ratio;
        auto first = int64_t(std::floor(prev)) + 1, last = int64_t(std::floor(x));
        double limit = l.length ? l.length : looper::capacity;
        for (int64_t k = std::max<int64_t>(first, 0); k <= last && k < int64_t(limit); ++k) {
            float t = float((double(k) - prev) / ratio);
            write_at(l, n, uint32_t(k), l.in_l + (in_l - l.in_l) * t, l.in_r + (in_r - l.in_r) * t);
        }
        // A free take grows past its span: the waveform halves into the first half.
        if (!l.bars && !l.length && x >= l.span && l.span < looper::capacity) {
            for (unsigned c = 0; c < looper::peak_columns / 2; ++c) l.peaks[c] = std::max(l.peaks[2 * c], l.peaks[2 * c + 1]);
            std::fill(l.peaks.begin() + looper::peak_columns / 2, l.peaks.end(), 0);
            l.span = std::min<uint32_t>(looper::capacity, l.span * 2);
        }
        l.wx = x;
        l.in_l = in_l; l.in_r = in_r;
        if (l.length && x >= double(l.length) - 1) take_written(l);
        return;
    }
    // Overdub: where the play side was `lag` ago, in the loop.
    if (!l.length) return;
    double len = l.length;
    if (!l.wx_known) { l.wx = x; l.wx_known = true; l.in_l = in_l; l.in_r = in_r; return; }
    double dx = x - l.wx;
    if (dx < -len / 2) dx += len;
    if (dx > len / 2) dx -= len;
    if (dx <= 0 || dx > 4) { l.wx = x; l.in_l = in_l; l.in_r = in_r; return; } // a jump: on from here
    double prev = l.wx;
    auto first = int64_t(std::floor(prev)) + 1, last = int64_t(std::floor(prev + dx));
    for (int64_t k = first; k <= last; ++k) {
        float t = float((double(k) - prev) / dx);
        auto at = uint32_t(wrap(double(k), len));
        write_at(l, n, at, l.in_l + (in_l - l.in_l) * t, l.in_r + (in_r - l.in_r) * t);
    }
    l.wx = x;
    l.in_l = in_l; l.in_r = in_r;
}

void Looper::play_frame(Loop& l, double tick, float& out_l, float& out_r) {
    out_l = out_r = 0;
    bool sound = (l.state == State::Playing || l.state == State::Overdub) && !halted && !l.clearing && l.length;
    float target = sound ? 1.0f : 0.0f;
    if (l.gain < target) l.gain = std::min(target, l.gain + 1.0f / looper::ramp);
    else if (l.gain > target) l.gain = std::max(target, l.gain - 1.0f / looper::ramp);
    if (l.gain <= 0 || !l.length || l.ticks <= 0 || !l.audio) { l.was_stretched = false; return; }
    const float* al = l.audio->left.get();
    const float* ar = l.audio->right.get();
    double at = index_at(l, tick);
    bool stretch = stretched(l) && l.length >= 4 * LoopStretch::window;
    float a = 0, b = 0;
    if (stretch) {
        if (!l.was_stretched) l.stretch.reset();
        l.stretch.frame(al, ar, l.length, at, dsp::model_rate / rate, a, b);
    } else {
        if (l.was_stretched) l.xfade = looper::ramp;
        read(al, ar, l.length, at, a, b);
        if (l.xfade) { // from the stretch back to plain reading
            float c, d;
            l.stretch.frame(al, ar, l.length, at, dsp::model_rate / rate, c, d);
            float g = float(l.xfade) / looper::ramp;
            a = a * (1 - g) + c * g; b = b * (1 - g) + d * g;
            --l.xfade;
        }
    }
    l.was_stretched = stretch;
    out_l = a * l.gain; out_r = b * l.gain;
}

void Looper::peak_column(Loop& l, unsigned column) {
    if (!l.audio || !l.span) { l.peaks[column] = 0; return; }
    uint32_t span = l.span, from = uint32_t(uint64_t(column) * span / looper::peak_columns),
             to = std::min<uint32_t>(uint32_t(uint64_t(column + 1) * span / looper::peak_columns), l.length ? l.length : span);
    float m = 0;
    const float* al = l.audio->left.get();
    const float* ar = l.audio->right.get();
    for (uint32_t k = from; k < to; ++k) m = std::max(m, std::max(std::fabs(al[k]), std::fabs(ar[k])));
    l.peaks[column] = uint8_t(std::min(1.0f, m) * 255);
}

void Looper::swap_step(Loop& l) {
    uint32_t len = l.length, count = l.dub_count;
    uint32_t end = std::min(count, l.swap_at + looper::swap_chunk);
    float* al = l.audio->left.get(); float* ar = l.audio->right.get();
    float* ul = l.undo->left.get(); float* ur = l.undo->right.get();
    for (uint32_t s = l.swap_at; s < end; ++s) {
        uint32_t k = (l.dub_start + s) % len;
        std::swap(al[k], ul[k]); std::swap(ar[k], ur[k]);
    }
    l.swap_at = end;
    if (l.swap_at < count) return;
    l.swapping = false;
    if (l.undo_state == looper::Undo::Undo) { l.undo_state = looper::Undo::Redo; if (l.layers > 1) --l.layers; }
    else { l.undo_state = looper::Undo::Undo; ++l.layers; }
    l.peaks_next = 0;
}

void Looper::fit_start(Loop& l, bool then_dub) {
    double frames = l.ticks / last_per_frame * (dsp::model_rate / rate);
    if (frames > looper::capacity || frames < 64) { refuse(l, looper::Error::Tempo); return; }
    l.fit_length = uint32_t(std::round(frames));
    l.fit_tempo = uint16_t(std::clamp<float>(std::round(tenths_of(l.ticks / frames)), 1, 65535));
    l.fit_then_dub = then_dub;
    l.undo_state = looper::Undo::None; // the undo buffer becomes the fitted loop
    l.fit_cancel.store(false);
    l.fit.store(FitState::Wanted, std::memory_order_release);
}

void Looper::fit_finish(Loop& l) {
    auto f = l.fit.load(std::memory_order_acquire);
    if (f == FitState::Done) {
        l.fit.store(FitState::None);
        if (l.state != State::Playing && l.state != State::Stopped && l.state != State::Overdub) return;
        if (l.fit_cancel.load()) return;
        std::swap(l.audio, l.undo);
        l.length = l.fit_length;
        l.tempo = l.fit_tempo;
        l.span = l.length;
        l.peaks_next = 0;
        l.stretch.reset();
        l.was_stretched = false;
        if (l.fit_then_dub && l.state == State::Playing && !halted) punch(l, true, lag_of(l, block_frames));
    } else if (f == FitState::Failed) {
        l.fit.store(FitState::None);
        refuse(l, looper::Error::Tempo);
    }
}

void Looper::run(const Clock& clock, const Sources& sources, unsigned frames, float (*out_l)[looper::block], float (*out_r)[looper::block]) {
    frames = std::min(frames, looper::block);
    for (unsigned n = 0; n < loop_tracks; ++n) { std::fill(out_l[n], out_l[n] + frames, 0.0f); std::fill(out_r[n], out_r[n] + frames, 0.0f); }
    if (clock.rate != rate) {
        rate = clock.rate;
        const float q[2] {0.5412f, 1.3066f};
        float w = 2 * dsp::pi * 19000.0f / rate, cw = std::cos(w);
        for (unsigned s = 0; s < 2; ++s) {
            float alpha = std::sin(w) / (2 * q[s]), a0 = 1 + alpha;
            lp_coef[s] = {(1 - cw) / 2 / a0, (1 - cw) / a0, (1 - cw) / 2 / a0, -2 * cw / a0, (1 - alpha) / a0};
        }
    }
    last_bar = clock.bar; last_beat = clock.beat;
    if (clock.per_frame > 0) last_per_frame = clock.per_frame;
    block_frames = sources.frames ? sources.frames : frames;
    default_lag = clock.input_lag;

    // Memory handed in: into the loops that lack it, else kept spare (or back, when the Looper is off).
    LoopBuffer* b = nullptr;
    while (pool.pop(b)) {
        bool kept = false;
        for (auto& s : spare) if (!s) { s = b; kept = true; break; }
        if (!kept) { LoopBuffer* x = b; retire(x); }
    }
    if (enabled && !disable_wanted)
        for (auto& l : loops)
            for (LoopBuffer** slot : {&l.audio, &l.undo})
                if (!*slot) for (auto& s : spare) if (s) { *slot = s; s = nullptr; break; }
    // Saved loops coming back: each swapped in for its loop's audio.
    LoopLoad item;
    while (loads.pop(item)) {
        auto& w = waiting[item.loop % loop_tracks];
        if (w.buffer) { LoopBuffer* x = w.buffer; retire(x); }
        w = item;
    }
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto& w = waiting[n];
        auto& l = loops[n];
        if (!w.buffer) continue;
        if (!enabled) { retire(w.buffer); l.waiting_load = false; continue; }
        if (l.held || l.fit.load() != FitState::None || l.write != Write::Off || l.write_in >= 0) continue;
        reset(l, true);
        if (l.audio) { LoopBuffer* x = l.audio; if (!retire(x)) continue; }
        l.audio = w.buffer; w.buffer = nullptr;
        l.length = std::min(w.frames, looper::capacity);
        l.ticks = w.ticks; l.tempo = w.tempo; l.layers = w.layers;
        l.state = !(l.length && l.ticks > 0) ? State::Empty : w.stopped ? State::Stopped : State::Playing;
        l.start = 0;
        l.span = l.length;
        l.peaks_next = 0;
        l.waiting_load = false;
    }

    // The clock. Following the transport, the loops wait while it is stopped or counting in; free, they keep time.
    bool follow = transport_mode == uint8_t(looper::Transport::Follow);
    bool now_halted = clock.counting || (follow && !clock.running);
    bool restart = clock.restart && clock.running && !clock.counting;
    double per = clock.per_frame > 0 ? clock.per_frame : last_per_frame;
    if (clock.running && !clock.counting) {
        // The sequencer's place; swing and MIDI clock move it a little either side, so small differences are eased.
        double d = clock.tick - own_tick;
        if (restart || !was_running || std::fabs(d) > 2.0 * ticks_per_quarter) own_tick = clock.tick;
        else own_tick += d * 0.002;
    }
    if (now_halted && !halted) { // the transport stopped: takes close, overdubs end, every loop fades and holds
        for (auto& l : loops) {
            l.halt_tick = own_tick;
            if (l.state == State::Recording) close_on_bar(l, own_tick, false);
            if (l.state == State::Overdub) { punch(l, false, 0); l.state = State::Playing; }
            if (l.pending != Pending::None) l.target = -1;
        }
    }
    halted = now_halted;
    was_running = clock.running && !clock.counting;
    if (restart) {
        for (auto& l : loops) {
            if (l.state == State::Recording) close_on_bar(l, own_tick, false);
            if (l.length) l.start = 0; // every loop lines up with bar 1
            l.stretch.reset();
        }
    }
    if (!halted)
        for (auto& l : loops)
            if (l.pending != Pending::None && (l.target < 0 || restart)) l.target = l.pending == Pending::Start ? take_start(l, own_tick) : boundary(own_tick);

    // A fit done, an undo going on, a clear faded out, the Looper turned off once nothing is in the way.
    for (auto& l : loops) {
        fit_finish(l);
        if (l.swapping && l.write == Write::Off && l.write_in < 0) swap_step(l);
        if (l.clearing) { // whatever it was writing is dropped; it empties once its output has faded
            l.write_in = -1; l.write = Write::Off; l.dub = 0; l.undo_wanted = false; l.swapping = false;
            if (l.gain <= 0 && l.fit.load() == FitState::None && !l.held) reset(l, true);
        }
    }
    if (disable_wanted) {
        bool busy = false;
        for (auto& l : loops) busy |= l.fit.load() != FitState::None || l.held;
        if (!busy) {
            for (auto& l : loops) reset(l, false);
            for (auto& s : spare) retire(s);
            enabled = false; disable_wanted = false;
            want_memory.store(false, std::memory_order_release);
        }
    }
    // Held for the service thread: granted once nothing writes it; asks waiting on it come back once let go.
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto& l = loops[n];
        bool asked = l.hold_asked.load(std::memory_order_acquire);
        if (asked && !l.held && l.audio && l.length && (l.state == State::Playing || l.state == State::Stopped) && l.write == Write::Off
            && l.write_in < 0 && !l.swapping && l.fit.load() == FitState::None && !l.clearing) {
            l.held = true;
            held_buffer[n].store(l.audio, std::memory_order_release);
        }
        if (!asked && l.held) {
            l.held = false;
            held_buffer[n].store(nullptr, std::memory_order_release);
            unsigned count = deferred_count;
            deferred_count = 0;
            std::array<Asked, 16> again = deferred;
            for (unsigned k = 0; k < count; ++k) ask(again[k].what, again[k].loop, again[k].value, again[k].position);
        }
    }

    // Each loop through the block: its pending action on its frame, the writer `lag` behind, the play side.
    double ratio = dsp::model_rate / rate;
    for (unsigned n = 0; n < loop_tracks && enabled; ++n) {
        auto& l = loops[n];
        unsigned lag = lag_of(l, block_frames);
        const auto& src = sources.at[l.source % looper::source_count];
        unsigned f = 0;
        unsigned guard = 0;
        while (f < frames && ++guard < 64) {
            unsigned end = frames;
            double tick_f = own_tick + f * per;
            if (!halted && l.pending != Pending::None && l.target >= 0) {
                double d = (l.target - tick_f) / per;
                if (d <= 1e-9) { act(l, l.pending, l.target, lag); continue; }
                end = std::min<unsigned>(end, f + unsigned(std::ceil(d - 1e-9)));
            }
            if (l.state == State::Recording && !halted) {
                double left = (double(looper::capacity) - l.take_at) / ratio;
                if (left <= 0) { close_take(l, tick_f); continue; }
                end = std::min<unsigned>(end, f + unsigned(std::ceil(left)));
            }
            if (l.write_in == 0) { l.write_in = -1; set_write(l, l.write_next); continue; }
            if (l.write_in > 0) end = std::min<unsigned>(end, f + unsigned(l.write_in));
            if (end <= f) end = f + 1;
            for (unsigned k = f; k < end; ++k) {
                double tick = halted ? (l.halt_tick += per) : own_tick + k * per;
                play_frame(l, tick, out_l[n][k], out_r[n][k]);
                if (l.write != Write::Off) {
                    float in_l = 0, in_r = 0;
                    if (src[0] && k < sources.frames) { in_l = src[0][k]; in_r = src[1] ? src[1][k] : in_l; }
                    if (rate > 50000.0f) {
                        for (unsigned s = 0; s < 2; ++s) {
                            in_l = biquad(lp_coef[s], l.lp[s * 2], in_l);
                            in_r = biquad(lp_coef[s], l.lp[s * 2 + 1], in_r);
                        }
                    }
                    if (l.write == Write::DubIn) { l.dub = std::min(1.0f, l.dub + 1.0f / looper::fade); if (l.dub >= 1) l.write = Write::Dub; }
                    double x = l.write == Write::Take ? 0 : index_at(l, (halted ? tick : own_tick + k * per) - lag * per);
                    write_frame(l, n, x, in_l, in_r);
                    if (l.write == Write::DubOut) { l.dub = std::max(0.0f, l.dub - 1.0f / looper::fade); if (l.dub <= 0) set_write(l, Write::Off); }
                }
            }
            unsigned done = end - f;
            if (l.write_in > 0) l.write_in = std::max(0, l.write_in - int(done));
            if (l.state == State::Recording && !halted) l.take_at += done * ratio;
            f = end;
        }
        // The waveform worked out again a few columns a block after a take, undo, fit or load.
        for (unsigned c = 0; c < 4 && l.peaks_next < looper::peak_columns; ++c) peak_column(l, l.peaks_next++);
    }
    if (!halted || !follow) own_tick += frames * per;
    for (unsigned n = 0; n < loop_tracks; ++n) publish(n);
}

void Looper::publish(unsigned n) {
    const auto& l = loops[n];
    auto& v = loops_view[n];
    v.state.store(uint8_t(l.state), std::memory_order_relaxed);
    v.pending.store(uint8_t(l.pending), std::memory_order_relaxed);
    v.bars.store(l.bars, std::memory_order_relaxed);
    v.layers.store(l.layers, std::memory_order_relaxed);
    v.source.store(l.source, std::memory_order_relaxed);
    v.feedback.store(uint8_t(std::lround(l.feedback * 100)), std::memory_order_relaxed);
    v.undo.store(uint8_t(l.undo_state), std::memory_order_relaxed);
    v.error.store(uint8_t(l.error), std::memory_order_relaxed);
    v.errors.store(l.errors, std::memory_order_relaxed);
    v.length.store(l.length, std::memory_order_relaxed);
    uint32_t place = l.state == State::Recording ? uint32_t(l.take_at) : uint32_t(index_at(l, halted ? l.halt_tick : own_tick));
    v.position.store(place, std::memory_order_relaxed);
    v.span.store(l.span, std::memory_order_relaxed);
    v.ticks.store(l.ticks, std::memory_order_relaxed);
    v.tempo.store(l.tempo, std::memory_order_relaxed);
    v.stretched.store(stretched(l), std::memory_order_relaxed);
    v.fitting.store(l.fit.load() != FitState::None, std::memory_order_relaxed);
    v.sounding.store(l.gain > 0, std::memory_order_relaxed);
    v.held.store(l.held, std::memory_order_relaxed);
    v.ready.store(l.audio && l.undo, std::memory_order_relaxed);
    for (unsigned c = 0; c < looper::peak_columns; ++c) v.peaks[c].store(l.peaks[c], std::memory_order_relaxed);
    if (n + 1 == loop_tracks) {
        bool armed = false, recording = false;
        for (auto& x : loops) {
            armed |= x.state == State::Armed;
            recording |= x.state == State::Recording || x.state == State::Overdub || (x.write != Write::Off && x.write != Write::Take);
        }
        any_armed.store(armed, std::memory_order_relaxed);
        any_recording.store(recording, std::memory_order_relaxed);
        enabled_shown.store(enabled, std::memory_order_relaxed);
        quantize_now.store(quantize_mode, std::memory_order_relaxed);
        then_now.store(then_mode, std::memory_order_relaxed);
        transport_now.store(transport_mode, std::memory_order_relaxed);
        latency_now.store(input_latency, std::memory_order_relaxed);
        tick_shown.store(own_tick, std::memory_order_relaxed);
    }
}

Looper::View Looper::view(unsigned n) const {
    const auto& v = loops_view[n % loop_tracks];
    View out;
    out.state = State(v.state.load(std::memory_order_relaxed));
    out.pending = Pending(v.pending.load(std::memory_order_relaxed));
    out.bars = v.bars.load(std::memory_order_relaxed);
    out.layers = v.layers.load(std::memory_order_relaxed);
    out.source = v.source.load(std::memory_order_relaxed);
    out.feedback = v.feedback.load(std::memory_order_relaxed);
    out.undo = looper::Undo(v.undo.load(std::memory_order_relaxed));
    out.error = looper::Error(v.error.load(std::memory_order_relaxed));
    out.errors = v.errors.load(std::memory_order_relaxed);
    out.length = v.length.load(std::memory_order_relaxed);
    out.position = v.position.load(std::memory_order_relaxed);
    out.span = v.span.load(std::memory_order_relaxed);
    out.ticks = v.ticks.load(std::memory_order_relaxed);
    out.tempo = v.tempo.load(std::memory_order_relaxed);
    out.stretched = v.stretched.load(std::memory_order_relaxed);
    out.fitting = v.fitting.load(std::memory_order_relaxed);
    out.sounding = v.sounding.load(std::memory_order_relaxed);
    out.held = v.held.load(std::memory_order_relaxed);
    out.ready = v.ready.load(std::memory_order_relaxed);
    return out;
}

// ---- Service thread -----------------------------------------------------------

void Looper::service() {
    LoopBuffer* b = nullptr;
    while (retired.pop(b)) { delete b; owned.fetch_sub(1, std::memory_order_acq_rel); }
    // Two buffers a loop while the Looper is on (a load brings its own and hands one back).
    if (want_memory.load(std::memory_order_acquire) && owned.load() < 2 * loop_tracks) {
        auto* made = new LoopBuffer;
        if (pool.push(made)) owned.fetch_add(1, std::memory_order_acq_rel);
        else delete made;
    }
    for (auto& l : loops) {
        auto expected = FitState::Wanted;
        if (!l.fit.compare_exchange_strong(expected, FitState::Running)) continue;
        if (l.fit_cancel.load()) { l.fit.store(FitState::None); continue; }
        fit_render(l);
    }
}

bool Looper::give_load(LoopLoad item) {
    if (!loads.push(item)) return false;
    owned.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

// Renders the loop's audio at its new length into its undo buffer, the stretch keeping its pitch, its end
// joining its start; the audio thread swaps the two once it is done. The audio thread neither writes the
// audio nor touches the undo meanwhile (fit_start, primary).
void Looper::fit_render(Loop& l) {
    const float* al = l.audio->left.get();
    const float* ar = l.audio->right.get();
    float* dl = l.undo->left.get();
    float* dr = l.undo->right.get();
    uint32_t length = l.length, out = l.fit_length;
    LoopStretch s;
    s.reset();
    double per = double(length) / out;
    // A window ahead of the start first, so the taps are going where the loop begins.
    for (int64_t j = -int64_t(LoopStretch::window); j < int64_t(out); ++j) {
        if ((j & 4095) == 0 && l.fit_cancel.load(std::memory_order_relaxed)) { l.fit.store(FitState::None); return; }
        float a, b;
        s.frame(al, ar, length, wrap(double(j) * per, length), 1.0, a, b);
        if (j >= 0) { dl[j] = a; dr[j] = b; }
    }
    if (out >= 4 * looper::fade)
        for (unsigned k = 0; k < looper::fade; ++k) { // the seam a touch softer
            float g = 0.5f + 0.5f * float(k) / looper::fade;
            dl[k] *= g; dr[k] *= g; dl[out - 1 - k] *= g; dr[out - 1 - k] *= g;
        }
    l.fit.store(FitState::Done, std::memory_order_release);
}

bool Looper::hold(unsigned loop) {
    auto& l = loops[loop % loop_tracks];
    l.hold_asked.store(true, std::memory_order_release);
    return held_buffer[loop % loop_tracks].load(std::memory_order_acquire) != nullptr;
}

void Looper::release(unsigned loop) { loops[loop % loop_tracks].hold_asked.store(false, std::memory_order_release); }

} // namespace melodee
