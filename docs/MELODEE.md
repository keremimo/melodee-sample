# Melodee on the MPC Sample

Melodee is a groovebox app on sample-base: sixteen tracks, one per pad,
each playing one sound engine (four synths, a 909 and a sampler), sequenced
in patterns and a song, mixed and effected on the MPC. Every printed button
keeps its MPC meaning; Melodee's own pages sit where the MPC leaves room.
Start it with `python3 device.py start`; `./release.sh` packages it for an SD card.

Everything is written for sample-base: interface, sequencer and engines.
Nothing from the FM-1 firmware is compiled or emulated; the engine ports in
`mpc-sample-plugins` are a reference for what each engine does. How each
MPC job came to be built, and what is still open, is in
[MELODEE_PARITY.md](MELODEE_PARITY.md); this file says what Melodee does
now.

## Source layout

Everything lives in `src/melodee/`, with `src/main.cpp` wiring it to the
sample-base runtime in the `base/` submodule. Layering follows
[APPS.md](../base/docs/APPS.md).

| Folder | Responsibility |
| --- | --- |
| `melodee_app.*` | The `App`: hands each runtime hook to its owner below. Thin. |
| `model/` | The project: tracks, patterns (event lists), song, mix, motion, kits, limits (`limits.h`). Plain data, no threads. |
| `engine/` | Audio: DSP pieces, the six engines and MIDI's silent one, effects and punches, the engine host (16 tracks, mixer, sequencer clock, MIDI out timing) and the command queue that feeds it. |
| `ui/` | The design system: tokens, the Rubik typeface, the components every page is built from. |
| `pages/` | One file per page: what it draws from a snapshot and what its pads, pots, wheel and soft keys do. |
| `shell/` | The control grammar and everything around the pages: printed buttons and their Shift jobs, Shift + pad (`shift_pad_jobs`), Pad Bank, mute mode, Note Repeat, Seq + Sample, undo, the fader, takeover, lights, MIDI in and out, the card (`project_store`, `sample_library`, `card_access`), audio input and renders, and the snapshot handed to the screen. |
| `tools.*` | `--melodee-preview DIR [PAGE]`: draws named pages to PNG on a host. |

The design boards (`.local/melodee-design/`, not in git) are the reference
for every page; halve their sizes for device pixels. `ControlMap.dc.html`
there shows the panel as this file describes it.

## Threads and data

Each runtime hook runs on one thread ([APPS.md](../base/docs/APPS.md)); Melodee keeps
one owner per piece of state.

- **MIDI thread** owns the project and the interface state. Pads, buttons,
  pots, the wheel, the fader and external MIDI change them there. Every
  change that affects sound becomes a small `Command` pushed onto one
  single-producer queue to the audio thread.
- **Audio thread** owns the engines and copies of the patterns. Each block
  drains the queue, advances the clock, plays the engines, mixes and
  effects the tracks, and decides which MIDI goes out at which frame. It
  never allocates, locks or waits: patterns, samples, projects and track
  copies arrive whole by pointer and go back to be freed elsewhere.
- **Screen thread** copies a `Snapshot` under a short lock shared with the
  MIDI thread, then draws without it.
- **Service thread** does every file on the card (projects, sounds,
  settings, samples, takes, renders), the input routes and the speaker,
  the power key, and SD card access. It leaves the card alone while the
  card is lent to a computer.

Wheel Push belongs to Melodee (`App::claims_wheel_push()`); the Exit sheet
(a short press of Power) saves and returns to the stock app through
`App::finished()`. Crash, remote stop and the recovery service are the
runtime's.

## The panel

Left column top to bottom, then right, as on the device (pad 1 is bottom
left). "Shift" is the job printed under the button; while Shift is held
the buttons with a Shift job glow and the knob strip shows the Shift + K
cards.

