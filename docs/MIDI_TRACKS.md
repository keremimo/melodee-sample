# Melodee: MIDI tracks (design)

A track can load **MIDI** as its engine, as it loads a synth (a plug-in) or the Sampler.
It makes no sound of its own: its pads, patterns, knobs, motion and the
song drive an external instrument on a port and channel of its own, and it
can take an audio input back as its sound so the instrument sits in the mix.

Status: Kerem's answers of 2026-11-10. **M1 built**; **M2–M5 built** in
roadmap P4 (2026-11-12, see "Phases" and "As built in P4").

## Decisions (Kerem, 2026-11-10)

| Question | Answer |
| --- | --- |
| What a "MIDI out device" is | A track engine, `EngineKind::Midi`. |
| Where it sends | Each MIDI track picks its own port and channel. MIDI Config's port stays for pads on audio tracks, sync and Thru. |
| What it sends | Notes (velocity, length), CC from the knobs and motion, Program Change and Bank Select at launch, pitch bend, aftertouch and the mod wheel, NRPN, and SysEx. |
| Pads | Keys (Scale, Chromatic, Chords, as the synth tracks) or Drum (each pad a fixed, named note), chosen per track. |
| Audio back | Optional: the track can take an input as its audio (strip, FX, sends, mute, stems, bounce). |
| Knobs | Editable slots in pages of three (K1-K3). |
| SysEx | A `.syx` dump from the card, sent when the project loads or on demand. |
| Timing | Sent when its frame is heard, as now, plus a delay of the track's own. |
| MIDI in on a MIDI track | Echo and record (Claude's pick, asked for "the best option"; see "MIDI in"). |
| Pad MIDI Out and a track's own channel | Kept for audio tracks (layering). A MIDI track always sends on its own port and channel and ignores Pad MIDI Out. |
| Stop, mute, panic | Note Offs for held notes only; a panic job sends All Sound Off, All Notes Off and pitch bend centre everywhere. |
| A USB device that is unplugged | The track sends to every USB output until it comes back. |

## Model

`model/midi_track.h` (new, plain data):

```cpp
enum class MidiDest : uint8_t { External, UsbC, UsbDevice, AllUsb, Count };
enum class MidiSlotKind : uint8_t { Off, Cc, Cc14, Nrpn, PitchBend, ChannelPressure, Count };
enum class MidiPads : uint8_t { Keys, Drum, Count };

struct MidiSlot {                 // one knob's job
    MidiSlotKind kind = MidiSlotKind::Off;
    uint16_t number = 0;          // CC 0..119 (Cc14: 0..31, LSB at +32), NRPN 0..16383
    uint16_t low = 0, high = 127; // the knob's range, in the message's own values
    char name[8] {};              // as Edit shows it ("CUTOFF")
};

struct MidiDevice {
    MidiDest dest = MidiDest::External;
    char device[32] {};           // UsbDevice: the ALSA port name, matched on hotplug
    uint8_t channel = 1;          // 1..16
    int8_t delay_ms = 0;          // -50..50, see "Timing"
    MidiPads pads = MidiPads::Keys;
    std::array<uint8_t, sampler_pads> drum_note {}; // Drum: pad (16 x 8 banks) -> note, GM from 36
    std::array<char[6], sampler_pads> drum_name {};
    uint8_t program = 0;          // 0 none, 1..128
    uint8_t bank_msb = 255, bank_lsb = 255;          // 255 none
    std::array<uint8_t, pattern_count> pattern_program {}; // 0: the track's
    std::array<MidiSlot, midi_slots> slots {};      // midi_slots = 24: eight pages of three
    char sysex[64] {};            // a .syx on the card, relative to the project
    bool sysex_on_load = false;
    uint8_t audio_in = 0;         // 0 none, else InputSource + 1; see "Audio back"
};
```

`Track` gains `MidiDevice midi` from M2 on (about 1.6 KB a track) for what
is not a number: slot definitions, the drum map, names, the device's name,
the SysEx file. `EngineKind::Midi` is added before `Count`, so older files
keep their engine numbers. In M1 port and channel are parameters, so
`MidiDevice` does not exist yet.

**The device and the slots are the engine's parameters.** Port and Channel
are parameters 0 and 1 (M1, `engine/midi_engine.h`); M2's slot values
follow from parameter 8 (room for the device's later per-track values).
That is the main reuse:
motion lanes, takeover, undo, Seq Record, the fader's targets, Erase + knob
and track copy already work on `Track::params`. A value moves the knob and
the engine host sends the slot's message when it changes. Parameters past
the slots hold what is per performance rather than per device: pressure
mode (channel or poly), pitch bend range shown, and so on.

File: a `MIDT` section per MIDI track (track, a version byte, then the
fields from M2 on). `TRAK` writes the engine as None for a MIDI track:
older builds clamp an engine number they do not know to their last engine
(the Sampler), so they must not see 7. They skip `MIDT` and load the track
empty, its port and channel left in its parameters.

## Engine (audio thread)

`engine/midi_engine.{h,cpp}`: the `Midi` engine renders silence, or the
track's input (see "Audio back"), and turns what reaches it into
`MidiOutEvent`s:

- Note on and off from pads, patterns, the arp, chords and MIDI in.
- Param changes on a used slot as its message. Cc14 and NRPN are emitted
  as one event; the MIDI thread expands them into CC pairs or the four-CC
  NRPN sequence.
- Pressure as channel or poly aftertouch. The fader's Mod Wheel target
  becomes CC 1, and a new Pitch Bend target becomes pitch bend.
- Pattern launch: Bank Select MSB/LSB, then Program Change, at the
  launch frame and ahead of that frame's notes.

`MidiOutEvent` gains the kinds `Cc`, `Cc14`, `Nrpn`, `PitchBend`,
`ChannelPressure`, `PolyPressure`, `Program` and `Bank`, and a `track`
byte (255 = MIDI Config's port, as today). The audio thread never
resolves ports. The MIDI thread maps a track to its outputs from the
project, cached and rebuilt when `HardwareMidi::generation()` changes.
Same-frame events must keep their order (bank before program before
notes), so `MidiIo::pending` insertion stays stable.

**Note Offs.** The table of held notes in `engine_host.cpp` (around
line 803) becomes per track. Mute, Stop, a pattern change, an engine change
and a project load send Note Off for that track's held notes only.
**Panic** sends CC 120, CC 123 and pitch bend centre on every channel of
every output. Its binding is open question 1.

**CC rate.** At most one message per slot every 2 ms. The latest value
wins, and the last value is always sent. DIN runs at 31,250 baud, about
1,000 three-byte messages a second, and one knob turn must not starve the
notes.

## Timing

Sequenced events go out when their frame is heard, as today, so external
and internal sounds line up. The track's `delay_ms` shifts its events:

- **Positive** (late gear is early, or to nudge): `frame + ms`. No limit
  inside the range.
- **Negative** (slow gear): can only reach as far back as the audio
  latency, because the event is decided when its block renders. At 48 kHz
  with the default buffer that is about 8-24 ms, depending on Settings'
  buffer. Anything further is clamped (the Edit row shows the clamp). A
  full −50 ms would need sequencer lookahead, which is left for later.

Live playing (pads, MIDI in) on a MIDI track goes out at once, plus any
positive delay. Waiting a latency to line up with internal engines only
makes an external synth feel late under the fingers. Recording still
stamps the frame the note was heard at.

## Ports

Destinations and how the MIDI thread resolves them:

| `MidiDest` | Output |
| --- | --- |
| External | The 1/8" jacks (UART5, `port_is(External)`). |
| UsbC | The gadget's f_midi ("MPC Sample" on a computer). |
| UsbDevice | The `snd-usb-audio` port whose name matches `device`. While it is missing: every USB output (Kerem). The Edit row shows `OFFLINE → ALL USB`. |
| AllUsb | Every USB output, gadget and devices. |

The USB-C gadget's known flaw (a computer with no MIDI app open buffers what
is sent, then bursts it) applies to MIDI tracks too. The "Later" box in
phase 9 (drop USB output older than a moment) should land with phase M1.

## Pads, Steps and the arp

- **Keys**: the synth tracks' layouts (`PadLayout`), octave and transpose.
- **Drum**: pad (with Pad Bank A-H) maps to `drum_note`, a kit-like grid
  with names on the pads and on Steps and Roll rows. The map is edited on
  Edit's PADS page: tap a pad, wheel the note, Push names it.
- **Patterns store the MIDI note itself**, not the pad, so switching Keys
  and Drum, or editing the map, never changes what plays. (Claude's
  choice. Kerem can overturn it, so that remapping a pad moves its notes as
  a kit does.)
- Note Repeat, the arp, chords, 16 Levels (velocity) and Time Correct work
  as on a synth track.

## Edit pages

The Edit page for MIDI comes from the track, not a static `EngineInfo`.
Static pages get a hook for dynamic labels (`param_label(track, index)`).

| Page | K1 | K2 | K3 |
| --- | --- | --- | --- |
| DEVICE | port (External, USB-C, USB device: wheel through the connected ones, All USB) | channel | delay ms |
| PROGRAM | bank MSB | bank LSB | program (Shift + K3: this pattern's own) |
| PADS | Keys / Drum | (Drum) the last pad's note | octave (Keys) |
| SLOTS 1..8 | slot values | | |
| AUDIO | input (None, Rear L, Rear R, Rear, Mic, USB) | monitor | level |
| SYSEX | file (the browser, `.syx`) | on load off / on | B3 SEND |

On a SLOTS page, **Shift + Kn** edits that slot's definition: kind,
number, then range. **Push** names it with the Name sheet. Without a used
slot the knob shows `—` and does nothing.

Mod matrix routes onto slots (LFO to CC) are left for later. They need
the engine to stream CC from a modulated value, rate-limited, and nobody
has asked for that yet.

## MIDI in on a MIDI track

What comes in on MIDI Config's In channel, while a MIDI track is selected
(or on a MIDI track's own `midi_channel`):

1. **Echoes at once** to the track's port and channel, re-channelled:
   notes, pressure, pitch bend, and CC (passed through, not mapped through
   slots).
2. **Records** into the pattern under Seq Record, as pads do. A CC that
   matches a used slot records into that slot's motion lane.
3. **Loop guard**: no echo when the input port is the track's output port
   and the incoming channel is the track's channel. That is the classic
   keyboard-synth loop, where the instrument should use Local Off instead.
4. Global **Thru** skips what a MIDI track already echoed, so nothing
   doubles.

## Audio back

A MIDI track with `audio_in` set renders that input into its strip as its
sound: Knob FX, the track chain, level, pan, sends, mute and solo, the
18-channel USB stems and bounce. Limits come from the hardware, through
`shell/input_route`:

- The ADC is routed to the mic **or** the rear inputs, never both. Two MIDI
  tracks can still take Rear L and Rear R (two mono synths).
- Input Config's source, Audio Recall and sampling share that route. If a
  MIDI track asks for a source the route cannot give at the same time, the
  AUDIO row shows `IN USE`, and the track is silent rather than taking the
  input from a recording.
- Monitoring through a track adds the audio round trip (two buffers),
  which is fine for synths.
- A bounce or render records the input live. Bouncing a MIDI track plays
  the external instrument in real time, so the bounce cannot be offline
  (open question 4).

## SysEx

`sysex` names a `.syx` file. The service thread reads it (bounded, 64 KB)
when the project loads or on B3 SEND, and hands the buffer to the MIDI
thread. The MIDI thread streams it to the track's port in pieces:
`MidiPacket` holds 256 bytes, so `HardwareMidi` gains a raw, chunked send
that keeps SysEx whole on the wire. DIN is paced at 31,250 baud, with a
short gap between messages in the file (some instruments need one). The
track's header shows `SYSEX n%` while it sends. Sequenced events to that
port wait behind it, with a warning if Play is pressed meanwhile.

## Remote, previews, tests (as every phase)

- `status`: `melodee_midi_track_*` (dest, device, online, channel, delay,
  pads, program, slots used, held notes, CC sent / thinned, sysex state).
- `melodee-midi panic`.
- Previews: `miditrack`, `mididevice`, `midislots`, `midislotedit`,
  `mididrum`, `midiaudio`, `midisysex`, `midioffline`.
- `tests/melodee.cpp`, `midi_tracks()`: the events per kind and their
  order at launch, Note Offs on mute, Stop and pattern change, CC thinning,
  NRPN and 14-bit expansion, the loop guard, the Thru skip, delay clamping,
  the file round trip, an old file and an old build (MIDI track as empty),
  and the offline fallback.
- On the device: the Mac over USB-C with a CoreMIDI monitor (timing against
  the clock out), and the jacks with a real synth.

## Phases

Each phase passes `./build.sh`, the tests and a device check, then commits
and fast-forwards `feat/melodee-app` (standing rule).

**Stage (2026-11-10): program and CC. Built.** Stage's MIDI pages: DEVICE
(port, channel), PROGRAM (bank MSB, bank LSB, program as the engine's
parameters 2-4, each Off until set; bank selects and the program go out on a
change and when the track loads), CC (three controllers, values on K1-K3 and
their numbers on Shift + K, General MIDI names; sent on a turn and as Motion
plays, recorded as Motion lanes). `MidiOutEvent::Control` / `Program`; the
`MIDT` section's version 2 (version 1 files get the new parameters' initial
values). Edit gained a PROGRAM page.

**M1: MIDI track, notes out. Built.** `EngineKind::Midi` (K1 on Sounds
picks it, sound MIDI OUT), `engine/midi_engine.h` (a silent `Engine`, so
the arp, note lengths and motion reach it as they reach a synth), port and
channel as parameters on Edit's DEVICE page (no mod matrix page, no motion
lane for them), `MidiOutEvent::dest` (the audio thread names the port,
the MIDI thread matches it: `dest_is()`), Note Offs on mute, solo, a new
port or channel, pattern launch, engine change and Stop, Keys pads (the
synth layouts), the `MIDT` section, MIDI Config's MIDI Panic row and
`melodee-midi panic`, `status` fields, previews `miditrack` and
`midipanic`, test `midi_tracks()`. Settings keep a favourites row for it.
Left out of M1: the USB-C stale-output drop (phase 9's Later box). The
kernel's f_midi holds up to `qlen` USB requests that user space cannot
take back, so it needs a device spike of its own in the base.

**M2: knobs, motion, expression.** Slots as params, SLOTS pages and slot
definition editing, Cc / Cc14 / NRPN / pitch bend / channel pressure,
CC thinning, motion record and playback, takeover, the fader's Mod Wheel
and Pitch Bend targets, pad pressure (channel or poly).

**M3: programs, drums, MIDI in.** Bank and Program per track and per
pattern at launch, the PROGRAM page, Drum pads with the map editor and
names on Steps and Roll, MIDI in echo and record with the loop guard and
Thru skip.

**M4: timing, devices, SysEx.** `delay_ms` with the negative clamp, USB
devices by name with hotplug and the All USB fallback, the SYSEX page,
chunked paced sending, on load and on demand.

**M5: audio back.** The AUDIO page, inputs into the strip, route conflicts
(`IN USE`), stems and bounce.

## As built in P4 (2026-11-12)

What the build changed from the design above, each a call P4 made under the
roadmap's rules (docs/ROADMAP.md "Decisions made by sessions"):

- **Slots are parameters, their definitions too.** `MidiDevice` holds only
  what is no number (`model/midi_track.h`: the device's name, the drum map,
  the SysEx file). A slot's address is a parameter (one number: -1 off,
  CC 0-119, 14-bit CC 0-31, pitch bend, channel pressure, NRPN 0-1023), so
  undo, track copy and the file come for free. Slot values are parameters
  8-31 (slots 1-3 where Stage's CC page had them).
- **Defining a slot** is the SLOTS page's second row (the wheel), not
  Shift + Kn; a range per slot and the Name sheet are not built: a knob's
  0-127 spans the message's range, and the card names the slot by what it
  sends ("CC 74", "NRPN 300", "BEND").
- **Bend** is a parameter (EXPRESS, -64..63) and a Motion lane; the fader
  reaches it by learning its pot (fader C), not a new Pitch Bend target.
- **Drum map** on PADS: K1 the layout, K2 the last pad's note; names are
  General MIDI's (shortened to fit a pad), not typed.
- **SysEx** files live in the card's `SysEx` folder (K1 picks one), not
  beside the project; sent whole in 256-byte pieces through the base's
  existing send (no base change), paced at 320 µs a byte on the jacks, and
  every other message waits behind it.
- **USB devices** are listed by their ALSA port's name each MIDI pass; a
  missing device sends to every USB output and shows OFFLINE.
- **Audio back**: the shell decides the input against the ADC's route
  (InputRoute's needs now include MIDI tracks'): a track asking for the
  input the route is not on gets none (IN USE).

## Open questions (decided in P4)

1. Panic's binding: left as built (MIDI Config's row and `melodee-midi
   panic`); no printed button taken.
2. USB devices to test with: none plugged in; name matching and the
   offline fallback wait for a hand check.
3. Slot templates: none; every MIDI track starts with CC 74, 71 and 1 on
   its first three slots, as Stage's CC page did, the rest off.
4. Bounce with MIDI tracks: real time, as every bounce already is; a MIDI
   track's input is recorded live.
5. Patterns store notes, not pads: kept.

## Open questions as first asked (2026-11-10)

1. **Panic's binding.** M1 built it as a MIDI Config row plus
   `melodee-midi panic` (no printed button taken). Should Shift + Stop do
   it too, if the MPC Sample leaves that free?
2. **USB devices (M4).** Which synths will be plugged into the USB host
   port to test with? A real `snd-usb-audio` MIDI device is needed for the
   name matching and hotplug check.
3. **Slot templates.** Ship ready slot sets for common gear (Volca, Minilogue,
   TR-8S, a generic GM one), or start every MIDI track blank?
4. **Bounce with MIDI tracks (M5).** Bounce of a MIDI track must run in
   real time while the instrument plays. Accept that, or leave MIDI tracks
   out of the offline bounce and the song export?
5. **Patterns store notes, not pads (M3).** Keep this, or make Drum mode
   behave like a kit (store pads, remapping moves the notes)?
