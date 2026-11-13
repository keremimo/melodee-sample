# Melodee on the MPC Sample

Melodee is a groovebox app on sample-base: sixteen tracks, one per pad,
each a kit of samples, a synth or a MIDI track, sequenced in patterns and a
song, mixed and effected on the MPC. Its synths are plug-ins of their own
(below, "Synths"): Melodee Poly, PD and Chip, played on PLUGIN tracks. Every printed button
keeps its MPC meaning; Melodee's own pages sit where the MPC leaves room.
Start it with `python3 device.py start`; `./release.sh` packages it for an SD card.

Everything is written for sample-base: interface, sequencer and engines.
Nothing from the FM-1 firmware is compiled or emulated. How each
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
| `engine/` | Audio: DSP pieces, the sampler, PLUGIN tracks' engine and MIDI's silent one, effects and punches, the Looper (`looper.*`), the engine host (16 tracks and L1–L4, mixer, sequencer clock, MIDI out timing) and the command queue that feeds it. |
| `plugin/` | Plug-ins (below, "Plug-ins"): the rack that loads them on its own thread, JUCE's headless VST3 and LV2 hosts, Melodee's own CLAP host (`clap_instance.*`) and VST2 host with its 32-bit bridge (`bridge32/`), the GUI-library stubs and bridge runtime carried in the binary. Only `plugin_runner.h` reaches the engine host. |
| `ui/` | The design system: tokens, the Rubik typeface, the components every page is built from. |
| `pages/` | One file per page: what it draws from a snapshot and what its pads, pots, wheel and soft keys do. |
| `shell/` | The control grammar and everything around the pages: printed buttons and their Shift jobs, Shift + pad (`shift_pad_jobs`), Pad Bank, mute mode, Note Repeat, the Seq layer (`seq_layer`), undo, the fader, takeover, lights, MIDI in and out, the card (`project_store`, `sample_library`, `card_access`), audio input and renders, and the snapshot handed to the screen. |
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
  single-producer queue to the audio thread. It runs at `SCHED_FIFO` 15
  (the base's, under the audio thread's 30), so a pad is read as it
  arrives. A note played by hand or by MIDI in carries its receive stamp
  (`Context::hit_ns`): it plays a block after the frame it came in at, at
  that frame (`EngineHost::send_at`), so two hits keep their spacing;
  status `melodee_timed` counts those on their frame, those that came
  after it, and the latest by (frames).
- **Audio thread** owns the engines and copies of the patterns. Each block
  drains the queue (a timed note waits for its frame and splits the block
  there; what was sent after it waits behind it, and inside a block only
  notes and values are applied), advances the clock, plays the engines, mixes and
  effects the tracks, and decides which MIDI goes out at which frame. It
  never allocates, locks or waits: patterns, samples, projects and track
  copies arrive whole by pointer and go back to be freed elsewhere.
- **Render workers** (`engine/render_pool.*`, three on the MPC's four
  cores, `$MELODEE_RENDER_THREADS` to choose) share the audio thread's
  block at its real-time priority. The audio thread first sequences the
  whole block (ticks, notes by hand at their frames, ratchets, Motion):
  what each track's engine is told past the block's first frame is kept
  with its frame. Then one run: a job a track, its engine over the whole
  block with those calls made at their frames, then its level, pan and
  chain (where plug-in effects sit), the heaviest of the last block first,
  and the Looper's four loops and their chains as one more job (they play
  what came in the block before). The audio thread sums the tracks and
  loops into the buses and master in order; a second run takes each bus's
  chain and Knob FX's apart; the master's follows here. A block sounds the
  same with workers or without, and as it did when every track was cut at
  every tick (tests check both sample for sample). The workers
  are woken as a block starts, wait awake only while it is sequenced and
  between its two runs, and sleep between blocks. Below 150 µs of track
  work a block they sleep throughout, as they do while one plug-in
  instance plays in two places at once. Kit tracks reach the stream pager
  under a short spin lock. A single plug-in still gets one core's share of
  a block: a 16 ms block from one cannot be split.
  `$MELODEE_RENDER_THREADS=pinned` keeps the audio thread on core 1, two
  workers on cores 2 and 3 and the rest of Melodee on core 0; by default
  every thread floats over the four (measured faster on the MPC).
  `--melodee-dsp-bench` plays a busy project as the audio thread would and
  prints block times and CPU, for comparing on the MPC.
- **Overload guard**: a block that took 85 % of its time or more makes the
  next lighter before one runs late: the busiest track (its engine and
  chain, last block) gives up its oldest voice, faded in tens of ms, and
  every engine's released voices end their tails as fast (a kit's pads
  let go are choked). Until the blocks come back under 70 % the tails stay
  short and no more voices go. Plug-ins have none to give.
- **Screen thread** copies a `Snapshot` under a short lock shared with the
  MIDI thread, then draws without it.
- **Rack thread** (`plugin/plugin_rack.*`) opens and frees plug-ins: it
  copies one into RAM, opens it, and offers it to the audio thread by
  pointer, as samples go; the one it replaces comes back to be freed there.
  VST3 plug-ins are made on a JUCE message thread of the rack's own, and
  LV2 plug-ins too.
  CLAP plug-ins take every main-thread call (open, state, parameter
  text) from the rack's threads, one at a time, and their audio calls
  from whichever thread runs their block (`plugin/clap_instance.h`).
- **Service thread** does every file on the card (projects, sounds,
  settings, samples, takes, renders), the input routes and the speaker,
  the power key, and SD card access. It leaves the card alone while the
  card is lent to a computer.

Wheel Push belongs to Melodee (`App::claims_wheel_push()`); the Exit sheet
(a short press of Power) saves and returns to the stock app through
`App::finished()`, or saves and asks systemd to power the MPC off (the
recovery script sees the system stopping and leaves the stock app off).
Crash, remote stop and the recovery service are the runtime's.

## The panel

Left column top to bottom, then right, as on the device (pad 1 is bottom
left). "Shift" is the job printed under the button; while Shift is held
the buttons with a Shift job glow and the knob strip shows the Shift + K
cards.

| Control | Press | With Shift |
| --- | --- | --- |
| Main volume | Master level, at once | |
| Sample | The SOUND hub (docs/HUBS_PLAN.md): Stage from any page; on Stage, B1's first page; it opens no other page. Held: after 0.3 s the track's pads (Pads on a kit, Keys otherwise) over the page, K1-K3 theirs, B1-B3 Stage's sections; on Steps and Roll the editor stays and the header names the held sound. Held + pad: that kit pad becomes the last pad, unheard. A hold released after 0.3 s with nothing used opens nothing | Input Config |
| Seq | The SEQUENCE hub (docs/HUBS_PLAN.md). Tap: the track's editor (Steps, Roll or Motion, the last open) from anywhere; on an editor home, Steps; on Step Edit back to its editor. Patterns is the ARRANGE hub's (Shift + Pad 12: Patterns, Scenes, Song, B1-B3 between them). Held, on any page: the Sequence layer (Sequencing, below); after 0.3 s the Sequence sheet | Step Edit, under the editor; again back to it |
| Pad FX | Pad FX (the punches); again on a kit track, Pad Punch. Acts on its release: with Knob FX, in either order, the MIX hub on Tracks (shell/mix_hub.h), and neither's own job | Flex Beat (at once) |
| Knob FX | Knob FX mode on (lit, its page) or off, on its release; with Pad FX, the MIX hub | FX Select: the effect chains, Track → Sends → Bus → Master (at once) |
| Shift | The second layer, held | |
| Pad Bank | Next bank of the kit, A–H round. Held: the pads are the tracks at once, a pad picks one, and after a second the screen shows all sixteen; −/+ or the wheel jump banks; B2 MOVE moves a track to another pad (one there trades places with it), B3 COPY (or Shift + Erase) copies one, B1 steps back; Erase + pad clears that track, asked first (sound, kit, patterns; its mix strip stays; one undo). One job a tap, on release: no double tap. On Roll with a synth or MIDI track: the pads an octave up | Previous bank; on Roll the octave down |
| Fader | The pot turned last on the track (PIN keeps it) or a target the Fader page's wheel picks, or the page's own job (Fader below); its light follows the value | |
| Erase | Held + pad, pot, step or fader: clear it (below). A tap on its own erases the page's selection (below); on Song, the clip under the cursor | Copy of what the page holds (below). With Pad Bank held, Erase + pad clears a track |
| Note Repeat | A tap latches, a hold lasts (its arp is Stage's ARP, B2). On Song, repeats the clip to the next marker | |
| Chop | Chop on the last pad's region; again back to Stage's TRIM. On Song, splits the clip at the cursor | Note On (the last pad plays while held), or One shot |
| Mute | Mute mode: a tap latches, a hold lasts | Unmute everything |
| Loop | On release, the last kit pad's loop on or off, lit while it loops (on Song, the clip's bars as the song loop). Held: the pads are the loop grid on any page while it is down (lit at once; half a second or a grid pad makes it a hold, and its release does nothing). With 16 Levels, pressed together in either order: the Looper from any page, and neither does its own job. No double tap | Reverse the last pad; held half a second: the Looper on or off (off asks first while a loop holds audio) |
| 16 Levels | A tap: the last pad (or a synth's last note) on all sixteen pads, again off; the page stays. Held: K1–K3 are its settings on any page (the type, then the two ends or the key's root and scale) and − / + its octave or Zones' page; let go after a hold, nothing. Of type Loop, the Looper. With Loop, pressed together in either order: the Looper, and no toggle | The type: Velocity, Filter (kits), Tune, Scale, Zones (an instrument's pad), Loop |
| Sample Select | Samples (the browser) on a kit, the plug-in picker on a PLUGIN track, Sounds elsewhere | Kit: Save Sample; on Samples, Save Bank. MIDI: Save sound. On Sounds: star |
| Tap Tempo | Tap the tempo; held half a second, the Tempo page | Metronome Off → On → Rec |
| Wheel | Moves the cursor or the value; turned while a pot was touched last with Shift, fine edit (on Roll: marks notes; the wheel alone walks each chord's notes, then the next step) | |
| Push | Opens or confirms | Back along the page stack (16 deep; on Song a marker, on Project up a folder) |
| − / + | Lists move as the wheel does; on play pages the octave (16 Levels held: its octave or Zones' page; on Roll the pitch: the chord in the step, then a semitone); Settings and Tempo the value; Song markers; Steps nudge; sampler pages and Punch the bank | Undo / redo, on every page |
| Sample Record | Record on a kit (there it ends the take and opens Stage's TRIM on the pad; B3 PRINT the PRINT sheet); the PRINT sheet on a PLUGIN or MIDI track (Print and freeze, below); with Seq held, the pattern pads freeze or thaw | Audio Recall: the last 25 s onto the next empty pad |
| Seq Record | Stopped: arms (flashing), Play starts; playing: records at once; recording: stops recording, playback goes on | Recall (Capture): what every track played last into its next empty pattern, no page |
| Stop | Stop (on Record, ends the take) | All notes off |
| Play | Play from the start (on Song from the cursor); held while playing, Fill (FILL trig conditions play) | Continue from where it stopped or the wheel put it |
| B1–B3 | The page's soft keys, named above them; with Note Repeat on, its rate (on Stage only while Note Repeat is held; not on Song or the Looper) | Page jobs (Stage's TRIM on a kit, Chop) |
| K1–K3 | The cards above them; Knob FX mode takes them for its effect | The card's second parameter (cards with the Shift icon) |
| Pads | The page's job (most pages play the selected track) | The jobs printed under the pads |

### Shift + pads

As printed, on every page but the sample browser (where Shift + pad loads
the file under the cursor onto the next empty pad). `shell/shift_pad_jobs.cpp`
holds the table the dispatch and the lights share.

| Pad | Job | Pad | Job |
| --- | --- | --- | --- |
| 1 | Full Level (every hit at 127; again back) | 9 | Fader page |
| 2 | Half Seq | 10 | Rec Quantize: on / off (B3) and its grid |
| 3 | Double Seq | 11 | Resample (then a pad) |
| 4 | Count-In on / off | 12 | Song |
| 5 | Compressor | 13 | Trim Sample |
| 6 | Half Speed | 14 | Time Correct |
| 7 | Double Speed | 15 | Warp: Time stretch or Pitch for the kit |
| 8 | Settings (seven doors; MIDI Config one of them) | 16 | Project |

Toggles (1, 4, 10) are kept in the settings file. While Shift is held the
pads light white, the panel's own: a toggle that is on Full, a job Dim, and
dark where the job would only answer with a notice (Half and Double Seq,
Half and Double Speed and Time Correct with no notes in the pattern;
Resample, Trim Sample and Warp off a kit or on an instrument). The screen draws no
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
(brightest at the middle for pan and tune). The meters right of the screen
show the mix, left and right, before main volume; on Record and Input
Config, and while a take waits or runs, the input source instead (as on
the MPC), so Main Volume never starves them. For the mix the five LEDs
light at −36, −27, −18, −11 and −7 dBFS, matched to Melodee's headroom (a
synth in the greens with loud notes yellow, a drum kit green and yellow with
its loudest hits flashing red); for the input at −42, −30, −18, −9 and −2
dBFS, red where the converter clips. They fall 30 dB a second; no LED is held.
`melodee_meter_db=` in `status` gives the levels shown.
`shell/button_jobs.cpp`, `shell/lights.cpp`, `shell/meter_lights.h`.

### Pad lights

The pads light as what a press does there: `pad_context()`
(`shell/pad_context.h`) picks the context in the order `Shell::hit()`
dispatches presses, and both switch on it, so a layer cannot get a job
without a light. `shell/pad_lights.cpp` has a function a context, in one
grammar (`shell/pad_light.h`):

- **Colour** says whose pad it is: the track's colour (a loop's on the
  Looper; a kit pad's own colour where it has one, Stage's COLOUR or its MPC
  kit's), white for the panel's own jobs and the pad in your hand, red for
  a loss or a recording (muted, about to be erased, the take).
- **Brightness** has five rungs: Off, Faint 0.25, Dim 0.4, Mid 0.65 and
  Full. The LEDs square a level before the 0–127 drive, so below about 0.2
  a colour falls apart; Faint is the lowest rung at which all sixteen track
  colours keep their hue.
- **Motion** is timing: a blink waits for a boundary (a pattern or a track's
  next pattern for the bar), a flare is a sound fading (a kit's voices, the
  pattern at the playhead for synths and MIDI, a pad let go), the
  Count-In pulses every pad white on its beats, the bar's first brighter.

| Context | The pads |
| --- | --- |
| A kit (play pages, the sample pages, the browser) | The selected pad Full (an empty one too), a sample Dim, empty dark, a muted pad Faint red; each flares as it sounds; held white, red while Seq Record writes it |
| A synth or MIDI track | The key's roots in the track's colour (Mid), its other notes white Dim, out of the key (chromatic) white Faint; a held chord's notes white Full. The key is the track's: the project's moved by its transpose. Record with a synth as the source shows its keys the same way |
| 16 Levels | Soft to loud (Velocity, Filter) Faint to Full; Scale the key's roots in the track's colour (Mid) from pad 1, its other notes white Dim; Tune the same, out of the key white Faint (a kit's sample as recorded counts as the root; a synth's pads by the note they play); Zones a pad holding a key in the track's colour Full, the rest dark; the pad hit last white as its sound fades |
| Tracks, Pad Bank held | Picked Full, heard Dim rising with the meter, muted Faint red, a pattern change blinking for the bar; a solo white, the tracks it silences Faint; a track copy's source white, where it lands Faint (red where it replaces); a move's in their own colour, Dim where a track trades places |
| Mute mode | Muted Dim red, heard Mid (kit pads flaring), soloed white, silenced by a solo Faint |
| Song | The lane picked Full, a clip under the cursor Mid, clips elsewhere Dim, not in the song Faint |
| Seq held | The pattern playing Full, queued blinking, with notes Dim, frozen white (Dim; playing Full); + Shift a column, white by how many tracks hold notes there; + Erase red; a copy's source white, where it lands Faint (red where it replaces); + Sample Record the patterns to freeze white |
| PRINT sheet | Patterns lit for printing Full, others with notes Faint; printing: done white, the one printing blinking white, waiting Dim |
| A deep-frozen track | Its own pads dark: they play nothing |
| Steps | The bar's steps by velocity (accent Full, 90 and up Mid, softer Dim, empty Faint), past the pattern's end dark, the playhead white (red while recording); on a kit the last pad's lane |
| Step Edit | The step's events by velocity, the chosen one white |
| Roll | The note in focus white, the cursor's step Full, notes elsewhere Dim, none Faint |
| Time Correct | Whose notes move Full, with notes Dim |
| Motion on a kit | The last pad Full, pads with recorded motion Mid |
| Chop | The slices, the one in hand Full, flaring as they play; Manual armed: pad 1 flashing red; cutting: pad 1 white, the rest white Faint; + Erase the slices that would go Faint red |
| Record, Resample | The take's pad red (flashing while it waits), cuts white; Resample's empty pads pulse |
| Punch | A held punch white as deep as it is pressed, latched Full, waiting blinking; Erase held: the punches the pattern keeps Dim red, the rest dark |
| Knob FX, Flex Beat, Pad Punch, sidechain | On or playing Full, the rest Dim or Faint, empty dark |
| Shift held | The jobs white (above) |
| Erase held on a kit | Stopped: the pads that would empty Faint red; playing: the pads with notes Dim red, flaring as they play |
| Pad Bank + Erase held | The tracks Erase + pad would clear Faint red (a sound, or notes in its patterns), empty ones dark |
| Shift + Erase on a kit | The pads that can be copied Dim; then the source white, where it lands Faint (red where it replaces) |
| Erase held on a synth's keys | The keys with notes in the pattern Dim red, flaring as they play |
| Key or step copy | The keys (or Steps' steps) with notes Dim; then the source white, where it lands Faint (red where it replaces) |
| Name sheet | Keys white Dim, delete red, the key just typed bright for the retap window |
| A question, Exit | The pad the question is about flashing red, the rest dark; Exit all dark |

The Pad lights setting (Colour, White, Off) comes last. `status` reports each
lit pad's hue and rung (`melodee_pads=2:t4,9:r1`: pad 2 the track's colour
Full, pad 9 red Faint; hues t track, l loop, w white, r red, o orange).

### Light show

Sample + Mute turns the light show on or off (`shell/light_show.h`); while
it is on, Sample + Minus / Plus picks the style. The pads, buttons and pages
work as usual. SD card access still shows its own lights. The show is off
at startup.

Every style keeps time with the tempo (stopped: at the tempo, half as
bright) and changes colour every four bars. A pad press flashes that pad
white; a sound starting on a pad of the selected track's bank in view (the
sequence, Note Repeat) flashes it more softly. Playing, the bank lights A-H
step through the bar in eighths, Play is lit and Tap Tempo flashes on the
beat; the meters show the mix.