| Control | Press | With Shift |
| --- | --- | --- |
| Main volume | Master level, at once | |
| Sample | Stage → Keys (Pads on a kit) → Mix, round; from any other page Stage. Held + pad: that kit pad becomes the last pad, unheard. With Seq: see Seq + Sample | Input Config |
| Seq | Patterns. Held with Sample (either first): Patterns with the pads playing the selected track, a kit's pads (PADS PLAY KIT, both buttons blink); Seq or Sample again, or another page, ends it | Step Edit; again Steps → Roll → Motion → Step Edit |
| Pad FX | Pad FX (the punches); again on a kit track, Pad Punch | Flex Beat |
| Knob FX | Knob FX mode on (lit, its page) or off | FX Select: the effect chains, Track → Sends → Bus → Master |
| Shift | The second layer, held | |
| Pad Bank | Next bank of the kit, A–H round. Held: the screen shows the tracks, eight at a time (the half with the selected track), a pad picks one, −/+ or the wheel jump banks; with Shift + Erase, Copy takes tracks. Tapped twice (within 450 ms): the Tracks screen, latched; on a kit the first tap's bank step is undone, and a tap on Tracks goes back to Stage | Previous bank |
| Fader | The track's target (Fader page), or the page's own job (Fader below); its light follows the value | |
| Erase | Held + pad, pot, step or fader: clear it (below) | Copy: kit pads, patterns on Patterns, tracks with Pad Bank held |
| Note Repeat | A tap latches, a hold lasts; double tap opens Arp | |
| Chop | Chop on the last pad's region; again back to Stage's TRIM | Note On (the last pad plays while held), or One shot |
| Mute | Mute mode: a tap latches, a hold lasts | Unmute everything |
| Loop | The last kit pad's loop on or off, lit while it loops | Reverse the last pad |
| 16 Levels | The last pad (or a synth's last note) on all sixteen pads, again back | The type: Velocity, Filter (kits), Tune, Scale |
| Sample Select | Samples (the browser) on a kit, Sounds elsewhere | Kit: Save Sample. Synth or 909: Save sound. On Sounds: star |
| Tap Tempo | Tap the tempo; held half a second, the Tempo page | Metronome Off → On → Rec |
| Wheel | Moves the cursor or the value; turned while a pot was touched last with Shift, fine edit | |
| Push | Opens or confirms | Back along the page stack (16 deep; on Song a marker, on Project up a folder) |
| − / + | Lists move as the wheel does; on play pages the octave; Settings and Tempo the value; Song markers; Steps nudge; sampler pages and Punch the bank | Undo / redo, on every page |
| Sample Record | Record (kit tracks); on Record it ends the take and opens Stage's TRIM on the pad | Audio Recall: the last 25 s onto the next empty pad |
| Seq Record | Stopped: arms (flashing), Play starts; playing: records at once; recording: stops recording, playback goes on | Recall (Capture): the last bars played into a pattern |
| Stop | Stop (on Record, ends the take) | All notes off |
| Play | Play from the start (on Song from the cursor) | Continue from where it stopped or the wheel put it |
| B1–B3 | The page's soft keys, named above them; with Note Repeat on, its rate (on Stage only while Note Repeat is held) | Page jobs (Stage's TRIM on a kit, Chop) |
| K1–K3 | The cards above them; Knob FX mode takes them for its effect | The card's second parameter (cards with the Shift icon) |
| Pads | The page's job (most pages play the selected track) | The jobs printed under the pads |

### Shift + pads

As printed, on every page but the sample browser (where Shift + pad loads
the file under the cursor onto the next empty pad). `shell/shift_pad_jobs.cpp`
holds the table the dispatch and the lights share.

| Pad | Job | Pad | Job |
| --- | --- | --- | --- |
| 1 | Full Level (every hit at 127; again back) | 9 | Fader page |
| 2 | Half Seq | 10 | Rec Quantize on / off |
| 3 | Double Seq | 11 | Resample (then a pad) |
| 4 | Count-In on / off | 12 | Song |
| 5 | Compressor | 13 | Trim Sample |
| 6 | Half Speed | 14 | Time Correct |
| 7 | Double Speed | 15 | Warp: Time stretch or Pitch for the kit |
| 8 | MIDI Config | 16 | Project |

Toggles (1, 4, 10) are kept in the settings file. While Shift is held the
pads light: a toggle that is on bright, a job faint. The screen draws no
Shift layer, since the jobs are printed on the MPC: the page stays, and
the knob strip shows only the Shift + K cards.

### Lights

As on the stock MPC: a button is bright while in use (its page, Play while
playing, Seq Record recording, mute mode, Note Repeat on, Erase while
copying), faint where it does something on the page in view, dark where it
would do nothing (Chop needs a kit pad with a sample, Loop and Sample Record
a kit track, Erase something to clear). On the Name sheet only Erase, Play
and Stop glow; on the Exit sheet none; during SD card access only Play and
Stop. The bank lights show the kit's bank; the fader light shows its value
(brightest at the middle for pan and tune). `shell/button_jobs.cpp`,
`shell/lights.cpp`.

## Pages

### Playing

