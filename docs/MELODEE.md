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
  thirteen factory sounds made for it. FM6, CZ, SID, the 909 kit and the
  sampler are named in the model but not written yet.
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
- **Remote:** `--ctl melodee-press BUTTON` presses a button (lower case,
  spaces as dashes: `play`, `seq`, `wheel-push`, `top-right`), for checks
  without hands on the device.
- **Previews:** `build/sample-base --melodee-preview DIR` draws stage, keys,
  sounds, edit, mix, steps, soundmap, mod, roll, notesheet, motion, shift and exit at 1× and 2× (the 2× ones line up
  with the 640 × 480 boards). `--melodee-sound FILE.wav` renders two bars of
  the starting project and checks the level. `./build.sh` runs both.

Not yet: saving (the Exit sheet says so), Patterns / Song / Length,
effects and buses, punch, the sampler and chop, projects on the SD card,
Tempo / Chord / Arp / Capture, favourites.

Open questions from the handoff stay as drawn: 8 mod routes and 256 motion
events are single constants in `model/limits.h`; the Pad Bank and Mute holds
are built as drawn (question 3).