| Style | Lights |
| --- | --- |
| Pulse | The whole panel on the beat, harder on beat 1; white / red buttons red on beat 1 |
| Sweep | A bar of light across the panel, left to right and back, one beat each way |
| Chase | One pad a sixteenth, pad 1 to 16 through the bar; the buttons round the panel in eighths |
| Ripple | Rings from each pad pressed, and from the middle on beat 1, across pads, buttons, fader and bank lights |
| Levels | Pads fill bottom to top with the mix; buttons up both sides, bank lights as a bar |

## Pages

### Playing

- **Stage** (Sample): the selected track's quick pages. Every track gives
  **B1–B3** the same jobs (docs/HUBS_PLAN.md): B1 the source, B2 how it
  plays, B3 where it goes. Each holds a cycle of up to five pages (a MIDI
  track's B3 none: it does nothing): the lit one again
  steps to the next, another jumps to its first; the soft keys name each
  button's page, with a dot a page under a cycle. Each track keeps its own
  button and page. The pads play the track (a kit's pad played is the pad
  edited), the **wheel** fine-edits the card touched last (ringed), **Push**
  moves to the next card (PRESETS: the whole list), Shift +
  Push is Back, −/+ the octave (a kit's bank; EDIT: the rows).
  - **A synth** (a PLUGIN track: Melodee Poly, PD and Chip are plug-ins,
    below, "Synths"): B1 **PRESETS → EDIT**, B2 **ARP → CHORD**, B3 **MIX →
    SENDS → FX**. PRESETS: the plug-in's presets as a poster, the one
    playing large with the plug-in and its format under it and the ones
    before and after small in the corners, the header n / count; the wheel
    walks them, the one turned to loading 250 ms after the last turn
    (amber till then), Push opens the whole list (PRESETS, B1 back). K1–K3
    are the same three knobs on every plug-in, run on what it plays
    (`engine/macros.h`): **FILTER** (left low pass, right high pass, off in
    the middle, its resonance rising as it goes), **ECHO** (how much goes into
    a dotted-eighth echo on the song's tempo) and **SPACE** (how much goes into
    a reverb); the echo's repeats and the reverb's tail ring on when their knob
    is turned back. They are saved with the track and record as motion; Erase
    + pot sets one back to off. A plug-in with no presets: B1 says
    **PLUG-IN**, its face (name, format, the share of each block it takes).
    EDIT: its own values, as Edit's list (the wheel and −/+ the rows). MIX:
    level, pan and the quick send into bus A; its meter carries the five
    steps of the meter LEDs as a scale. SENDS: a track sends into each of the
    four buses: K1 picks the bus (A–D, the one Tracks' SENDS and Bus show
    too), K2 sets the send into it, K3 its return; the picture fans out
    from the track to the four buses, each line as thick as its send, each
    bus with its first effect and a bar of what it receives, the bus in view
    on a chip in K1's shade with its line in K2's. ARP: mode (Up,
    Down, Up-down, Random, Order), rate 1/4 .. 1/32T, octaves, Shift + K1
    gate, Shift + K3 hold; the header says whether it is on (Note Repeat
    turns it on); its picture is a keyboard along the foot with the keys
    held filled and the notes to come hanging over their keys, the next one
    white, joined in the order they play. CHORD: one pad a chord from the
    scale, K1 type (Triad, Dia 7, Sus2, Sus4, Power, Dia 9), K2 voicing
    (Close, Open, Drop 2, Spread), K3 inversion, Shift + K1 chords on or
    off, Shift + K3 hold; the chord the last pad played drawn with its notes
    on two octaves of keys. FX: the slot of the track's chain the cards edit
    (Push the next one; with none yet, Push opens the effect picker; Shift +
    Knob FX adds effects), all six slots drawn with their family glyphs.
  - **A kit**: Akai's pages on the last pad hit, B1 **TRIM →
    LAYERS → TUNE → AMP ENV** (LAYERS the pad's four samples), B2 **PLAY → COLOUR → FOLLOW → CLOUD**, B3 **MIX → SENDS → FILTER →
    FILT ENV → KIT FILT** (SENDS the kit track's four; the sampler pages keep the same groups, without SENDS): the pad's
    waveform with its volume on the left and pan on the right, chips for
    how it plays; TRIM draws its handles (Shift + K1–K3 zoom at each; Shift
    + B1 Loop Lock, Shift + B2 Normalize), the envelopes and the filter lie
    over the region. MIX: volume, kit volume, pan; Shift + K2 on AMP ENV and
    FILT ENV decay from start or end, Shift + K3 on TUNE # Beats, Shift + K2
    on PLAY Pad Link, Shift + K3 on PLAY the loop's seam crossfade. FOLLOW: on, mode (Beats, Tones, Tape), pitch; the
    project's beats drawn over the pad's region in amber (the bar's first
    brighter), as many as # Beats says or its length at the tempo, and the
    sample's own hits as dots above, each joined to the beat it is moved
    onto; Follow on or off is an LED in the header. COLOUR: the bank's pads as
    they sit, each filled with the colour it lights in, the last pad ringed; K1 and the wheel walk that pad's colour
    (`PadSound::colour`, `shell/pad_colour.h`): Track (0, the track's), the sixteen track colours by name, then Kit, the
    pad's own when it is none of those (an MPC kit's), until another is picked. The Pads page draws each pad in its
    colour too; names, the header and the cards keep the track's. A colour is one undo step on the pad and is not heard, so a frozen track keeps its
    print. CLOUD
    (the whole kit's pads as grain clouds): K1 position, K2 grain size, K3
    density, Shift + K1 scan, Shift + K2 spread, Shift + K3 Cloud on or off,
    the place and grains drawn on the wave. KIT FILT: cutoff, resonance,
    type of the one filter after every pad. While the pad sounds each of its
    voices draws a head on the waveform (the four newest; the newest with
    its wake, older ones at half), a voice cut by Mono or a mute group fading
    out over 60 ms, and PLAY counts them ("3 VOICES"). Reversed, the head runs
    right to left; looping, a ghost stays a moment at the place it left, the
    loop flag lights as it goes round, and TRIM counts the passes. On CLOUD
    each grain of a held pad's cloud is a short pill in K2's shade from where
    it began to where it reads, at its window's strength, leaving a faint
    mark for 150 ms; the bars a grain covers lighten, and with Scan moving
    the cloud travels and GRAINS AROUND follows it (K1's line stays).
  - **A MIDI track**: B1 **DEVICE → PROGRAM → CC**, B2 **ARP → CHORD**, B3 nothing (no audio). DEVICE
    (and Edit's DEVICE alike): the route, alive. The track, its port (the
    MIDI Out jack, a computer over USB-C, every USB device; amber when
    nothing is there) and the instrument as glyphs on one line, notes
    travelling along it as they go out, the instrument's name (the track's)
    under it, and the last four seconds of notes sent as a roll underneath;
    K1 port and K2 channel on the cards. PROGRAM:
    bank MSB, bank LSB and program (each Off until set), sent on a change
    and when the track loads. CC: three controllers on K1–K3 with their
    General MIDI names (74 Brightness, 71 Resonance, 1 Mod wheel at first),
    Shift + K picks a controller's number; they go out on a turn and record
    as Motion.
  - **An empty track**: three doors on one page, a tile under each key
    showing what it would make, the pot under it choosing: **SYNTH** (a
    PLUGIN track: the plug-in picker opens on the card's Plugins folder),
    **KIT** (K2 where its first sound comes from: samples on
    the card or record the input; Audio Recall and Resample stay on their
    keys and need a kit track), **MIDI** (K3 the port). The key under a
    tile makes that track at once; Push makes the one of the pot touched
    last (its key lit), the wheel turns that pot's list. Sound and channel
    are picked after, on Stage.
- **Tracks** (Pad FX + Knob FX together, the MIX hub's B1): the sixteen tracks as the
  pads sit, each cell its number, the pattern it plays, its engine as a
  glyph and its level up the right edge (muted: a crossed speaker); the
  track in hand as a strip on the right with its name, fader, meter, pan
  and its four sends as bars (A–D, top to bottom). Along each cell's
  bottom, what the track takes of a block (its engine and chain, the
  whole width a core's whole block; green, amber from 60 %, red from
  85 %), and the track in hand's in the header. A pad picks its track; K1 the track, K2 its level, K3 its pan;
  Shift + K1 its playback **speed** (×½, ×¾, ×1, ×1½, ×2: its patterns
  play that much faster or slower against the tempo, their notes and
  motion as written, ×1½ turning sixteenths into sixteenth triplets; the
  header names it, Patterns too; kept in TSPD);
  the wheel or −/+ the track, Push its Stage; the fader is its level. The MIX
  hub's keys, **B1 TRACKS**, **B2 SENDS**, **B3 FX** (Tracks, Sends and the
  chains share them; FX steps track, bus, master, a kit's pad); with Shift,
  **MOVE** and **COPY** (below). Pad Bank
  held + pad still picks a track from any page, the same picture over it.
- **Keys** (Sample held on a synth): the sixteen pads fill the
  picture, each its note large and its degree in the scale small, the roots
  filled in the track's colour, the pad played white; the key and scale are
  the header's chip, the octave beside it. K1 root, K2 scale, K3 layout;
  octave on −/+; Shift + K3 transposes the pads −12..+12 (kept with the
  track). Push opens Edit. The layout is each track's own; the key is the
  project's.
- **Drums** (K3's last layout): a drum plug-in's or MIDI drum machine's pads
  play as a kit's, notes 36 up whatever the key, octave or transpose: pad 1
  of bank A is 36 (the MPC's and Ableton's drum-rack notes), pad n of the
  bank in view 36 + 16 × bank + n, banks A–D (to 99) on Pad Bank or −/+, lit
  on the bank lights. The chip says DRUMS, the pill the bank; each pad
  names the plug-in's voice there (else its note), a pad with no voice
  dark, the drum hit last in the track's colour, and the pads light the
  same. Steps works one drum's lane at a time as on a kit (the drum hit
  last); Roll draws pad lanes; chords play the pad's note alone; 16 Levels
  is Velocity alone (another pitch would be another drum); the variations
  that move octaves are left out, as on a kit.
- **Pads** (Sample held on a kit): the bank as the pads sit, each
  pad drawing its own waveform from start to end, so a kit is known by its
  shapes; a pad that sounds lights and a head runs through it, the pad in
  hand (filled) and the bank are named in the header; K1 pitch, K2 level, K3 pan;
  Shift + K1 and K2 the project's root and scale; Push opens the map of the
  sample pages.
- **Sounds** (Sample Select on a MIDI track): the factory sounds and one's
  own for the engine, names in Title Case, the loaded one with a check at
  its right edge; K1 jumps (as on every list), the wheel walks, Push loads;
  FAVOURITES lists the starred (Shift + Sample Select). A synth's sounds are
  its plug-in's own presets (PRESETS on Edit, below, "Plug-ins").
- **Edit** (a MIDI track's Stage B3 EDIT; a PLUGIN track's parameters are Stage's B1 EDIT): the engine's pages: a MIDI track's DEVICE and
  PROGRAM; a synth's, its plug-in's own values (below, "Plug-ins"). No title
  row: the page is the header's chip, and its curve fills the picture with
  every parameter named at the point it moves, in its knob's colour while
  on the row the pots hold, dim otherwise; the rest are listed small at the
  top left. B1 and B3 name the pages either side with chevrons (with two
  pages, a MIDI track's, B3 alone goes to the other); **B2 MAP**
  (its dots the page in view) opens the page map (wheel, Push jumps, B2 or
  Shift + Push closes).
- **16 Levels**: on a kit the last pad on all sixteen pads, on a synth its
  last note. It has no page: a tap turns it on or off where you are (the
  notice names the type), and while it is held K1–K3 show its cards over
  the page (`levels_held_cards()`, shell/sample_mode.h) and − / + move its
  octave or Zones' page; a hold, a pot or − / + leaves the release doing
  nothing. Melodee's Scale first (a new track's type), Tune (semitones),
  Zones (a kit pad holding an instrument), Velocity, Filter (kit pads: the
  cutoff). **K1** (LEVELS) the type. **Zones**: the keys the instrument's zones
  sound on when pressed (a release sample's not), lowest on pad 1, each pad
  one key at its real pitch, its velocity layers and round robins by the
  hit; pads past the last key are dark and silent; more than 16 keys make
  pages (up to six), − / + walk them, the picture a block a pad holding a
  key with the page's first and last key under it and the page number; no
  K2 / K3 (`levels_of()`, `SampleRef::keys`). On an instrument whose zones
  each sit on a key of their own (two keys or more: a drum or percussion
  layout, `SampleRef::one_key_zones`), 16 Levels opens in Zones where the
  track had Scale or Tune, which would play its hits from other keys
  (`levels_opening()`); Velocity and Filter stay. **Scale**:
  **K2** the root, **K3** the scale (the project's key, as on Keys), pad 1
  the root and each pad the next note of the scale up, no repeats: a synth
  plays those notes in the track's octave; a kit's sample counts as a C,
  moved the nearest way to the root (Shift + K1, the pad's own pitch, tunes
  a sample that is not a C). The others: **K2** pad 1's end, **K3** pad
  16's, the pads between evenly from one to the other, kept for each type
  with the track (sixteen levels between 40 and 127, a filter sweep over an
  octave; at first Velocity 8–127, Tune −3 to +12 with the original on pad
  4, Filter 0–100). The pad's own pitch, volume and pan are on Pads.
  Seq Record keeps a Filter pad's cutoff with each note it writes (roadmap
  P3, the file's `LVCF`), so the pattern plays that note at that cutoff
  (the pad's filter, Classic where it has none), as it was heard.
  Shift + 16 Levels goes round the types too. The pad lights show the
  levels. − / + (16 Levels held) move Scale an octave (a synth's pads with
  the track; a kit three down to five up, kept with the track), and a
  kit's Tune too.
  On a kit pad (no arp), a MIDI keyboard plays that pad on every key
  (`levels_key()`, shell/midi_route.h), whatever the type: key 60 a sample
  as a C, the pad's Semi kept; an instrument at its real pitch, its Semi
  undone. The keyboard's velocity plays as it comes.
- **Note Repeat** stays on the page: while on, B1–B3 are 1/8, 1/16, 1/32
  (Shift: their triplets); the lit B1 again is 1/4, the lit B3 again 1/64
  (dots under them), and back; straight rates swing with the project. It is
  the track's arp; a kit repeats the held pad. The repeats play as hard as
  the pad is pressed (its pressure, once it reports one; with Full Level at
  127), as on the MPC; under Seq Record each repeat is written as a note
  where it fell, as long as the arp held it (the press itself is not, as it
  is not heard).
- **Tempo** (hold Tap Tempo): K1 tempo, K2 swing, K3 click level, Shift + K1
  and K2 the project's root and scale from any track; B1 halves,
  B3 doubles, B2 CLICK the metronome Off / On / Rec (its LED green on,
  amber on Rec); the wheel by 0.1, −/+ by 1. The picture is a ring that
  beats: sixteen marks for the sixteenths, the swung ones off their places,
  an arc sweeping the bar as it plays, the tempo in the middle.

### Looper

Four tempo-synced audio loops on tracks of their own, **L1–L4**, there while
the Looper is on ([LOOPER.md](LOOPER.md) has the design). Each loop track has
a mix strip (level, pan, mute, solo, send and bus) and a chain of six.

- **Looper** (Loop and 16 Levels pressed together, in
  either order, from any page; 16 Levels of type Loop): off, it asks first
  (B3 or Push turns it on: 45 MB a loop, made in the background). **B1 LOOPS**:
  four rings, a loop each: its waveform bent round from twelve o'clock, a
  hand sweeping where it plays (in sync, the hands point the same way); red
  while recording (what is recorded so far), amber overdubbing, its colour
  playing, grey stopped, a broken amber ring while armed. Under each its
  state as a word (ARM, REC, DUB, PLAY, STOP, FIT; a change waiting blinks)
  and its length (a loop at another tempo shows the tempo it was recorded
  at, amber); K1 the loop,
  K2 its length (Free, 1, 2, 4, 8, 16 bars; before a take), K3 its level;
  Shift + K1 feedback, Shift + K2 Quantize (Bar, Beat, Off), Shift + K3 After
  take (Play, Overdub, Stop). **B2 MIX**: L1–L4 as strips; K1 level, K2 pan,
  K3 send, Shift + K3 the bus. **B3 MORE**: the loop's sheet, one screen,
  no list: three panels as the keys sit, under B1 the loop itself (its ring,
  state and length), under B2 and B3 their jobs. **B1** BACK, **B2** COMMIT
  (the loop onto the kit's next empty pad, which the panel names), **B3**
  PLAY or STOP on the next boundary (a change waiting: CANCEL); with Shift
  held **B1** CLEAR (asks), **B2** + NOTE (commit and a note at bar 1, the
  loop stopped), **B3** STOP at once (a take closes on its whole bars) or
  PLAY at once. A key that would do nothing is dim. **K1** SOURCE (Input,
  the tracks there are, the other loops, Mix, the hardware inputs that do
  not clash; between takes), **K2** length, **K3** level; Shift + K1–K3 as
  on LOOPS. It stays open after its jobs. Undo / redo of the last overdub is
  pads 5–8, Looper off Shift + Loop held. The wheel and −/+ pick the loop,
  on the sheet too. **Push**: record → close → overdub ⇄ play, the sheet
  open or not; on a loop at another tempo it first fits it (Push again
  cancels), then overdubs. Shift + Knob FX opens the loop track's chain
  (B1 back to the Looper).
- **Pads** on the Looper: a column a loop. 13–16 record / overdub / play (as
  Push), 9–12 play or stop on the next boundary (again cancels; Shift: at
  once), 5–8 undo then redo, 1–4 held 0.6 s clear. Lights: red recording
  (blinking armed), orange overdubbing; the loop's colour playing, brighter
  on the beat, white on the bar; white for undo, dim for redo; red filling
  while a clear is held. Mute mode: a column mutes its loop track, Shift solos.
- **Sources**: Input (Input Config's source after its gain), a track after
  its chain, another loop's track, or Mix (as Resample hears it: overdubbing
  from Mix replaces, a bounce). Each arrives a block late, Input a converter
  round trip more (`melodee-looper latency`); the loop writes it that far
  behind its play head.
- **Transport**: Play lines every loop up with bar 1 and starts armed takes
  on its downbeat (with Count-In, after the count); Stop closes takes, ends
  overdubs and holds the loops silent. The metronome's Rec setting clicks
  while a loop records. `melodee-looper transport free` keeps time while
  stopped.
- **Tempo**: a loop keeps its pitch and stays on the beat at any tempo (a
  WSOLA stretch on the audio thread); Fit renders it at the tempo now on the
  service thread.
- **Kept**: with the project, as `<project> Loops/Loop N.wav` beside its
  file (hidden on Project, and moved, renamed, duplicated and deleted with
  it); a loop saved stopped comes back stopped. **Commit to pad** (a kit
  track selected): `Samples/Looper/L<n> Loop m.wav` onto the kit's next empty
  pad, one undo; + note writes a note at bar 1 as long as the loop (the
  pattern made long enough) and stops the loop.

### Sequencing

The clock runs at 96 ticks a quarter. A pattern is a list of note events
(tick, length, key, velocity, chance, accent / slide / tie, a ratchet) up
to 128 bars of the project's time signature; its **Q** (1/4 .. 1/64 with
triplets) is only the grid. Swing (50–75 %) stretches the first sixteenth
of each eighth.

A **ratchet** plays a note as 1–8 hits spread evenly over its length, each
on its own frame and held as a note of that length would be; the chance is
rolled once for all of them, an accent lifts every one. Its **ramp**
(−100..100 %) sets the last hit's velocity against the note's, the hits
between going evenly; on a kit pad a **pitch ramp** (−24..24 semitones)
does the same to its tune. Slide and Tie wait for a note of one hit. The
editors' TRIG row holds them: the lit key again (STEPS on Steps, ROLL on
Roll) turns the cards to K1 the hits, K2 the ramp, Shift + K2 the pitch
ramp (a kit); again, back. Files keep them in RTCH.

A **trig condition** (the TRIG row's K3, beside the chance on K3 of the
main row) picks the passes a note plays on: ALWAYS, n:m (the n-th of every
m passes, 1:2 .. 8:8), FILL / NOT FILL (Play held while playing is Fill),
FIRST / NOT FIRST (the first pass since the pattern began: launched, Play,
or its clip), PRE / NOT PRE (the last note weighed before it on its lane
played, or not: a kit's pad, else the track), NEI / NOT NEI (the same of
the lane before: a kit's pad below, else the track before; nothing weighed
there yet counts as played). A note is weighed when it has a condition of
its own (not PRE or NEI) or a chance; the chance is rolled after the
condition. Steps rings a step's foot and Roll a note's end when it has
one; files keep them in COND. `status`: `melodee_trigs` (let through /
held back), `melodee_fill`, `melodee_hits` (ratchets' hits after their
first).

Seq has three levels and a layer (`shell/seq_layer.h`): **tap to look,
hold to steer, Shift + Seq to zoom in**.

- A **tap** of Seq opens the selected track's editor from any page: the one
  last open on that track, else Steps on a kit and Roll on a synth or MIDI
  track. On an editor it goes home, Steps; on Step Edit back to its editor.
  B1–B3 on every editor are STEPS, ROLL, MOTION. Patterns, Scenes and Song are
  the ARRANGE hub (Shift + Pad 12), B1–B3 PATTERNS, SCENES, SONG on Patterns
  and Scenes (Song keeps its own keys).
- **Seq held**, on any page, is the Sequence layer. A track has 128
  patterns, eight banks of sixteen (A–H, as the MPC's 128 sequences). The
  pads light at once as the track's patterns in the bank in view (playing
  bright, queued blinking, with notes faint, empty dark) and the bank
  lights show that bank; after 0.3 s the **Sequence sheet** shows them as the
  pads sit and beside them the chain: what plays now, over what comes next,
  over what follows (a chain of more than three adds a count; each chained
  pad carries its place), the tempo under them. Let go within 0.3 s with
  nothing touched: a tap. While held:
  - a pad launches that pattern of the selected track (next bar; stopped,
    at once); an empty one starts as long as the one playing. More pads
    tapped in the same hold make a **pattern chain** (up to eight): each
    pattern plays once through and the next starts at its end, round
    again, a song for one track without Song (kept in the project, CHAN). A
    single tap, or a pattern launched any other way, ends it. Shift + pad
    launches that column on every track holding notes there (and on the
    selected one): its **scene**, with the scene's tempo. Hold a pad
    and tap another to copy. Erase + pad asks,
    then clears that pattern (one undo). Shift + Erase starts a pattern
    copy: the pattern, then where (on this or another track; the pads stay
    patterns until it ends).
  - Pad Bank the next bank of patterns (Shift: the previous); the pads,
    the sheet, Patterns, Scenes and Time Correct's groove row show it. It
    follows the selected track's pattern again whenever that changes. Hold
    a pad, step the bank, tap a pad: a copy across banks.
  - −/+ the previous / next pattern, across the banks; the wheel the tempo (Push: whole BPM
    or tenths); K1–K3 the pattern's length, Q and swing (Shift: any bar
    count, time signature, metronome level), over Knob FX's.
  - B1–B3 open Steps, Roll and Motion (the sheet stays away until Seq is
    let go); Seq Record starts a new take in the next empty pattern, armed
    (playing: recording at once). Other buttons keep their jobs.
- **Patterns** (Shift + Pad 12, ARRANGE's B1): tracks down, the bank in view's sixteen patterns
  across (the header names the bank). The pads play
  the selected track, so it is played and recorded with its patterns in
  view (Erase + pad and mute mode as on any play page). K1 length (1, 2, 4
  .. 128 bars; Shift: any count), K2 Q (Shift: time signature, 1/4 ..
  15/16), K3 swing (Shift: metronome level). The wheel moves the playhead
  by a beat (Shift: by Q); stopped, that is where Shift + Play continues.
  The header shows the position, time signature and Rec Quantize. BPM per
  pattern (each pattern number its own tempo, the one launched last
  leading) or one for the project: Push on the Tempo page. Push opens
  Scenes. A pattern with a follow action carries a dot in its corner.
- **Scenes** (ARRANGE's B2, or Push on Patterns; roadmap P5): a scene is a column, pattern
  n of every track, with a name and a tempo of its own. The sixteen sit as
  the pads do: a pad launches its scene, pattern n on every track holding
  notes there (and on the selected track) at the next bar (stopped, at
  once), tracks with nothing there playing on; its tempo takes over on that
  bar (with BPM per pattern on, a pattern's own still leads). Lit bright
  while the scene plays whole, blinking while it waits for its bar. Push
  names the scene in hand (the ready names are the markers'), the wheel
  sets its tempo (below 30 BPM: none), Erase + pad clears both. Beside the
  grid: the scene in hand, its tempo, the follow action and a dot a track
  (in the scene, playing another pattern, or stopped: a ring). Files keep
  scenes in SCNE. Minus / Plus move the track.
- **Follow actions** (Scenes' cards): what the selected track's pattern
  does once it has played a while, as melodee-ios's clips. K1 FOLLOW Next,
  Previous (round the track's patterns with notes), Any (another at
  random), Again (from its top), Stop (the track silent until a launch or
  Play); K2 AFTER 1-64 bars or 1-64 plays of itself (at its track's
  speed); K3 CHANCE, the first action's odds; Shift + K1 OR, the second,
  on the odds the first leaves (with none, the pattern plays on another
  stretch and the dice roll again). The audio thread brings it on the tick
  it is due, so a pattern of 1.5 bars followed after a play changes there,
  not on a bar line. A launch waiting for its bar comes first; in the song
  none come. A track playing a **pattern chain** plays the chain: its
  patterns' follow actions wait while it is chained (the chain is the
  order asked for). Shift + B3 ALL puts the follow action on every track's
  pattern with notes in its column (one undo); each turn is one undo of
  the pattern. Files keep them in FOLW. `status`: `melodee_follows`,
  `melodee_stopped`.
- **Seq Record**: arm and Play, or record at once; again stops recording.
  **Count-In** plays a bar of clicks first. Notes keep the tick and length
  they were heard at (the pad's stamp less the output latency, to the
  nearest tick, not where the playhead has got to by the time the MIDI
  thread sees it); with **Rec Quantize** on they land on its grid (a note
  moved ahead of the playhead is not played twice). Shift + Pad 10 opens
  REC QUANTIZE: B3 turns it on or off, K1, the wheel or −/+ pick the grid,
  the pattern's Q or one of its own (1/4 .. 1/64 with triplets), kept with
  Melodee's settings; B1 back, the pads play. Patterns' header names it
  (REC Q 1/32T, REC Q OFF). Overdub. A take is one undo.
  While the song plays, a pattern launched is written as a clip.
- **Step Edit** (Shift + Seq, under the editor): step entry, a pattern
  typed in with the transport stopped. A caret stands on a step of Q
  (shared with Steps and Roll); play a pad or a chord and it is written
  there, heard as it goes in, and when the pads let go the caret moves on;
  B2 REST moves it on without writing, B3 ERASE steps back and takes that
  step out, the wheel moves it without writing (all round the pattern).
  K1–K3 are what a pad writes: its length in steps, velocity (Played: as
  hit) and chance; Shift + K1 the pattern's length, Shift + K2 Q, Shift + K3
  the time signature; −/+ the octave (a kit's bank). Sixteen steps round
  the caret show each step's notes as bars (lower keys lower, taller for
  harder), and a panel the pads held (else what the step holds) and the
  step the caret goes to. A chord on a step is one undo. Erase + pad asks,
  then erases that pad's events. Push, Seq or Shift + Seq go back
  to the editor.
- **Roll** (B2): the pattern's notes, each at its own tick for its own length, over bar
  lines, any number to a step. The cursor is a step of Q and a pitch: the
  wheel goes through a step's notes one by one, then on to the next step
  (low to high going on, high to low going back; an empty step is one
  detent and keeps the pitch), −/+ walk the chord in the step and then a
  semitone; Shift + wheel marks notes one by one along the same walk
  (header n SEL, marked notes outlined). With Shift let go the marked notes
  take the controls: the wheel moves them a step of Q (hopping over notes
  in the way), Shift + wheel moves them a row, Erase deletes them, Mute
  mutes them (drawn hollow; kept but not played or exported; Mute again
  plays them), Note Repeat steps their ratchet 2, 3, 4, 6, 8, off, and
  K1-K3 set length, velocity and chance on all of them, Shift + K2 their
  velocity ramp (VEL RAMP, the last note this far off the first), Erase +
  wheel pulls them a quarter of the way to the grid each detent, − / + move
  them an octave, and Shift + Erase copies them (header COPY: Push pastes
  them at the cursor, the copies marked; Erase leaves). A Shift tap clears
  the marks. On Roll, Steps and Step Edit the printed buttons act on the
  marked notes, else the note in hand: Chop splits each note at the grid
  line nearest its middle, Shift + Chop holds each note until the next
  starts (legato; on a kit, until that pad's next hit), Shift + Loop reverses
  them (with none marked, the notes in view), Shift + Note Repeat writes the
  track's arp (Stage's ARP mode, rate, octaves and gate) over each chord as
  notes (with none marked, the chord in the cursor's step; keys only), and a
  Loop tap plays only the bars in view (the marked notes' bars) over and
  over, Loop lit and header LOOP, until Loop again or the editor, track or
  pattern changes;
  the view scrolls a step at a time, and dots on its edges mark notes above
  and below. It opens on the pattern's notes: when none lies in the rows it
  would show, the cursor's pitch goes to the note nearest its step. The note at the cursor is in focus, drawn white and named in
  the header; K1 its length, K2 velocity, K3 chance, Shift + K1 NUDGE (how
  many ticks it sits early or late of its nearest grid line; the cursor
  follows it) (ROLL again: its ratchet, drawn as its hits along the note). A pad held on the note in
  focus turns the cards to locks on it, as a step held on Steps (header
  LOCK; a locked note has a square at its start). A pad writes its note
  at the cursor (one step of Q long) or focuses it if it is there; Pad Bank
  is the pads' octave (Shift: down). Erase tapped alone deletes the note in
  focus; Erase + pad asks, then erases that key's notes. Push opens its
  sheet: accent, slide, tie, delete as cards (`ui::card_sheet`), accent
  lit; the wheel moves, Push does the card (the switches stay open to see
  them change), B1 back; a pad or a pot closes it too. Accent plays the note about 4 dB louder (and 24
  velocity harder); Tie holds it until the next note, which goes on from it
  without a new attack (the same key: one long note); Slide does the same
  and glides the pitch there in about 60 ms (the GLIDE knob's rate when it
  is up). A synth (a PLUGIN track) is told: the key it goes on from as CC 84
  (portamento control) just before the next note, a tie with the legato
  pedal (CC 68) held over it, as Melodee Poly, PD and Chip take them; the
  accent as a velocity 1.6 times as hard. The sampler plays the next note and
  releases the held one; MIDI out overlaps them, which a mono synth plays legato. With Seq Record on while playing, pads play in
  live. On a kit the rows are its pads, a lane
  each, the kit's bank in view and numbered as the pads are (every other
  lane, the cursor's always); it opens on the last pad's lane.
- **Steps, Motion** (B1, B3): Steps the bar's steps
  on the pads, drawn as a drum machine draws a bar: sixteen columns in four
  beats, each stacking its notes by pitch, taller for harder hits (a
  ratchet as its hits side by side, each as tall as it plays), a dot
  over it for an accent, an arc under it for a tie, the numbers under them
  and the pattern's bars as blocks at the foot; the step in hand is the
  header's. On a kit the pads are the last pad's lane: a tap sets or clears
  that pad's note and leaves the others' (Roll edits the whole kit), the
  cards are that pad's note in the step, and
  the wheel moves a held step's note to the next pad, the lane with it.
  Shift + K1 there is the lane's own **length** (LANE: 1 step to the
  pattern's, All): a shorter lane loops on its own from where the pattern
  began, polymeter inside the kit, and counts its own passes for trig
  conditions; Steps darkens what lies past it, its playhead goes round with
  it and the header names it (LANE 6). Files keep lanes in LANE.
  On a kit STEPS a third time (after the TRIG row) is the **EUCLID** row of
  the last pad's lane: K1 hits, K2 steps (1–64), K3 rotation; each turn
  lays that rhythm on the lane at once as notes a step of Q each (its old
  notes go, one undo), the lane looping over its steps on its own; the
  header names it (E(5,8)) and EUCL keeps it with the pattern. **Push** opens
  **VARY**: Thinner (the weakest notes by where they fall in the bar and
  how hard), Busier (a kit: softer hits beside a lane's; a line: passing
  notes in the project's key), Humanised, Reversed, Octave up and down (not
  on a kit); the wheel picks, K1 the amount of the first three, B3 DO IT or
  Push makes it into the next empty pattern, which plays from the next bar
  (one undo); B1 or a pad backs out.
  **Held, a step is locked**: while a pad is down the cards are the Motion
  page's three lanes, each showing its lock on that step (else the track's
  own value), and a turn writes a lock there, for as long as the step's
  longest note on its lane (a step at least); a locked step has a square
  at its top. Motion the lanes of recorded pot moves (Seq Record + a pot
  writes at the playing tick; a kit pad's level, tune, pan, envelopes,
  filter and more are lanes of that pad; Erase + pot asks, then clears the
  lane, locks too), all three on one large graph in their knobs' colours,
  the one in hand heavy, each lock a short raised bar at its value for as
  long as it holds; the cards are its legend. Motion is each pattern's own
  (up to 8192 points, at tick resolution): it goes with the pattern when it
  is copied, doubled, halved or sped up, and a lock's lane goes back to what
  it holds as recorded (else the track's value) when the lock ends.
- **MOD: modulators and macros** (roadmap P4): MOTION again on Motion
  (lit) opens MOD. B1 MOD: eight modulators, each an LFO (Sine, Tri, Saw,
  Ramp, Square, S&H, Drift; free from 0.02 to 20 Hz, or on the song's
  clock from 1/16 to 8 bars, its cycle locked to the bars) or a follower
  of how loud a track or bus is after its chain (rise 1–200 ms, fall
  10 ms–2 s), each moving up to four lanes of any track: an engine
  parameter, an effect of its chain, a kit pad's own value, its level,
  pan or sends, the master's chain or a return. K1 which, K2 rate (a
  follower's rise), K3 the target in hand's depth (±100 % of the lane's
  range: an LFO swings half each way, a follower pushes it one way);
  Shift + K1 Off / LFO / Follow, Shift + K2 the shape or the source,
  Shift + K3 Free / Sync or the fall. B3 ADD puts the lane a pot turned
  last (anywhere) on it; the wheel picks the target in hand, Push takes it
  off. A modulator swings a lane around where it stands, as set or where
  Motion has it, every block; Off, the lane goes back there. B2 MACRO:
  eight macros, each one knob (K2) turning up to eight lanes, each from
  its low (Shift + K3) to its high (K3); ADD takes the lane learned last
  from where it stands to the far end of its range. A macro's turn sets
  each lane as its own pot would (Motion records the track in hand's).
  The picture: the wave with where it is now (a follower's level) and the
  targets as rows (track chip, lane, depth); a macro's value as an arc
  and its targets four at a time. Settings are part of undo with the
  master's; files keep them in MODS and MACR.
- **The mix in Motion** (roadmap P4): with Seq Record on, a track's LEVEL,
  PAN and SEND A–D turned on Stage, Tracks or Sends write lanes of the
  selected track's pattern, and so do the master chain's parameters (FX
  Select's MASTER) and the buses' RETURNs: those land in the selected
  track's pattern too, and while it plays they move the master's values
  for the whole mix. They play as other lanes (the strip eases there as a
  turned knob does), show on Motion's graph as LEVEL, SEND B, MASTER WIDTH,
  RETURN A, and go back where they were set on Stop and at the pattern's
  start, as every lane does.
- **A pattern's feel** (roadmap P1): how its notes play without rewriting
  them, heard at once and saved with the pattern (FEEL): STRENGTH (0–100 %
  toward the nearest step of its Q, its SWING 50–75 % on every second
  step of a straight Q), HUMANIZE (each note early or late by up to a
  quarter of a step) and HUMAN VEL (softer or harder by up to 20), each a
  fixed offset a note so the pattern plays the same pass after pass, and a
  GROOVE taken from another pattern (any track's: how far each of its
  sixteenths falls off the grid and how hard it plays against its mean,
  folded onto 64 steps at most) laid on its notes by AMOUNT. The audio
  thread plays the pattern with its feel laid on (`feel_pattern`); Steps,
  Roll and the file keep the notes as written. Time Correct's page holds
  the cards (`feel_cards`).
- **Time Correct** (Shift + Pad 14): heard live, no commit. The selected
  pattern plays as close to its Q as K2 STRENGTH says, the second of each
  pair late by K3 SWING, as they turn; turning down takes it back (the
  notes stay as written: the pattern's feel). K1 Q; Shift + K1 HUMANIZE,
  Shift + K2 its velocity, Shift + K3 the groove's amount; a pad takes the
  groove from that pattern of the track (again: none). B3 QUANTIZE, lit
  while the feel is heard, takes it off to compare by ear and puts it
  back; B2 ALL lays this feel on the track's other patterns (each one
  undo); Shift + Pad 14 again closes it. Two beats drawn over the grid of Q: each note where it is
  written (a ring) and where it plays (a dot), the swung steps where they
  land; under it the track's patterns, the groove's lit.
- **Half Seq, Double Seq, Half / Double Speed** (Shift + Pad 2, 3, 6, 7):
  each one undo; the length stays for the speeds. Settings' Double Seq row
  picks what the new half holds: Copy notes (the MPC's way, motion too) or
  Empty.
- **Recall** (Shift + Seq Record): no page. Roadmap P5, always-on
  capture: every note played on any track (the pads, Keys, MIDI in on its
  channel) is kept, Seq Record on or not, the last minutes of it (8192
  notes, in a ring made at start; the audio thread never sees it). Played
  while the transport runs a note keeps the tick it was heard on, as Seq
  Record places it; played stopped, its time. Recall lays the last phrase
  (back to where no track played for two bars, up to 64 bars) into
  patterns: every track that played in it, each into its next empty
  pattern, which then plays, the same bars for all so they line up; one
  undo for all. Played while running, the bars are the transport's, from
  the bar the phrase began in to the bar after its last note; played
  stopped, its first note is the downbeat, at the tempo now. The page
  stays, and the header shows for a few seconds what landed (✓ P3 for the
  selected track, +2 for the others) and the undo keys (Shift + −). How
  many bars (Auto, the phrase; or 1, 2, 4, 8, 16, 32 back from the bar
  after the last note) and how they go on the grid (as played, 1/16, 1/8)
  are Settings rows behind the PADS door, beside Double Seq; bars follow
  the time signature. With Note Repeat on, the presses are kept, not the
  repeats. Nothing played, or no pattern empty: a notice, and nothing
  changes. `status`: `melodee_capture` (notes kept).
- **Song** (Shift + Pad 12): a lane per track, clips of patterns, markers,
  a loop brace, the cursor. K1 zoom (4–64 bars), K2 / K3 the next clip's
  pattern and length (or the clip's under the cursor); wheel by the bar,
  pads pick the lane, −/+ jump markers. Clip jobs are on buttons, each lit
  only when it can act on what the cursor is on:
  **B3** Place (on a clip, Duplicate after it), **B2** Mute / Unmute,
  **Shift + B2** its Motion, **Shift + B3** its Own Pattern, **Push** opens
  its notes (Roll, or Step Edit on a kit; on empty time Places), **Chop**
  splits at the cursor, **Erase** deletes, **Note Repeat** repeats to the
  next marker, **Loop** loops its bars (again, or on empty time, off). Each
  is one undo. Shift + Push adds a marker
  (on a marker, names it). Play here plays the song from the cursor.
  **Tempo lane** (roadmap P5): Shift + K1 BAR TEMPO sets the tempo from
  the cursor's bar (with no point there the card shows the lane's tempo
  and a turn adds one; under 30 BPM, Off, takes it away), Shift + K2 SHAPE
  a Step on its bar or a Ramp, even across the bars from the point before
  (the first from bar 1 at the project's tempo); 64 points, one undo a
  turn, kept in TMPL. Before the first point the project's tempo plays.
  With points the lane is an amber strip over the tracks (one track lane
  fewer), its tempo at the cursor beside it, and the header's clock counts
  through it. While the song plays the audio thread works it out a tick at
  a time, so everything on the clock follows: the delays' SYNC, LFOs on
  the clock, the arps, the click and count-in, MIDI clock out, Seq
  Record's places, a mixdown and its stems; a MIDI file carries it as
  tempo changes (every sixteenth a ramp moves); MTC in places the song
  through it. MIDI clock in still leads; BPM per pattern waits while the
  song with a lane plays. **B1
  Export**: B2 audio mixdown (Name sheet; once through, tails up to 10 s,
  `Melodee/Mixdowns/<name>.wav`, 32-bit float at the device's rate so a mix
  past full scale is kept, not clipped; Stop drops it), B3 New pattern
  (the song laid end to end into the first free pattern number). Roadmap
  P4: the wheel picks what B2 makes: the mixdown as 32-bit float, 24-bit
  or 16-bit (integer PCM, TPDF dithered, a step's worth: the only files
  Melodee dithers; float needs none), STEMS (`Melodee/Stems/<name>/`: the
  mix as `Mix.wav` and each track with a sound, `NN <name>.wav`, as it
  joins the mix after its chain, level, pan and mute, and each bus a
  track sends into, after its chain at its return; all 32-bit float,
  the same frames, in one real-time pass), or a MIDI FILE
  (`Melodee/MIDI/<name>.mid`, format 1 at 96 ticks a quarter, the tempo
  and time signature first: the song's tracks as they play it, or with no
  song each track's pattern once; a kit's pads as notes 36 on, on channel
  10, a MIDI track on its own). A `.mid` in the browser (a note glyph,
  MIDI) loads with Push: every note onto the selected track's pattern
  playing, as many bars as they take (128 at most), one undo; a kit takes
  notes 36 on as its pads.

### Print and freeze

A track can be frozen: each of its patterns is printed to an audio file
and the track plays the prints instead of running its engine and its
chain, which saves CPU. The pads still play the sound live (the engine
wakes while notes sound and sleeps again a second after it falls quiet).
Level, pan, sends, mute, solo and punches stay live. Unfreezing is at
once and keeps the prints.

**PRINT sheet.** Sample Record on a PLUGIN or MIDI track opens it. On a
kit, Sample Record opens Record as before; B3 PRINT there opens the same
sheet. The pads are the track's patterns of the bank in view
(Minus / Plus: the bank): lit ones are printed (a snowflake in the
corner, the one playing ringed), a tap leaves one out or takes it back,
B2 ALL lights every pattern with notes (again: none). K1 TAP: Post FX
(engine and chain) or Pre FX (the engine alone; the chain runs live on
the print). K2 TAIL: Loop (two passes printed, the second kept, so a tail
wraps into the start), Ring (one pass, then its tail until it falls
silent, 10 s at most) or Cut (one pass). K3 is empty. Beside the pads:
how many patterns are lit, what the prints take on the card, and the
share of a block the track takes now (what freezing saves). B1 CANCEL,
B3 FREEZE (Push too), Shift + B3 DEEP FREEZE (below).

While prints are made the header shows PRINTING n/m, each pad its print's
progress (a ring filling; done ones solid with a snowflake), and the
column the speed against real time and the time left. The track plays
live until its last print is ready, then switches on a bar line and the
sheet closes ("Track N frozen"). B1 calls the prints off and nothing
changes. A MIDI track's sheet says MIDI TRACKS: Later (its prints need
its synth's audio back in real time) and B3 does nothing.

**Seq held + Sample Record.** While Seq stays down, each pattern pad
freezes or thaws its pattern of the selected track. The Sequence sheet
shows PRINT, the frozen patterns ice white with a snowflake, FROZEN n of
m and CPU. On the release of Seq the prints are made and the track
freezes with those patterns (none left: it plays live again). Patterns
not frozen play live.

**A frozen track.** Tracks shows a snowflake in place of its engine and
its strip; the header's CPU chip is the share of each block all tracks
take. Stage (its sound pages; MIX and SENDS stay live) shows the print of
the pattern playing with its playhead, FREEZE and PADS PLAY LIVE, and
what the print holds (tap, tail, bars). Cards whose values a print holds
are dim with a snowflake on every page. The Sequence sheet marks frozen
patterns with a snowflake; with Seq held their pads light white.

**Deep freeze.** Shift + B3 DEEP FREEZE on the PRINT sheet, or on a
frozen track's Stage, freezes the whole track (every pattern with notes,
whatever is lit, at Post FX) and then unloads the track's plug-in and the
plug-ins on its chain, which saves their RAM as well as the CPU. They are
let go once the print plays and the engine has been quiet for a second
(8 s at most). The pads play nothing and stay dark, MIDI keyboard notes
and the arp too; a pattern without a print is silent. The glyph is the
snowflake on a filled ice tile, on Tracks, Stage, the Sequence sheet and
the PRINT sheet; Stage shows DEEP FREEZE and PADS OFF. Seq held + Sample
Record does not pick patterns on a deep-frozen track. A plug-in a bus or
the master also uses stays loaded. Built-in chain effects and a kit's
samples stay in memory.

Unfreezing asks "Unfreeze track N?" as for a freeze. On B3 the track
thaws: Stage shows THAWING while the plug-ins load again, off the audio
thread, with the state they had when they were unloaded; the prints play
meanwhile. Once every plug-in has been open for 1.5 s (time for work a
plug-in does after loading its state, such as TONE3000 preparing its
models) the track plays live ("Track N live"). One that does not open in 30 s, or fails, goes live anyway and
says so.

A project saved with a deep-frozen track keeps that (FRZN state 2). Opened,
the track's plug-ins are not loaded at all; their saved state stays in the
file as it was, so a thaw later restores it exactly. Files from before deep
freeze open with a normal freeze.

**Editing a frozen track.** Anything a print is made of (its sound and
plug-in's values, arp and chords, a kit pad, its chain at Post FX, a
frozen pattern's notes, motion, length, Q or swing, the track's speed)
asks "Unfreeze track N?" first: B3 UNFREEZE, B1 CANCEL. A turned card
asks before it changes; any other edit is put back as it was and asks.
Seq Record and a Sample Record take on a frozen track ask the same.
Unfrozen, edit, then freeze again: patterns whose notes, sound and chain
did not change keep their prints, so only the edited ones are printed
again.

**How prints are made.** A print host runs beside the live one on a
spare core at the lowest priority, with the threads its plug-ins start
(`shell/print_engine.h`), one pattern
at a time, faster than real time, into `<project> Prints/Track N A1.wav`
(32-bit float at the device's rate). With effects on its chain a track
is printed at its own level and pan, as the chain hears them live (a
Compressor, Drive or Tape reacts to the level it is fed); playback then
takes the level and pan relative to those. A track with an empty chain
is printed at unity and centred. Each print has a record (PRNT) with a
hash of what made it; a freeze (FRZN) keeps its tap, tail and patterns
with the project. Save As copies the Prints folder beside the new file.
Not handled yet: Knob FX on kit pads, a Sidechain key pad on a frozen
track; chance and random trig conditions print once.

Remote: `melodee-print TRACK [patterns] [pre|post] [loop|ring|cut]`,
`melodee-print status|cancel`, `melodee-freeze TRACK [patterns]
[pre|post] [loop|ring|cut] [deep]`, `melodee-freeze [status]` (frozen,
deep and thawing tracks, the last thaw in ms, the plug-ins with `asleep`
for unloaded ones), `melodee-unfreeze TRACK` (a deep-frozen track thaws
first).

### Sampler

A kit has 128 pads in eight banks (A–H), each a region of one of the
project's 64 samples. Samples are WAV (16, 24 or 32-bit PCM or float), MP3
or FLAC (any rate, up to 60 s; an MP3's encoder delay and padding, from its
LAME tag, trimmed), read on the service thread and handed whole to the audio
thread. Each is kept at 44.1 kHz (the unit a project counts frames in) and,
at 48 or 96 kHz, also as a copy at the device's rate made once as it is
read: from the file itself, its own frames when the rates agree, else one
band-limited conversion (the base's windowed sinc). A pad plays the copy a
frame each frame, only its pitch moving the read; nothing converts per
voice. The copies share a 256 MB budget; past it, and for instruments
streamed from the card, a voice reads the 44.1 kHz frames that much
further each frame, as before. A rate change makes each copy again from
its file, a sample at a time. Takes, Audio Recall and Resample are kept as
they came, at the device's rate, never converted on their way in: saved at
it, their own frames are what the pad plays, and their 44.1 kHz frames,
made once, are what the project counts (a region, a cut). Save Sample,
Trim and Extract cut a sample's own frames when those are what plays, so a
48 kHz take trimmed stays 48 kHz. Loops are taken and saved at the
device's rate too (LOOPER.md).
These are saved 24-bit, or 32-bit float when they go past full scale (a
resampled mix can), so a file never clips what played; what plays is what
the file holds, to the bit.

- **Samples** (Sample Select on a kit): the browser, each row a glyph
  first (a folder with its chevron, a sample file, a kit, an instrument's keyboard,
  a bank, an expansion's arrow) and its facts last (the format, the pad a
  sample is on as a chip; MP3 or FLAC on a sample); K1 jumps. MPC drum programs
  (KIT) first, then instruments (XPM, XTY, SFZ), then folders, then files;
  an expansion's `[Previews]` and a program's own `_[TrackData]` are not
  listed; wheel and −/+ move, Push opens a folder or loads onto the last pad
  (keeping its settings), B1 back out, B2 the drive (CARD from `Samples`,
  or INTERNAL: the factory categories and the stock app's User folder, read
  only), B3 PREVIEW (its LED) plays each file as the cursor lands (its row
  fills in the track colour as it plays, with the seconds heard of its
  length on the right). A preview starts once its first eighth of a second
  is read, however long the file: the sample is laid out at its full length
  (up to 30 s) and read in a piece each service pass while it plays
  (`SampleFill`, `AudioReader`), and the voice reads only what has landed
  (`SampleGrowth`), holding silent at that edge should the reading fall
  behind; Shift + pad loads onto the
  next empty pad. Melodee writes only to the card. On **Pads** every pad that
  sounds lights, with a head through its waveform as far as its newest voice
  has played through its region.
- **SFZ instruments** (`.sfz` in the browser, tagged SFZ): load onto a pad as
  a WAV does; the pad keeps all its controls and its note and velocity pick
  zones (up to 512; round robin, layers, `group` / `off_by`, each zone's own
  filter and envelope as offsets from the pad's). Roadmap P3: release samples
  (`trigger=release` or `release_key`) play when the key is let go, at its
  press's velocity, `rt_decay` dB quieter a second it was held; crossfades
  (`xfin_lokey` / `hikey`, `xfout_lokey` / `hikey`, the same over velocity,
  `xf_keycurve` / `xf_velcurve` power or gain) set each zone's level where
  ranges overlap; keyswitches (`sw_lokey` / `sw_hikey`, `sw_last`,
  `sw_default`): a key in the switch range sounds nothing and picks the zones
  that play from then on (the default first). A hit plays key 60 plus the
  pad's Semi. **16 Levels** on the pad plays it: Tune or Scale for notes,
  Zones its keys that sound one a pad, Velocity through its layers; while 16 Levels is on it, a MIDI keyboard plays
  it at real pitch. On such a pad Stage's TRIM is **ZONES** (round 3
  "instrument C"): a tile a zone over a keyboard of its range (keys across,
  velocities up, a dot at each root), its format, zones, files, velocity
  layers and round robin above, the zone in hand outlined with its file,
  keys, root and velocities under it. The key played (a pad, 16 Levels, a
  MIDI keyboard) picks the zone in hand, as a hit picks the pad; K1 walks
  them; K2 TUNE (-100..100 cents) and K3 LEVEL (-24..+12 dB) trim that zone
  on this pad without touching the file (one undo a turn, as the pad's other
  values; up to 64 a track, kept in the file's ZTRM; a trim made on another
  instrument is not heard and is let go at the next trim or save). A file
  still being read is an outline (a ring counts them), a silent one amber,
  and what it asked for that is not played is named in chips (with the
  regions left out). AMP ENV, FILT ENV, TUNE, FILTER and KIT FILT keep a
  strip of the same keys on top, the key a hit plays lit, and draw their own
  picture under it (the envelope, two octaves round that key with C4
  outlined once Semi moved it, the filter); the other kit pages draw the map.
  Samples may be WAV, FLAC, Ogg or AIFF; a
  loop may come from the region, a WAV's `smpl` chunk or one kept in a FLAC.
  The first load decodes each file once into `Melodee/Cache`; after that
  opening is near instant and audio streams from the card (a third of a
  second of each zone's start stays in memory, `engine/stream_pager.h` pins
  the rest ahead of each voice, so the audio thread never waits on the card).
  Chop, Trim Sample, Save Sample, Normalize and Warp refuse on it; Cloud stays
  in the held start. SD card access lets go of instruments and reloads them
  when the card comes back. An SFZ takes one of the project's 64 sample slots.
- **MPC keygroup programs** (`.xpm` in the browser, tagged XPM): load and
  stream as an SFZ does. Each keygroup is a key range and each of its layers a
  sample over a velocity range, found as its SampleName with `.WAV` beside the
  program (any case). Carried over: root, tune, layer volume (its gain) and
  pan, the keygroup's volume and tune, start, end and forward loop, one shot,
  keytrack off, Cycle (a layer a press) and Random, and the amp envelope.
  The program's own volume and tune go over every keygroup, and its Velocity
  Sense of 0 (an organ's) plays every press full. A note-off keygroup
  (TriggerMode 1) plays its layers as release samples, on the key let go
  (roadmap P3). Named, not played: filters, reversed loops. A layer naming a sample the folder does
  not hold (expansions keep some from their full version) is left out and
  counted on TRIM, not called an error; a program with none found is.
- **MPC 3 programs** (`.xty`, an MPC 3 track, tagged XTY; and an `.xpm`
  MPC 3 saved, gzip around JSON): read as `.xpm` ones are, keygroup or drum,
  their samples in `<name>_[TrackData]` beside them.
- **EXS24 instruments** (`.exs`, Logic's EXS24 and Sampler, little- or
  big-endian, tagged EXS): load and stream as an SFZ does. Each zone is a key
  and velocity range playing one sample. Its samples are found as Logic finds
  them: beside it, then in the folder it was saved from (that path's last
  parts under the instrument's folder or the two above), then by name
  anywhere under those. Carried over: root, coarse and fine tune, Pitch off
  (keytrack off), volume and pan (the zone's and its group's), start, end,
  forward loop (Play to End on Release looping while held), one shot, the
  group's velocity and key ranges, exclusive groups as chokes, round robin
  chains, the instrument's volume, transpose and tune, ENV1 as the amp
  envelope and Level via Vel as velocity. Groups picked by something held (a
  keyswitch, a controller, a channel, an articulation, a tempo) keep those at
  rest, the lowest of each kind. A release-trigger group plays on the key
  let go (roadmap P3). Left out and counted on TRIM: muted groups, missing
  samples. Named, not played: the filter, reverse, backwards loops, loop
  crossfades, random sample select.
- **SoundFonts** (`.sf2`, tagged SF2; roadmap P3): listed as a folder; Push
  opens it, its presets inside it by bank and program (those with nothing to
  play left out), and Push on one loads it onto a pad as an SFZ does,
  `<file>.sf2#<preset>`. The file is mapped, never read whole: only the
  chosen preset's sounds are taken out, one a pass, into `Melodee/Cache`,
  then streamed as an SFZ's are. Carried over: key and velocity ranges (the
  preset's over its instrument's), root (overriding or the sound's own),
  coarse and fine tune with the sound's correction, scale tuning, attenuation
  (at the 0.4 the hardware took), pan, start, end and loop offsets, loop
  modes (on, and until released), exclusive class as a choke, the volume
  envelope (hold into decay, sustain in dB) and the filter's cutoff and
  resonance; two sounds panned hard apart, alike in all else, play as one
  stereo sound. 24-bit sounds (`sm24`) keep their low byte. Named, not
  played: modulators, the LFOs, the modulation envelope, envelope delay. A
  packed file (`.sf3`) is refused. JUCE and inMusic's open-source packages
  have no SoundFont reader, so Melodee reads the format itself.
- **DecentSampler presets** (`.dspreset`, tagged DS; roadmap P3): load and
  stream as an SFZ does, its XML read with JUCE's XmlDocument: each
  `<sample>` under its `<group>` under `<groups>`, each level's attributes
  over the one above; root, note and velocity ranges, tuning (and the
  group's and global's), pitch keytrack, volume and pan (linear or dB, the
  three levels and tags added), start, end and loop, round robin and random
  sequences, the amp envelope, tags as chokes (`silencedByTags`), and
  `trigger=release` samples on the key let go. Left out and counted: groups
  or samples disabled, a tag turned off, a legato trigger, a controller
  where it does not start. Named, not played: effects, modulators. The
  preset opens as its own screen shows it: each knob's, button's and menu's
  starting value goes through its bindings (volume, tuning, pan, velocity,
  the amp envelope, sample start, end and loop, groups and tags on or off),
  so a start set past a file's silence plays from there; a layer at volume
  0 is left out.
- **MPC drum programs** (`.xpm` or `.xty` of type Drum, tagged KIT): Push loads the
  program as the selected kit track's kit, as Load Kit does (one undo), each
  instrument onto its pad (1 is A01, 17 is B01). A pad of one layer plays its
  WAV; one of two to four layers takes them as its own pad layers (roadmap
  P3: Velocity, Cycle or Random as ZonePlay says, each layer's file, region,
  velocities, tune, volume and pan) while the project's free sample slots
  hold every layer's file; one of more, or one the slots no longer hold,
  plays its layers as an instrument named `<program>.xpm#<pad>`, streamed
  from the card as an SFZ is, as before. MPC projects (`.xpj`) alike. Carried over: the pad's volume, pan, tune, play type (One
  Shot, Note On), mono, mute group, envelope, velocity, filter (a one-pole as
  the two-pole kind) and filter envelope; each layer's tune, volume, pan,
  region and loop; the pads' colours (the program's ProgramPads: each pad's
  RGB, or the one Universal colour on every pad; pads that follow the track's
  colour keep Melodee's), raised to full brightness so the rungs still read
  (`mpc_pad_colours()`, `PadSound::colour`, saved in PCOL, a bank's BCOL).
  Shift + pad on it says Push loads a kit.
- **MPC expansions** (`.xpn`, tagged INSTALL): Push installs one onto the card,
  a file a pass, into `Samples/Expansions/<its title>` (from its
  `Expansion.xml`), then opens that folder; a file already there at its size
  is kept, so installing again only finishes what was left. B3 PREVIEW on a
  program plays its own preview, `[Previews]/<program file>.mp3`, when the
  expansion has one (JUCE's MP3 decoder); else a keygroup plays the zone
  nearest middle C and a kit stays quiet.
- **Banks** (kit tracks only): one bank of a kit's sixteen pads as a file.
  **Save Bank** is Shift + Sample Select on Samples: the Name sheet (the kit's
  name and bank letter first), then `Samples/Banks/<name>.mldb` with every
  pad's values, its own chain and the paths of the files it plays (nothing
  is copied). Bank files show in the browser tagged BANK; **Push** on one
  loads it into the bank in view (Pad Bank picks it): its sixteen pads and
  their chains replace that bank's, their mutes cleared; the other banks,
  the kit's own values and the patterns stay. Each file takes a sample slot
  once; a pad without a free slot stays empty and is counted. One undo.
- **The eleven sample pages**, in Stage's kit groups: **B1** Trim → Layers →
  Tune → Amp Env, **B2** Play → Follow → Cloud, **B3** Mix → Filter → Filt
  Env → Kit filter. A B key goes round its group, or opens its first page from
  elsewhere; a dot a page under each key, as on Stage. **Push** opens the
  page map (three columns; wheel, Push jumps, a B key or Shift + Push
  closes). Trim itself lives on Stage (a kit's
  B1 TRIM): every way to Trim (B1 here, Chop again, Sample Record on
  Record, Shift + Pad 13) lands there.
  - Trim: K1 start, K2 end, K3 loop point; Shift + K1–K3 zoom at that
    handle; Shift + B1 Loop Lock; Shift + B2 Normalize (a gain, one undo).
    On a pad holding an instrument it is ZONES (above, SFZ instruments).
  - Layers (roadmap P3): up to four samples on a pad. Layer 1 is the pad's
    own sample and region; layers 2–4 each play another sample (its whole
    file, or the region an MPC program gave it) with a tune and level of
    their own over the pad's. A hit plays every layer its velocity reaches
    (**Velocity**, the MPC's default: two ranges that overlap both play),
    the next layer in turn (**Cycle**, round the layers that hold a sample)
    or one at random (**Random**). Everything else (envelope, filter, play,
    loop from each layer's own start, warp, Pad FX) is the pad's, over
    every layer. K1 the layer in hand, K2 / K3 its lowest and highest
    velocity (Velocity only: the other end moves along rather than cross);
    Shift + K1 Velocity, Cycle or Random, Shift + K2 / K3 the layer's tune
    (±24 st) and level (−48..+12 dB; layer 1's are Tune's and Mix's).
    Shift + B1 **LOAD** opens Samples, whose Push loads the file onto the
    layer in hand and comes back (one undo; a layer keeps its tune, level
    and velocities when its sample changes; an instrument is refused);
    Shift + B2 **CLEAR** takes the layer off (layer 1 is the pad). The
    picture: a row a layer, the one in hand outlined, one heard tinted, each
    with its sample's name, its offsets, and on the right its velocity range
    across 1–127 (K2 and K3 its ends) or, under Cycle and Random, a dot a
    layer with its own turn lit. The header pill says how a hit picks. A
    pad playing an instrument (SFZ, keygroup) has its zones instead. Kept in
    the file's `LAYR` section and bank files' `BLSM` / `BLYR`.
  - Mix: K1 volume in dB (−inf, −74..+6) with its meter, K3 pan, Shift + K2
    kit volume.
  - Amp Env: attack, decay (One shot) or release (Note On), velocity
    sensitivity; Shift + K2 decay from start or end; row 2 the rest.
  - Tune: semitones, cents, Warp (Off, 50–200 %, Seq from # Beats; Shift +
    K3 # Beats). Shift + Pad 15 picks Time stretch or Pitch, which matters
    with Warp on: Time stretch holds a tuned pad or a higher note to Warp's
    length, Pitch moves pitch with length and turns Semi and Fine off. With
    Warp Off a higher note plays faster and shorter (Follow's PITCH Stretch
    keeps the length for the whole kit).
  - Play: Mono / Poly, mute group 1–16, offset; Shift + K2 Pad Link; row 2
    Note On, reverse, loop. Pad Link chains (roadmap P3): a linked pad's own
    link plays too, and so on through the bank, each pad once (a chain that
    comes back round stops there; a muted pad is silent but links on).
    Shift + K3 (either row) **XFADE** (roadmap P3): the loop's seam
    crossfaded over 0–250 ms, the same time at any rate: as the end nears it
    fades into what lies before the loop point (reversed: past the end);
    with nothing there, just after the turn out of what follows the end;
    with neither (the whole file looping), a dip to nothing and back over
    that time. Off by default, so a loop already seamless stays as it was.
  - Filter (Off, Classic, LPF2/4, HPF2/4, BPF2/4) and Filt Env (up to seven
    octaves; Shift + K2 decay from).
  - Melodee's Follow (the kit keeps the project's tempo: Beats, Tones or
    Tape), Cloud (pads as grain clouds) and Kit filter (one filter after
    every pad).
  - Each page draws the one thing its knobs change, in their colours, over
    the shared kit picture: TRIM a flag for start, end and loop (the
    kept length at the bottom right, the file's after it, dim; Shift + K1-K3
    ZOOM START, END, LOOP; Shift + B1 LOOP LOCK with its LED); AMP ENV
    attack and decay as coloured segments with handles and the softer hit's
    envelope dashed (K3, velocity); TUNE a pitch ruler, two octaves each way;
    FILTER and KIT FILT their response in a band of their own under the
    wave; CLOUD POSITION as a line, SIZE the band round it, Spread a paler
    band, Scan an arrow, its place in seconds at the line and its LED in
    the header.
- **Loop, Shift + Loop, Shift + Chop** act on the last pad from any page;
  the sample pages' title shows note, loop and reverse icons.
- **Chop** (Chop): the last pad's region in slices that touch. Pads play and
  pick slices; K1 start, K2 end, K3 type (Threshold, Regions, Beats,
  Manual), Shift + K3 its amount, Shift + K1 / K2 zoom. Threshold keeps the
  hits at least that strong: a hit is a change in the spectrum (a drum, a hat
  over a held pad, a chord change at the same loudness), cut at the quiet
  point just before its attack, its strength against the sample's strongest
  hit (`engine/onsets.cpp`). Beats cuts every 1-8 of the sample's own beats,
  counted from its beat nearest the region's start (a lead-in before it a
  slice of its own), or from where its steady beat starts when that is later
  (a free intro before it, and a free ending after its last beat, one slice
  each): its tempo, beats and where they fall are found when it
  loads (the flux's autocorrelation, a beat tracker, the phase where the flux
  is highest beat after beat; a loop's tempo exact from its length, and a
  loop cut to bars that starts with an attack starts on a beat), the tempo
  shown at the picture's top right; each cut takes the tracked beat within an
  eighth of a beat (so a take that drifts is followed), else the hit within
  30 ms. Which beat is a bar's 1 is the region's start: kicks and snares alone
  did not tell it apart. A sample with no steady pulse cuts at the project's
  tempo.
  Manual (K3 or Push):
  pad 1 plays the region and pads tapped meanwhile cut it, each pulled onto
  the strongest hit. Shift + B1 Extract (`Samples/Edits/<NAME>-Slice n.wav`
  onto the next empty pad), Shift + B2 Split, Shift + B3 Merge; Erase + pad
  takes a slice out. B3 TO PADS lays the slices on pads, B1 CANCEL drops
  the chop. The slices are kept with the project (roadmap P3, the file's
  `CHOP`): each sample keeps the chop of the region last chopped, its type,
  amount and slices, so Chop on that region again, after a save and a load
  too, finds them as they were left rather than cut afresh. The slices are bands, every other one a shade darker, the one
  in hand lit with the pad it lands on as a chip over it and its length at
  the bottom right; the header counts them (scissors).
  The slice a pad plays lights faintly in the track colour with a head
  and its wake running through it, its bars full colour and its number
  bold in the track colour (the newest, if several); the slice in hand
  keeps its ground and handles. Manual's take line follows the voice as
  heard. Zoomed in, the view stays put.
  B2 SNAP (its LED lit while on) is Zero Snap (on by default, kept with the settings):
  every cut (each type's, Manual's taps, K1 / K2, Split, Sample Record's
  pad cuts) lands on the nearest zero crossing of both sides summed,
  looking up to 5 ms back and 1 ms ahead, so a slice starts and stops
  without a click. A region that starts or ends inside its file (a slice,
  a trimmed pad; not a loop) also fades over 2 ms at those edges, and
  Extract writes those fades into its file.
- **Trim Sample** (Shift + Pad 13): the region as
  `Samples/Edits/<NAME>-Trimmed.wav` in a new slot, the pad onto all of it;
  one undo; then Stage's TRIM. **Save Sample** (Shift + Sample Select on a kit): the Name
  sheet, then `Samples/<name>.wav`; the pad unchanged.
- **Input Config** (Shift + Sample; B2 on Record): K1 source (Mic, Rear,
  Rear L, Rear R, Resample, USB, USB L, USB R, then **Track n** for each
  track with a sound of its own: a synth, another kit; not the kit
  in view, empty or MIDI tracks), K2 rec length (Free, Seq, 1–16 bars), K3
  threshold (−96..0 dB), Shift + K3 gain; with a track, Shift + K1 its tap:
  **Post FX** (after its chain, level and pan, before mute: a muted track
  still records) or **Pre FX** (the engine alone). B1 back (as on every
  sheet), B2 monitor Off / Auto / On (its LED amber on Auto, green on On;
  the speaker goes quiet while the mic is monitored; a monitored track
  leaves the mix and is heard through the input instead, so once), B3 Rec
  Input FX (its LED; Knob FX on the input during a take). The picture is the
  back panel: the sources as the sockets they are (mic, rear jacks, USB-C,
  then Melodee's own: Resample and a track), the one chosen glowing, which
  sides it takes over its lead running into the stereo meter, the threshold
  a marker on it. Kept with the settings. The Looper's Input source
  hears the same.
- **Record** (Sample Record on a kit): the same cards; B1 the monitor (its
  LED), B2 Input Config. The take fills a frame of as many bars as its
  length, numbered (the one being taken lit), so what is left shows; a Free
  take grows across it. At the corner the state as a glyph (ready, waiting,
  the next loop, recording, saving) and the time so far; the cuts carry
  their pads as chips; the meter is stereo; what the pads do now is drawn
  as pairs under it (this pad ends, other pads cut). Tap a pad to
  record onto it (from the threshold, or with Seq where the pattern comes
  round: a track source's own pattern); other pads tapped during the take
  cut it there. The pad again, Stop or Push ends it and Record stays;
  Sample Record ends it and opens Stage's TRIM.
  `Samples/Recorded/TAKE n.wav`, up to 60 s. **Sampling Melodee's own
  sound** (a Track source or Resample): the tap only picks the pad; from
  then until the take ends the 16 pads play the instrument (a track: as if
  it were selected, its layout, scale, chord, arp, Levels and octave, never
  into its pattern, − / + its octave; Resample: the kit), lit white while
  held. With a threshold, the first note starts the take. With rec length
  Free, the take ends on its own after a quiet stretch (Settings: Take auto
  end), cut 10 ms after the last sound; Push or Stop end it sooner.
- **Audio Recall** (Shift + Sample Record): the source always fills a 25 s
  ring; this copies it out as `Samples/Recorded/RECALL n.wav` onto the next
  empty pad, cut as Settings says: the last phrase (after the last quiet gap
  of Recall gap, the default), from the first sound, or the whole 25 s;
  the quiet after the last sound left out unless Recall trim end is off.
  One undo.
- **Resample** (Shift + Pad 11, then a pad): the selected track's pattern
  with every track playing, rendered in real time onto that pad
  (`Samples/Recorded/RESAMPLE n.wav`, up to 60 s). One undo. On **Song**
  (roadmap P3) it is the song's: from bar 1 to its end, once, loop or not,
  on through its tails as the mixdown plays it, onto the pad tapped (the
  header says SONG: TAP A PAD); a song past 60 s gives its first 60 s and
  stops there.

### Effects

Each track runs a chain of six effects, then one send into a bus (A–D, six
slots and a return each), then the master chain (six) before the
compressor and main volume. A kit pad can have its own chain of four,
ahead of the track's. 41 effects: reverbs, delays (Echo, Tape echo, Ping
pong, Looper, Diff delay, Sample delay), modulation, dirt (Drive, Tape,
Crush, Spit, Fuzz, Amp sim, Tube drive, Soft clipper, Vintage, Vinyl, Tape
emulator), filters and EQ, dynamics (Compressor, Gate, Transient, Limiter,
Pumper), Spring, Ensemble, Multi-chorus, Auto-wah, Formant, Width.
Every effect, Pad FX and the master's Color sound the same at 48 and 96 kHz
as at 44.1 kHz: what each was written with a 44.1 kHz frame at a time (a
fall, a smoothing, a filter's corner, a hold, a short delay, a chance a
frame) is made the device's, so Vinyl crackles as often, Crush holds as
long and a reverb's damping stays where it was; hiss and clicks stay as
loud in what is heard. At 44.1 kHz they play to the bit as before.

- **FX Select** (Shift + Knob FX, round Track → Sends → Bus → Master; B1 /
  B3 walk too, named with chevrons): Stage's six-slot strip (each slot its
  family's glyph and name, empty ones dots, the next a plus) and under it
  the effect in hand, its name and family and what it does to sound drawn
  from its own values (a tail for space, taps for time, a wave for motion,
  the curve that bends it for dirt, the response for tone, the gain curve
  for dynamics; two dots by it while it has a second row on −/+). The wheel
  picks, Push on an effect opens its sheet: change, presets, bypass, move
  left / right, remove as cards, bypass lit (it stays open to hear the
  difference); the wheel moves, Push does it, B1 back; Push on the
  plus opens the picker, its families each with the strip's glyph. Sends
  (also B2 SENDS on Tracks and the chains): one bus at a time, every track's send into it as a
  strip; K1 the selected track's send into it, K2 which bus (the one Bus
  then shows), K3 the bus's return. B1 from Track on a kit: the last
  pad's chain.
- **Keyed Compressor and Gate (roadmap P4).** On a track, bus, master or
  loop-track chain, a Compressor's or Gate's last row (−/+ past its
  parameters, a dot more) is KEY on K1: Off (it hears itself), a track
  1–16, or a bus A–D. Its detector then hears that track as it leaves its
  chain and level (muted or not, a ghost key as the master's Sidechain),
  or that bus after its chain, and works on what goes through it: a bass
  ducked under the kick, a pad gated by a hat. A track keyed by a track
  runs its chain after the others in the same block, so the key is on
  time; a key that itself waits for one, a bus keying a track or bus, and
  any key of a loop track's chain are heard a block (128 frames) late. The
  file keeps keys in FXKY; the master compressor's Sidechain (one track or
  one of its kit pads) is as it was.
- **SYNC on the delays (roadmap P4).** Echo (Knob FX's Delay), Ping pong
  (their K2 on the second row), Tape echo and Diff delay (K3 there) take a
  SYNC: Off (TIME in ms, as before) or a note value, 1/32 to 1/1 with
  triplets and dotted ones. On the clock the echo is that note at the
  song's tempo, followed every block, so a tempo change (a pattern's own
  BPM, MIDI clock in) moves it, gliding over about 50 ms as a tape's speed
  would rather than clicking; TIME's card shows the note value. A Diff
  delay on the clock takes its diffusers' own delay off its line, so its
  echo lands on the note.
- **Knob FX** (Knob FX): the mode on or off. While on, K1–K3 (and Shift +
  K1–K3) are its effect's on every page. K1–K3 keep two settings, **A**
  (B1) and **B** (B2): the pots change the one in hand, heard at it, and
  the fader on the page slides the effect from A to B (a filter opening
  while a delay closes, in one move); each pot is drawn as a slot with its
  A, its B, the way between and where the fader has it. The wheel or −/+
  walks the MPC's 28 then Melodee's 15 (the one turned to in amber until
  Push loads it, A and B both at its initial values). Pads switch it per
  kit pad (kit tracks) or per track, their lights showing which; Shift + B2
  all, B3 bypass. Undo takes back A, B and the fader's place with the
  values.
- **Pad FX** (Pad FX): 47 punches in three banks (Pad Bank or −/+): the
  MPC's sixteen in its order, then Melodee 1 and 2, named on the pads as
  they sit (sounding lit, latched underlined). Hold pads to punch, up to
  four at once (a fifth bypasses the oldest); pressure is depth. B1 LATCH
  (its LED) keeps the last one on, B2 master or track. Shift + B3
  QUANTIZE (its LED, on at first; saved with the project, one undo): on,
  a punch pressed while playing starts on the snap grid's next line; off,
  at once, the header's magnet saying OFF (on the right: two chips at most
  on the left). Letting go is at once either way, and the snap grid still sets
  a repeat's slice and a gate's steps. Over the pads, the
  bar the selected track's pattern is in: a row per punch, drawn where it
  sounds in the pass you play it. The punches of one pass are a take; a
  punch in a later pass begins a new one. Until **B3 KEEP** only live
  punches sound (KEEP's LED lit while a take waits); KEEP lays the take in
  that pattern, each punch where it fell (one undo), and they loop with it
  like Motion, drawn quieter, four at once, on the grid and tail they were
  played with (kept in `KEPT`). A live punch wins: while one sounds the
  kept ones are quiet, and they come back for the rest of their length
  after. Erase + pad takes that pad's kept punches out (one undo; while
  Erase is held the kept ones show red, on the pads and the screen), and
  Erase + KEEP takes them all, asked first.
  An MPC punch's K1–K3 are its own, Shift + K1–K3 depth, snap, tail.
- **Flex Beat** (Shift + Pad FX): a time effect over the master for a bar;
  pad 1 Empty, 2–16 fifteen effects; K1 One Shot / Loop, K3 mix, B3
  quantize.
- **Pad Punch** (Pad FX again on a kit): each kit pad its own punch, rate
  and depth.
- **Compressor** (Shift + Pad 5): on the master. B1 Sidechain, B2 Color,
  B3 Bypass, each lit while on, with no messages. The header shows signs,
  not words: an LED beside COMPRESSOR (lit while it works, a ring while
  bypassed), and on the right MASTER or, with Sidechain, the pump sign (the
  graph's dip) and the source's tag. K1 attack, K2 release, K3 amount, Shift + K3 In Boost. With
  Sidechain one track keys it, whichever track is selected: the pads pick
  it (the grid on the left; the wheel or −/+ step it). On a sample kit the key can be one pad, the kick: Push (or
  the kit's pad again) shows its pads, a pad keys alone and sounds as it is
  picked, the same pad again keys the whole kit, the wheel or −/+ walk the
  banks. A key pad leaves the kit after its own chain (skipping the kit
  filter and the track's chain, as Knob FX's pads do) and the rest of the
  kit ducks with everything else. The key's sound after its chain, muted
  or not, drives the detector,
  and the rest of the mix (other tracks, the Looper, the bus returns, Knob
  FX) ducks under it before the master chain, with no makeup; the key
  passes untouched, In Boost drives the detector alone, and Color stays on
  the master. The picture is one bar of the song, beats numbered under it:
  what was taken off hangs from the top across it up to where the
  transport is, the bar before dashed as a ghost, and the detector's hits
  (the key's, in its colour, with Sidechain) marked along the foot, so
  release is tuned against the groove; at the bottom right the dB taken off
  now. With Sidechain the header names the key; stopped, the last bars
  stay.

### Fader

The fader follows the pot turned last on its track: any of the track's
values (an engine parameter, an effect on its chain, a kit pad's own value
on the last pad hit, its level, pan or a send); a pot on anything else (a
tempo, a pattern's length, a bus's effect) leaves it where it is. **PIN**
keeps what it has. The **Fader page** (Shift + Pad 9) shows what it
follows: that pot's knob in its shade, its name and value, a dashed arrow
to the fader (the pin on it while PIN holds), and while the fader waits to
pick a value up, where the hand has it and the way to move. K1–K3 there are
the track's favourites, the one it follows ringed; B1 back, B2 PIN, B3 off /
on with its LED. The wheel or −/+ walks the stock targets instead (a dot
each): the MPC's Pad Volume, Pad Pan, Pad Tune, Pad Attack, Pad Decay /
Release, Pad Cutoff (the last pad's) and Kit Volume on a kit, then on every
track Mod Wheel, then Pitch Bend on a plug-in or MIDI track (a MIDI track's
is its BEND, a plug-in's its own PITCH BEND lane on Motion; the plug-in has
to play pitch bend itself, as Melodee Poly, PD and Chip do and Nekobi does
not). A bend has a wide middle, 21 of the fader's 128 places, that is no
bend, always picks up, and springs back to the middle once no note is held
on its track and the fader has been still for a second; the fader then picks
it up again from the middle. Then Track Level, Track Pan and Send A–D (the track's own mix,
as Tracks and Sends show it). Until a pot or the wheel picks, a kit takes Pad Volume
and a synth the mod wheel. Pages with an obvious job keep it: Tracks the track's level, Sends its
send, Knob FX from A to B, Compressor its amount,
Flex Beat its mix. The fader takes a value over by Fader takeover (Pickup
by default); Seq Record writes the lane of what it moves; Erase + fader
asks to erase that lane. For a moment after it moves, the K card showing
what it drives lights as if turned, with the fader's glyph; where no card
shows it, the header says what and how much.

### MIDI and sync

**MIDI Config** (Settings' MIDI door, Shift + Pad 8; wheel, Push opens a value, −/+ change it, B1
back): port (External jacks or USB), In channel (All, 1–16: plays the
selected track), Out channel, Track Channel (a track's own: plays it
whichever is selected, and its notes go out there), Pad MIDI In / Out
(Never, Always, Empty), Sync In / Out (Off, MIDI Clock, MTC), Thru, Program
Change (launches that pattern), CV/Sync Out with Base 1–8 and Division 1–24
(pulses a quarter), the three takeover rows, MIDI Panic (Push: every note
here ends, and All Sound Off, All Notes Off and pitch bend centre go out on
every channel of every port) and Reset Factory Settings (B3 YES, B1 NO). The header shows IN / OUT as MIDI passes, EXT CLOCK or EXT MTC
while following.

- **In**: notes and pressure, CC 1 as the mod wheel, the pitch wheel (a
  plug-in track's PITCH BEND at full resolution, a MIDI track's BEND), All Notes Off; a kit
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
- **MIDI tracks, roadmap P4** (M2-M5): DEVICE's K1 also lists each USB
  device connected by name (its port's) and, gone, keeps it OFFLINE
  while its messages go to every USB output; K3 DELAY (-50..+50 ms, never
  earlier than the output's latency). Eight SLOTS pages: three knobs a
  page (24 in all; Stage's CC the first three), the wheel's second row
  each slot's address: a CC, a 14-bit CC (0-31), pitch bend, channel
  pressure or NRPN (0-1023); a knob's 0-127 spans the message's whole
  range; at most one message a slot every 2 ms (the latest goes);
  recorded and played as Motion. EXPRESS: BEND (a lane too) and how pad
  pressure goes out (Poly, Channel, Off); the fader's mod wheel is CC 1.
  PROGRAM's second row: this pattern's own program, sent (with the
  track's banks) as it launches, ahead of its notes. PADS: Keys or Drums,
  and on Drums the last pad's note (K2), the bank's map drawn with
  General MIDI names. MIDI in on a MIDI track goes out again on its port
  and channel (notes, pressure, bend, CCs; a CC a slot sends turns that
  slot, recorded under Seq Record) unless it would loop back (its input
  is the track's output on its channel); Thru skips what was echoed.
  AUDIO: an input (Rear L / R, Rear, Mic, USB) as the track's sound
  through its strip, chain, sends, stems and bounce, MONITOR and LEVEL;
  IN USE while the ADC is routed to the other input. SYSEX: a `.syx` from
  the card's `SysEx` folder (K1), ON LOAD, Push sends it in pieces paced
  for the jacks while everything else waits behind it.

### Project and system

- **Project** (Shift + Pad 16): the open project's three jobs as tiles under
  the keys: **B1 SAVE** (to its file in one press, or with no file yet the
  Name sheet first; Push too), **B2 OPEN** (the list), **B3 NEW** (asks
  first: B3 NEW, B1 CANCEL, saying whether it is saved first). A save frees
  the sample slots nothing uses any more (roadmap P3): a slot no pad's layer
  plays, no undo or redo could bring back, no file being made holds and
  Chop is not on is emptied, its chop with it, so the file keeps only what
  plays and the 64 slots come back; nothing is deleted from the card. The header
  carries UNSAVED (amber) while there are changes. The list: files
  (`.mldp`) in `Melodee/Projects` on the card, in folders, each row a glyph
  first and its facts last: **SD card access** at the top, folders with how
  many they hold (Push or B3 in, B1 or Shift + Push out, B1 at the top back
  to the tiles) and projects with their tracks and tempo, the open one
  OPEN (after an amber dot while unsaved). K1 jumps, K2 sorts. Push or B3 on
  a project opens its sheet: its jobs as cards, the likely one first and
  lit (SAVE on the open one, LOAD on another), the wheel moving, Push doing
  it, B1 back: on the open one Save, Save as new, Rename, Duplicate, Move to folder, New folder here,
  Open at start, Delete (asks again); on another Load and **Load kit (no
  patterns)** (its first kit onto the selected track, one undo) first.
  Settings and About are Shift + Pad 8's.
- **Loading**: a project loaded, imported or new (and a change of sample
  rate) fades the sound out over 10 ms and parks the audio thread, silent,
  while the MIDI thread builds the whole project, so however many effects
  and tracks it has no block runs late; the sound fades back in on the new
  project, and the delay memory the last one left is cleared on the way.
  The **Loading** sheet covers the screen (the project's name, a bar and
  the samples in of those it plays) until its samples are read, 20 s at
  most, then the heap the last project held is given back. While it shows,
  presses wait (their releases too); a pad or key held from before lets go
  as ever, K1-K3 and the fader only keep where they stand and the main
  volume still turns. Status: `melodee_loading` (up / samples to come),
  `melodee_parks`, `melodee_load_us` (the last build's time).
- **MPC Sample projects** (`.xpj`): listed beside Melodee's, wherever they
  sit under `Melodee/Projects`; the row **MPC Sample** at the top opens the
  stock app's own `MPC-Sample/Projects`. Their sheet has Load and Load kit
  only, and the `.xpj` and its `_[ProjectData]` folder are only read: Load
  opens a copy, unsaved, and Save makes a `.mldp` that plays the samples
  from where they are. Each Drum track becomes a kit track and each
  sequence that numbered pattern, with its tempo. Carried over: pad regions,
  tune, volume, pan, play type, mono, mute group, envelope, loop, Warp, and
  the filter types stock's Sample Mode offers. A pad of more than one layer
  keeps them all, as a drum program's does (`<project>.xpj#<track>.<pad>`).
  Not carried over: Pad FX and inserts, sends, and Songs. A project past
  16 tracks, 16 sequences or 64 samples loses the rest and says so.
- **SD card access** (Project's list, at the top; as the MPC's): the card
  goes to a computer over USB-C as a drive. The page draws the MPC and the
  computer with the link between them, and the three steps on the link,
  done (a check), now (amber, the only one with a word) and to come:
  connect, then eject the drive on the computer, then **B1** TAKE BACK
  (Projects and Samples are read again, and Project is in view); what still
  answers is four dim glyphs in the corner. If the computer still holds the drive, B1 says so
  and B1 again asks whether to take it anyway (B3, B1 Cancel). Meanwhile
  every other mode waits: only B1, Play, Stop, the pads (the selected
  track's sound) and main volume answer, and Melodee reads and writes
  nothing on the card. USB audio and stems, USB MIDI and the network keep
  working. It will not start while a take or render writes to the card.
  The base does the lending (`drivers/card_share.h`, HARDWARE.md "SD card
  access"); the page is `pages/card_access_page.cpp`, the controls
  `shell/card_access.*`.
- **Settings** (Shift + Pad 8; Melodee's, the same for every project, as is
  the metronome): seven doors, four by two (the wheel picks, Push or B3
  opens, B1 back), each a short list (Push opens a value, the wheel changes
  it, −/+ at once, B1 back to the doors). **PADS**: pad response (soft,
  medium, hard, full level), pad pressure on / off, pad lights, Double Seq.
  **KNOBS**: Knob / Knob FX / Fader takeover (Pickup, Scaled, Instant;
  Scaled, Scaled, Pickup by default). **SCREEN**: palette, brightness,
  where the place bar sits (Top, Bottom). **AUDIO**: sample rate (44.1, 48,
  96 kHz) and audio buffer (256, 384, 512, 768, 1024 frames, shown with the
  delay it makes: longer rides out a busy moment without a crackle, shorter
  is heard sooner); the header says what the device runs at now and, in
  amber, its dropouts since start. **SAMPLING**: Recall grab (Last Phrase, First Sound, Whole 25 s), Recall
  gap (0.5, 1, 2, 4 s), Recall trim end (On, Off), Take auto end (Off,
  Synths & Resample, All Sources: a Free take ends on its own), Auto end
  after (0.5, 1, 1.5, 3 s) and Silence below (−48, −60, −72 dB: what
  counts as quiet for both). A rate or
  buffer change reopens the audio device (the codec powers down and up
  behind the board's mute and with the speaker held off, so without a click:
  about a second of silence in the headphones, line out and speaker, as at
  every start) and a rate change
  stops playback: the engine builds every sound and effect again at the new
  rate. Melodee reads the rate and buffer from the card before the device
  first opens, so it starts at them without a reopen. Under the rows,
  DSP: what the audio blocks take of their time (their recent peak),
  beside the dropouts (a check while there are none). The computer sees the MPC's USB-C audio at the new rate too (the
  gadget reconnects: USB audio, MIDI and the network drop for a second; not
  while the card is lent). Every frame count in a project stays in 44.1 kHz
  frames whatever the rate, so projects open the same at any rate; samples
  play from a copy at the device's rate, and takes, Audio Recall, Resample,
  mixdowns and loops are recorded and saved at it (Sampler, Looper). Plug-ins are loaded
  again at the new rate, Monitor hears the source at it, and effects keep
  their timing at it (Effects). Kept in `Melodee/settings` with the project to open at
  start. **MIDI**: MIDI Config's page. **ABOUT**: how the machine is
  doing, the page to open when something crackles: RENDER (each core's load
  as a meter, the audio blocks' share of their time), AUDIO (rate and
  buffer; its dropouts in amber, a check while there are none), CARD (room
  on it, and the times it could not stream a sample in time), PLUG-INS (how
  many, what they cost); green, amber from 60 %, red from 85 %. B3 ABOUT
  again: engines, effects, limits, versions, licence.
- **Name sheet** (Save, Rename, markers, sounds, samples): pads type like
  phone keys, each pad showing both its letters with the one a tap gives
  now bright (pad 14 space, 15 digits and `- _ .`, 16 delete, as glyphs); a
  thin gauge under the name shows how much of its room it takes; the wheel
  moves the cursor, Shift + wheel or K2 the character, K1 a ready name,
  Erase deletes (Shift + Erase clears); B1 cancels, B2 Aa, B3 SAVE or Push
  keeps.
- **Exit** (Power, short): a tile under each key. B3 MPC APP saves the
  project first, then returns to the stock app; B2 POWER OFF saves it, then
  turns the MPC off; the header says UNSAVED while there are changes and
  SAVED once they are kept (or NO CARD); B1 stays
  (and calls off a save still on its way). At the next start Melodee opens
  the project set to open at start, or else the one last opened or saved.

## Across every page

- **Screen grammar** (`ui/glyphs.*`, `ui/chrome.*`, `ui/knobs.*`,
  `ui/widgets.*`): no sentences on screen. A state is a glyph and two words
  at most (the glyphs are SVG path data on a 24 unit grid, drawn by
  `ui::icon`); explanations live in this file. B1 is back or cancel, B3 the
  verb (in REC when it erases); a key that turns something on or off names
  it and carries an LED, never ON / OFF. A pot with no job draws nothing.
  Knob FX's cards are white with the knob glyph, and the header names its
  effect while it holds the pots. The picture shows what the cards cannot:
  shapes, meters, routes, pads; never the three values again. Questions
  before an erase or a loss share one sheet, the one place with a line of
  words: the question ("Clear pattern 2?"), one or two short lines saying
  what goes and whether undo brings it back, what it is about as a chip,
  and the undo keys. Choices under the keys are plain tiles without
  outlines, the likely one a shade lighter; the wheel moves the highlight
  across them and Push presses its key (`shell/key_pick.h`). A sheet behind
  Push (a project's, a note's, an effect's) shows its jobs as cards two to
  a row, three rows in view, the likely one lit; the wheel moves (the rows
  scroll past the third), Push does it, B1 is BACK and B2 / B3 do nothing
  (`ui::card_sheet`). Lists show
  what a row is as a glyph before its name and its facts at the right
  edge; K1 jumps through every list, the other pots draw nothing. Words: labels, keys and chips
  in capitals; names of sounds, pads, effects and projects in Title Case
  (a factory name stored in capitals is shown so, roman numerals and
  acronyms kept); setting rows in sentence case; units as written (Hz,
  kHz, dB, ms, s, st, ct, BPM). The sample pages draw with Stage's own kit picture: the pad's
  region fills the width, the file a strip above with the region bracketed.
- **Header**: transport (play triangle, stop square, the red dot while
  recording, a red ring while Seq Record is armed), tempo, at most two
  state chips (the count-in first, an amber metronome chip; then the page's
  own) and the track's tag, which gives up its engine, then its sound
  name, never its number. Where the playhead is, is the place bar's job.
- **Place bar** (`shell/place_bar.h`, `pages/place_bar.cpp`): a 2 px strip
  with no text on every screen, over every sheet, between the soft keys and
  the header (rows 21-22) or, with Settings' Place bar on Bottom, under the
  knob cards (rows 238-239). Its segments are the bars of the pattern
  playing on the track in view (never more than 16: longer patterns group
  their bars), the Song's sections between its markers while the Song plays
  (or stopped on Song), or on the Looper page the selected loop's bars. The
  fill is how far it has played; the segment it is in is a shade lighter,
  notched at its beats; the leading edge grows a row on each beat. Green
  playing, red recording or overdubbing, amber waiting (Count-In: a block a
  beat, emptying; Seq Record or a loop armed, a loop queued), grey stopped,
  held where Shift + Play carries on.
- **Mute mode** (Mute): the pads mute kit pads (bank in view) where they
  play a kit, tracks elsewhere; Shift + pad solos. Muted pads red, soloed
  white. Shift + Mute unmutes everything. Kit pad mutes are saved.
- **Erase**: + pad on a kit, stopped, empties the pad; playing or recording,
  erases that pad's notes (one undo). + pad where the pads play a synth's,
  plug-in's or MIDI track's keys asks, then erases that key's notes. With
  Seq held + pad asks, then clears that pattern; on Step Edit and Roll asks.
  On Tracks + pad asks, then clears that track. + pot sets the initial
  value (on Motion: asks to clear the lane). + fader asks to clear its lane.
  Tapped on its own (nothing else pressed or turned while it is down) it
  erases the page's selection (shell/erase_copy.h): Roll's note in focus,
  Steps' step under the cursor (a kit: the lane's note), Step Edit's
  event, Patterns' pattern in hand (asks), Tracks' track (asks), Scenes'
  scene name and tempo, the Looper's loop (asks), a chain's effect in
  focus, Chop's slice in hand (it joins the one before), LAYERS' layer in
  hand, MOD's target in hand, and on Project the project under the cursor
  (the sheet opens on DELETE, armed; a second tap deletes). Song's Erase
  deletes the clip as it goes down. Each is one undo where its page keeps
  undo.
- **Copy** (Shift + Erase) copies what the page holds. Pad Bank held, or
  Tracks: whole tracks (sound, kit, its 128 patterns and their motion; the
  target keeps its mix strip). Seq held, or Patterns: patterns. Steps: a
  step onto another (a kit: the lane's notes). Roll, Step Edit and a
  synth's play pages: a key's notes onto another key in the pattern. A
  kit's play pages: kit pads. These pick: the first pad is the source, the
  next the target, whose own notes or sound go first (one undo). Again, or
  Erase, leaves. Song: the clip under the cursor after itself (DUPLICATE).
  Project: the project under the cursor (DUPLICATE). A chain: the effect
  in focus into the slot after it, values and bypass too, the rest moving
  along (not a plug-in, not into a full chain). Chop: the slice in hand
  onto the next empty pad (Extract).
- **Move** (B2 with Pad Bank held, or Shift + B2 on Tracks; B3 there is
  Copy): the track, then where it goes. It moves whole, mix strip too; a
  track already there trades places with it, so nothing is lost. The song's
  clips and punches, the Looper's sources, Knob FX's tracks and pads, the
  compressor's key and Input Config's track follow it. One undo. Not during
  Seq Record, nor while a loop or Sample Record takes from either track.
  While picking, B2 and B3 switch between moving and copying, B1 steps back.
- **Undo** (Shift + −, redo Shift + +): the last 32 steps. A take is one; a
  pot's turns are one per control (closed by another control, a press or
  two seconds still); a press on a page is one step of what the page edits.
  On Roll, Steps and Step Edit a pad is one step of the pattern from press
  to release: the note it writes, toggles or picks and whatever the wheel,
  the pots or the fader do to it while it is held (undo while holding takes
  that step back). The note cards and Roll's NUDGE are steps like any
  pot; on Step Edit each step typed in is one. Mute, solo, mode buttons and the transport are not steps.
  A load or a new project forgets them.
- **Takeover**: a pot drives its value once it meets it (Pickup), moves it
  part of the way (Scaled) or at once (Instant); the card shows the value
  dim with a tick where the pot sits until then.
- **Pad expression**: a pad's pressure reaches every note it started (the
  arp's an octave up too) as that note's own pressure (poly aftertouch) on
  a synth, as Melodee Poly, PD and Chip turn into vibrato and brightness;
  off in Settings.
- **Text size**: nothing on screen is drawn under 11 px (`ui::type::floor`
  in `ui/tokens.h`; labels 11, body 13, lists 15, knob values 22), so the
  320 × 240 panel reads at arm's length. Text is never cut: where a page
  runs out of room it shows fewer things at a time (Sends eight tracks,
  Song six lanes, FX chains three devices), or a long
  capitalised name turns to title case in a narrow cell.

## Sound engines

- **Sampler**: the kit above.
- **PLUGIN**: a plug-in instrument from the card (below, "Plug-ins"); a
  synth track. Its parameters are the plug-in's own.
- **MIDI**: no sound; an external instrument on the track's own port and
  channel (above, "MIDI and sync").

Tracks render in one block of 128 frames.

## Synths

Melodee's synths are plug-ins of their own, Open MPC plug-ins
(open-mpc.github.io) that also play on MPC OS devices, each in its own
repository: **Melodee Poly** (`mpc-vst-melodee-poly`: two oscillators with
sync, noise, a four-pole ladder, two envelopes, LFO, poly mod, glide,
unison, drift; eight voices), **Melodee PD** (`mpc-vst-melodee-pd`: phase
distortion, eight waves, two lines, DCW and DCA envelopes; eight voices)
and **Melodee Chip** (`mpc-vst-melodee-chip`: a chip voice with ring and
sync, pulse width LFO, crush, a multimode filter; six voices). Each has its
factory sounds as presets, pad pressure and an eight-route mod matrix of
its own. They were Melodee's built-in PROPHET, CZ and SID engines, the same
voices to the sample; FM6 and the 909 kit are gone. Copy a synth's folder
(`Ellic Studio - VST - Melodee Poly`, from its aarch64 release) into
`Plugins/` on the card.

A project saved while the synths were built in (0.1.0-alpha.6 and before)
opens a PROPHET, CZ or SID track as a PLUGIN track on Melodee Poly, PD or
Chip, named from the card's top (`Plugins/Ellic Studio - VST - …`, so it
plays from whichever card holds it), with the track's values and routes as
the plug-in's state: it sounds as it did. An FM6 or 909 track opens empty,
its patterns kept (`model/project_file.cpp`, `legacy_engine` in
`model/project.h`).

## Plug-ins

VST3, LV2, CLAP and VST2 plug-ins, instruments and effects, from `Plugins/` at the top
of the card (beside `Samples/`), in any folders under it.

- **What loads.** Builds for this MPC only: a `.vst3` bundle with
  `Contents/aarch64-linux/`, an `.lv2` bundle, a `.clap` library (both
  aarch64), an
  [Open MPC](https://open-mpc.github.io) plug-in folder (its
  `plugin-meta.xml` beside a VST2 `.so`), or a VST2 `.so` on its own;
  aarch64, or 32-bit ARM (armv7) through the bridge. The
  picker lists the rest dimmed with the reason ("Windows and Mac builds
  only"). Most commercial plug-ins have no Linux ARM build.
- **An instrument.** PLUGIN is an engine like the others: SYNTH on Stage,
  or K1 on Sounds, then the PLUG-INS picker opens on the card's folder.
  Sample Select on a PLUGIN track opens the picker again. Stage's B1 EDIT
  (or Edit) shows the plug-in: the share of each audio block it takes in the
  header, and its parameters as a list, each name on one full line with its
  number: the three on K1–K3 raised and ticked in their colours (their
  values on the cards, which carry their names: capitals, else mixed case,
  else over two lines), the row before (or
  after) with its values; the wheel or Minus / Plus step the rows, B1 opens
  the picker. Shift + wheel steps the last card turned by one of the
  parameter's own values (a thousandth of its range when it has more), and
  Erase + pot puts it back to the plug-in's default. Open MPC plug-ins put
  their skin's Q-Link pages first.
  MIDI-CC stand-ins, host settings and the plug-in's own bypass (the slot
  has one) are left out.
- **Drum machines.** An Open MPC drum machine that states its voices (8W8,
  9W9 and their siblings carry their screen's map in the library,
  `"pad_layout":"drums"` with each voice's name and note) turns its track's
  pads to Drums when it is picked, and Keys names its voices. They carry a
  map for each Note Map setting; Melodee names the one they start on (the
  drum rack from 36: 8W8's Rack 36, 9W9's Drum Rack), so keep Note Map
  there. Any other drum plug-in plays on Drums too: K3 on Keys. Picking a
  plug-in that is no drum machine puts a Drums track back on Scale.
- **Bundles of several.** A bundle or a CLAP library holding many plug-ins
  (LSP's VST3 holds 198) lists as one row, the plug glyph with its count and a chevron; Push
  opens it, a row for each. Every row is a glyph first (folder, plug, a
  broken plug with why for one that cannot load here) and its facts last
  (the format, a check on the one loaded, a keyboard or sliders once known
  to be an instrument or an effect); K1 jumps.
  The project keeps which one (its id; a CLAP's is a hash of its CLAP id)
  and its name.
- **CLAP.** JUCE hosts no CLAP, so Melodee has its own host on the CLAP
  1.2 headers (`plugin/clap_instance.*`, a JUCE `AudioPluginInstance` like
  the VST2 host). Notes go as CLAP note events when the plug-in prefers
  them, else as MIDI; CCs, bend and pressure as MIDI when its note port
  takes MIDI. A parameter set on K1–K3 goes to the plug-in with its next
  block; the value shown is the one the plug-in last gave. A project keeps
  the plug-in's own state. Its latency counts for delay compensation. CLAP
  has no program list: a plug-in's presets are files its library's preset
  discovery names, and preset-load loads one by its file. The rack looks
  for them once the plug-in plays, reading each file the first time
  (Surge XT's 3,563 take a while) and keeping what it read in
  `Melodee/Plugin Data/.cache/melodee/`, so later it reads only new or
  changed files. Each preset's folder under its location is its folder in
  PRESETS; with more than one location holding presets (Surge XT: Factory,
  Third Party, User), each location is a folder too, named without the
  plug-in's name. No editor windows, as for VST3.
- **LV2.** JUCE's own LV2 host (lilv, bundled in JUCE), with a lilv world
  for each plug-in opened, made on the message thread. A bundle's presets
  (`pset:Preset`) are its programs, so PRESETS lists them. JUCE cannot
  tell an LV2 instrument by its class, so one with audio out and no audio
  in counts as an instrument (a bundle of one also when its `.ttl` says
  `lv2:InstrumentPlugin`). The GUI library beside it (`_ui.so`) is never
  loaded.
- **An effect.** The effect picker's last family, Plug-ins, opens the same
  folder for the slot; on the chain pages K1–K3 hold its parameters and
  Minus / Plus its rows. Track, bus, master and loop-track chains take
  plug-ins; a kit pad's own chain does not.
- **Presets.** A plug-in's own programs, those with a name: B2 PRESETS on
  a PLUGIN track's Edit, or Presets on a plug-in effect's sheet (Push on the
  slot; the row shows only when it has some). The wheel or Minus / Plus
  move, Push loads one and stays, so the next is heard straight after (the
  pads play the track); the one picked says LOADED, B1 goes back. Picking
  the one loaded loads it again, dropping edits since. What a preset loads
  goes into the plug-in's state, so the project keeps it. Presets in
  folders (a CLAP plug-in's) list as those folders first, each with how
  many presets it holds; Push opens one, B1 comes back out on it, and the
  folder holding the one picked has the check. PRESETS opens in the folder
  of the one picked last (`plugin/preset_rows.h`).
- **Plugin Data.** Plug-ins keep presets and settings under their home
  folder, and the MPC's is read only: before a plug-in opens, `$HOME` is
  `Melodee/Plugin Data` on the card, laid out as on a Linux computer. Most
  use `.config` there, which a Mac's Finder hides (Cmd-Shift-. shows it):
  TONE3000 reads its presets from `.config/TONE3000/Presets` when it
  opens.
- **Neural Amp Modeler.** The amp sim: the official plug-in by Steven
  Atkinson, built for the MPC without its editor
  (fork `mpc-vst-nam-official`, its `linux/README.md`). Its
  Model and Cab parameters pick files from `Neural Amp Modeler/Models`
  (`.nam`) and `Neural Amp Modeler/IRs` (`.wav`) in Plugin Data, which
  Melodee makes before it opens (`plugin/nam.h`); a turn loads the file on
  the plug-in's own thread, and the state keeps its path. Slim, the size an
  A2 capture plays at, is held at 0 (lite) after its state loads and kept
  off the page: full costs about four times lite. The page starts Model,
  Cab, Output, then Bass, Middle, Treble; long names are shorter on the
  cards (Gate, Tone, Cab on, Out mode, Calibrate, Cal level). On the MPC
  (48 kHz) a lite capture takes about 17% of a block, an IR of 8192 taps
  (where the build cuts a WAV IR) about 17%; printed beside its live copy
  it caused no xruns, and a deep freeze and thaw left RSS within 0.2 MB.
- **TONE3000: lite captures.** TONE3000 plays each amp capture (NAM A2) at
  one of two sizes, lite or full. The size is kept in the preset, not on a
  knob. Full takes about three times the CPU of lite here and crackles, so
  Melodee makes every capture lite (`plugin/tone3000.h`): the presets in
  `.config/TONE3000/Presets` before TONE3000 opens and before a preset is
  picked (only the size changes in the file), and a project's saved
  TONE3000 before it loads. A long reverb IR (Marshall Bluesbreaker's
  1.78 s) still crackles: that is the IR, not the capture.
- **Open MPC's rate.** An Open MPC plug-in is written to MPC OS's contract,
  44.1 kHz whatever the host says. Melodee opens one at 44.1 kHz and, at 48
  or 96 kHz, hears it through the base's band-limited converter
  (`plugin/rate_bridge.h`): an instrument's output converted up, under a
  millisecond late; an effect's input down and its output up, about 3 ms
  late. So it plays in tune at any rate.
- **Delay compensation (roadmap P4).** A plug-in that reports a latency
  (VST3's, a VST2's `initialDelay`, a CLAP's latency, an LV2's latency port), the 44.1 kHz converter in front of an
  Open MPC plug-in at 48 or 96 kHz (measured once as it opens: an
  effect's about 3 ms, an instrument's none) and a Limiter's LOOKAHEAD
  (0-10 ms, its third card) make their path late. Every block the engine
  host sums each chain's latencies (bypassed slots left out) and holds the
  earlier paths back in lines made at start (`engine/compensation.h`,
  16384 frames each; nothing allocates when a latency changes, a moved
  line eases across one block): each track and loop track meets the
  latest of them (a PLUGIN track's instrument counts), the direct mix
  waits for the latest bus or Knob FX return, each of those for the
  latest, and kits' pads into Knob FX and the Sidechain's key pad wait as
  long as the latest track. The master's chain delays everything alike.
  MIDI out (notes, clock, MTC), the metronome and Seq Record's placing
  of a hit move by the whole amount, so outside gear and played takes
  line up with what is heard. Settings' AUDIO shows it (PDC, in ms) while
  it holds anything back; `status` has `melodee_pdc` and each track's,
  bus's and the master's own.
- **Notes.** Notes, the wheel (CC 1), bend and each note's pressure (poly
  aftertouch) go to a PLUGIN track's plug-in as MIDI; the sequencer's accent
  as a harder velocity, a Slide or Tie as CC 84 with the key it goes on
  from (and CC 68 held over a tie), above, "Steps".
- **Loading.** The card is mounted `noexec`, so a plug-in runs from a copy
  in `/run` (RAM), made when a project uses it and removed when nothing
  does. The folders beside it on the card are linked beside the copy, so a
  plug-in that looks beside itself for its data finds it there (Surge XT:
  `SurgeXTData`). The picker opens each plug-in it lists once, to tag it INSTRUMENT
  or EFFECT. Opening takes from a few ms (VST2) to a quarter second (a
  32-bit plug-in starting its bridge).
- **Support carried in the binary.** No-op stand-ins for the GUI libraries
  editors link (`libGL`, the X11 and xcb families, `libcairo`; Melodee never
  opens an editor), the real `libsndfile` with its codecs (plug-ins read
  audio files with it), `libsamplerate` and FFTW (single and double
  precision), and the 32-bit bridge with Debian's armhf runtime
  go into `/run/melodee-plugins/support` the first time a plug-in loads.
  `tools/plugin_support/SURVEY.md` says why these and no others.
- **32-bit plug-ins** run in a bridge process: audio, MIDI and parameters
  cross in shared memory, a block at a time in the same period (about a
  microsecond each way). Once the plug-in is open the bridge answers at
  the audio thread's priority (`SCHED_FIFO` 30; threads the plug-in starts
  stay normal), and the lock the audio thread shares with parameter reads
  inherits priority, so a block never waits on a normal thread; a state
  is read or written a parameter a lock. If the plug-in crashes, only the bridge goes:
  the page says it stopped working and picking it again reopens it.
- **Saving.** A project names each plug-in by path (`Project::plugins`,
  32 entries) and keeps what each saves of itself (its state) in the file.
  A path not starting at `/` is the card's (`Plugins/...` from its top): it
  opens from whichever card is in, and tries again when a card comes.
- **Not yet.** Plug-in parameters take no Motion and no undo; Knob FX and
  a kit pad's chain take no plug-ins; editors are never shown.
- `tools/vst_probe/` is a command-line host on the same code, for trying a
  plug-in on the MPC before loading it in Melodee (its README has
  measurements).

## Files on the card

- `Melodee/Projects/**.mldp`: tagged sections, so older files stay
  readable and only what differs from empty is written (`EVNT` events,
  `MOTK` each pattern's motion (older files' `MOT2` / `MOTP` / `MOTW` land in their patterns), `TIME`, `PAD4` / `KITX` / `PMUT` kits,
  `KNFX`, `CMPR`, `PFXP`, `FADR`, `CHAN` (a track's pattern chain), `KEPT` (punches kept with a pattern), `MIDC`, `MIDT`, `PVER`, `PTCH` (notes' pitches on a kit pad), `SNDS` (a track's
  four sends; TRAK keeps the largest for older builds), `LVLE` (16 Levels' ends) and the rest; a
  MIDI track is None in `TRAK` and MIDI in `MIDT`, a PLUGIN track None in
  `TRAK` and itself in `PLGT`, so older builds load them empty; `PLUG` holds
  each plug-in's path and state). Saved beside
  the file, then renamed into place.
- `Melodee/Sounds/<name>.mlds`: a sound's values and mod routes.
- `Plugins/`: plug-ins, read only (above, "Plug-ins").
- `Melodee/Plugin Data/`: the plug-ins' home: their presets and settings
  (above, "Plug-ins").
- `Melodee/settings`: Melodee's settings, MIDI Config, Input Config, the
  favourites, the project to open at start (`start=`) and the one last
  opened or saved (`last=`), opened at start when none is set.
- `Samples/`: loaded from anywhere under it; Melodee writes `Recorded/`,
  `Edits/` and saved samples there, bank files in `Banks/`, and committed
  loops in `Looper/`.
- `Melodee/Cache/*.f32`: SFZ samples decoded for streaming, named by the
  source's name, size and date; the least lately played go past 4 GB. Safe to
  delete: they are made again.
- `Melodee/Projects/**/<project> Loops/Loop N.wav`: a project's loops.
  `Melodee/Mixdowns/` holds song mixdowns, `Melodee/Stems/` stems,
  `Melodee/MIDI/` MIDI files, `SysEx/` (at the card's top) a MIDI track's
  `.syx` files.

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
- `melodee-looper status | on | off | rec|toggle|halt|now|undo|clear N |
  length N 1-16|free | feedback N 0-1 | level|pan|send|bus N V | mute|solo N
  0|1 | quantize bar|beat|off | then play|overdub|stop | source N
  input|track M|loop M|mix|rear [l|r]|usb [l|r]|mic | latency FRAMES|auto |
  transport follow|free |
  commit N [note]` (docs/LOOPER.md).
- `melodee-render [serial|parallel]`: the render workers left asleep (the
  audio thread renders alone) or used again, from the next block.
- `melodee-midi [usb] HEX...`: one message into the input path, as if from
  the External (or USB) port. `melodee-midi panic`: MIDI Config's MIDI
  Panic.
- `card-share [on|off|force]` (the runtime's): SD card access from the
  remote; Melodee opens its page as if B1 had been pressed there.

`status` adds Melodee's fields: `melodee_playing`, `melodee_dropped`,
`melodee_render_threads`, `melodee_render_load` (the recent peak block's
time against its budget, tenths of a percent), `melodee_render_parallel`
(blocks the workers helped with), `melodee_dropout_track` and
`melodee_dropout_load` (at the last dropout the busiest track, 1–16, 0
none, and its share of a block in tenths of a percent), `melodee_guard`
(voices the overload guard stole / blocks it lightened), the
page and track, the lit buttons (`melodee_buttons=seq:2,stop:1`, 1 faint,
2 bright) and pads (`melodee_pads=2:t4`: hue and rung, Pad lights above), the meters (`melodee_meter=3,1,out`:
left and right as masks, bit 0 the bottom LED, then `out` or `in`); the sequencer (`melodee_rec`,
`melodee_count_in`, `melodee_position`, `melodee_timed` (notes played by
hand on their frame / after it / the latest by, frames), `melodee_pattern_ticks`,
`melodee_events`, `melodee_q`, `melodee_first_tick`, `melodee_signature`,
`melodee_ticks`, `melodee_song_bar`, `melodee_song_clips`); Step Edit
(`melodee_step`, `melodee_event`, `melodee_event_tick`); `melodee_ask` (0
none, 1 erase a pad's events, 2 erase a lane, 3 reset settings, 4 new
project, 5 take the card back); `melodee_motion` (the pattern's points); the last pad
(`melodee_last_pad`, `melodee_pad_loop` .. `melodee_pad_gain`,
`melodee_pad_slots`, `melodee_pad_start`, `melodee_pad_end`,
`melodee_pad_name`; roadmap P3's `melodee_pad_layers`, `melodee_layer_play`,
`melodee_pad_xfade`, and the selected track's voices heard
`melodee_voices` with the sample slots they play `melodee_voice_slots`), 16 Levels (`melodee_levels`, `melodee_levels_kind`);
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
(`melodee_undo`, `melodee_redo`, `melodee_undo_last`, `melodee_seq_held`,
`melodee_map`, `melodee_gesture`, `melodee_engine`); SD
card access (`melodee_card` 0 home, 1 lending, 2 lent, 3 returning;
`melodee_card_note`, the reason a lend did not start, `CardNote`;
`melodee_card_ejected`, `melodee_card_locked`); the Looper (`melodee_looper`,
`melodee_loop_quantize`, `melodee_loop_then`, `melodee_loop_transport`,
`melodee_loop_latency`, `melodee_loop_memory`, and `melodee_loop1` ..
`melodee_loop4`: state, length, place, bars, layers, undo, source, feedback,
tempo, stretched, fitting, pending); plug-ins (`melodee_plugins`: each
entry in use as entry:state:name:format:load, the load in tenths of a
percent of a block, `none` when there are none). The runtime's own
`card_share=` says the same from the base's side.

## Previews

`build/melodee --melodee-preview DIR [PAGE]` draws every named page at
1× and 2× (the 2× ones line up with the 640 × 480 boards); the names are in
`src/melodee/tools.cpp` (`cardrow`, `cardaccess`, `cardejected`,
`cardlocked`, `cardnocomputer`, `cardnone`, `miditrack` and `midipanic`
among them).
`--melodee-sound FILE.wav` renders two bars of a starter project (its kit's
beat on three drums made for the check; its synths are plug-ins, not loaded
there) and
checks the level. `./build.sh` runs both. The preview also checks text:
anything asked for under 11 px, wider than its box, or a header whose two
sides meet prints a `MELODEE_TEXT` line, and any such line fails the run.

## Installing

`python3 device.py install-sd --app melodee` (see README) starts it from
the card at every boot; leaving it returns to stock until the next boot.

### Updates from the card

Melodee installs a newer release zip from the top folder of the card
(`shell/update.*`). The base's `install/sd-update.sh` and
`drivers/sd_update.*` do the install (base docs/APPS.md, "Updates from the
card"). When the card is mounted or returned from a computer, the service
thread runs the script's `check` on each `*.zip` in the card's top folder.
It offers the newest zip whose `RELEASE.sha256` is signed with the key in
`shell/update.h` (`release_key_pem`) and whose VERSION is newer than
`melodee_version()`. A pre-release is older than its release; a git
describe version is newer than its tag.

When playback is stopped and no other sheet is open, Melodee shows
**Update to v0.2.0?** B1 (LATER) closes it; Melodee asks again when the
card is returned from a computer or at the next start. B3 (INSTALL) stops
playback, saves the project if it has unsaved changes, and starts the
install. The update sheet then fills the screen and ignores the controls.
It shows the steps SAVING PROJECT, CHECKING FILE, COPYING TO CARD and
RESTARTING. When the new version has started, its header shows **Updated
to v0.2.0**. If the install fails, the sheet shows UPDATE FAILED and the
reason (for example SD card is full, Invalid update file, Autorun folder is
not Melodee's), the card is not changed, and B1 closes the sheet. If the
new version does not start within 30 seconds, the script restores the
card and restarts the current version, which shows **Update to v0.2.0
failed**. The previous version stays on the card in `autorun/melodee.prev`.
The update only replaces an autorun set up by Melodee's release zip, or
installs one on a card without an autorun. It does not change a card whose
`autorun.sh` starts something else (`device.py install-sd`, MPC Sample
Plugins, a boot menu).

Releases: `tools/make_release.py` adds `RELEASE.sha256` (the SHA-256 of
every other file in the zip) and `RELEASE.sha256.sig` (its Ed25519
signature). It signs with `~/.config/melodee/release-signing.pem` (or
`$MELODEE_SIGNING_KEY`) using OpenSSL 3, and fails if the key does not
match the public key in `shell/update.h`. Back up the signing key:
installed versions of Melodee only accept updates signed with it, and a
new key means installing the next version by hand.

Remote: `melodee-update [DIR|card]` checks for an update now, in DIR
instead of the card (for device tests). `status` adds `melodee_update`
(UpdatePhase: 0 none, 1 saving, 2 checking, 3 writing, 4 starting,
5 failed) and `melodee_update_offer`. Previews: `updateask`, `updating`,
`updatefailed`.

Limits that are single constants (`model/limits.h`): 16 tracks, 16
patterns a track, 8 mod routes, 8192 motion points a pattern, 64 samples.