- **Stage** (Sample): the selected track's quick pages, as the MPC's Sample
  mode. **B1–B3** each hold a cycle of up to three pages: the lit one again
  steps to the next, another jumps to its first; the soft keys name each
  button's page, with a dot a page under a cycle. Each track keeps its own
  button and page. The pads play the track (a kit's pad played is the pad
  edited), the **wheel** fine-edits the card touched last (ringed), **Push**
  moves to the next card, Shift + Push is Back, −/+ the octave (a kit's
  bank). Track 1 starts as a PROPHET, track 5 an empty kit, track 6 a 909
  with a beat.
  - **A synth** (PROPHET, FM6, CZ, SID): B1 **PRESET → TONE → MIX**, B2
    **SHAPE**, B3 **ARP → its own page → FX**. PRESET: the sound before,
    the one in hand with its star, the one after, where it sits; K1 or the
    wheel walks the sounds and the one turned to loads once the turning
    has stopped 250 ms; K2 the list (all, starred, recent), K3 the engine;
    Push opens Sounds. TONE: the engine's three favourites (PROPHET and SID
    draw the filter). MIX: level, pan, send; Shift + K3 the bus the send
    goes to (Melodee has one send a track). SHAPE on PROPHET: FILTER ENV →
    AMP ENV → LFO (Shift + K1 release); FM6: OP 1 ENV → OP 2 ENV → TOUCH;
    CZ: DCW ENV → DCA ENV → TOUCH; SID: ENVELOPE → PWM → TOUCH. ARP: mode,
    rate, octaves, Shift + K1 gate, Shift + K3 hold. The own page (PERFORM,
    RATIOS, LINES, OSC) draws its Shift row. FX: the slot of the track's
    chain the cards edit (Push the next one; Shift + Knob FX adds effects).
  - **A kit**: Akai's pages in Akai's order on the last pad hit, B1 **TRIM →
    MIX → AMP ENV**, B2 **TUNE → PLAY**, B3 **FILTER → FILT ENV**: the pad's
    waveform with its volume on the left and pan on the right, chips for
    how it plays; TRIM draws its handles (Shift + K1–K3 zoom at each; Shift
    + B1 Loop Lock, Shift + B2 Normalize), the envelopes and the filter lie
    over the region. MIX: volume, kit volume, pan; Shift + K2 on AMP ENV and
    FILT ENV decay from start or end, Shift + K3 on TUNE # Beats, Shift + K2
    on PLAY Pad Link.
  - **A 909**: B1 **PRESET → VOICE → LEVELS**, B2 MIX, B3 FX. VOICE edits
    the drum the last pad played (Snare, Clap and Rim share a page) and the
    B1 key reads its name (KICK, SNARE, TOMS, HATS, CYMBALS); LEVELS has the
    toms, hats and cymbals on Shift.
  - **A MIDI track**: B1 **DEVICE → PROGRAM**, B2 **CC**, B3 ARP. PROGRAM:
    bank MSB, bank LSB and program (each Off until set), sent on a change
    and when the track loads. CC: three controllers on K1–K3 with their
    General MIDI names (74 Brightness, 71 Resonance, 1 Mod wheel at first),
    Shift + K picks a controller's number; they go out on a turn and record
    as Motion.
  - **An empty track**: B1 **SYNTH** (K1 engine, K2 sound), B2 **KIT** (K1:
    samples on the card, record the input, Audio Recall, Resample), B3
    **MIDI** (K1 port, K2 channel); Push makes the track that.
- **Tracks** (Pad Bank tapped twice): the sixteen tracks as the pads sit,
  eight at a time, each cell engine, sound, the pattern playing and level.
  B1 tracks 1–8, B2 tracks 9–16, B3 back to Stage; a pad picks its track
  and goes back to Stage; K1 the track, K2 its level, K3 its pattern; the
  wheel the track. Pad Bank held + pad still picks a track from any page.
- **Keys** (Sample again on a synth): the pads in the project's scale
  (layouts, octave on −/+); Shift + K3 transposes the pads −12..+12 (kept
  with the track). Push opens Edit.
- **Pads** (Sample again on a kit): the bank as the pads sit, the last pad
  with its length, play mode and waveform; K1 pitch, K2 level, K3 pan; Push
  opens the map of the ten sample pages.
- **Mix** (Sample a third time): eight strips at a time (the half with the
  selected track), level, pan, mute and solo,
  the send and its bus, meters. The fader is the selected track's level here.
- **Sounds** (Sample Select on a synth or 909): the factory sounds and one's
  own for the engine; K1 steps the engine (six), the wheel walks, Push
  loads; FAVOURITES lists the starred (Shift + Sample Select).
- **Edit** (Push on Keys): the engine's pages, then **Touch**
  (velocity and pressure: AT > VIB adds vibrato, AT > BRIGHT opens the
  sound), then the **mod matrix**: eight routes a track, source (LFO,
  velocity, aftertouch, wheel, key track, random, filter envelope),
  destination (each engine names its own) and amount −100..100. B2 opens the
  page map (wheel, Push jumps, B2 or Shift + Push closes).
- **16 Levels**: on a kit the last pad on all sixteen pads, on a synth its
  last note. Velocity (8–127), Filter (kit pads: pads 1–8 up to the pad's
  cutoff, 9–16 on to the top), Tune (the original on pad 4), and Melodee's
  Scale. Shift + 16 Levels or B1–B3 pick the type.
- **Note Repeat** stays on the page: while on, B1–B3 are 1/8, 1/16, 1/32
  (Shift: their triplets); straight rates swing with the project. It is the
  track's arp; a kit repeats the held pad.
- **Arp** (double tap Note Repeat): Up, Down, Up-down, Random, Order; K2
  rate 1/4 .. 1/32T, K3 octaves, −/+ gate, B3 Hold. **Chord** (B1 on Arp):
  one pad a chord from the scale (Triad, Dia 7, Sus2, Sus4, Power, Dia 9;
  Close, Open, Drop 2, Spread; inversions).
- **Tempo** (hold Tap Tempo): K1 tempo, K2 swing, K3 click level; B1 halves,
  B3 doubles, B2 the metronome Off / On / Rec; the wheel by 0.1, −/+ by 1.

### Sequencing

The clock runs at 96 ticks a quarter. A pattern is a list of note events
(tick, length, key, velocity, chance, accent / slide / tie) up to 128 bars
of the project's time signature; its **Q** (1/4 .. 1/64 with triplets) is
only the grid. Swing (50–75 %) stretches the first sixteenth of each
eighth.

