# Melodee on the MPC Sample

Melodee is a groovebox app on sample-base: sixteen tracks, one per pad,
each playing one sound engine, sequenced in patterns and mixed on the MPC.
It lives entirely in `src/melodee/` so it can later move to its own
repository as one folder. Start it with `--app melodee`
(`python3 device.py start --app melodee`).

Everything here is written for sample-base: the user interface, the
sequencer and the sound engines. Nothing from the FM-1 firmware is compiled
or emulated; Melodee's engine ports in `mpc-sample-plugins` are a reference
for what each engine does, not code that is built.

## Layout

| Folder | Responsibility |
| --- | --- |
| `src/melodee/melodee_app.*` | The `App`: hands each hook to the parts below. Thin. |
| `src/melodee/model/` | The project: tracks, patterns and steps, mix, scales, limits. Plain data, no threads. |
| `src/melodee/engine/` | Audio: the DSP pieces, each engine, the engine host (16 tracks, mixer, sequencer clock) and the command queue that feeds it. |
| `src/melodee/ui/` | The design system: tokens, the Rubik typeface, and the components every page is built from (soft keys, header, pill, chip, track tag, knob strip, page dots, list row, sheet). |
| `src/melodee/pages/` | One file per page: what it draws from a snapshot and what its pads, pots, wheel and soft keys do. |
| `src/melodee/shell/` | The page router and the control grammar: printed buttons, Shift layer, Pad Bank (`pad_bank`, `track_copy`), mute mode (`mute_mode`), Note Repeat (`note_repeat`), pot pickup, lights, and the snapshot handed to the screen. |
| `src/melodee/tools.*` | `--melodee-preview DIR [PAGE]`: draws named pages to PNG on the host. |

The design boards (`.local/melodee-design/`, not in git) are the reference
for every page: halve their sizes for device pixels.

## Threads and data

The runtime calls each hook on one thread (docs/APPS.md). Melodee keeps
one owner per piece of state:

- **MIDI thread** owns the project and the user-interface state. Pads,
  buttons, pots and the wheel change them there. Every change that affects
  sound becomes a small `Command` (note, parameter, mix, step, transport)
  pushed onto one single-producer queue to the audio thread. Only the MIDI
  thread pushes.
- **Audio thread** owns the engines and a mirror of the patterns. Each
  `render()` drains the queue, advances the sequencer clock, plays the
  engines and mixes the tracks. It never allocates, locks or waits. It
  publishes the playhead, track meters and output peak as atomics.
- **Screen thread** takes a `Snapshot` every frame: a short lock shared
  with the MIDI thread copies the UI state, the track list and the one
  pattern in view, then drawing runs without the lock. The audio thread
  never takes that lock.
- **Service thread** watches the power key (short press opens the Exit
  sheet) and will do project file work on the SD card.

Wheel Push belongs to Melodee (it opens and confirms), so the app claims
it through `App::claims_wheel_push()`; the Exit sheet's LEAVE returns to the
stock app through `App::finished()`. Crash, remote stop and the recovery
service are unchanged.

## Milestones

1. Plan (this file).
2. Design system and host previews of pages at 320 × 240.
3. Shell: router, control grammar, pickup, lights.
4. Project model with the real limits, and the engine host.
5. First playable slice: Stage, Keys, Sounds and Edit on one synth engine;
   then Mix; then Steps with transport (Play, Stop, Seq Record, Tap Tempo).
6. Then, in the handoff's board order: Roll and Motion, sampler and chop,
   Patterns / Song / Length, effects, punch, projects on the SD card.

## Status

