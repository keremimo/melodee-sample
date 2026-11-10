# Melodee: MPC Sample parity plan

Melodee must do everything the stock MPC Sample does, as printed on the
panel, on top of its own pages. This file carries the plan across sessions:
**one phase per session**. Each session reads this file and
[MELODEE.md](MELODEE.md) first, does one phase, ticks its boxes, adds a line
to the log at the bottom, and updates MELODEE.md where behaviour changed.

Reference for every MPC behaviour: Akai's guide,
`https://www.akaipro.com/guides/mpc-sample/<page>.htm` (features, sample_mode,
pad_play, loading_and_saving_samples, sample_record_mode, sequence_mode,
recording_sequences, editing_sequences, step_edit, song_mode, pad_fx,
flex_beat, knob_fx, compressor, input_configuration, fader, time_correct,
midi_configuration, project). Fetch the page for the phase at hand; don't
work from memory.

## Rules for every phase

- Printed labels keep their MPC meaning; Melodee's own pages move to
  whatever is free (decisions below). Where Melodee goes beyond the MPC
  (16 tracks, Song arrange, chains, punches), keep it.
- Keep the layering (`AGENTS.md`, `docs/APPS.md`): controls in
  `shell/`, one page per file in `pages/`, plain data in `model/`, audio in
  `engine/`. No feature piled into `shell.cpp` or `context.cpp` when it can
  have its own file.
- Audio thread: no allocation, locks or waits. New audio state goes through
  `Command`s.
- Project file: new fields are new tagged sections, so older `.mldp` files
  still load.
- Every new page gets a `--melodee-preview` name; `./build.sh` passes;
  check on the device with `python3 device.py start --app melodee` (never
  return to stock) and `--ctl melodee-press`.
- Update the Shift overlay (`pages/overlays.cpp`) whenever a Shift job
  changes.
- Commits: subject line only, no attribution (see `~/CLAUDE.md`).

## Decisions (Kerem, 2026-11-10)