- **Patterns** (Seq): tracks down, sixteen patterns across. Pads are the
  track's patterns: a tap queues (next bar, or at once when stopped), Erase
  + pad clears, hold one and tap another to copy. K1 length (1, 2, 4 .. 128
  bars; Shift: any count), K2 Q (Shift: time signature, 1/4 .. 15/16), K3
  swing (Shift: metronome level). The wheel moves the playhead by a beat
  (Shift: by Q); stopped, that is where Shift + Play continues. Hold B1 +
  wheel for the BPM (Push: whole / tenths); B2 SEQ / GBL BPM (each pattern
  number its own tempo, the one launched last leading); B3 COLUMN latches
  column launch. The header shows the position, time signature and Rec
  Quantize.
- **Seq Record**: arm and Play, or record at once; again stops recording.
  **Count-In** plays a bar of clicks first. Notes keep the tick and length
  they were played; with **Rec Quantize** on they land on Q (a note moved
  ahead of the playhead is not played twice). Overdub. A take is one undo.
  While the song plays, a pattern launched is written as a clip.
- **Step Edit** (Shift + Seq): the events starting in one step of Q, listed.
  Wheel a step, −/+ through its events, a pad picks its event; K1 pattern
  length, K2 Q, K3 velocity, Shift + K1 the event's length, Shift + K2 time
  signature; the fader nudges the event a tick a step; B3 erases the event,
  Erase + pad asks, then erases that pad's events. Hold B1 for the BPM.
- **Steps, Roll, Motion** (Shift + Seq again, round): Steps the bar's steps
  on the pads; Roll a piano roll of a bar (pads write up to four notes a
  step, Push on a note opens its sheet: accent, slide, tie, copy, delete);
  Motion the lanes of recorded pot moves (Seq Record + a pot writes at the
  playing sixteenth; a kit pad's level, tune, pan, envelopes, filter and
  more are lanes of that pad; Erase + pot asks, then clears the lane).
- **Time Correct** (Shift + Pad 14): the selected pattern onto Q; pads pick
  whose notes move, B2 all; K1 Q, K2 shift ±48 ticks, K3 swing; B3 DO IT.
- **Half Seq, Double Seq, Half / Double Speed** (Shift + Pad 2, 3, 6, 7):
  each one undo; the length stays for the speeds.
- **Recall** (Shift + Seq Record): the last 1–4 bars the pads played,
  quantised into the first empty pattern; B3 or Push keeps it, B1 drops.
- **Song** (Shift + Pad 12): a lane per track, clips of patterns, markers,
  a loop brace, the cursor. K1 zoom (4–64 bars), K2 / K3 the next clip's
  pattern and length (or the clip's under the cursor); wheel by the bar,
  pads pick the lane, Push places a clip or opens its sheet (split,
  duplicate, repeat to the next marker, loop these bars, own pattern,
  motion lane, mute, delete); −/+ jump markers; Shift + Push adds a marker
  (on a marker, names it). Play here plays the song from the cursor. **B1
  Export**: B2 audio mixdown (Name sheet; once through, tails up to 10 s,
  `Melodee/Mixdowns/<name>.wav`, 24-bit; Stop drops it), B3 New pattern
  (the song laid end to end into the first free pattern number).

### Sampler

A kit has 128 pads in eight banks (A–H), each a region of one of the
project's 64 samples. Samples are WAV (16, 24 or 32-bit PCM or float, any
rate, up to 60 s), read on the service thread and handed whole to the audio
thread.

- **Samples** (Sample Select on a kit): the browser. Folders first, then
  files; wheel and −/+ move, Push opens a folder or loads onto the last pad
  (keeping its settings), B1 back out, B2 the drive (CARD from `Samples`,
  or INTERNAL: the factory categories and the stock app's User folder, read
  only), B3 PREVIEW plays each file as the cursor lands; Shift + pad loads
  onto the next empty pad. Melodee writes only to the card.
- **The ten sample pages**, in the MPC's groups: **B1** Trim → Mix → Amp
  Env, **B2** Tune → Play → Follow → Cloud, **B3** Filter → Filt Env → Kit
  filter. A B key goes round its group, or opens its first page from
  elsewhere; a dot a page under each key, as on Stage. **Push** opens the
  page map (three columns; wheel, Push jumps, a B key or Shift + Push
  closes); Push on Pads opens it too. Trim itself lives on Stage (a kit's
  B1 TRIM): every way to Trim (B1 here, Chop again, Sample Record on
  Record, Shift + Pad 13) lands there.
  - Trim: K1 start, K2 end, K3 loop point; Shift + K1–K3 zoom at that
    handle; Shift + B1 Loop Lock; Shift + B2 Normalize (a gain, one undo).
  - Mix: K1 volume in dB (−inf, −74..+6) with its meter, K3 pan, Shift + K2
    kit volume.
  - Amp Env: attack, decay (One shot) or release (Note On), velocity
    sensitivity; Shift + K2 decay from start or end; row 2 the rest.
  - Tune: semitones, cents, Warp (Off, 50–200 %, Seq from # Beats; Shift +
    K3 # Beats). Shift + Pad 15 picks Time stretch or Pitch.
  - Play: Mono / Poly, mute group 1–16, offset; Shift + K2 Pad Link; row 2
    Note On, reverse, loop.
  - Filter (Off, Classic, LPF2/4, HPF2/4, BPF2/4) and Filt Env (up to seven
    octaves; Shift + K2 decay from).
  - Melodee's Follow (the kit keeps the project's tempo: Beats, Tones or
    Tape), Cloud (pads as grain clouds) and Kit filter (one filter after
    every pad).
