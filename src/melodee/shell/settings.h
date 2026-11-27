// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include "drivers/screen_mirror.h"
#include "melodee/model/input_config.h"
#include "melodee/model/midi_config.h"
#include "melodee/ui/faces.h"
#include "melodee/ui/palettes.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

// Melodee's own settings, not the project's: how the pads respond and
// light, whether their pressure is used, how the knobs, Knob FX and the fader take over a value, the screen and where its
// place bar sits, the audio
// device's rate and buffer, whether USB-C's main mix follows the main volume knob, and the MPC's recording toggles
// (Shift + Pad 4 Count-In, Shift + Pad 10 Rec Quantize), what Double Seq puts in the new half and Input Config (Shift
// + Sample), and Screen Mirror (the screen as a USB webcam, drivers/screen_mirror.h, and its size), File sharing (the
// card as a network drive, drivers/file_share.h), and USB port host status. Kept as key=value lines in the `settings`
// file next to theprojects (with the project to open at start).
namespace melodee {

// How a knob or the fader takes over a value it does not sit at (the MPC's
// MIDI Config): Pickup waits until it reaches the value, Scaled moves the
// value the way the control goes, scaled so the two meet at the end of its
// travel, Instant jumps to the control (parity phase 8; shell/takeover.h).
enum class Takeover : uint8_t { Pickup, Scaled, Instant, Count };
inline const char* takeover_name(Takeover t) {
    static const char* const names[] {"Pickup", "Scaled", "Instant"};
    return names[unsigned(t) % unsigned(Takeover::Count)];
}

struct Settings {
    enum Row : uint8_t { Palette, PadResponse, PadPressure, PadLights, KnobTakeover, KnobFxTakeover, FaderTakeover, DoubleSeq, CaptureBars, CaptureQuantize,
                         Screen, PlaceBarRow, SampleRate,
                         AudioBuffer, RecallGrabRow, RecallGap, RecallTrimEnd, TakeAutoEnd, AutoEndAfter, SilenceBelow, Mirror, MirrorSize, MirrorRate, PadFade, TapLight, RepeatVelocity, SeqFollow, Font, RecQuantize, UsbVolume, UsbDirect, DirectLevel, Autosave, NotePreview, MuteKind, PluginSkins, UsbOut, FileSharingRow, UsbPort, Rows };
    // FEATURES (shell/features.h; Settings' K1): Full shows every page, Basic the stock app's pages plus tracks,
    // plug-ins, the mixer and Song. Full unless the file says Basic: a first install, and a file without the row
    // (from before it, or never saved with a setting changed), start on Full (read()).
    bool full_features = true;
    uint8_t lessons_done = 0; // a bit a lesson finished (shell/lesson.h): its check on LEARN
    bool usage_log = false;   // Settings' doors, K2 USAGE LOG: what the hands do, kept in Melodee/Usage (shell/usage_log.h)
    uint8_t pad_response = 1; // 0 soft, 1 medium, 2 hard, 3 full level (127, Shift + Pad 1)
    uint8_t response_before = 1; // what Shift + Pad 1 goes back to
    bool count_in = false;    // a bar before Seq Record records (phase 2)
    bool rec_quantize = true; // recorded notes onto the pattern's Q, as the MPC (Shift + Pad 10, or SEQ's Rec quantize row)
    bool zero_snap = true;    // Chop's cuts onto zero crossings, always (shell/chop.h; the file's row kept for older builds)
    uint8_t slice_play = 0;   // Chop's TO PADS: the last playback picked, 0 Poly, 1 Mono, 2 Thru, 3 Kit (shell/chop.h SlicePlay)
    bool slice_pattern = true; // and whether it writes a pattern of the slices too (Live's Slice to New MIDI Track)
    uint8_t chop_mode = 0;    // Chop's K3: the type picked last, which a sample with no chop of its own opens on (model/chop_slices.h ChopMode)
    bool stems_best = true;   // STEMS' B2 BEST: up to 16 kHz in two passes, about three times as long (shell/stems.h)
    bool preview = false;     // the sample browser's B3 PREVIEW: each file plays as the cursor lands on it
    bool double_copies = true; // Double Seq (Shift + Pad 3): the new half plays the notes again (the MPC's way), or starts empty
    // Capture (Shift + Seq Record; round 2, capture C): how many of the last bars played it keeps (roadmap P5: 0 Auto,
    // the phrase as long as it is), and how it puts them on the grid: 0 as played, 1 onto sixteenths, 2 onto eighths.
    static constexpr std::array<uint8_t, 7> capture_lengths {{0, 1, 2, 4, 8, 16, 32}};
    uint8_t capture_bars = 0, capture_quantise = 1;
    uint8_t click = 0;        // the metronome: 0 off, 1 on, 2 only while Seq Record (Shift + Tap Tempo)
    uint8_t click_level = 40;
    bool full_level() const { return pad_response == 3; }
    // Shift + Pad 1: full level on, or back to the curve it came from.
    void toggle_full_level() {
        if (full_level()) pad_response = response_before == 3 ? 1 : response_before;
        else { response_before = pad_response; pad_response = 3; }
    }
    bool pressure = true;     // off: the pads send no pressure (synths, Pad FX depth)
    // Note Repeat's velocity: Strike, every repeat as hard as the pad was struck; Pressure, each as hard as the pad is
    // held then. Full Level plays them all at 127 either way.
    bool repeat_pressure = false;
    bool repeats_by_pressure() const { return repeat_pressure && pressure && !full_level(); }
    uint8_t pad_lights = 0;   // 0 track colours, 1 white, 2 off
    // How fast a pad's light fades when its sound stops or it is let go (shell/pad_lights.cpp): ms, one of fades.
    static constexpr std::array<uint16_t, 4> fades {{60, 100, 150, 250}};
    uint8_t pad_fade = 1;
    uint32_t fade_ms() const { return fades[pad_fade % fades.size()]; }
    bool tap_light = true;    // Tap Tempo flashes on the beat while playing
    // Background save (shell/autosave.h, issue 18): On, the project saves itself a moment after the controls rest; Off,
    // only SAVE writes it, and what was not saved can be put back with REVERT or left behind (New, Load, Exit).
    bool autosave = true;
    // Step Edit and Roll while the pattern plays (shell/step_edit.cpp seq_follow): 0 Off, they stay where they are;
    // 1 Page, the caret goes to the same step of the bar that plays when a new bar starts; 2 Step, the caret on the
    // step that plays.
    uint8_t seq_follow = 0;
    // Plug-in screens: a plug-in's own screen (shell/screen_view.h) and page files' skins (docs/PLUGIN_PAGES.md, Skins);
    // off, Melodee's own pages for every plug-in. Kept as plugin_skins, its first name.
    bool plugin_skins = true;
    // Note preview (shell/note_preview.h): a note written or moved on Step Edit or Roll without a pad playing it is
    // heard once (a step set in STEPS, a held step's or the marked notes' pitch, a paste).
    bool note_preview = true;
    // Mute (issue 27): Audio, a muted track or kit pad is silent at once, as on the MPC; Notes, it starts no new notes
    // and what it plays rings on (a MIDI track's notes end on their own), as on Elektron machines.
    bool note_mutes = false;
    // The MPC's defaults: Scaled for the knobs and Knob FX, Pickup for the fader. Before parity
    // phase 8 one row, "Knob pickup" (key pickup): off reads as Instant for all three.
    Takeover knob_takeover = Takeover::Scaled, knob_fx_takeover = Takeover::Scaled, fader_takeover = Takeover::Pickup;
    uint8_t screen = 100;     // brightness, 20..100 %
    uint8_t palette = 0;      // the screen's colours: ui/palettes.h (0 NIGHT)
    uint8_t font = 0;         // the screen's typeface: ui/font_data.h (0 Rubik)
    bool place_bottom = false; // the place bar (shell/place_bar.h): under the knob cards, else between the soft keys and the header
    // The audio device (App::audio_format()): its rate, and the 128-frame periods queued ahead of the jacks. More
    // periods ride out a slow block (no crackle) and are heard later; a higher rate costs more CPU a second.
    static constexpr std::array<unsigned, 3> rates {{44100, 48000, 96000}};
    static constexpr std::array<uint8_t, 5> buffers {{2, 3, 4, 6, 8}}; // periods: 256, 384, 512, 768, 1024 frames
    uint32_t audio_rate = 44100;
    uint8_t audio_periods = 3;
    // USB volume: what a computer records over USB-C (channels 1 and 2) at full main volume, the knob left out
    // (Full), or as the jacks play it (Knob).
    bool usb_full = true;
    // USB-C in: Direct, heard straight at the jacks a few milliseconds after it comes, dry, at the direct level
    // (drivers/direct_monitor.h), wherever Melodee would play it (a MIDI track's AUDIO input with MONITOR on, Input
    // Config's Monitor, a loop on it); Tracks, through those, with their effects, about 8 ms later.
    bool usb_direct = true;
    // USB OUT: what a computer records over USB-C after the mix (channels 1 and 2): Tracks, tracks 1-16 one mono
    // channel each (3-18); Submixes, submixes 1-8 in stereo (3-4 the first, up to 17-18), each as it joins the master.
    // Both are 18 channels, so a change needs no new connection.
    bool usb_submixes = false;
    static constexpr std::array<int8_t, 9> direct_levels {{-24, -18, -12, -9, -6, -3, 0, 3, 6}}; // dB
    uint8_t direct_level = 6; // 0 dB
    float direct_gain() const { return std::pow(10.0f, float(direct_levels[direct_level % direct_levels.size()]) / 20.0f); }
    // USB-C port role: 0 off = gadget (peripheral, as the stock app) and 1 on = host. Changed in Settings'
    // SYSTEM door: and the new role is applied at once (shell/usb_host.cpp).
    uint8_t usb_port = 0;
    // Screen Mirror: the screen offered to a computer over USB-C as a webcam, at one of ScreenMirror's sizes, the
    // screen times a whole number (0 320 x 240, 1 640 x 480, 2 1280 x 720, 3 1280 x 960), 30 or 60 fps. Off until
    // asked for: on or off, another size or rate rebinds the USB gadget (about a second without USB audio and MIDI).
    bool mirror = false;
    // File sharing: the card as a network drive (SMB) for the computer on USB-C, at smb://melodee.local, while it is
    // the MPC's (base drivers/file_share.h). On unless turned off; off closes the computer's connection.
    bool file_sharing = true;
    uint8_t mirror_size = 3;  // 1280 x 960: the sharpest (1280 x 720 is stretched soft in a Retina window)
    uint8_t mirror_fps = 30;  // 30 or 60
    ScreenMirrorMode mirror_mode() const {
        unsigned n = mirror_size % ScreenMirror::size_count;
        return {mirror, ScreenMirror::widths[n], ScreenMirror::heights[n], mirror_fps == 60 ? 60u : 30u};
    }
    InputConfig input;        // Input Config: what Sample Record and Audio Recall hear (parity phase 6)
    SamplingPrefs sampling;   // how Audio Recall cuts the ring, and when a Free take ends on its own
    MidiConfig midi;          // MIDI Config (Shift + Pad 8, parity phase 9)
    bool scale_keys = false;  // MIDI Config's Scale on white keys: MIDI in's white keys play the scale (shell/scale_keys.h)
    // Starred factory sounds, by engine (EngineKind's order), for the Sounds page's FAVOURITES.
    static constexpr unsigned engines = 11, sounds = 256; // a row per EngineKind (MIDI the eighth, PLUGIN the ninth, KEYGROUP the tenth, AUDIO the eleventh)
    std::array<std::bitset<sounds>, engines> favourites {};
    bool favourite(unsigned engine, unsigned sound) const { return engine < engines && sound < sounds && favourites[engine][sound]; }
    void toggle_favourite(unsigned engine, unsigned sound) { if (engine < engines && sound < sounds) favourites[engine].flip(sound); }

