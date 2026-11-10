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
| `src/melodee/shell/` | The page router and the control grammar: printed buttons, Shift layer, Pad Bank and Mute holds, pot pickup, lights, and the snapshot handed to the screen. |
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
sheet) and Motion work with the Shift layer and the Exit sheet. SEQ steps
through Steps, Roll and Motion; B1–B3 pick them too.

- **Roll:** one bar of twelve semitones, the view an octave and a bit above
  the pads' lowest note; the wheel moves the shared step cursor, pads write
  up to four notes on it (or play in live with Seq Record), Minus / Plus move
  the octave and the view. Push on a note opens the note sheet: accent,
  slide, tie, copy half a bar on, delete.
- **Edit, page map and mod matrix:** the engine's pages are followed by
  the mod matrix, which every engine shares (PROPHET: page 11 of 11). B2
  opens the page map: the wheel moves, Push jumps there, Shift + Push or B2
  closes it, and the pots stay on the page underneath. The matrix has
  eight routes a track (`mod_routes`, open question 1), all on one screen;
  the wheel picks the route, Push on an empty row starts one (LFO into
  cutoff, amount 0), K1 source, K2 destination, K3 amount (−100..100,
  centre is none). Sources: LFO, velocity, aftertouch (pad pressure),
  wheel (the fader), key track, random, filter envelope. Destinations:
  cutoff, resonance, amp level, pitch, oscillator A or B pitch, pulse width,
  LFO rate, noise; each engine decides how far a destination moves.
- **Clock:** 24 ticks a quarter. Each pattern has its own division (1/8,
  1/8T, 1/16, 1/16T, 1/32); swing (project-wide, 50–75 %) stretches the
  first sixteenth of every eighth.
- **Patterns** (LOOP): tracks down, sixteen patterns across, eight tracks
  in view following the selected one. Pads are its patterns: a tap queues
  (it starts on the next bar, or at once when stopped), Erase + pad clears,
  hold one and tap another to copy, Shift + pad launches that column on
  every track (open question 8, built as drawn).
- **Length** (B3): 1–256 steps; K1 by the bar, the wheel by the step, K2
  division, K3 swing.