Milestones 1–5 exist, and the sequence group is complete: the app starts
into Stage, and Stage, Keys, Mix, Sounds, Edit, Steps, Roll (with its note
sheet) and Motion work with the Shift layer and the Exit sheet. Seq opens
Patterns (the MPC's Sequence mode); Shift + Seq is Step Edit and, pressed
again, steps through Steps, Roll and Motion and back to Step Edit; on
Steps, Roll and Motion B1–B3 pick those three.

- **USB-C stems:** a computer recording from the MPC gets the main mix on
  inputs 1–2 and each track on its own mono input, track 1 on 3 through
  track 16 on 18. A stem is the track after its chain and fader as you hear
  it (muted and soloed-out tracks are silent), summed to mono with its pan
  law taken back out so its level does not follow the pan, before the
  buses, the master chain and main volume. Sends and returns are only in
  the main mix. See HARDWARE.md, "Stems".
- **Roll:** one bar of twelve semitones, the view an octave and a bit above
  the pads' lowest note; the wheel moves the shared step cursor, pads write
  up to four notes on it (or play in live with Seq Record), Minus / Plus move
  the octave and the view. Push on a note opens the note sheet: accent,
  slide, tie, copy half a bar on, delete.
- **Edit, page map and mod matrix:** the engine's pages are followed by
  the mod matrix, which every synth hears (PROPHET: page 12 of 12). B2
  opens the page map: the wheel moves, Push jumps there, Shift + Push or B2
  closes it, and the pots stay on the page underneath. The matrix has
  eight routes a track (`mod_routes`, open question 1), all on one screen;
  the wheel picks the route, Push on an empty row starts one (LFO into
  cutoff, amount 0), K1 source, K2 destination, K3 amount (−100..100,
  centre is none). Sources: LFO, velocity, aftertouch (pad pressure),
  wheel (the fader), key track, random, filter envelope (FM6: operator 1's,
  CZ: the DCW's). Destinations:
  cutoff, resonance, amp level, pitch, oscillator A or B pitch, pulse width,
  LFO rate, noise; each engine decides how far a destination moves and
  names them its own way, and K2 skips the ones it has none of (a route
  kept from another engine is drawn dim and does nothing). FM6: Brightness
  (every modulator, 2 octaves of index at full), Feedback, Amp level, Pitch,
  Carrier pitch, Mod pitch, LFO rate. SID: Cutoff, Resonance, Amp level,
  Pitch, Osc pitch, Osc 2 pitch (the ring / sync partner), Pulse width, LFO
  rate. CZ: DCW, Amp level, Pitch, Line 1 pitch, Line 2 pitch, LFO rate.
  `engine/expression.h` sums the routes for all four.
- **Pad expression (parity phase 1d):** velocity is heard on every synth at
  its defaults through one squared curve (`velocity_gain`: at depth 70 a
  hit at 40 plays about 9 dB under a full one): PROPHET VEL > AMP 65 and
  VEL > FLT 40, FM6 VELOCITY 70 per operator (modulators too, so soft hits
  are darker), SID VELOCITY 70 (new), CZ VELOCITY 70. Pad pressure does
  what aftertouch does on synths: **AT > VIB** adds vibrato (100: a
  semitone each way at full pressure; 35 by default) and **AT > BRIGHT**
  opens the sound (100: 4 octaves of cutoff on PROPHET and SID, 0.6 of DCW
  on CZ, 2 octaves of modulator index on FM6; 50 by default). Each voice
  follows its key's pressure over about 10 ms. The vibrato runs at VIB RATE
  on PROPHET (its own sine, apart from the LFO) and LFO RATE on the others
  (their LFO, also the matrix's), about 5.3 Hz. A route from Aftertouch
  still adds to both. Every synth has a **Touch** page before the mod
  matrix with these (PROPHET and SID also show velocity there). Pressure
  goes to every note a pad started (all of a chord) and, with the arp on,
  to the arp's notes from that pad's keys, octaves up too, while the pad is
  held (a latched arp plays on without it). Settings' **Pad pressure Off**
  stops the pads sending pressure at all (synths and Pad Punch, which then
  uses its depth); AT > VIB and AT > BRIGHT at 0 turn it off for one sound.
  New parameters sit at the end of each engine's list; project files carry
  a `PVER` section and sound files a last byte (`params_version`), and
  older ones load the new parameters at their initial values
  (`upgrade_params`) instead of zeros.
- **Clock and patterns (parity phase 2):** 96 ticks a quarter, as the
  MPC60 counted. A pattern is a list of note events (tick, length in ticks,
  key, velocity, chance, accent / slide / tie), up to 128 bars of the
  project's time signature (`model/pattern.h`); a note sits on any tick.
  Each pattern has a **Q**: 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T,
  1/64. Q is only the grid: Steps and Roll show the pattern in steps of it
  (a note played off the grid is drawn where it fell in its step and keeps
  that place when a step is edited), Rec Quantize and Time Correct snap to
  it; changing Q moves no note. Swing (project-wide, 50–75 %) stretches the
  first sixteenth of every eighth as it plays. The **time signature** is
  the project's (Shift + K2 on Patterns): 1/4 to 9/4, 3/8 to 15/8, 5/16 to
  15/16; a pattern of whole bars keeps its bar count when it changes. The
  audio thread plays copies of the patterns: each edit makes a copy on the
  MIDI thread (`Command::PatternData`), the audio thread swaps it in by
  pointer and hands the old one back to be freed there, so it never
  allocates. Older project files (24 ticks a quarter, a fixed array of
  steps, `PATT`, motion by the pattern's own steps in `MOTN`) load into
  events and sixteenths; new files write `EVNT`, `MOT2` and `TIME`.
- **Patterns** (Seq): tracks down, sixteen patterns across, eight tracks
  in view following the selected one. K1 the selected pattern's length, 1,
  2, 4, 8, 16, 32, 64 or 128 bars (Shift + K1: any count from 1 to 128); K2
  its Q (Shift + K2: the time signature); K3 swing (project-wide; Shift +
  K3 the metronome's level). The **wheel moves the playhead** by a beat
  (Shift + wheel: by Q); stopped, it sets where Shift + Play carries on
  from. The header shows where it is (bar.beat.tick), the time signature
  and Rec Quantize (REC Q 1/16, or REC Q OFF). **Hold B1** for the BPM
  window: the wheel sets the BPM, Push switches whole BPM / tenths. **B2**
  switches **BPM per pattern (SEQ) or global (GBL)**: with SEQ on, each
  pattern number has a tempo of its own, and the one launched last (on any
  track, or the column) leads; until a pattern's tempo is set it plays at
  the global one (a dot over a column's number marks one that is set).
  Tap Tempo and the Tempo page set the leading pattern's tempo then. Song is
  Shift + Pad 12 (B2 was a second way in until phase 2 gave it the BPM, as
  on the MPC). Pads are the track's patterns: a tap queues (it starts on
  the next bar, or at once when stopped), Erase + pad clears, hold one and
  tap another to copy. B3 COLUMN latches column launch (the header says
  LAUNCH COLUMN): a tap then launches that column on every track (open
  question 8). Minus / Plus move the track. Shift + Erase copies too: the
  pattern to copy, then where it goes, on this track or (Minus / Plus moved
  on) another; Shift + Erase again or Erase leaves.
- **Seq Record** (as on the MPC): pressed while stopped it arms (its light
  flashes, the header says REC ARMED) and Play starts the take; pressed
  while playing it records at once; pressed while recording it stops
  recording and playback goes on; Stop ends both. **Count-In** (Shift + Pad
  4) plays a bar of the metronome first (the header counts the beats, Seq
  Record flashes); nothing is recorded meanwhile. Notes keep the time they
  were played, to the tick, and their held length; with **Rec Quantize**
  (Shift + Pad 10, on by default) they land on the nearest step of Q, and a
  note moved ahead of the playhead is not played again on that pass (it was
  heard as it was played). Overdub: what the pattern holds stays.
- **Time Correct** (Shift + Pad 14): the selected track's pattern onto its
  Q. The pads choose whose notes move (kit pads of the bank in view, the
  909's drums, or the notes a synth's pads play; lit on the page), B2 all
  of them; K1 Q, K2 shift (−48 to +48 ticks), K3 swing (every second step of
  a straight Q late by it). B3 DO IT, one undo; B1 CANCEL goes back.
- **Half Seq, Double Seq, Half / Double Speed** (Shift + Pad 2, 3, 6, 7):
  the selected pattern half as long (what lies past the end goes), twice
  as long with its notes and motion twice, its notes twice as far apart or
  half (the length stays; what falls past the end goes). Each is one undo.
- **Song** (Shift + Pad 12, or B2 on Patterns): lanes per track, clips of patterns with their notes in
  small and notches at each repeat, markers, loop brace, cursor, playhead
  and an overview strip; K1 zoom (4–64 bars; at 4 the header says SNAP
  1 BAR), K2 pattern and K3 length for the next clip, or of the clip under
  the cursor. The wheel moves the cursor by the bar, pads pick the lane,
  Push on empty time places a clip, Push on a clip opens the clip sheet
  (split at the cursor, duplicate after, repeat to the next marker, loop
  these bars, make its own pattern, show motion lane, mute, delete; B1 back,
  B3 or Push does it). Minus / Plus jump between markers, or between clip
  edges while there are none. **Play on the Song page plays the arrangement
  from the cursor; Play anywhere else plays the patterns** (assumed, part of
  open question 4). The song stops at its end unless a loop is set.
  **Shift + Push on Song adds a marker at the cursor bar**, named after the
  one before it (INTRO, VERSE, CHORUS, BRIDGE, DROP, BREAK, OUTRO); Shift +
  Push on a marker opens the Name sheet to type its name (K1 there steps
  the ready names). This is the one place Shift + Push is not Back
  (Kerem's choice); the Shift layer says so on Song.
- **Effects along the signal:** each track runs its chain (6 slots), then
  its one send goes into the bus it picks (A–D, 6 slots each, with a
  return), and everything meets in the master chain (6 slots) before main
  volume. Slot counts and the four buses are constants in
  `model/limits.h` (open question 5). The effects are written here, all 28
  of the picker's: Room, Reverb, Plate, Shimmer (the reverb with an
  octave-up shifter in its feedback); Echo, Tape echo, Ping pong, Looper
  (up to 1.5 s, RECORD overdubs, KEEP fades the old layers); Chorus,
  Flanger, Phaser, Pedal chorus (one dark, slightly driven line read twice),
  Pedal phaser (four stages, COLOUR adds feedback), Auto-pan, Tremolo;
  Drive, Tape, Crush, Spit (now and then a few milliseconds held, crushed
  and jumping in level), Fuzz; EQ, Filter, Formant (three band passes
  morphing through A E I O U, SHIFT moves them), Width; Compressor, Gate,
  Transient, Limiter. Delay memory (reverbs, echoes, chorus, flanger, tape) comes from a
  pool of 48 blocks of 1.5 s made at start, so the audio thread never
  allocates; past 48 such effects, a new one passes sound through.
- **Effects pages** (KNOB FX steps Track → Sends → Bus → Master; B1/B3 walk
  along too): four device cards in view with a picture of each effect, the
  device in view with its row (Minus / Plus) and how full the chain is. The
  wheel picks a device; Push on it opens Change, Bypass, Move left / right,
  Remove; Push on + ADD opens the picker (families left, effects right; Push
  enters a family, B3 adds, B1 backs out). Sends shows the four buses and
  sixteen send strips: K1 send, K2 bus, K3 that bus's return. Bus and Master
  are the same page in neutral colours. With Seq Record on, pot moves on the
  track's own effects land in Motion like any other parameter (the lanes
  name them by their effect's labels); adding, changing, moving or
  removing one of the track's effects clears that motion, since its slots
  would point elsewhere.
- **Sampler** (an engine like the others; Stage's track 5 starts as an
  empty kit): 128 pads in eight banks, A–H, each a region of one of the project's
  samples (64 a project), as deep as the MPC's Sample Mode (parity phase
  4): volume in dB (−inf, −74 to +6; the kit's volume on the same scale),
  pitch, fine, pan, Note On (gate) or one shot, loop with a loop point and
  Loop Lock, reverse, Mono or Poly, a mute group 1–16, Pad Link (a pad of
  the same bank played with it), offset (the start waits up to a
  sixteenth), velocity sensitivity, an envelope that decays from the start
  or (the MPC's default) over the decay before the end, its own filter
  (Off, Classic, LPF2 / LPF4, HPF2 / HPF4, BPF2 / BPF4) with a filter
  envelope, Warp (50–200 %, or Seq: the project's tempo from # Beats) and
  a Normalize gain; one filter on the whole kit after every pad (K1–K3 on
  Stage: TUNE, FILTER, DECAY). Their values go in the project file's
  `PAD4` section; files from before it load with the MPC's defaults, their
  pads decaying from the start as they did. Pad Bank tapped steps the bank
  round A–H (Shift: back; Minus / Plus on Pads too), held with Minus / Plus
  or the wheel it jumps; the eight bank lights show it, the header names it
  for a moment. On a track without banks a tap says that Pad Bank + pad
  picks a track. Kit notes are 36 + pad, so banks F–H run past MIDI's 127
  (to 163); they stay inside Melodee. Pads of banks E–H go in the project
  file's `KITX` section, so older files load as they were. Steps writes the
  pad hit last. **Loop** turns the last pad's loop on or off (on, Loop Lock
  on with it) and is lit while that pad loops; Shift + Loop flips its
  reverse. A one shot that loops plays until it is hit again; Note On loops
  while held. **Shift + Chop** is Note On: the pad plays while held
  (Gate), or One shot; the sampler pages' title shows a note, a loop and
  an arrow for Note On, loop and reverse. **Hold Sample + pad** picks a kit
  pad as the last pad without playing it (Sample alone acts on its
  release). Samples are WAV
  files (16, 24 or 32-bit PCM or 32-bit float, any rate, up to 60 s) on the
  card (from its `Samples` folder, `$MELODEE_SAMPLES` on a host) or on the
  internal drive, read only (the factory library's categories in
  `/content/Samples` and the stock app's own samples in
  `/data/UserContent/Samples`; `$MELODEE_INTERNAL`, folders split by `:`, on
  a host). Melodee writes only to the card. The service thread reads a file, makes
  its waveform and finds its hits, and hands the audio to the audio thread
  through a lock-free queue; the sample it replaces comes back the same way
  to be freed, so the audio thread never allocates or frees.
- **Pads** (B2 on a sampler track, or 16 Levels): the bank as the pads sit
  (muted pads dim, marked MUTE), the last pad hit with its length, play
  mode and waveform, the banks A–H; K1 pitch, K2 level, K3 pan. Push opens
  Trim.
- **Delete and copy pads** (on every page where the pads play the kit,
  not on Levels): Erase + pad, stopped, empties that pad, its sound and its
  own effects; while playing or with Seq Record on it takes that pad's notes
  out of the pattern instead (one undo; inside a take, part of the take).
  Shift + Erase is Copy, as printed (Erase lit, the header says so): the
  first pad is the one copied, the next one where it goes (Pad Bank in
  between reaches the other banks); Shift + Erase again or Erase leaves.
  On Patterns it copies patterns; with Pad Bank held it copies a **track**:
  the pads are the sixteen tracks, the first is the one copied, the next
  where it goes. The copy takes the sound (engine, sound, mod routes,
  effects, keys, arp and chord), the kit (pads, their chains and mutes, the
  bank) and all sixteen patterns with their motion; the target keeps its
  own mix strip. One undo. The audio thread takes the track whole
  (`TrackLoad`), so the song plays on. Elsewhere the header says what Copy
  copies. Clearing a pattern is Erase + pad on Patterns.
- **Mute** (as printed): a tap latches mute mode, a hold lasts while Mute
  is held, Shift + Mute unmutes everything (tracks, solos, every kit pad).
  In mute mode the pads mute instead of playing: on a page where they play
  a kit (Pads, Edit, Keys, the sampler pages, ...) they mute **kit pads** of
  the bank in view; on Stage, Mix and every other page they mute
  **tracks**. Shift + pad solos: a track, or a kit pad (every other used pad
  of the kit muted; again on the soloed pad brings the kit back). Mute is
  bright and the header says "Mute: pads" or "Mute: tracks" while it is on;
  the pads light muted red, soloed white, the rest in the colour. A muted
  kit pad is not heard, played or sequenced (its notes stay) and what it
  was playing fades out; mutes are saved with the project (`PMUT`).
- **Undo and redo** (Shift + Minus, Shift + Plus, on every page), one
  history for every edit it keeps, the last 32: pad deletes and copies; a
  Seq Record take as a whole (its notes, the clips and punches it wrote
  into the song, its pot moves on Motion, and every edit of the take's
  pattern meanwhile, Erase + pad among them; Shift + Minus while recording
  ends the take and takes it back); Erase + pad on Steps and Patterns, a
  kit pad's notes erased, pattern copies, Double Seq and Recall (Capture).
  Time Correct, Half Seq, Half / Double Speed. The header names what came back. A new
  project or a load forgets them. Pot moves outside a take and Song page
  edits are not kept.
- **Samples** (Sample Select on a sampler track): the MPC's browser. Folders
  first, as categories, then the WAV files, each by name; the wheel and
  Minus / Plus move, Push opens a folder or loads the file onto the last pad
  hit, B1 goes back out (the cursor lands on the folder it came from), B2
  picks the drive (CARD, starting in `Samples`, up to the card's top;
  INTERNAL, the factory categories and the stock app's User folder side by
  side), B3 PREVIEW plays each file as the cursor lands on it (the first 30
  s, read into a slot of its own the engine keeps apart from the project;
  it stops when the cursor moves or the page closes). K2 jumps. A load onto
  a pad that holds a sample keeps the pad's settings (volume, pan, tune,
  envelopes, filter, warp, play mode ...; its region, loop point, Normalize
  and # Beats start again, as they belonged to the old file); onto an empty
  pad it starts at the defaults; one undo. **Shift + pad** here loads the
  file onto the kit's next empty pad (after the last pad hit), instead of
  the printed Shift + pad jobs. A file a pad of this kit plays shows ON A01.
- **Trim** (Push on a sampler track, or on Pads): the last pad's region over
  the sample; K1 start, K2 end, K3 the loop point (turning it lets go of
  Loop Lock), Shift + K1–K3 zoom in at that handle (to 16 times closer),
  Shift + B1 Loop Lock (the loop point on the start), Shift + B2 Normalize
  (the region's loudest sample to 0 dB as a gain on the pad, the file
  untouched; one undo).
- **Trim Sample** (Shift + Pad 13, as printed): the last pad's region
  becomes a file of its own, `Samples/Edits/<NAME>-Trimmed.wav` (" 2", " 3"
  when the name is taken), in a new sample slot; the pad then plays all of
  it, its loop point kept where it was in the sound. One undo puts the pad
  back on the old file and region (the old file is untouched and stays in
  the project). Without a card the new sample lives in memory for the
  session (the header says so).
- **Save Sample** (Shift + Sample Select on a kit, as printed): the Name
  sheet with the pad's sample name, then the last pad's region as
  `Samples/<name>.wav` (24-bit, 44.1 kHz; " 2" when taken). The pad is left
  as it is; the header says "Saved NAME".
- **The sampler's ten pages** (B1 / B3 go round them, PAGE n / 10), the
  MPC's Sample Mode pages first: Trim; Mix (K1 volume in dB with its meter,
  K3 pan, Shift + K2 the kit's volume); Amp Env (attack, decay on a one
  shot or release on Note On, velocity sensitivity; Shift + K2 decay from
  the start or the end; row 2 sustain and the other); Tune (semitones,
  cents, Warp Off / 50–200 % / Seq; Shift + K3 # Beats, Auto works it out
  from the length; Shift + Pad 15 Warp picks Time stretch or Pitch, which is
  the kit's Follow how: Pitch is Tape, Time stretch goes back to Beats or
  Tones); Play (Mono / Poly, mute group 1–16 with its pads shown, offset;
  Shift + K2 Pad Link; row 2 Note On, reverse, loop); Filter (the pad's
  own: cutoff, resonance, type); Filt Env (attack, decay or release, depth
  up to seven octaves; Shift + K2 decay from; row 2 sustain and the other).
  Then Melodee's: Kit filter (cutoff, resonance, low / band / high pass, the
  whole kit); Follow (the whole kit keeps the project's tempo, each pad's own
  tempo worked out from its length as whole bars; K2 how: Beats plays a
  sixteenth-long segment from where the sound should be, so hits stay
  whole, Tones overlaps 60 ms windows for held sounds, Tape moves speed and
  pitch together; K3 what a pitch change does: Speed, or Stretch to keep
  the length; written here as a two-tap granular stretcher in the voice); Cloud (pads as clouds of grains while held: position,
  grain size 10 ms to 1 s, density, then scan, spread and on / off).
- **Chop** (Chop on a sampler track, as the MPC's Chop Mode; Chop again
  goes back to Trim): the last pad's region, between its start and end, cut
  into slices that touch (one ends where the next starts). Pads play their
  slice and pick it (the wheel and Minus / Plus pick too, past 16). K1 the
  slice's start, K2 its end (the slice before's end or the next one's start
  move with them; never under 20 ms), K3 the chop type: Threshold (hits at
  least as strong as Shift + K3's threshold; as on the MPC, the higher, the
  fewer slices), Regions (Shift + K3 slices,
  2–64; the MPC's 4 / 8 / 16 among them), Beats (Melodee's: Shift + K3
  beats a slice at the project's tempo) or Manual. Shift + K1 / K2 zoom in
  at the slice's start or end (to 16 times closer; the pots then turn
  across that window). Manual, armed (K3 onto it, or Push): "PAD 1 PLAYS,
  TAP TO CUT"; pad 1 plays the region and every pad tapped while it plays
  cuts it there, up to 16 slices, each cut pulled onto the strongest hit
  from 50 ms before the tap to 10 ms after. Editing a slice (K1, K2, Split,
  Merge, Erase + pad) makes the chop Manual, as on the MPC. **Shift + B1
  Extract**: the slice as a file, `Samples/Edits/<NAME>-Slice n.wav`, onto
  the kit's next empty pad after the chopped one (one undo; the source is
  untouched). **Shift + B2 Split**: the slice in two halves. **Shift + B3
  Merge**: the slice into the one before. Erase + pad takes a slice out (it
  joins the one before). Slice edits are not undo steps (as on the MPC). B3
  TO PADS lays every slice on pads from the first free one (regions of the
  same file), B1 DROP goes back. The slices stay while the same region
  comes back to Chop.
- **Levels** (16 Levels): on a kit the last pad hit on all sixteen pads,
  on a synth its last note (16 Levels lit while they play levels). The
  type, as the MPC's (Shift + 16 Levels goes round them; B1–B3 pick):
  Velocity (8 to 127); Filter (kit pads) the pad's cutoff from the bottom
  up to its own over pads 1–8 and on to the top over 9–16 (Classic when the
  pad has no filter); Tune by semitones with the original on pad 4; and
  Melodee's Scale, up the project's scale (B3 again on a kit, B2 on a
  synth). The pots stay on that pad. 16 Levels again goes back to the kit
  (Pads) or Keys; until then the pads keep playing the levels on every
  page. Seq Record keeps the velocity of a levels pad, not its pitch or
  cutoff yet.
- **Input Config** (Shift + Sample, as printed; B2 on Record): B1–B3 pick
  where Record takes its sound from: the mic, the line inputs or the mix
  (resample, before main volume); K1 the threshold a take waits for (−60
  dB: at once). The input meter moves while the page is open. Monitor,
  record length and input effects come in phase 6.
- **Record** (Sample Record on a sampler track; elsewhere the header says
  it wants a kit): from Input Config's source, shown in the header. Sample
  Record or Push arms; the take starts when the level passes Input Config's
  threshold or on a second press, and ends after K2's bars (0: until
  stopped) or on Stop or a third press. K3 gain. The take is recorded into a 60 s buffer made
  at start; the service thread writes it to `Samples/Recorded/TAKE n.wav`
  (24-bit) on the card and it lands on the first empty pad. The input is
  routed only while Record or Input Config is open or a take runs. The
  browser shows them in `Samples/Recorded`.
- **Projects** (Shift + Pad 16): files (`.mldp`) in `Melodee/Projects` on
  the card, in folders (`$MELODEE_PROJECTS` on a host). The format is
  tagged sections, so older files stay readable and only what differs from
  empty is written. The page lists New project, the open project while it
  has no file, folders (Push goes in, Shift + Push comes out) and projects
  with their tracks and tempo; K2 jumps, K3 sorts (newest, name). Push on a
  project opens its sheet: Save, Save as new, Duplicate, New folder here,
  Open at start, Delete (asks once more); on another project Load heads it.
  Save (the first time), Save as new and Rename open the Name sheet, its
  first name a free "Project n" (or the project's own); a typed name a
  project in the folder has already gets " 2", " 3"; Rename onto one is
  refused with a message. Move to folder opens a picker of every folder
  under Projects (four deep) and the top, all but the one the project is
  in: the wheel picks, Push or B3 moves it there (" 2" when that folder has
  the name already), B1 backs out; the open project follows its file, and
  so does Open at start. Copies are "<name> copy", folders Folder n. The service thread does every file operation (writing beside
  the file, then renaming it into place); a load swaps the model on the MIDI
  thread and gives the audio thread a whole copy to switch to at once,
  then the project's samples load again. Nothing is created on the card
  until something is saved. LEAVE on the Exit sheet saves first (a new file
  if the project has none). Open at start is kept in `Melodee/settings`.
- **Tempo** (hold Tap Tempo half a second; tapping works anywhere): K1
  tempo, K2 swing, K3 click level; B1 halves, B3 doubles, B2 (or Shift +
  Tap Tempo anywhere) turns the metronome Off → On → Rec (Rec: only while
  Seq Record is on), a short blip on each beat while playing, higher on
  the bar. The header says which for a moment. The wheel moves the tempo by
  0.1, Minus / Plus by a whole BPM. With BPM per pattern on (B2 on
  Patterns) the tempo here, tapped or turned, is the leading pattern's.
  The beat lights follow the time signature.
- **Note Repeat** stays on the page, as on the MPC: a tap latches it (a
  tap again turns it off), a hold lasts while held, a double tap opens the
  Arp page. While it is on, B1–B3 are its rate, 1/8, 1/16, 1/32, and with
  Shift held their triplets (the soft keys say which, the one in use
  marked; on Arp and Chord their own keys stay). Every second note of a
  straight rate comes late by the project's swing, as the sequencer's off
  sixteenths do; triplets stay even. The repeats line up with the bar from
  Play. Note Repeat is lit while on. It is the track's arp: a kit repeats
  the held pad.
- **Arp** (double tap on Note Repeat): Up, Down, Up-down, Random, Order;
  K2 rate 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, K3 octaves, Minus
  / Plus the gate. B3 Hold latches the keys. It runs on its own clock at the
  tempo, so it plays with the song stopped too.
- **Chord** (B1 on Arp): one pad, one chord from the scale's own degrees:
  Triad, Dia 7, Sus2, Sus4, Power, Dia 9; voicing Close, Open, Drop 2,
  Spread; inversions. B1 there switches chords on or off. Chords play
  through the arp too, and record into Steps and Roll as up to four notes.
- **Recall** (Shift + Seq Record, the Capture page): what the pads played on the track
  lately (a history of the last 512 presses), the last 1–4 bars quantised
  to 1/16 or 1/8 and laid into a pattern (the first empty one); B3 or Push
  keeps it and that pattern plays, B1 drops.
- **Step Edit** (Shift + Seq, parity phase 3, as Akai's step_edit page
  has it): the selected track's pattern one step of Q at a time and the
  events that start in it, listed (pad or note, ticks after the step, length
  in ticks, velocity). The **wheel** moves a step of Q (the step's first
  event chosen), **Minus / Plus** go through the step's events, a **pad**
  plays as anywhere (and records with Seq Record) and chooses its event on
  the step. **K1** the pattern's length (1, 2, 4 .. 128 bars, as on
  Patterns), **K2** Q, **K3** the event's velocity; **Shift + K1** the
  event's length in steps of Q (1–64), **Shift + K2** the time signature.
  The **fader** nudges the event's timing: a tick for each step the fader
  moves, up later, down earlier, inside the pattern (the cursor follows it
  into the next step); this is its context override until the Fader page
  (phase 8). **B3** erases the event; **Erase + pad** asks first (B1
  Cancel, B3 Do it) and then erases every event of that pad in the pattern.
  Both erases are one undo, part of the take while Seq Record is on. Hold
  **B1** for the BPM (the wheel; Push whole / tenths), as on Patterns. The
  pads light where the step has an event of theirs, white for the chosen
  one. The cursor is the same as Steps' and Roll's.
- **Motion:** Seq Record + a pot on any page writes an event at the playing
  sixteenth of the pattern, whatever its Q (256 for the project,
  `model/limits.h`). The audio thread plays them sixteenth by sixteenth, each lane starting the pattern at the track's own value, and
  gives the track its own values back on Stop. On a kit, a pad's own values
  are lanes of that pad (parity phase 3, the MPC's pad automation): level,
  tune, fine, pan and the amp envelope (attack, decay, sustain, release),
  turned on Pads, the sampler pages or Motion with Seq Record on; a hit
  hears the value its lane holds when it starts. Velocity sensitivity,
  offset and the per-pad filter envelope have their places
  (`model/pad_params.h`) and are recorded once phase 4 gives pads those
  values. Erase + pot on Motion asks first (B1 Cancel, B3 Do it), then
  clears that lane, one undo. The lanes are the parameters holding motion
  (on a kit, the last pad hit's), then on a kit that pad's tune, level and
  pan, else the favourites. Pad lanes are saved in their own `MOTP`
  section, so older files and builds see only the rest.

- **Engine:** PROPHET, a two-oscillator polysynth written here (saw and
  pulse with hard sync, saw/triangle/pulse B with low-frequency mode,
  noise, a zero-delay four-pole ladder, two ADSRs, LFO, wheel on the fader,
  poly mod, glide, unison, drift, pressure vibrato and brightness), eight
  voices a track, eleven Edit pages and thirteen factory sounds made for it.
- **FM6:** six sine operators (from a table) with ratio, fine, level, an
  ADSR and velocity each; eight algorithms (stack, two stacks, three and
  three, three pairs, stack and two, one to five, pair and four, all heard)
  with feedback on operator 6, an LFO for pressure vibrato and the matrix;
  eight voices, eight Edit pages, nine sounds.
- **SID:** a chip voice: triangle, saw, pulse or noise, pulse width with its
  own LFO, ring and sync against a second oscillator, crush, a multimode
  filter with envelope and key tracking, the 8580 or the grittier 6581,
  velocity, an LFO for pressure vibrato and the matrix; six voices, five
  pages, nine sounds.
- **CZ:** phase distortion: eight waves bent by the DCW and its envelope (a
  pure sine at no bend), a second wave alternating each cycle, lines 1, 2,
  1+1' and 1+2' with detune and octave, key follow, DCA envelope, an LFO
  for pressure vibrato and the matrix; eight voices, five pages, eight
  sounds.
  Sounds' K1 steps through all six engines.
- **909:** a drum kit written here in the shape of the classic machine:
  kick and toms as sines with a falling pitch, snare and clap as tone and
  filtered noise, rim from two resonators, hats and cymbals from six
  detuned square waves through high passes; the closed hat chokes the open
  one. Pads 1–11 play kick, snare, clap, rim, three toms, closed and open
  hat, crash and ride; pads 12–16 stay dark, and a hit says the drums are on
  1–11. Six Edit pages (kick, snare and clap, toms, hats,
  cymbals, levels with drive), five kits. Stage's track 6 starts as a 909
  with a beat.
- **Seq Record into the song:** with Seq Record on while the song plays
  (Play on Song), a pattern launched on Patterns is written as a clip from
  the next bar and grows a bar at a time (a new clip after 16 bars) while
  it keeps playing; the song plays on past its end meanwhile. Turning Seq
  Record off or stopping ends each clip with the bar that was playing.
  Punches held or locked meanwhile land on the song's punch lane (under the
  lanes, in the target's colour) with their sixteenth and length, and play
  back there; a punch held by hand wins over a written one.
- **Pad effects (B1 from Track, sampler tracks):** each of a kit's 128
  pads has its own chain of four effects, ahead of the track's; the page
  shows the last pad hit, with PAD first on the signal strip. The sampler
  plays a pad with a chain on a lane of its own (eight at once; a lane
  stays three seconds after the pad falls silent, for tails). Cloud pads
  are left out, their grains have no pad. Saved with the project.
- **Punch (Pad FX):** 27 punches written here, sixteen to a bank on the
  pads (Pad Bank or Minus / Plus for the other bank): loops of 1/4 to 1/32
  of a bar, stutter, reverse, tape stop, half speed, downer, upper,
  scratch, low, high, phone, sweeps down and up, crush, alias, gate,
  tremolo, pan, echo, ring, flange, drive, vinyl. B1 aims them at the
  master, B2 at the selected track. Hold a pad to punch, the last pressed
  wins; Push while holding one locks it on, Push again lets it go (it was
  Shift + pad until Shift + pad became the printed jobs). While playing a punch waits for
  the next step of its snap grid. Repeats let one slice through, then
  freeze the punch's two-second history and play it again. K1 depth, K2
  snap, K3 tail (cut, fade, a long ring). Pad lights show the bank, held
  and locked bright.
- **Pad Punch (Pad FX again, or B3, on a sampler track):** each kit pad
  carries a punch of its own, its rate and depth; the pad plays punched by
  itself while the kit runs on (the sampler renders that pad's voices
  apart). Pressure is depth. K1 punch of the last pad hit, K2 rate, K3
  depth. Punch settings and the pads' punches are saved with the project.
  Written into the Song with Seq Record on (see below).
- **Name sheet:** typing a name, over any page. Thirteen pads hold two
  letters each like phone keys (a second tap within 0.6 s gives the
  other), pad 14 a space, pad 15 digits and `- _ .`, pad 16 deletes; the
  wheel moves the cursor, Shift + wheel or K2 turns the character under it
  through the whole set, Erase deletes (Shift + Erase clears), Minus / Plus
  move too, K1 fills in a ready name. B1 cancels, B2 Aa, B3 or Push keeps
  it; an empty name changes nothing. Markers are capitals (11), projects
  31, sounds 23; characters a card refuses in a file name are never
  offered. Play and Stop still work under it.
- **Sounds of one's own:** Shift + Sample Select away from Sounds on a
  synth or drum track is Save sound (on a kit it is Save Sample, above): the Name sheet with the sound's name, then the engine's values
  and mod routes go to `Melodee/Sounds/<name>.mlds` (a few hundred bytes;
  the same name replaces it). They follow the factory sounds in Sounds,
  numbered on, for their engine, and load, audition, star and show under
  RECENT like them.
- **Favourites:** on Sounds, Shift + Sample Select stars the sound under
  the cursor; again unstars it. FAVOURITES lists the starred ones, with a
  star on each in ALL. Kept by engine in the settings file.
- **Settings (B2 on Project):** Melodee's own, kept in the `settings` file
  next to the projects (with the project to open at start): palette (NIGHT,
  the one there is), pad response (soft, medium, hard, full level: every hit
  at 127; Shift + Pad 1 turns full level on and back), pad pressure (on;
  off: the pads send no pressure), pad
  lights (track colours, white, off), knob pickup (off: values jump to the
  pot), screen brightness (20–100 %; the backlight is left as the stock
  app had it until this asks for less, and the runtime puts it back on
  leaving). Wheel moves, Push opens a value, wheel changes it, Push keeps
  it; Minus / Plus change it at once. Nothing is written until a value
  changes. **About (B3):** engines, effects, punches, limits, JUCE version,
  font and licence. Audio out and MIDI in from the board's first guess are
  left out: Melodee has no MIDI input path yet and the outputs follow the
  jacks: the built-in speaker plays while neither headphones nor line out
  is plugged in.
- **Host:** sixteen track slots, level / pan / mute / solo, the patterns'
  copies, a sample-accurate tick clock (96 a quarter) with swing, the time
  signature, count-in, locate and BPM per pattern; chance, accent and tie, meters
  and a soft-clipped master after main volume. The send is stored, not heard
  (no buses yet).
- **Shell:** printed buttons (Sample cycles Stage → Keys → Mix, Seq
  Patterns, Loop the last pad's loop on a kit, Chop, Sample held + pad
  picks a kit pad,
  Sample Select, 16 Levels, Play, Stop, Seq Record, Tap Tempo, Minus /
  Plus, Erase), soft keys, wheel and Push, Minus / Plus (the octave on the
  play pages Stage, Keys, Edit, Chord and Roll; on the lists Sounds,
  Samples, Project, Patterns, Mix and Sends they move the list as the wheel
  does; Settings and Tempo change the value, Song jumps markers, Steps
  nudges, the sampler pages and Punch step the bank), Shift + Push back along a page
  stack (16 deep; turning through a group such as Steps / Roll / Motion or
  Stage / Keys / Mix is one stop, and opening a page already on the stack
  goes back to it), Shift + wheel fine edit,
  Erase + pot to the initial value, Shift + Erase copies, Shift +
  Stop all notes off, Pad Bank held (pad picks a track, Minus / Plus or the
  wheel jump banks, the screen shows the sixteen tracks as the pads sit,
  bank pill for a kit), mute mode (above), pot pickup, the fader as mod
  wheel, pad / fader / button lights. Power (short) opens the Exit sheet; LEAVE returns to stock.
- **Button lights** (as on the stock MPC): bright when in use (the page's
  button, Play while playing, Seq Record recording, mute mode, Note Repeat
  on, Pad Bank held or a track copy, Erase while copying), faint when the button does something on the
  page in view, dark when it would do nothing: Chop needs a kit pad with a
  sample, Loop and Sample Record a kit track, Erase a pad, step, marker or
  pot it can clear (or Copy); on the Name sheet only Erase, Play and Stop
  glow, on the Exit sheet none (`shell/button_jobs.cpp`). While Shift is
  held, as Akai's guide has it, the buttons with a Shift job built here glow
  faintly (Tap Tempo bright with the metronome on, Erase while copying,
  Shift itself bright) and the rest go dark.
- **Shift layer:** drawn as on its board, every printed second job:
  Sample → Input Config, Seq → Step Edit, Pad Bank → previous bank, Erase →
  Copy (kit pads, patterns on Patterns, a track with Pad Bank held), Loop → reverse the last pad
  (kit tracks; bright while reversed), Chop → Note On (bright while on), 16
  Levels → its type, Sample Select → by engine (kit: Save Sample; synth:
  Save sound; on Sounds: star), Mute → unmute all (in mute mode, Shift + pad
  solos), Tap Tempo → metronome
  Off / On / Rec, Sample Record → Audio Recall (not yet, phase 6), Seq
  Record → Recall (Capture), Play → continue from the bar the song stopped
  in, Stop → all notes off, Minus / Plus → undo / redo on every page, Push
  → back. **Shift + Pad 1–16** are the jobs printed under the pads, on
  every page but the browser (where Shift + pad loads onto the next empty
  pad; `shell/shift_pad_jobs.cpp` holds the table, from Akai's
  guide): 1 Full Level (every hit at 127, again back to the pad response it
  came from), 2 Half Seq, 3 Double Seq (the pattern twice as long, notes
  and motion played twice), 4 Count-In and 10 Rec Quantize (toggles, kept
  in the settings file), 6 Half Speed, 7 Double Speed, 12 Song, 13 Trim
  Sample, 14 Time Correct, 15 Warp (Time stretch or Pitch for the kit), 16
  Project. The others (5 Compressor, 8 MIDI Config, 9 Fader, 11 Resample)
  say which phase brings them
  ("Compressor: phase 7"). The overlay draws the sixteen as the pads
  sit (pad 1 bottom left): a job on a surface card, a toggle that is on in
  the track's colour, the rest dim with their phase; the pads light the
  same way while Shift is held. **Shift + K1–K3:** a card with a second
  parameter carries the Shift icon (an up arrow); with Shift held the pot
  turns that one and the card shows it (filled icon), and the overlay's knob
  strip shows only those cards. Keys: Shift + K3 transposes the pads, −12
  to +12 semitones, saved with the track (not on a 909 track).
- **Remote:** `--ctl melodee-press [shift+]BUTTON` presses a button (lower
  case, spaces as dashes: `play`, `seq`, `wheel-push`, `shift+wheel-push`);
  `wheel+N` / `wheel-N` turn the wheel, `pad1`..`pad16` strike a pad
  (with a button held around it: `shift+pad5`, `erase+pad5`,
  `pad-bank+pad5`, `mute+pad5`: a held Mute, mute mode for that pad;
  struck and let go at once, so a recorded note is a tick long: `hold-pad5`
  ... `release-pad5` holds it),
  `k1=V`..`k3=V` move a pot to V (0..127), `fader=V` the fader, `shift+k3=V` with Shift held
  (the card's second parameter), and `hold-shift` ... `release-shift`
  keep any button (or `hold-pad5` a pad) down across other presses (the
  Shift overlay and its lights, a held punch).
  `status` lists the lit buttons as `melodee_buttons=seq:2,stop:1,...`
  (1 faint, 2 bright) and the lit pads as `melodee_pads=1,3,12`; for the
  sequencer `melodee_rec=` (0 off, 1 armed, 2 recording),
  `melodee_count_in=` (the beat, 0 none), `melodee_position=` (the selected
  track's tick in its pattern, −1 none), `melodee_pattern_ticks=`,
  `melodee_events=`, `melodee_q=` (ticks), `melodee_first_tick=` and
  `melodee_signature=`; Step Edit's `melodee_step=` (from 1),
  `melodee_event=n/count` (0 none) and `melodee_event_tick=`;
  `melodee_ask=` (0 none, 1 erase a pad's events, 2 erase a lane) and
  `melodee_motion=` (the project's motion events); the last pad's Sample
  Mode values `melodee_last_pad=` (1..128), `melodee_pad_loop=`,
  `melodee_pad_lock=`, `melodee_pad_note_on=`, `melodee_pad_reverse=`,
  `melodee_pad_mono=`, `melodee_pad_filter=`, `melodee_pad_link=`,
  `melodee_pad_decay_from=`, `melodee_pad_gain=` (hundredths of a dB) and
  16 Levels' `melodee_levels=` and `melodee_levels_kind=`; phase 5 adds
  Chop's `melodee_chop_type=`, `melodee_chop_slices=`, `melodee_chop_slice=`,
  `melodee_chop_armed=`, `melodee_chop_taking=`, the sample slots used
  (`melodee_pad_slots=`), the last pad's `melodee_pad_start=` /
  `melodee_pad_end=` (frames) and `melodee_pad_name=`, and the browser's
  `melodee_files=`, `melodee_file_cursor=`, `melodee_drive=`,
  `melodee_preview=` and `melodee_folder=` (spaces as `_`). For checks
  without hands on the device.
- **Previews:** `build/sample-base --melodee-preview DIR` draws stage, keys,
  transpose, sounds, edit, mix, steps, soundmap, mod, touch, roll, notesheet, motion, patterns, patterncopy, song, songzoom,
  clipsheet, rack, sends, bus, fxpick, pads, samples, browserinternal, trim, chop, chopmanual, chopshift,
  levels, padcopy, mutepads, padbank, copytrack, repeat, repeattriplets, undo, inputconfig, record, project, filesheet, tempo, arp, chord,
  bpm, countin, recarmed, timecorrect, stepedit, erasepad, padmotion, eraselane,
  capture, pad, play, envelope, filter, follow, cloud, padmix, padfilter, filterenv, trimloop, trimshift, ampenv, tune,
  levelsfilter, levelssynth, punch, padpunch, padfx, settings, about, name, namesound, savesample, movefolder, shift, shiftkeys, shiftkit and exit at 1× and 2× (the 2× ones line up
  with the 640 × 480 boards). `--melodee-sound FILE.wav` renders two bars of
  the starting project and checks the level. `./build.sh` runs both.

Installed on the card with `python3 device.py install-sd --app melodee`
(see README): it starts at every boot and leaving it returns to stock until
the next one.

Open questions from the handoff stay as drawn: 8 mod routes and 256 motion
events are single constants in `model/limits.h`; the Pad Bank hold is built
as drawn (question 3); Mute became the MPC's mute mode in parity phase 1c.