    // MIDI Config's Reset Factory Settings: every setting back to how Melodee comes, the starred sounds,
    // FEATURES and USAGE LOG kept.
    void reset_factory() {
        auto kept = favourites;
        bool full = full_features;
        uint8_t lessons = lessons_done;
        bool usage = usage_log;
        *this = Settings {};
        favourites = kept;
        full_features = full;
        lessons_done = lessons;
        usage_log = usage;
    }

    static const char* row_name(unsigned row) {
        static const char* names[] {"Palette", "Pad response", "Pad pressure", "Pad lights", "Knob takeover", "Knob FX takeover",
                                    "Fader takeover", "Double Seq", "Capture bars", "Capture quantize", "Brightness", "Place bar", "Sample rate", "Audio buffer", "Recall grab", "Recall gap",
                                    "Recall trim end", "Take auto end", "Auto end after", "Silence below", "Screen mirror", "Resolution", "Frame rate",
                                    "Pad fade", "Tap Tempo light", "Repeat velocity", "Follow", "Font", "Rec quantize", "USB volume",
                                    "USB-C in", "Direct level", "Autosave", "Note preview", "Mute", "Plug-in screens", "USB out", "File sharing", "USB host"};
        return names[row % Rows];
    }
    std::string value_text(unsigned row) const {
        static const char* responses[] {"Soft", "Medium", "Hard", "Full level"};
        static const char* lights[] {"Track colours", "White", "Off"};
        switch (row) {
        case Palette: return ui::palettes[palette % ui::palettes.size()].name;
        case Font: { // a palette with a face of its own (LCD) shows that one
            int own = ui::palettes[palette % ui::palettes.size()].face;
            return ui::faces[own >= 0 ? unsigned(own) : font % ui::face_choices].name;
        }
        case PadResponse: return responses[pad_response % 4];
        case PadPressure: return pressure ? "On" : "Off";
        case PadLights: return lights[pad_lights % 3];
        case KnobTakeover: return takeover_name(knob_takeover);
        case KnobFxTakeover: return takeover_name(knob_fx_takeover);
        case FaderTakeover: return takeover_name(fader_takeover);
        case DoubleSeq: return double_copies ? "Copy notes" : "Empty";
        case CaptureBars: return !capture_bars ? std::string("Auto") : std::to_string(capture_bars) + (capture_bars == 1 ? " bar" : " bars");
        case CaptureQuantize: return capture_quantise == 0 ? "As played" : capture_quantise == 1 ? "1/16" : "1/8";
        case PlaceBarRow: return place_bottom ? "Bottom" : "Top";
        case SampleRate: return audio_rate == 44100 ? "44.1 kHz" : std::to_string(audio_rate / 1000) + " kHz";
        case AudioBuffer: { // the frames queued, and how late that is heard at the rate chosen
            unsigned frames = 128u * audio_periods;
            char text[32];
            std::snprintf(text, sizeof(text), "%u  %.1f ms", frames, 1000.0 * frames / audio_rate);
            return text;
        }
        case RecallGrabRow: return recall_grab_name(sampling.grab);
        case RecallGap: return seconds_text(SamplingPrefs::gap_seconds[sampling.gap % SamplingPrefs::gap_count]);
        case RecallTrimEnd: return sampling.trim_end ? "On" : "Off";
        case TakeAutoEnd: return auto_end_name(sampling.auto_end);
        case AutoEndAfter: return seconds_text(SamplingPrefs::end_seconds[sampling.end_after % SamplingPrefs::end_count]);
        case SilenceBelow: return std::to_string(sampling.silence_db()) + " dB";
        case Mirror: return mirror ? "On" : "Off";
        case MirrorSize: { auto m = mirror_mode(); return std::to_string(m.width) + " x " + std::to_string(m.height); }
        case MirrorRate: return std::to_string(mirror_mode().fps) + " fps";
        case PadFade: return std::to_string(fade_ms()) + " ms";
        case TapLight: return tap_light ? "On" : "Off";
        case Autosave: return autosave ? "On" : "Off";
        case NotePreview: return note_preview ? "On" : "Off";
        case MuteKind: return note_mutes ? "Notes" : "Audio";
        case PluginSkins: return plugin_skins ? "On" : "Off";
        case RepeatVelocity: return repeat_pressure ? "Pressure" : "Strike";
        case SeqFollow: return seq_follow == 1 ? "Page" : seq_follow == 2 ? "Step" : "Off";
        case RecQuantize: return rec_quantize ? "On" : "Off";
        case UsbVolume: return usb_full ? "Full" : "Knob";
        case UsbDirect: return usb_direct ? "Direct" : "Tracks";
        case UsbOut: return usb_submixes ? "Submixes" : "Tracks";
        case FileSharingRow: return file_sharing ? "On" : "Off";
        case DirectLevel: { int db = direct_levels[direct_level % direct_levels.size()]; return (db > 0 ? "+" : "") + std::to_string(db) + " dB"; }
        case UsbPort: return usb_port == 1 ? "On" : "Off";
        default: return std::to_string(screen) + " %";
        }
    }
    // One step along a row.
    void step(unsigned row, int way) {
        switch (row) {
        case Palette: palette = uint8_t(step_in(palette % ui::palettes.size(), unsigned(ui::palettes.size()), way)); break;
        case Font: font = uint8_t(step_in(font % ui::face_choices, ui::face_choices, way)); break;
        case PadResponse: pad_response = uint8_t((pad_response + 4 + way) % 4); break;
        case PadPressure: pressure = !pressure; break;
        case PadLights: pad_lights = uint8_t((pad_lights + 3 + way) % 3); break;
        case KnobTakeover: knob_takeover = step_takeover(knob_takeover, way); break;
        case KnobFxTakeover: knob_fx_takeover = step_takeover(knob_fx_takeover, way); break;
        case FaderTakeover: fader_takeover = step_takeover(fader_takeover, way); break;
        case DoubleSeq: double_copies = !double_copies; break;
        case CaptureBars: capture_bars = capture_lengths[step_in(capture_index(), unsigned(capture_lengths.size()), way)]; break;
        case CaptureQuantize: capture_quantise = uint8_t(step_in(capture_quantise, 3, way)); break;
        case PlaceBarRow: place_bottom = !place_bottom; break;
        case Screen: screen = uint8_t(std::clamp(int(screen) + way * 10, 20, 100)); break;
        case SampleRate: audio_rate = rates[size_t(std::clamp(rate_index() + (way < 0 ? -1 : 1), 0, int(rates.size()) - 1))]; break;
        case AudioBuffer: audio_periods = buffers[size_t(std::clamp(buffer_index() + (way < 0 ? -1 : 1), 0, int(buffers.size()) - 1))]; break;
        case RecallGrabRow: sampling.grab = RecallGrab(step_in(unsigned(sampling.grab), unsigned(RecallGrab::Count), way)); break;
        case RecallGap: sampling.gap = uint8_t(step_in(sampling.gap, SamplingPrefs::gap_count, way)); break;
        case RecallTrimEnd: sampling.trim_end = !sampling.trim_end; break;
        case TakeAutoEnd: sampling.auto_end = AutoEnd(step_in(unsigned(sampling.auto_end), unsigned(AutoEnd::Count), way)); break;
        case AutoEndAfter: sampling.end_after = uint8_t(step_in(sampling.end_after, SamplingPrefs::end_count, way)); break;
        case SilenceBelow: sampling.silence = uint8_t(step_in(sampling.silence, SamplingPrefs::silence_count, way)); break;
        case Mirror: mirror = !mirror; break;
        case MirrorSize: mirror_size = uint8_t(step_in(mirror_size % ScreenMirror::size_count, ScreenMirror::size_count, way)); break;
        case MirrorRate: mirror_fps = way < 0 ? 30 : 60; break;
        case PadFade: pad_fade = uint8_t(step_in(pad_fade % fades.size(), unsigned(fades.size()), way)); break;
        case TapLight: tap_light = !tap_light; break;
        case Autosave: autosave = !autosave; break;
        case NotePreview: note_preview = !note_preview; break;
        case MuteKind: note_mutes = !note_mutes; break;
        case PluginSkins: plugin_skins = !plugin_skins; break;
        case RepeatVelocity: repeat_pressure = !repeat_pressure; break;
        case SeqFollow: seq_follow = uint8_t(step_in(seq_follow % 3, 3, way)); break;
        case RecQuantize: rec_quantize = !rec_quantize; break;
        case UsbVolume: usb_full = !usb_full; break;
        case UsbDirect: usb_direct = !usb_direct; break;
        case UsbOut: usb_submixes = !usb_submixes; break;
        case FileSharingRow: file_sharing = !file_sharing; break;
        case DirectLevel: direct_level = uint8_t(step_in(direct_level % direct_levels.size(), unsigned(direct_levels.size()), way)); break;
        case UsbPort: usb_port = uint8_t(std::max(0, (int(usb_port) + (way < 0 ? -1 : 1)) % 2)); break;
        default: break;
        }
    }
    // A row's values as a list, for K1 (pages/settings_page.cpp): how many, which one it is at, and set to one.
    // On / Off rows are Off first; Brightness goes 20 to 100 % in tens.
    unsigned choices(unsigned row) const {
        switch (row) {
        case Palette: return unsigned(ui::palettes.size());
        case Font: return ui::face_choices;
        case PadResponse: return 4;
        case PadLights: case CaptureQuantize: case SeqFollow: return 3;
        case KnobTakeover: case KnobFxTakeover: case FaderTakeover: return unsigned(Takeover::Count);
        case CaptureBars: return unsigned(capture_lengths.size());
        case Screen: return 9;
        case SampleRate: return unsigned(rates.size());
        case AudioBuffer: return unsigned(buffers.size());
        case RecallGrabRow: return unsigned(RecallGrab::Count);
        case RecallGap: return SamplingPrefs::gap_count;
        case TakeAutoEnd: return unsigned(AutoEnd::Count);
        case AutoEndAfter: return SamplingPrefs::end_count;
        case SilenceBelow: return SamplingPrefs::silence_count;
        case MirrorSize: return ScreenMirror::size_count;
        case PadFade: return unsigned(fades.size());
        case DirectLevel: return unsigned(direct_levels.size());
        case UsbPort: return 2;
        default: return row < Rows ? 2 : 1; // On / Off, and the two-way rows
        }
    }
    unsigned choice(unsigned row) const {
        switch (row) {
        case Palette: return palette % ui::palettes.size();
        case Font: return font % ui::face_choices;
        case PadResponse: return pad_response % 4u;
        case PadPressure: return pressure;
        case PadLights: return pad_lights % 3u;
        case KnobTakeover: return unsigned(knob_takeover);
        case KnobFxTakeover: return unsigned(knob_fx_takeover);
        case FaderTakeover: return unsigned(fader_takeover);
        case DoubleSeq: return double_copies;
        case CaptureBars: return capture_index();
        case CaptureQuantize: return capture_quantise % 3u;
        case Screen: return unsigned(std::clamp((int(screen) - 20 + 5) / 10, 0, 8));
        case PlaceBarRow: return place_bottom;
        case SampleRate: return unsigned(rate_index());
        case AudioBuffer: return unsigned(buffer_index());
        case RecallGrabRow: return unsigned(sampling.grab);
        case RecallGap: return sampling.gap;
        case RecallTrimEnd: return sampling.trim_end;
        case TakeAutoEnd: return unsigned(sampling.auto_end);
        case AutoEndAfter: return sampling.end_after;
        case SilenceBelow: return sampling.silence;
        case Mirror: return mirror;
        case MirrorSize: return mirror_size % ScreenMirror::size_count;
        case MirrorRate: return mirror_fps == 60;
        case PadFade: return pad_fade % fades.size();
        case TapLight: return tap_light;
        case Autosave: return autosave;
        case NotePreview: return note_preview;
        case MuteKind: return note_mutes;
        case PluginSkins: return plugin_skins;
        case RepeatVelocity: return repeat_pressure;
        case SeqFollow: return seq_follow % 3u;
        case RecQuantize: return rec_quantize;
        case UsbVolume: return usb_full;
        case UsbDirect: return usb_direct;
        case UsbOut: return usb_submixes;
        case FileSharingRow: return file_sharing;
        case DirectLevel: return direct_level % direct_levels.size();
        case UsbPort: return usb_port % 2u;
        default: return 0;
        }
    }
    void set_choice(unsigned row, unsigned n) {
        n = std::min(n, choices(row) - 1);
        switch (row) {
        case Palette: palette = uint8_t(n); break;
        case Font: font = uint8_t(n); break;
        case PadResponse: pad_response = uint8_t(n); break;
        case PadPressure: pressure = n; break;
        case PadLights: pad_lights = uint8_t(n); break;
        case KnobTakeover: knob_takeover = Takeover(n); break;
        case KnobFxTakeover: knob_fx_takeover = Takeover(n); break;
        case FaderTakeover: fader_takeover = Takeover(n); break;
        case DoubleSeq: double_copies = n; break;
        case CaptureBars: capture_bars = capture_lengths[n]; break;
        case CaptureQuantize: capture_quantise = uint8_t(n); break;
        case Screen: screen = uint8_t(20 + 10 * n); break;
        case PlaceBarRow: place_bottom = n; break;
        case SampleRate: audio_rate = rates[n]; break;
        case AudioBuffer: audio_periods = buffers[n]; break;
        case RecallGrabRow: sampling.grab = RecallGrab(n); break;
        case RecallGap: sampling.gap = uint8_t(n); break;
        case RecallTrimEnd: sampling.trim_end = n; break;
        case TakeAutoEnd: sampling.auto_end = AutoEnd(n); break;
        case AutoEndAfter: sampling.end_after = uint8_t(n); break;
        case SilenceBelow: sampling.silence = uint8_t(n); break;
        case Mirror: mirror = n; break;
        case MirrorSize: mirror_size = uint8_t(n); break;
        case MirrorRate: mirror_fps = n ? 60 : 30; break;
        case PadFade: pad_fade = uint8_t(n); break;
        case TapLight: tap_light = n; break;
        case Autosave: autosave = n; break;
        case NotePreview: note_preview = n; break;
        case MuteKind: note_mutes = n; break;
        case PluginSkins: plugin_skins = n; break;
        case RepeatVelocity: repeat_pressure = n; break;
        case SeqFollow: seq_follow = uint8_t(n); break;
        case RecQuantize: rec_quantize = n; break;
        case UsbVolume: usb_full = n; break;
        case UsbDirect: usb_direct = n; break;
        case UsbOut: usb_submixes = n; break;
        case FileSharingRow: file_sharing = n; break;
        case DirectLevel: direct_level = uint8_t(n); break;
        case UsbPort: usb_port = uint8_t(n); break;
        default: break;
        }
    }
    // The rows that reopen the audio device or the USB camera when they change: K1 sets them once it rests.
    static bool reopens(unsigned row) { return row == SampleRate || row == AudioBuffer || row == Mirror || row == MirrorSize || row == MirrorRate; }
    unsigned capture_index() const {
        for (unsigned n = 0; n < capture_lengths.size(); ++n) if (capture_lengths[n] == capture_bars) return n;
        return 0;
    }
    // A list's next or previous entry, stopping at its ends.
    static unsigned step_in(unsigned at, unsigned count, int way) { return unsigned(std::clamp(int(at) + (way < 0 ? -1 : 1), 0, int(count) - 1)); }
    static std::string seconds_text(float s) {
        char text[16];
        std::snprintf(text, sizeof(text), s == std::floor(s) ? "%.0f s" : "%.1f s", double(s));
        return text;
    }
    int rate_index() const { return int(std::find(rates.begin(), rates.end(), audio_rate) - rates.begin()) % int(rates.size()); }
    int buffer_index() const {
        auto at = std::lower_bound(buffers.begin(), buffers.end(), audio_periods);
        return at == buffers.end() ? int(buffers.size()) - 1 : int(at - buffers.begin());
    }
    static Takeover step_takeover(Takeover t, int way) {
        int count = int(Takeover::Count);
        return Takeover((int(t) + count + (way < 0 ? -1 : 1)) % count);
    }
    // A pad's velocity through the response curve.
    uint8_t velocity(uint8_t v) const {
        if (pad_response == 3) return 127; // Full Level, as on the MPC
        if (pad_response == 1 || v == 0) return v;
        float x = v / 127.0f, curved = std::pow(x, pad_response == 0 ? 0.6f : 1.7f);
        return uint8_t(std::clamp(int(curved * 127 + 0.5f), 1, 127));
    }