- **Song** (B2): lanes per track, clips of patterns with their notes in
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
  **Shift + Push on Song adds a marker at the cursor bar**; again on a
  marker gives it the next name (INTRO, VERSE, CHORUS, BRIDGE, DROP,
  BREAK, OUTRO), and after OUTRO removes it. A new marker takes the name
  after the one before it. This is the one place Shift + Push is not Back
  (Kerem's choice); the Shift layer says so on Song. Names come from that
  list until a page for typing exists (open question 9). SongZoom's motion and punch lanes are not drawn yet.
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
  are the same page in neutral colours. Pot moves on effects are not yet
  recorded into Motion; the sampler's pad effects (PadFX) come with the
  sampler.
- **Sampler** (an engine like the others; Stage's track 5 starts as an
  empty kit): 64 pads in four banks, each a region of one of the project's
  samples (64 a project), with pitch, fine, level, pan, gate or one shot,
  reverse, a mute group and an envelope; one low-pass filter on the whole
  kit (K1–K3 on Stage: TUNE, FILTER, DECAY). Pad Bank tapped (or Minus /
  Plus on Pads) steps the bank; bank lights A–D show it. Samples are WAV
  files (16, 24 or 32-bit PCM or 32-bit float, any rate, up to 60 s) in the
  card's `Samples` folder and one level of folders under it
  (`$MELODEE_SAMPLES` on a host). The service thread reads a file, makes
  its waveform and finds its hits, and hands the audio to the audio thread
  through a lock-free queue; the sample it replaces comes back the same way
  to be freed, so the audio thread never allocates or frees.
- **Pads** (B2 on a sampler track, or 16 Levels): the bank as the pads sit,
  the last pad hit with its length, play mode and waveform; K1 pitch, K2
  level, K3 pan. Push opens Trim.
- **Samples** (Sample Select on a sampler track): the files; Push loads the
  one under the cursor onto the last pad hit, K2 jumps. RECORDED lists the
  takes; RECENT waits for saved projects.
- **Trim** (Push on a sampler track, or on Pads): the last pad's region over
  the sample; K1 start, K2 end, K3 zoom (to 16 times closer).
- **The sampler's seven pages** (B1 / B3 go round them, PAGE n / 7): Trim;
  Pad (pitch, fine, level); Play (gate or one shot, mute group with its
  pads shown, reverse); Envelope (attack, decay, release; sustain on row 2;
  this pad only); Filter (cutoff, resonance, low / band / high pass, the
  whole kit); Follow (the whole kit keeps the project's tempo, each pad's own
  tempo worked out from its length as whole bars; K2 how: Beats plays a
  sixteenth-long segment from where the sound should be, so hits stay
  whole, Tones overlaps 60 ms windows for held sounds, Tape moves speed and
  pitch together; K3 what a pitch change does: Speed, or Stretch to keep
  the length; written here as a two-tap granular stretcher in the voice); Cloud (pads as clouds of grains while held: position,
  grain size 10 ms to 1 s, density, then scan, spread and on / off).
- **Chop** (Chop on a sampler track): the last pad's sample cut by Hits
  (sensitivity), Even (2–64 slices) or Beats (1–8 beats a slice at the
  project's tempo). Pads audition slices, the wheel picks one, K3 moves its
  start, Push splits it, Erase + pad removes a marker; B3 TO PADS lays the
  slices on pads from the first free one (regions of the same file), B1
  DROP goes back.
- **Levels** (16 Levels on a sampler track): the last pad hit on all
  sixteen pads; B1–B3 pick what rises: velocity (8 to 127), pitch up the
  project's scale, or semitones. The pots stay on that pad. 16 Levels again
  goes back to the kit; until then the pads keep playing the levels on
  every page. Seq Record keeps the velocity of a levels pad, not its pitch
  yet.
- **Record** (Sample Record on a sampler track): B1–B3 pick the mic, the
  line inputs or the mix (resample, before main volume). Sample Record or
  Push arms; the take starts when the level passes K1 (−60 dB: at once) or
  on a second press, and ends after K2's bars (0: until stopped) or on Stop
  or a third press. K3 gain. The take is recorded into a 60 s buffer made
  at start; the service thread writes it to `Samples/Recorded/TAKE n.wav`
  (24-bit) on the card and it lands on the first empty pad. The input is
  routed only while the page is open or a take runs. Samples' RECORDED shows
  those files.
- **Projects** (Shift + Sample): files (`.mldp`) in `Melodee/Projects` on
  the card, in folders (`$MELODEE_PROJECTS` on a host). The format is
  tagged sections, so older files stay readable and only what differs from
  empty is written. The page lists New project, the open project while it
  has no file, folders (Push goes in, Shift + Push comes out) and projects
  with their tracks and tempo; K2 jumps, K3 sorts (newest, name). Push on a
  project opens its sheet: Save, Save as new, Duplicate, New folder here,
  Open at start, Delete (asks once more); on another project Load heads it.
  Rename and Move to folder are dim until there is a page to type on (open
  question 9). New names are Project n, copies "<name> copy", folders
  Folder n. The service thread does every file operation (writing beside
  the file, then renaming it into place); a load swaps the model on the MIDI
  thread and gives the audio thread a whole copy to switch to at once,
  then the project's samples load again. Nothing is created on the card
  until something is saved. LEAVE on the Exit sheet saves first (a new file
  if the project has none). Open at start is kept in `Melodee/settings`.
- **Tempo** (hold Tap Tempo half a second; tapping works anywhere): K1
  tempo, K2 swing, K3 click level; B1 halves, B3 doubles, B2 (or Shift +
  Tap Tempo anywhere) the metronome, a short blip on each beat while
  playing, higher on the bar. The wheel moves the tempo by 0.1.
- **Arp** (Note Repeat: held, momentary; tapped, it stays on; tapped again
  on its page, off): Up, Down, Up-down, Random, Order; K2 rate 1/4 to 1/32,
  K3 octaves, Minus / Plus the gate. B3 Hold latches the keys. It runs on
  its own clock at the tempo, so it plays with the song stopped too. On a
  sampler track it is note repeat: the held pad at the rate.
- **Chord** (B1 on Arp): one pad, one chord from the scale's own degrees:
  Triad, Dia 7, Sus2, Sus4, Power, Dia 9; voicing Close, Open, Drop 2,
  Spread; inversions. B1 there switches chords on or off. Chords play
  through the arp too, and record into Steps and Roll as up to four notes.
- **Capture** (Shift + Sample Record): what the pads played on the track
  lately (a history of the last 512 presses), the last 1–4 bars quantised
  to 1/16 or 1/8 and laid into a pattern (the first empty one); B3 or Push
  keeps it and that pattern plays, B1 drops.
- **Motion:** Seq Record + a pot on any page writes an event at the playing
  step (256 for the project, `model/limits.h`). The audio thread plays them
  step by step, each lane starting the pattern at the track's own value, and
  gives the track its own values back on Stop. Erase + pot on Motion clears
  that lane. The lanes are the parameters holding motion, then the
  favourites.

- **Engine:** PROPHET, a two-oscillator polysynth written here (saw and
  pulse with hard sync, saw/triangle/pulse B with low-frequency mode,
  noise, a zero-delay four-pole ladder, two ADSRs, LFO, wheel on the fader,
  poly mod, glide, unison, drift), eight voices a track, ten Edit pages and
  thirteen factory sounds made for it.
- **FM6:** six sine operators (from a table) with ratio, fine, level, an
  ADSR and velocity each; eight algorithms (stack, two stacks, three and
  three, three pairs, stack and two, one to five, pair and four, all heard)
  with feedback on operator 6; eight voices, seven Edit pages, nine sounds.
- **SID:** a chip voice: triangle, saw, pulse or noise, pulse width with its
  own LFO, ring and sync against a second oscillator, crush, a multimode
  filter with envelope and key tracking, the 8580 or the grittier 6581;
  six voices, four pages, nine sounds.
- **CZ:** phase distortion: eight waves bent by the DCW and its envelope (a
  pure sine at no bend), a second wave alternating each cycle, lines 1, 2,
  1+1' and 1+2' with detune and octave, key follow, DCA envelope; eight
  voices, four pages, eight sounds.
  Sounds' K1 steps through all six engines.
- **909:** a drum kit written here in the shape of the classic machine:
  kick and toms as sines with a falling pitch, snare and clap as tone and
  filtered noise, rim from two resonators, hats and cymbals from six
  detuned square waves through high passes; the closed hat chokes the open
  one. Pads 1–11 play kick, snare, clap, rim, three toms, closed and open
  hat, crash and ride. Six Edit pages (kick, snare and clap, toms, hats,
  cymbals, levels with drive), five kits. Stage's track 6 starts as a 909
  with a beat.
- **Pad effects (B1 from Track, sampler tracks):** each of a kit's 64
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
  wins; Shift + pad locks one on and off. While playing a punch waits for
  the next step of its snap grid. Repeats let one slice through, then
  freeze the punch's two-second history and play it again. K1 depth, K2
  snap, K3 tail (cut, fade, a long ring). Pad lights show the bank, held
  and locked bright.
- **Pad Punch (Pad FX again, or B3, on a sampler track):** each kit pad
  carries a punch of its own, its rate and depth; the pad plays punched by
  itself while the kit runs on (the sampler renders that pad's voices
  apart). Pressure is depth. K1 punch of the last pad hit, K2 rate, K3
  depth. Punch settings and the pads' punches are saved with the project.
  Not recorded into the Song yet.
- **Settings (B2 on Project):** Melodee's own, kept in the `settings` file
  next to the projects (with the project to open at start): palette (NIGHT,
  the one there is), pad response (soft, medium, hard, fixed velocity), pad
  lights (track colours, white, off), knob pickup (off: values jump to the
  pot), screen brightness (20–100 %; the backlight is left as the stock
  app had it until this asks for less, and the runtime puts it back on
  leaving). Wheel moves, Push opens a value, wheel changes it, Push keeps
  it; Minus / Plus change it at once. Nothing is written until a value
  changes. **About (B3):** engines, effects, punches, limits, JUCE version,
  font and licence. Audio out and MIDI in from the board's first guess are
  left out: Melodee has no MIDI input path yet and the outputs follow the
  jacks.
- **Host:** sixteen track slots, level / pan / mute / solo, a pattern mirror,
  a sample-accurate step clock with swing, chance, accent and tie, meters
  and a soft-clipped master after main volume. The send is stored, not heard
  (no buses yet).
- **Shell:** printed buttons (Sample cycles Stage → Keys → Mix, SEQ, Sample
  Select, 16 Levels, Play, Stop, Seq Record, Tap Tempo, Minus / Plus, Erase),
  soft keys, wheel and Push, Shift + Push back, Shift + wheel fine edit,
  Erase + pot to the initial value, Shift + Erase clears the pattern, Shift +
  Stop all notes off, Pad Bank + pad picks a track, Mute + pad mutes and with
  Shift solos, pot pickup, the fader as mod wheel, pad / fader / button
  lights. Power (short) opens the Exit sheet; LEAVE returns to stock.
- **Shift layer:** drawn as on its board. Of its second jobs, Erase, Stop and
  Push work; the others wait for their pages.
- **Remote:** `--ctl melodee-press [shift+]BUTTON` presses a button (lower
  case, spaces as dashes: `play`, `seq`, `wheel-push`, `shift+wheel-push`);
  `wheel+N` / `wheel-N` turn the wheel, `pad1`..`pad16` strike a pad
  (`shift+pad5` with Shift held) and
  `k1=V`..`k3=V` move a pot to V (0..127).
  For checks without hands on the device.
- **Previews:** `build/sample-base --melodee-preview DIR` draws stage, keys,
  sounds, edit, mix, steps, soundmap, mod, roll, notesheet, motion, patterns, song, songzoom,
  clipsheet, length, rack, sends, bus, fxpick, pads, samples, trim, chop,
  levels, record, project, filesheet, tempo, arp, chord,
  capture, pad, play, envelope, filter, follow, cloud, punch, padpunch, padfx, settings, about, shift and exit at 1× and 2× (the 2× ones line up
  with the 640 × 480 boards). `--melodee-sound FILE.wav` renders two bars of
  the starting project and checks the level. `./build.sh` runs both.

Not yet: typed names (markers, projects), Seq
Record writing launches and punches into the song, favourites.

Open questions from the handoff stay as drawn: 8 mod routes and 256 motion
events are single constants in `model/limits.h`; the Pad Bank and Mute holds
are built as drawn (question 3).