- **Loop, Shift + Loop, Shift + Chop** act on the last pad from any page;
  the sample pages' title shows note, loop and reverse icons.
- **Chop** (Chop): the last pad's region in slices that touch. Pads play and
  pick slices; K1 start, K2 end, K3 type (Threshold, Regions, Beats,
  Manual), Shift + K3 its amount, Shift + K1 / K2 zoom. Manual (K3 or Push):
  pad 1 plays the region and pads tapped meanwhile cut it, each pulled onto
  the strongest hit. Shift + B1 Extract (`Samples/Edits/<NAME>-Slice n.wav`
  onto the next empty pad), Shift + B2 Split, Shift + B3 Merge; Erase + pad
  takes a slice out. B3 TO PADS lays the slices on pads, B1 DROP back.
- **Trim Sample** (Shift + Pad 13): the region as
  `Samples/Edits/<NAME>-Trimmed.wav` in a new slot, the pad onto all of it;
  one undo; then Stage's TRIM. **Save Sample** (Shift + Sample Select on a kit): the Name
  sheet, then `Samples/<name>.wav`; the pad unchanged.
- **Input Config** (Shift + Sample; B2 on Record): K1 source (Mic, Rear,
  Rear L, Rear R, Resample, USB, USB L, USB R), K2 rec length (Free, Seq,
  1–16 bars), K3 threshold (−96..0 dB), Shift + K3 gain; B1 monitor Off /
  Auto / On (the speaker goes quiet while the mic is monitored), B2 Rec
  Input FX (Knob FX on the input during a take), B3 back. Kept with the
  settings.
- **Record** (Sample Record on a kit): the same cards and B1. Tap a pad to
  record onto it (from the threshold, or with Seq where the pattern comes
  round); other pads tapped during the take cut it there. The pad again,
  Stop or Push ends it and Record stays; Sample Record ends it and opens
  Stage's TRIM. `Samples/Recorded/TAKE n.wav`, up to 60 s.
- **Audio Recall** (Shift + Sample Record): the source always fills a 25 s
  ring; this copies it out from the first sound as
  `Samples/Recorded/RECALL n.wav` onto the next empty pad. One undo.
- **Resample** (Shift + Pad 11, then a pad): the selected track's pattern
  with every track playing, rendered in real time onto that pad
  (`Samples/Recorded/RESAMPLE n.wav`, up to 60 s). One undo.

### Effects

Each track runs a chain of six effects, then one send into a bus (A–D, six
slots and a return each), then the master chain (six) before the
compressor and main volume. A kit pad can have its own chain of four,
ahead of the track's. 41 effects: reverbs, delays (Echo, Tape echo, Ping
pong, Looper, Diff delay, Sample delay), modulation, dirt (Drive, Tape,
Crush, Spit, Fuzz, Amp sim, Tube drive, Soft clipper, Vintage, Vinyl, Tape
emulator), filters and EQ, dynamics (Compressor, Gate, Transient, Limiter,
Pumper), Spring, Ensemble, Multi-chorus, Auto-wah, Formant, Width.

- **FX Select** (Shift + Knob FX, round Track → Sends → Bus → Master; B1 /
  B3 walk too): four device cards in view; the wheel picks, Push opens
  Change, Bypass, Move, Remove; + ADD opens the picker. Sends: K1 send, K2
  bus, K3 the bus's return. B1 from Track on a kit: the last pad's chain.
- **Knob FX** (Knob FX): the mode on or off. While on, K1–K3 (and Shift +
  K1–K3) are its effect's on every page. The page lists the MPC's 28 then
  Melodee's 15 (wheel or −/+, Push loads). Pads switch it per kit pad (kit
  tracks) or per track; B2 all, B3 bypass.
- **Pad FX** (Pad FX): 47 punches in three banks (Pad Bank or −/+): the
  MPC's sixteen in its order, then Melodee 1 and 2. Hold pads to punch, up
  to four at once (a fifth bypasses the oldest); pressure is depth. B1 LATCH
  keeps the last one on, B2 master or track, B3 Pad Punch on a kit. An MPC
  punch's K1–K3 are its own, Shift + K1–K3 depth, snap, tail.
- **Flex Beat** (Shift + Pad FX): a time effect over the master for a bar;
  pad 1 Empty, 2–16 fifteen effects; K1 One Shot / Loop, K3 mix, B3
  quantize.
- **Pad Punch** (Pad FX again on a kit): each kit pad its own punch, rate
  and depth.