| Control | Decision |
| --- | --- |
| Pad Bank | Hold + pad stays **track select** (from any page; while held the screen shows the 16 tracks). Tap = next bank, Shift + tap = previous. Hold + −/+ (or wheel) jumps banks. Kits grow to 8 banks (A–H lights). |
| Seq | **Patterns** (MPC Sequence Mode: pads pick patterns and queue them; K1 length, K2 Q, K3 swing). |
| Shift + Seq | **Step Edit**; pressed again it steps through Steps → Roll → Motion. |
| Loop / Shift + Loop | Loop / reverse the **last pad's playback** (sampler). |
| Song | Shift + Pad 12. Length moves onto the Patterns cards; the Length page is dropped (Kerem, 2026-11-10). B2 on Patterns was a second way in until phase 2 gave it SEQ / GBL BPM, as the phase 2 box and the MPC have it. |
| Shift + Sample | **Input Config** (source, monitor, threshold, rec length, rec input FX). |
| Shift + Pad 16 | **Project** (B2 Settings, B3 About stay there). |
| Shift + Pad 8 | **MIDI Config**, which also holds the takeover modes. |
| Shift + Sample Record | **Audio Recall**: an always-on 25 s input ring, onto the next empty pad. |
| Shift + Seq Record | **Recall** = today's Capture (moved from Shift + Sample Record). |
| Shift + Sample Select | Sampler track: **Save Sample** (the last pad's region to WAV, Name sheet). Synth track: Save sound. On Sounds: star. |
| Shift + −/+ | Always **undo / redo**. Transpose moves to **Shift + K3 on Keys**; −/+ on Keys stays octave. |
| Mute | Tap latches mute mode, hold is temporary, **Shift + Mute = unmute all**. In mute mode on a kit page the pads mute **kit pads**; elsewhere they mute **tracks**. Shift + pad in mute mode solos. Light shows the mode. |
| Note Repeat | **Stays on the page.** Tap latches, hold is temporary. While on, B1–B3 show 1/8, 1/16, 1/32 (Shift: triplets); swing follows the project. **Double-tap** opens the Arp page (mode, octaves, gate, Chord on B1). |
| Knob FX | **MPC performance mode**: one effect on K1–K3, pads switch it on per kit pad (or per track off kit pages), B2 all, B3 bypass. |
| Shift + Knob FX | **FX SELECT** = Melodee's chains (Track → Sends → Bus → Master, picker). |
| Fader | Shift + Pad 9 picks a target per track: every MPC target plus mod wheel. Default: pad volume on sampler tracks, mod wheel on synths. **Context overrides** on pages with an obvious job (Mix: selected track level; Step Edit: nudge timing; more in phase 8). Light shows the value. |
| Pattern length | **Up to 128 bars** (MPC), replacing the 256-step limit; K1 on Patterns steps 1, 2, 4 … 128 bars, Shift + K1 any bar count. Needs event-list pattern storage (phase 2). |
| Erase + pad (kit) | Stopped: empty the pad. Playing or Seq Record: erase **that pad's notes** in the pattern (one undo). |
| Shift + Erase | **Copy** everywhere: kit pads (kit pages), patterns (Patterns), tracks (Pad Bank held). Clearing a pattern stays Erase + pad on Patterns. |
| Pad pressure on synths | What aftertouch does on synthesizers that have it (Kerem, 2026-11-10): pressing harder adds **vibrato** (the LFO into pitch) and **opens the sound** (cutoff on PROPHET and SID, DCW on CZ, modulator levels on FM6), as on a Prophet-6 / OB-6 or a DX7 patch. Each engine gets two parameters for it, AT > VIB and AT > BRIGHT, on its Edit pages next to the velocity ones, with defaults that are heard; a mod route from Aftertouch still adds to them. Phase 1d. |
| Printed jobs with no conflict | Follow the print: Shift + Pad FX = Flex Beat, Shift + Chop = Note On, Shift + 16 Levels = Type, Shift + Tap Tempo = metronome Off / On / Rec, Shift + Play = Continue, Shift + Pad 1–16 as printed. |

## Open questions (ask before the phase that needs them)

1. ~~Pattern length~~ decided 2026-11-10: **128 bars**, as on the MPC (see
   the Decisions table and phase 2).
2. Which other pages get fader context overrides (phase 8).
3. Whether the internal drive (eMMC) is offered next to the SD card in the
   browsers, or the card only (phase 5).

## Phases

### Phase 0: bugs and printed-label fixes (small)
- [x] Steps on a sampler track writes `track.last_note`, which kit pads never set: set it (or use `last_pad`) so steps get the pad last hit (`shell/context.cpp` `play_pad`, `pages/steps_page.cpp`).
- [x] 16 Levels light: lit on the Levels page, not on Keys (`shell/lights.cpp`).
- [x] Shift + Erase = Copy everywhere; drop "clear pattern" (`shell/shell.cpp` Erase branch); pattern copy via Shift + Erase on Patterns.
- [x] Erase + pad on a kit: stopped empties the pad, playing / Seq Record erases that pad's notes (one undo).
- [x] Shift + −/+ always undo / redo; transpose to Shift + K3 on Keys (needs the Shift + K card, see phase 1; until then a Keys card on row 2).
- [x] 909: pads 12–16 do something (another bank of voices or dark with a reason), not dead silently.
- [x] Pad Bank tap on a non-sampler track: no silent no-op (flash the header or select the next track's bank).

### Phase 1a: button remap
- [x] Seq → Patterns (with K1 length, K2 Q, K3 swing; Length page folded in or kept as B3).
- [x] Shift + Seq → Step Edit group (Steps → Roll → Motion); move "quantise take" to Time Correct (phase 2).
- [x] Loop / Shift + Loop → pad loop / reverse (loop itself lands in phase 4; until then reverse works and Loop opens the Play page).
- [x] Shift + Sample → Input Config page (contents in phase 6; first a page with source / threshold moved from Record).
- [x] Shift + Sample Record → Recall stub; Capture → Shift + Seq Record.
- [x] Shift + Sample Select by track engine (Save Sample needs phase 5; until then the Name sheet + "not yet").
- [x] Shift + Tap Tempo cycles Off / On / Rec (click only while recording).
- [x] Back: a real page stack (Shift + Push walks back more than one page).
- [x] Update the Shift overlay and MELODEE.md's Shell section.
- [x] Button lights as on the stock MPC: faint where the button has a job on the page in view, bright in use, dark otherwise (Kerem, 2026-11-10).

### Phase 1b: Shift + pads and Shift + knobs
- [x] Shift + Pad 1–16 dispatch in the shell (pages that use Shift + pad today: Patterns column launch, Punch lock; move those or keep them where the page owns the pads, and decide which wins).
- [x] Shift overlay shows the 16 pad jobs as the pads sit (pad 1 bottom left), and lights the pads that do something.
- [x] Pads wired now: 12 Song, 16 Project and 3 Double Seq (phase 1a routed these three in `shell/shift_pads.cpp`), 1 Full Level (the pad response setting), 4 Count-In and 10 Rec Quantize as toggles (behaviour in phase 2); the rest open a "coming in phase n" notice.
- [x] Shift + K1–K3 second parameters: Binding layer for a Shift row, Shift icon on cards that have one (MPC pattern). Use it for Keys transpose.
- [x] −/+ on list pages move the list or change the selected value; octave only on play pages (Stage, Keys, Edit, Chord, Roll).

### Phase 1c: Mute, Note Repeat, Pad Bank
- [x] Mute: latch on tap, momentary on hold, Shift + Mute unmute all, kit pads vs tracks by page, Shift + pad solo, light.
- [x] Per-pad mute on the sampler (model, engine, project file).
- [x] Note Repeat on the page: B1–B3 rates overlay, Shift triplets, double-tap opens Arp; triplet rates in the arp engine.
- [x] Shift + Erase with Pad Bank held copies a track (sound, kit, patterns; one undo). Phase 0 copies kit pads and patterns only.
- [x] Kits: 8 banks / 128 pads (`sampler_banks`, project file section, bank lights A–H); Pad Bank hold + −/+ or wheel jumps banks; hold shows the 16 tracks on screen.

### Phase 1d: synth pad expression (Kerem, 2026-11-10)
Kerem: synths don't respond to velocity or aftertouch. Found in phase 1b's
session: both reach the engine host (`Command::NoteOn` velocity,
`Command::Pressure` per key from the pads' poly aftertouch, `Context::pressure`),
but pressure is heard only on PROPHET and only through a mod route set to
Aftertouch (none by default); FM6, SID and CZ ignore `set_pressure` and the
whole mod matrix (`set_mod`), though Edit shows its page on every engine.
Velocity is heard everywhere (PROPHET VEL > AMP 50 / VEL > FLT 20, FM6 40 % per
operator, CZ VELOCITY, SID fixed 0.4–1.0), but Full Level (Shift + Pad 1) was
left on by phase 1b's device checks for a while, which plays every hit at 127.
- [x] Mod matrix and pressure on FM6, SID and CZ (each picks how far a destination moves, as PROPHET does), so the matrix page is heard on every engine.
- [x] Pressure does what aftertouch does on synths (decisions table): vibrato plus a brighter sound, through AT > VIB and AT > BRIGHT on every synth, heard at their defaults; a route from Aftertouch still adds to it. New parameters go at the end of each engine's list so older projects and sounds still load.
- [x] Velocity depth: clearly heard on every synth at its defaults (SID gets a velocity parameter like the others); factory sounds checked.
- [x] Pressure follows the notes a pad started: every note of a chord, and the arp's notes while the pad is held.
- [x] Checked by hand on the device (remote presses are fixed velocity 100 and send no pressure). Kerem played it: feels right.
- [x] Pad pressure can be switched off (Kerem, 2026-11-10): Settings' Pad pressure On / Off; AT > VIB and AT > BRIGHT at 0 per sound.

### Phase 2: timing foundation
- [x] Note offsets inside a step (ticks) in the model, engine and file; recording keeps the played time when Rec Quantize is off.
- [x] Rec Quantize on / off (Shift + Pad 10, and on Patterns).
- [x] Q list: 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 1/64.
- [x] Time signature (project-wide; Shift + K2 on Patterns).
- [x] BPM per pattern or global (B2 on Patterns); hold B1 + wheel for BPM, Push switches whole / tenths.
- [x] Count-in (Shift + Pad 4): one bar before recording.
- [x] Seq Record arms (flashing) and recording starts on Play; pressed while playing it records at once; Seq Record again stops recording, playback continues.
- [x] Half Seq (Shift + Pad 2), Double Seq (Shift + Pad 3, routed in phase 1a), Half / Double Speed (Shift + Pad 6 / 7).
- [x] Time Correct (Shift + Pad 14): K1 Q, K2 shift, K3 swing, pads choose, B2 all, B3 do it, undoable. Replaces "quantise take".
- [x] Wheel moves the playhead on Patterns (by beat; Shift by Q).
- [x] Patterns up to 128 bars: event-list storage instead of the fixed `steps` array (model, engine mirror, project file with a new section so old files load, Steps / Roll / Song / Motion / Capture / Double Seq follow). Audio thread still never allocates: the pool is made at start.

### Phase 3: Step Edit and automation
- [ ] Step Edit page: the step's events listed, wheel steps by Q, −/+ through events, pads pick a pad's event, K1 length, K2 Q, K3 velocity, B3 erase event, fader nudges timing.
- [ ] Erase + pad on Step Edit: all of that pad's events, B1 cancel / B3 do it.
- [ ] Motion records sampler pad parameters (volume, tune, pan, envelopes, velocity sensitivity, offset) per pad.
- [ ] Erase + pot on Motion asks B1 / B3 first.

### Phase 4: sampler pad depth
- [ ] Loop point and Loop Lock (Shift + B1 on Trim); Loop button toggles looping on the last pad; Shift + Loop reverse.
- [ ] Note On (Shift + Chop) as the pad's Gate / One shot toggle, with its icon.
- [ ] Per pad: offset, pad link (1–16, same bank), mono / poly, velocity sensitivity, decay from start / end, mute groups 1–16.
- [ ] Per-pad filter (Off, Classic, LPF2, LPF4, HPF2, HPF4, BPF2, BPF4) and filter envelope; keep the kit filter as the track's.
- [ ] Kit volume (Shift + K2 on the mix page); volumes shown in dB (−inf, −74 to +6).
- [ ] Normalize (Shift + B2 on Trim, undoable).
- [ ] Warp: 50–200 %, # beats, Seq lock; Shift + Pad 15 picks Time stretch / Pitch (maps onto Follow).
- [ ] 16 Levels: Filter type, Tune with the original on pad 4, Shift + 16 Levels cycles Type; on a synth track 16 Levels spreads the last note, not Keys.
- [ ] Hold Sample + pad: select a kit pad without playing it.

### Phase 5: chop, sample files and browser
- [ ] Chop: Manual (pads tapped live place cuts), slice end on K2, Shift + K zoom per handle, Shift + B1 Extract to the next free pad, Shift + B2 Split, Shift + B3 Merge.
- [ ] Trim Sample (Shift + Pad 13): cut the file to the region, a new file, undoable at the pad.
- [ ] Save Sample: the region as WAV under `Samples/`, Name sheet.
- [ ] Browser: folders as categories (wheel, Push in, B1 back), B2 drive, B3 auto-preview of the **file** (not the kit), −/+ move; loading keeps or offers the pad's settings; Shift + pad loads to the next empty pad.

### Phase 6: audio input and rendering
- [ ] Input Config page complete: source Mic / Rear / Rear L / Rear R / Resample / USB / USB L / USB R (the USB-C gadget capture exists in `drivers/usb_audio`), monitor Off / Auto / On, threshold −96..0 dB, rec length Free / Seq, rec input FX.
- [ ] Record: tap the pad to record onto; tap other pads during a take to chop it; Stop or the pad stays, Sample Record returns to the pad.
- [ ] Audio Recall (Shift + Sample Record): 25 s ring always filling from the source, onto the next empty pad, written by the service thread.
- [ ] Resample (Shift + Pad 11): render the pattern (or song) to a sample, then pick the pad.
- [ ] Song: audio mixdown to WAV (Name sheet), and Song → new pattern.

### Phase 7: performance effects
- [ ] Knob FX performance mode (decisions table) with the MPC list; pots locked to it while on.
- [ ] Missing effects in the picker: Diff delay, Sample (Haas) delay, Spring reverb, Pumper, Amp sim, Tube drive, Soft clipper, Ensemble, Multi-chorus, Auto-wah, Vintage (MPC3000 / MPC60 / SP1200 / SP1200 ring), Vinyl, Tape emulator; Shift + K second parameters for effects.
- [ ] Pad FX: four punches at once (the fifth bypasses the oldest), B1 latch, pressure as depth on every punch; add the MPC's Comb, Ring mod, LoFi, Color (cassette, flutter, tube, vinyl, saturation, radio), Granulator, Beat repeat, Rev stepper, filters with LFO.
- [ ] Flex Beat (Shift + Pad FX): pad 1 empty, 2–16 time effects, K1 one shot / loop, K3 mix, B3 quantize.
- [ ] Compressor (Shift + Pad 5): master, B1 Color, B3 bypass, K1 attack, K2 release, K3 amount, Shift + K3 in boost.

### Phase 8: fader
- [ ] Fader page (Shift + Pad 9): pad volume, pan, tune, attack, decay / release, cutoff, kit volume, mod wheel; B3 on / off; per track; defaults per engine.
- [ ] Context overrides (Mix: selected track level; Step Edit: nudge; Sends: send; open question 2 for the rest).
- [ ] Fader light brightness follows the value (brightest at centre for pan and tune).
- [ ] Takeover: Pickup / Scaled / Instant for knobs, Knob FX and fader (replaces Settings' pickup on / off).

### Phase 9: MIDI and sync
- [ ] MIDI Config page (Shift + Pad 8): port (External UART5 / USB), in channel All / 1–16, out channel, pad MIDI in / out (Never, Always, Empty), sync in / out (Off, Clock, MTC), thru, program change → pattern, CV sync out on / base 1–8 / division 1–24, reset factory settings.
- [ ] Melodee input path: notes to the selected track (or by channel to tracks), clock in following tempo and transport.
- [ ] Output: pads and sequences out, clock out, CV clock on `CV_CLK_OUT` (`ClockIo` in the base).

### Phase 10: projects, system, navigation polish
- [ ] Project: Load Kit only (samples, no patterns), demo projects, New project with B3 / B1.
- [ ] SD card access over USB (mass storage through the gadget / `edisksd`), other modes locked meanwhile, B1 leaves.
- [ ] Undo covers Song edits, mix, trim, chop and effect changes.
- [ ] Direct jumps to the sampler pages (page map like Edit's) instead of B1 / B3 one by one.
- [ ] Hold Seq + Sample: Patterns on screen, pads play the kit (both lights blink).
- [ ] MELODEE.md rewritten for the new control map; Shift overlay and ControlMap board match the device.

## Log

- 2026-11-10: gap list made against the MPC guide; decisions above taken; nothing built yet.
- 2026-11-10: phase 0 done. Steps writes the kit pad hit last (`Track::step_note()`); 16 Levels lit while the pads play levels; Shift + Erase is Copy (kit pads, patterns on Patterns, also across tracks; elsewhere a notice), pattern clear only by Erase + pad on Patterns; Erase + pad on a kit erases that pad's notes while playing or recording (one undo, folded into an open take); Shift + −/+ undo / redo everywhere, Transpose on Keys row 2 (wheel); 909 pads 12–16 dark with a notice; Pad Bank tap on a track without banks says what Pad Bank + pad does. Previews `transpose`, `patterncopy`. Track copy (Pad Bank held) moved to phase 1c. Checked on the device through `--ctl melodee-press` (which now also takes `erase+pad3`, `pad-bank+pad5`: any button held around a pad): Pad Bank notice, 909 pad 12 notice, Steps writing A03's note, Erase + pad playing / stopped / in a take with undo and redo, pattern copy across tracks, undo on Keys, Transpose on row 2. 16 Levels light seen by Kerem: lit on Levels, dark with levels off. Then (Kerem): every pattern edit on the take's pattern during Seq Record (Erase + pad on Steps, Patterns or a kit, quantise, double, copy) is part of the take, not an undo of its own; checked on the device (Steps erase in a take, one Undo take, then Nothing to undo).
- 2026-11-10: phase 1a done. Seq opens Patterns, whose cards are K1 length by the bar, K2 Q, K3 swing; the Length page is gone (Kerem: fold it in; B2 Song stays). Shift + Seq is Step Edit (Steps → Roll → Motion); "quantise take" is off the buttons until Time Correct. Loop opens the last pad's Play page on a kit, Shift + Loop flips its reverse. Shift + Sample opens Input Config (`pages/input_config_page.cpp`: source on B1–B3, threshold on K1, the input meter; a `RecordSource` command so the meter follows the source before arming); Record keeps K2 bars and K3 gain, B2 opens Input Config. Shift + Sample Record says Audio Recall is not there yet; Shift + Seq Record opens Recall (Capture). Shift + Sample Select: Sounds star, kit Save Sample (Name sheet, then "not yet"), synth Save sound. Shift + Tap Tempo cycles the metronome Off / On / Rec (`Project::click` is now a mode; Rec follows Seq Record). Back is a 16-deep page stack (`shell/page_stack.cpp`; sibling groups are one stop). Minimal Shift + pad route (`shell/shift_pads.cpp`): 3 Double Seq (was Shift + Loop), 12 Song, 16 Project (was Shift + Sample), except on Patterns and Punch, which keep Shift + pad for now. Button lights faint where the button has a job on the page, bright in use, dark otherwise (`shell/button_jobs.cpp`; Kerem's addition). Previews `inputconfig`, `shiftkit`; `length` dropped. Test `button_remap()`.
- 2026-11-10: phase 1b done. Shift + Pad 1–16 are the printed jobs on every page, names from Akai's features guide (`shell/shift_pad_jobs.cpp` holds the table the dispatch, the overlay and the lights share; `shell/shift_pads.cpp` dispatches). Printed wins on Patterns and Punch (the rule above): Patterns' column launch moved to a B3 COLUMN latch (header LAUNCH COLUMN), Punch's lock to Push while a punch is held (Push again unlocks). Wired: 1 Full Level (pad response "Full level", every hit at 127; again back to the curve it came from), 3 Double Seq, 4 Count-In and 10 Rec Quantize (toggles in Melodee's settings file; behaviour in phase 2), 12 Song, 16 Project; the rest say "<name>: phase n". The overlay draws the sixteen as the pads sit with ON / OFF / phase marks next to a one-line button list, and while Shift is held the pads light (toggle on bright, a job faint, not built dark) and, as Akai's guide says, the buttons with a built Shift job glow faintly and the rest go dark (`button_has_shift_job()`). Shift + K: `shift_bindings()` is the Shift row, `page_bindings()` swaps it in while Shift is held, cards carry a Shift icon (filled while held), the overlay's knob strip shows only Shift cards; Keys transpose is Shift + K3 and `keys_row` is gone. −/+: lists (Sounds, Samples, Project, Patterns, Mix, Sends) move as the wheel, Tempo by a whole BPM, Motion nothing; octave stays on Stage, Keys, Edit, Chord, Roll. Remote: `shift+k3=V`, `hold-BUTTON` / `release-BUTTON`, `hold-padN` / `release-padN`; `status` adds `melodee_pads=`. Previews `shiftkeys` added, `transpose` redrawn. Test `shift_pads()`. Checked on the MPC through `--ctl` (see phase1b.md).
- 2026-11-10: phase 1d added (Kerem: synths ignore velocity and aftertouch); findings in its preamble.
- 2026-11-10: phase 1c done. Akai's features and pad_play pages read for Mute, Note Repeat and Pad Bank. **Mute** (`shell/mute_mode.cpp`): a tap latches mute mode, a hold lasts while held, Shift + Mute unmutes every track, solo and kit pad; in mute mode the pads mute kit pads (bank in view) where they play a kit, tracks on Stage, Mix and the other pages; Shift + pad solos a track, or a kit pad (the kit's other used pads muted; again on it, the kit back). Mute bright, header "Mute: pads / tracks", pads muted red, soloed white. **Per-pad mute**: `Track::pad_mutes` (a bit a pad), `Command::PadMute`, the sampler skips and fades muted pads, project file `PMUT`. **Note Repeat** (`shell/note_repeat.cpp`) stays on the page: tap latches, hold is temporary, double tap (450 ms) opens Arp; while on, B1–B3 are 1/8, 1/16, 1/32 and with Shift 1/8T, 1/16T, 1/32T (not on Arp and Chord, whose keys stay); the arp's rates are now 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T (K2), every second straight note late by the project swing (`arp_swing_ticks`), the arp clock restarts on Play so the swing falls with the sequencer's. **Pad Bank** (`shell/pad_bank.cpp`): kits have 8 banks / 128 pads (`sampler_banks`), banks E–H in the file's `KITX` section (TRAK keeps A–D, so older files load unchanged), kit keys run to 163 (the host keeps keys past 127 from the synths); tap steps A–H round (a "Bank C" notice), held + −/+ or wheel jumps, held shows the sixteen tracks over the page (Stage's grid, `pages/track_grid.cpp`), bank lights A–H. **Track copy** (`shell/track_copy.cpp`): Shift + Erase with Pad Bank held, then a track and its target: sound, kit, patterns and their motion, the target's mix strip kept, one undo (`TrackState` in the history); the audio thread takes it whole through a `TrackPack` (`Command::TrackLoad`), handed back for freeing like a project pack. Overlays go through one `draw_overlays()` (app and previews). Previews `mutepads`, `padbank`, `copytrack`, `repeat`, `repeattriplets`. Test `parity_1c()`. Decisions taken here (Kerem can overturn): the MPC's Pad Bank + pads 1–8 bank jump is not built (Pad Bank + pad stays track select, per the table); a kit pad solo is "mute the others"; Shift + Mute unmutes everything, not only what the page mutes; mute mode on Stage and Mix mutes tracks even on a kit track; Shift + Note Repeat acts as Note Repeat (nothing printed for it).
- 2026-11-10: open question 4 answered (Kerem): pressure does what aftertouch normally does, vibrato and a brighter sound; in the decisions table.
- 2026-11-10: phase 1d done. `engine/expression.h` holds the mod matrix sum, the velocity curve, pressure smoothing and the vibrato for all four synths; FM6, SID and CZ now hear `set_mod` and `set_pressure` (each with its own destination names, K2 skips the ones it lacks: FM6 Brightness / Feedback / Carrier and Mod pitch, SID Osc and Osc 2 pitch, CZ DCW / Line 1 and 2 pitch) and gain an LFO (LFO RATE) for the matrix and the vibrato. Every synth has AT > VIB (35) and AT > BRIGHT (50) at the end of its list, on a new Touch page before the mod matrix; PROPHET's vibrato is its own sine (VIB RATE), apart from its LFO. Velocity: one squared curve, defaults raised (PROPHET VEL > AMP 65 / VEL > FLT 40, FM6 70 per operator, CZ 70, SID's new VELOCITY 70): a hit at 40 plays 6–9 dB under a full one. Pressure reaches every note of a chord and the arp's notes from a held pad (`Arp::pressure`, `EngineHost::arp_note`). Kerem asked for it to be optional: Settings' Pad pressure On / Off (off: the pads send no pressure at all). Older projects (`PVER` section) and sounds (a last byte) load the new parameters at their defaults (`upgrade_params`). Preview `touch`. Test `pad_expression()`. Played by Kerem on the MPC: feels right.
- 2026-11-10: phase 2 done. Akai's recording_sequences, sequence_mode, time_correct, editing_sequences and features pages read for it. **Clock** 96 ticks a quarter (the MPC60's; the arp keeps its own 24). **Patterns are event lists** (`model/pattern.h`: tick, length in ticks, key, velocity, chance, flags), up to 128 bars of the project's time signature; `Step` is now a view of the notes starting in one step of Q (`Pattern::step` / `set_step`, each note with its offset in the step), so Steps and Roll edit as before and draw an off-grid note where it fell. The audio thread plays copies: every edit sends `Command::PatternData` with a copy made on the MIDI thread, swapped in by pointer and handed back to be freed there (as samples and project packs are) rather than a pool; `SetStep`, `Length` and `Division` are gone. Project and track packs carry their patterns the same way. File: `EVNT` (events), `MOT2` (motion in sixteenths), `TIME` (signature, BPM per pattern); `PATT` / `MOTN` from older files load (24 ticks a quarter and the old five divisions mapped, motion steps turned into sixteenths). **Q**: 1/4 .. 1/64 with triplets; only the grid (changing it moves no note). **Rec Quantize** acts: off, a note keeps the tick it was played on and its held length; on, the nearest step of Q, and a note moved ahead of the playhead is skipped once (`Command::Recorded`) as it was heard live; Patterns' header shows REC Q 1/16 or REC Q OFF. **Seq Record** arms while stopped (flashing, header REC ARMED), Play starts the take, pressed while playing records at once, again stops recording with playback going on, Stop ends both. **Count-In** plays a bar of clicks before the pattern (header COUNT n, Seq Record flashing; nothing recorded meanwhile). **Patterns**: K1 1, 2, 4 .. 128 bars, Shift + K1 any count, K2 Q, Shift + K2 time signature (1/4–9/4, 3/8–15/8, 5/16–15/16; whole-bar patterns keep their bar count), K3 swing, Shift + K3 metronome level; wheel moves the playhead by a beat (Shift: by Q; stopped, where Shift + Play carries on); hold B1 + wheel for BPM (Push: whole / tenths); **B2 SEQ / GBL BPM** (Song stays on Shift + Pad 12: the phase 2 box gave B2 to the BPM, as on the MPC). **Half Seq, Half / Double Speed** (Shift + Pad 2, 6, 7) and **Time Correct** (Shift + Pad 14, `pages/time_correct_page.cpp`) built, each one undo; "quantise take" and its take notes are gone. Previews `bpm`, `countin`, `recarmed`, `timecorrect`. Test `timing()`. Decisions taken here (Kerem can overturn): BPM per pattern is per **pattern number** (the column), and the pattern launched last on any track leads; until a pattern's tempo is set it plays at the global one. Half / Double Speed keep the pattern's length (Akai only speaks of the events), so Half Speed drops what falls past the end. Time Correct's swing is its own (50–75 %, every second step of a straight Q), apart from the project's live swing, so notes are not swung twice; its shift runs −48..48 ticks. Play always starts from the pattern's start (Akai: "from the beginning"); Shift + Play from where it stopped or where the wheel put it. Checked on the MPC through `--ctl` (see phase2.md): Seq Record arm / record at once / stop recording with playback on / Stop, Count-In (COUNT n, nothing played until the bar is counted), notes recorded off the grid with Rec Quantize off (tick 40 on a 1/16 grid) and on it with Rec Quantize on, Time Correct with undo / redo, Half Seq, Double Seq, Half / Double Speed and their undos, K1 to 128 bars (49152 ticks), Shift + K2 to 2/4 and back, wheel and Shift + wheel locate, Shift + Play from there, B2 SEQ / GBL and B1 + wheel BPM (whole and tenths); xruns 0, dropped 0. Not heard yet: the count-in clicks, off-grid notes in time, a quantised note not doubled, BPM per pattern switching at the bar (asked of Kerem).