    std::string text() const {
        std::ostringstream out;
        // pickup= stays for the builds before parity phase 8, which know only that.
        out << "features=" << int(full_features) << "\nlessons=" << int(lessons_done) << "\nusage_log=" << int(usage_log) << "\npad_response=" << int(pad_response) << "\npad_lights=" << int(pad_lights) << "\npad_fade=" << int(pad_fade) << "\ntap_light=" << int(tap_light) << "\nautosave=" << int(autosave)
            << "\npickup=" << int(knob_takeover != Takeover::Instant) << "\nknob_takeover=" << int(knob_takeover)
            << "\nknob_fx_takeover=" << int(knob_fx_takeover) << "\nfader_takeover=" << int(fader_takeover)
            << "\nscreen=" << int(screen) << "\npalette=" << int(palette) << "\nfont=" << int(font) << "\nplace_bar=" << int(place_bottom) << "\ncount_in=" << int(count_in) << "\nrec_quantize=" << int(rec_quantize)
            << "\nzero_snap=" << int(zero_snap) << "\nslice_play=" << int(slice_play) << "\nslice_pattern=" << int(slice_pattern) << "\nchop_mode=" << int(chop_mode) << "\nstems_best=" << int(stems_best) << "\npreview=" << int(preview) << "\ndouble_copies=" << int(double_copies) << "\nclick=" << int(click) << "\nclick_level=" << int(click_level)
            << "\ncapture_bars=" << int(capture_bars) << "\ncapture_quantize=" << int(capture_quantise)
            << "\nresponse_before=" << int(response_before) << "\npressure=" << int(pressure) << "\nrepeat_pressure=" << int(repeat_pressure) << "\nseq_follow=" << int(seq_follow) << "\nnote_preview=" << int(note_preview) << "\nnote_mutes=" << int(note_mutes) << "\nplugin_skins=" << int(plugin_skins)
            << "\ninput_source=" << int(input.source) << "\nmonitor=" << int(input.monitor) << "\nmonitor_before_mic=" << int(input.before_mic) << "\nthreshold=" << int(input.threshold)
            << "\nrec_length=" << int(input.length) << "\nrec_fx=" << int(input.fx) << "\nrec_gain=" << int(input.gain)
            << "\ninput_track=" << int(input.track) << "\ninput_tap=" << int(input.post)
            << "\nrecall_grab=" << int(sampling.grab) << "\nrecall_gap=" << int(sampling.gap) << "\nrecall_trim=" << int(sampling.trim_end)
            << "\nauto_end=" << int(sampling.auto_end) << "\nauto_end_after=" << int(sampling.end_after)
            << "\nsilence_db=" << sampling.silence_db()
            << "\naudio_rate=" << audio_rate << "\naudio_periods=" << int(audio_periods) << "\nusb_volume=" << int(usb_full) << "\nusb_direct=" << int(usb_direct) << "\nusb_out=" << int(usb_submixes) << "\ndirect_level=" << int(direct_level)
            << "\nscreen_mirror=" << int(mirror) << "\nmirror_size=" << int(mirror_size) << "\nmirror_fps=" << int(mirror_fps)
            << "\nfile_sharing=" << int(file_sharing)
            << "\nscale_keys=" << int(scale_keys) << '\n';
        write_midi_config(out, midi);
        for (unsigned e = 0; e < engines; ++e) {
            if (favourites[e].none()) continue;
            out << "favourites" << e << '=';
            bool first = true;
            for (unsigned s = 0; s < sounds; ++s)
                if (favourites[e][s]) { out << (first ? "" : ",") << s; first = false; }
            out << '\n';
        }
        return out.str();
    }
    void read(const std::string& text) {
        std::istringstream in(text);
        std::string line;
        int pickup = -1;          // the old row, read only when the file has none of the new ones
        bool takeovers = false;
        bool features = false; // FEATURES: no row, Full
        auto takeover = [&](int value) { takeovers = true; return Takeover(std::clamp(value, 0, int(Takeover::Count) - 1)); };
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            int value = std::atoi(line.c_str() + eq + 1);
            if (key == "features") { full_features = value != 0; features = true; }
            else if (key == "lessons") lessons_done = uint8_t(std::clamp(value, 0, 255));
            else if (key == "usage_log") usage_log = value != 0;
            else if (key == "pad_response") pad_response = uint8_t(std::clamp(value, 0, 3));
            else if (key == "pad_lights") pad_lights = uint8_t(std::clamp(value, 0, 2));
            else if (key == "pad_fade") pad_fade = uint8_t(std::clamp(value, 0, int(fades.size()) - 1));
            else if (key == "tap_light") tap_light = value != 0;
            else if (key == "autosave") autosave = value != 0;
            else if (key == "pickup") pickup = value != 0;
            else if (key == "knob_takeover") knob_takeover = takeover(value);
            else if (key == "knob_fx_takeover") knob_fx_takeover = takeover(value);
            else if (key == "fader_takeover") fader_takeover = takeover(value);
            else if (key == "pressure") pressure = value != 0;
            else if (key == "repeat_pressure") repeat_pressure = value != 0;
            else if (key == "seq_follow") seq_follow = uint8_t(std::clamp(value, 0, 2));
            else if (key == "note_preview") note_preview = value != 0;
            else if (key == "note_mutes") note_mutes = value != 0;
            else if (key == "plugin_skins") plugin_skins = value != 0;
            else if (key == "screen") screen = uint8_t(std::clamp(value, 20, 100));
            else if (key == "palette") palette = uint8_t(std::clamp(value, 0, int(ui::palettes.size()) - 1));
            else if (key == "font") font = uint8_t(std::clamp(value, 0, int(ui::face_choices) - 1));
            else if (key == "place_bar") place_bottom = value != 0;
            else if (key == "count_in") count_in = value != 0;
            else if (key == "rec_quantize") rec_quantize = value != 0;
            else if (key == "zero_snap") zero_snap = true; // always on now (older settings files had it off)
            else if (key == "slice_play") slice_play = uint8_t(std::clamp(value, 0, 3));
            else if (key == "slice_pattern") slice_pattern = value != 0;
            else if (key == "chop_mode") chop_mode = uint8_t(std::clamp(value, 0, 4));
            else if (key == "stems_best") stems_best = value != 0;
            else if (key == "preview") preview = value != 0;
            else if (key == "double_copies") double_copies = value != 0;
            else if (key == "capture_bars") { capture_bars = 0; for (auto n : capture_lengths) if (n && int(n) <= value) capture_bars = n; } // the nearest below; 0 Auto
            else if (key == "capture_quantize") capture_quantise = uint8_t(std::clamp(value, 0, 2));
            else if (key == "click") click = uint8_t(std::clamp(value, 0, 2));
            else if (key == "click_level") click_level = uint8_t(std::clamp(value, 0, 100));
            else if (key == "response_before") response_before = uint8_t(std::clamp(value, 0, 2));
            else if (key == "input_source") input.source = InputSource(std::clamp(value, 0, int(InputSource::Count) - 1));
            else if (key == "monitor") input.monitor = Monitor(std::clamp(value, 0, int(Monitor::Count) - 1));
            else if (key == "monitor_before_mic") input.before_mic = int8_t(std::clamp(value, -1, int(Monitor::Count) - 1));
            else if (key == "threshold") input.threshold = int8_t(std::clamp(value, int(InputConfig::threshold_min), 0));
            else if (key == "rec_length") input.length = uint8_t(std::clamp(value, 0, int(rec_lengths) - 1));
            else if (key == "rec_fx") input.fx = value != 0;
            else if (key == "rec_gain") input.gain = int8_t(std::clamp(value, -12, 24));
            else if (key == "input_track") input.track = uint8_t(std::clamp(value, 0, 15));
            else if (key == "input_tap") input.post = value != 0;
            else if (key == "recall_grab") sampling.grab = RecallGrab(std::clamp(value, 0, int(RecallGrab::Count) - 1));
            else if (key == "recall_gap") sampling.gap = uint8_t(std::clamp(value, 0, int(SamplingPrefs::gap_count) - 1));
            else if (key == "recall_trim") sampling.trim_end = value != 0;
            else if (key == "auto_end") sampling.auto_end = AutoEnd(std::clamp(value, 0, int(AutoEnd::Count) - 1));
            else if (key == "auto_end_after") sampling.end_after = uint8_t(std::clamp(value, 0, int(SamplingPrefs::end_count) - 1));
            else if (key == "silence_db") sampling.silence = SamplingPrefs::silence_index(value);
            else if (key == "audio_rate") { if (std::find(rates.begin(), rates.end(), unsigned(value)) != rates.end()) audio_rate = uint32_t(value); }
            else if (key == "usb_volume") usb_full = value != 0;
            else if (key == "usb_direct") usb_direct = value != 0;
            else if (key == "usb_out") usb_submixes = value != 0;
            else if (key == "direct_level") direct_level = uint8_t(std::clamp(value, 0, int(direct_levels.size()) - 1));
            else if (key == "audio_periods") { audio_periods = uint8_t(std::clamp(value, 2, 8)); audio_periods = buffers[size_t(buffer_index())]; }
            else if (key == "screen_mirror") mirror = value != 0;
            else if (key == "file_sharing") file_sharing = value != 0;
            else if (key == "mirror_size") mirror_size = uint8_t(std::clamp(value, 0, int(ScreenMirror::size_count) - 1)); // 1920 x 1440 and up: 1280 x 960
            else if (key == "mirror_fps") mirror_fps = value >= 60 ? 60 : 30;
            else if (key == "scale_keys") scale_keys = value != 0;
            else if (read_midi_config(midi, key, value)) {}
            else if (key.rfind("favourites", 0) == 0) { // favourites3=0,4,12
                unsigned e = unsigned(std::atoi(key.c_str() + 10));
                if (e >= engines) continue;
                favourites[e].reset();
                std::istringstream list(line.substr(eq + 1));
                std::string item;
                while (std::getline(list, item, ','))
                    if (!item.empty()) { int s = std::atoi(item.c_str()); if (s >= 0 && unsigned(s) < sounds) favourites[e].set(unsigned(s)); }
            }
        }
        if (!features) full_features = true; // no row: Full (a first install, or a file with only the project to open)
        if (!takeovers && pickup == 0) knob_takeover = knob_fx_takeover = fader_takeover = Takeover::Instant; // "Knob pickup Off"
    }
};

} // namespace melodee