- **Compressor** (Shift + Pad 5): on the master. B1 Color, B3 engage; K1
  attack, K2 release, K3 amount, Shift + K3 In Boost.

### Fader

Each track has a target, picked on the **Fader page** (Shift + Pad 9; wheel
or −/+ move, Push picks, B3 off / on, B1 back): Pad Volume, Pad Pan, Pad
Tune, Pad Attack, Pad Decay / Release, Pad Cutoff (the last pad's), Kit
Volume, Mod Wheel. Until picked, a kit takes Pad Volume and a synth the mod
wheel. Pages with an obvious job keep it: Mix the track's level, Sends its
send, Step Edit nudges, Knob FX its effect's MIX, Compressor its amount,
Flex Beat its mix. The fader takes a value over by Fader takeover (Pickup
by default); Seq Record writes the lane of what it moves; Erase + fader
asks to erase that lane. A card at the picture's bottom right says what it
drives for a moment after it moves.

### MIDI and sync

**MIDI Config** (Shift + Pad 8; wheel, Push opens a value, −/+ change it, B1
back): port (External jacks or USB), In channel (All, 1–16: plays the
selected track), Out channel, Track Channel (a track's own: plays it
whichever is selected, and its notes go out there), Pad MIDI In / Out
(Never, Always, Empty), Sync In / Out (Off, MIDI Clock, MTC), Thru, Program
Change (launches that pattern), CV/Sync Out with Base 1–8 and Division 1–24
(pulses a quarter), the three takeover rows, MIDI Panic (Push: every note
here ends, and All Sound Off, All Notes Off and pitch bend centre go out on
every channel of every port) and Reset Factory Settings (B3 YES, B1 NO). The header shows IN / OUT as MIDI passes, EXT CLOCK or EXT MTC
while following.

- **In**: notes and pressure, CC 1 as the mod wheel, All Notes Off; a kit
  hears note 36 as A01; notes record under Seq Record like pads. Clock in:
  Start, Continue from Song Position, Stop, the tempo from the pulses. MTC
  in (24, 25, 29.97 df, 30 fps): follows after two whole codes agree, from
  0:00:00:00 (hours left out) through the tempo playing, kept on the
  master's time by a pace trim of up to 0.5 %.
- **Out**: decided on the audio thread per frame and sent by the MIDI thread
  when that frame is heard (`engine/midi_out.h`): pads, patterns, the arp,
  MIDI clock, MTC (25 fps) and the CV clock, while playing.
- **MIDI tracks** (`docs/MIDI_TRACKS.md`): a track with the MIDI engine
  sends its pads, patterns and arp on its own port and channel (Edit's
  DEVICE page: External, USB-C, All USB; 1–16), whatever Pad MIDI Out
  says. Muted or soloed out, a new port or channel, a pattern launched,
  another engine or Stop: the notes it holds end.

### Project and system

- **Project** (Shift + Pad 16): files (`.mldp`) in `Melodee/Projects` on
  the card, in folders. Rows: New project (asks first: B3 DO IT, B1
  CANCEL), the open project while it has no file, then at the top **Demos**
  (Melodee Starter, Night Drive, Broken Tape, made in code, opened unsaved)
  and **SD card access**, then folders (Push in, Shift + Push out) and
  projects with their tracks and tempo. K2 jumps, K3 sorts. Push on a
  project opens its sheet: on the open one Save, Save as new, Rename,
  Duplicate, Move to folder, New folder here, Open at start, Delete (asks
  again); on another Load and **Load kit (no patterns)** first (its first
  kit onto the selected track, one undo). B2 Settings, B3 About.
- **SD card access** (Project, under Demos; as the MPC's): the card goes to
  a computer over USB-C as a drive. The page shows where the card is (the
  MPC or the computer) and what to do: connect, then eject the drive on the
  computer, then **B1** brings it back (Projects and Samples are read again,
  and Project is in view). If the computer still holds the drive, B1 says so
  and B1 again asks whether to take it anyway (B3, B1 Cancel). Meanwhile
  every other mode waits: only B1, Play, Stop, the pads (the selected
  track's sound) and main volume answer, and Melodee reads and writes
  nothing on the card. USB audio and stems, USB MIDI and the network keep
  working. It will not start while a take or render writes to the card.
  The base does the lending (`drivers/card_share.h`, HARDWARE.md "SD card
  access"); the page is `pages/card_access_page.cpp`, the controls
  `shell/card_access.*`.
- **Settings** (B2 on Project): palette, pad response (soft, medium, hard,
  full level), pad pressure on / off, pad lights, Knob / Knob FX / Fader
  takeover (Pickup, Scaled, Instant; Scaled, Scaled, Pickup by default),
  screen brightness, sample rate (44.1, 48, 96 kHz) and audio buffer (256,
  384, 512, 768, 1024 frames, shown with the delay it makes: longer rides
  out a busy moment without a crackle, shorter is heard sooner); under the
  rows, what the device runs at now and its dropouts since start. A rate or
  buffer change reopens the audio device (a short silence) and a rate change
  stops playback: the engine builds every sound and effect again at the new
  rate. The computer sees the MPC's USB-C audio at the new rate too (the
  gadget reconnects: USB audio, MIDI and the network drop for a second; not
  while the card is lent). Samples, takes, renders and every frame count in a project stay at
  44.1 kHz whatever the rate (`engine/model_rate.h`), so projects open the
  same at any rate. Kept in `Melodee/settings` with the project to open at
  start. **About** (B3): engines, effects, limits, versions, licence.
- **Name sheet** (Save, Rename, markers, sounds, samples): pads type like
  phone keys (pad 14 space, 15 digits and `- _ .`, 16 delete); the wheel
  moves the cursor, Shift + wheel or K2 the character, K1 a ready name,
  Erase deletes (Shift + Erase clears); B1 cancels, B2 Aa, B3 or Push keeps.
- **Exit** (Power, short): B3 LEAVE saves the project first, then returns to
  the stock app; B1 stays.

## Across every page

- **Mute mode** (Mute): the pads mute kit pads (bank in view) where they
  play a kit, tracks elsewhere; Shift + pad solos. Muted pads red, soloed
  white. Shift + Mute unmutes everything. Kit pad mutes are saved.
- **Erase**: + pad on a kit, stopped, empties the pad; playing or recording,
  erases that pad's notes (one undo). + pad on Patterns clears the pattern,
  on Step Edit asks. + pot sets the initial value (on Motion: asks to clear
  the lane). + fader asks to clear its lane.
- **Copy** (Shift + Erase): the first pad is the source, the next the
  target; kit pads, patterns on Patterns, whole tracks with Pad Bank held
  (sound, kit, sixteen patterns and their motion; the target keeps its mix
  strip). Again, or Erase, leaves.
- **Undo** (Shift + −, redo Shift + +): the last 32 steps. A take is one; a
  pot's turns are one per control (closed by another control, a press or
  two seconds still); a press on a page is one step of what the page edits.
  On Roll, Steps and Step Edit a pad is one step of the pattern from press
  to release: the note it writes, toggles or picks and whatever the wheel,
  the pots or the fader do to it while it is held (undo while holding takes
  that step back). The note cards and Step Edit's fader nudge are steps
  like any pot. Mute, solo, mode buttons and the transport are not steps.
  A load or a new project forgets them.
- **Takeover**: a pot drives its value once it meets it (Pickup), moves it
  part of the way (Scaled) or at once (Instant); the card shows the value
  dim with a tick where the pot sits until then.
- **Pad expression**: velocity is heard on every synth through one curve;
  pad pressure adds vibrato and brightness (AT > VIB, AT > BRIGHT) and
  reaches every note a pad started; off in Settings.
- **Text size**: nothing on screen is drawn under 11 px (`ui::type::floor`
  in `ui/tokens.h`; labels 11, body 13, lists 15, knob values 22), so the
  320 × 240 panel reads at arm's length. Text is never cut: where a page
  runs out of room it shows fewer things at a time (Tracks, Mix and Sends
  eight tracks, Song six lanes, FX chains three devices), or a long
  capitalised name turns to title case in a narrow cell.

## Sound engines

- **PROPHET**: two oscillators (saw, pulse, sync), noise, a four-pole
  ladder, two ADSRs, LFO, poly mod, glide, unison, drift; eight voices,
  thirteen sounds.
- **FM6**: six sine operators, eight algorithms, feedback; eight voices,
  nine sounds.
- **SID**: a chip voice with ring and sync, pulse width LFO, crush, a
  multimode filter (8580 or 6581); six voices, nine sounds.
- **CZ**: phase distortion, eight waves, two lines, DCW and DCA envelopes;
  eight voices, eight sounds.
- **909**: kick, snare, clap, rim, three toms, closed and open hat, crash,
  ride on pads 1–11 (12–16 dark); six Edit pages, five kits.
- **Sampler**: the kit above.
- **MIDI**: no sound; an external instrument on the track's own port and
  channel (above, "MIDI and sync"). No mod matrix.

Every synth has the mod matrix, Touch and an LFO. Each track has eight
voices or the engine's own count; tracks render in one block of 128 frames.

## Files on the card

- `Melodee/Projects/**.mldp`: tagged sections, so older files stay
  readable and only what differs from empty is written (`EVNT` events,
  `MOT2` / `MOTP` / `MOTW` motion, `TIME`, `PAD4` / `KITX` / `PMUT` kits,
  `KNFX`, `CMPR`, `PFXP`, `FADR`, `MIDC`, `MIDT`, `PVER` and the rest; a
  MIDI track is None in `TRAK` and MIDI in `MIDT`, so older builds load it
  empty). Saved beside
  the file, then renamed into place.
- `Melodee/Sounds/<name>.mlds`: a sound's values and mod routes.
- `Melodee/settings`: Melodee's settings, MIDI Config, Input Config, the
  favourites and the project to open at start.
- `Samples/`: loaded from anywhere under it; Melodee writes `Recorded/`,
  `Edits/` and saved samples there. `Melodee/Mixdowns/` holds song
  mixdowns.

Nothing is created on the card until something is saved. On a host,
`$MELODEE_PROJECTS`, `$MELODEE_SAMPLES` and `$MELODEE_INTERNAL` stand in
for the card and the internal drive.

## Remote

`python3 device.py ctl ...` reaches the runtime's commands (HARDWARE.md)
and Melodee's:

- `melodee-press [shift+]BUTTON`: a press (`play`, `seq`, `wheel-push`,
  `top-left` ...); `wheel+N` / `wheel-N`; `pad1`..`pad16`, also with a
  button held around it (`shift+pad5`, `erase+pad5`, `pad-bank+pad5`,
  `mute+pad5`); `hold-BUTTON` / `release-BUTTON`, `hold-padN` /
  `release-padN`; `k1=V`..`k3=V`, `shift+k3=V`, `fader=V` (0..127).
- `melodee-midi [usb] HEX...`: one message into the input path, as if from
  the External (or USB) port. `melodee-midi panic`: MIDI Config's MIDI
  Panic.
- `card-share [on|off|force]` (the runtime's): SD card access from the
  remote; Melodee opens its page as if B1 had been pressed there.

`status` adds Melodee's fields: `melodee_playing`, `melodee_dropped`, the
page and track, the lit buttons (`melodee_buttons=seq:2,stop:1`, 1 faint,
2 bright) and pads (`melodee_pads=`); the sequencer (`melodee_rec`,
`melodee_count_in`, `melodee_position`, `melodee_pattern_ticks`,
`melodee_events`, `melodee_q`, `melodee_first_tick`, `melodee_signature`,
`melodee_ticks`, `melodee_song_bar`, `melodee_song_clips`); Step Edit
(`melodee_step`, `melodee_event`, `melodee_event_tick`); `melodee_ask` (0
none, 1 erase a pad's events, 2 erase a lane, 3 reset settings, 4 new
project, 5 take the card back); `melodee_motion`; the last pad
(`melodee_last_pad`, `melodee_pad_loop` .. `melodee_pad_gain`,
`melodee_pad_slots`, `melodee_pad_start`, `melodee_pad_end`,
`melodee_pad_name`), 16 Levels (`melodee_levels`, `melodee_levels_kind`);
Chop (`melodee_chop_*`); the browser (`melodee_files`,
`melodee_file_cursor`, `melodee_drive`, `melodee_preview`,
`melodee_folder`); input and renders (`melodee_take`,
`melodee_take_frames`, `melodee_rec_pad`, `melodee_rec_cuts`,
`melodee_input`, `melodee_input_engine`, `melodee_monitor`,
`melodee_monitoring`, `melodee_threshold`, `melodee_speaker`,
`melodee_recall_frames`, `melodee_render`, `melodee_render_frames`,
`melodee_rendering`, `melodee_resample_pick`); Knob FX and the compressor
(`melodee_knob*`, `melodee_rec_fx`, `melodee_comp*`); Pad FX and Flex Beat
(`melodee_punch_bank`, `melodee_engaged`, `melodee_latched`,
`melodee_punch_selected`, `melodee_punch_slots`, `melodee_live`,
`melodee_flex*`); the fader (`melodee_fader_*`, `melodee_wheel`,
`melodee_takeover`); MIDI (`melodee_midi_in`, `melodee_midi_out`,
`melodee_midi_last`, `melodee_clock_*`, `melodee_ext`, `melodee_ext_tempo`,
`melodee_cv_*`, `melodee_mtc*`, `melodee_midi_late_us`; MIDI tracks'
`melodee_midi_track_out`, `melodee_midi_last_out` (its three bytes, hex),
`melodee_midi_last_dest` (1 External, 2 USB-C, 3 All USB),
`melodee_midi_panics`; MIDI Config's rows
`melodee_midi_port` .. `melodee_midi_row`); undo and projects
(`melodee_undo`, `melodee_redo`, `melodee_undo_last`, `melodee_seq_sample`,
`melodee_map`, `melodee_demos`, `melodee_gesture`, `melodee_engine`); SD
card access (`melodee_card` 0 home, 1 lending, 2 lent, 3 returning;
`melodee_card_note`, the reason a lend did not start, `CardNote`;
`melodee_card_ejected`, `melodee_card_locked`). The runtime's own
`card_share=` says the same from the base's side.

## Previews

`build/melodee --melodee-preview DIR [PAGE]` draws every named page at
1× and 2× (the 2× ones line up with the 640 × 480 boards); the names are in
`src/melodee/tools.cpp` (`cardrow`, `cardaccess`, `cardejected`,
`cardlocked`, `cardnocomputer`, `cardnone`, `miditrack` and `midipanic`
among them).
`--melodee-sound FILE.wav` renders two bars of the starting project and
checks the level. `./build.sh` runs both. The preview also checks text:
anything asked for under 11 px, wider than its box, or a header whose two
sides meet prints a `MELODEE_TEXT` line, and any such line fails the run.

## Installing

`python3 device.py install-sd --app melodee` (see README) starts it from
the card at every boot; leaving it returns to stock until the next boot.
Limits that are single constants (`model/limits.h`): 16 tracks, 16
patterns a track, 8 mod routes, 256 motion events a project, 64 samples.
