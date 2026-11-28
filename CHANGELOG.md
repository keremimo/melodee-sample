# Changelog

Changes in Melodee for the Akai MPC Sample. Install and update steps, and the notes of earlier versions, are on the [releases page](https://github.com/keremimo/melodee-sample/releases).

## 0.10.3 (2026-11-28)

### New

- **Plug-ins reload by themselves.** Copy a new build of a plug-in over its file on the card (from the web page, over card access, or any other way) and a moment after the copy ends Melodee opens it again on every track that uses it, with the sound it had. The header shows **Reloaded** and the plug-in's name. Melodee looks only at the plug-in's own file: after changing the folders beside it, pick it again with **SAMPLE SELECT**. See [Plug-ins](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/plug-ins.md#what-runs).
- **For plug-in makers: CLAP `request_restart`.** A CLAP plug-in that calls it is opened again with its state, from a fresh copy of its library, so it reads the files beside it again. [Melodee Modules](https://github.com/keremimo/melodee-modules) uses it to pick up module and SysEx files added to its folders.

### Fixed

- **A MIDI keyboard on 16 Levels plays an instrument pad with its TUNE** (#43). C4 (key 60) plays the pad as its pads play it. In **Zones** the keyboard plays the instrument at its real pitch, as the pads do. See [Sampled instruments](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/kits-and-sampling.md#sampled-instruments).

## 0.10.2 (2026-11-28)

### New

- **Sequence colour from the Menu.** On Sequences, hold the encoder to open the Menu and push **COLOUR** (on Patterns: **SEQUENCE COLOUR**). The pads light in the 16 colours. Press a pad or turn the encoder to pick one for the sequence in hand. **B3** (**DONE**) keeps it as one undo step, **B1** (**BACK**) keeps the colour it had, **B2** (**NONE**) takes the colour away. See [Sequences page](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/sequencing.md#sequences-page).

### Licence

- **NOTICE**: Melodee's additional terms under section 7 of the AGPL: author attribution, origin, and the names and logos. It is in the repository and in the zip's `licenses` folder. Each source file of Melodee's own carries its copyright line.

## 0.10.1 (2026-11-28)

### Fixed

- **USB-C audio from a computer or phone at 44.1 kHz** (#74). With the MPC at 44.1 kHz, part of what a Mac or an iPhone played into the MPC could arrive as short bursts of silence: in a take, on a sampled pad and while monitoring, on some starts and not others. Melodee now starts its USB-C stream back to the computer as soon as the computer starts playing, so the computer keeps time with the MPC from the start.

### New

- **Record into the song on an audio track.** On Song, select an audio track's lane, move the cursor to the bar to start on, press **SEQ RECORD**, then **PLAY**. The track's input records into a new clip at the cursor, which grows a bar at a time. **SEQ RECORD** again or **STOP** ends it. **SHIFT** + **−** takes back the clip and the take together. See [Record into the song](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/song.md#audio-tracks).

## 0.10.0 (2026-11-27)

Back up `Melodee/Projects` before you update. In 0.9.1 and earlier, a track colour you picked shows as the colour of the track's place, and a pad whose layers follow a CC plays as **Random hold**.

### Changed: read this first

- **Mutes land on the grid** (#40). While a pattern plays with **Rec Quantize** on (**SHIFT** + **Pad 10**, on by default), a mute or solo waits for the next step of the pattern's **Q**. The pad blinks until then. Press it again before that to call it off. Turn **Rec Quantize** off for mutes at once.
- **SFZ regions follow their controller** (#66). Regions chosen by a controller (`locc`, `hicc`), such as a hi-hat's on CC 4, now switch as the controller moves. Before, only the region at the controller's starting value played. Regions started by a controller move (`on_locc`) are left out.
- **ERASE on Song with a loop range** takes the range's bars out (see Song below). Turn the loop off to delete one clip.
- **Song's patterns are named A1 to H16**, as on the pads, and **K2** steps through the track's patterns that hold notes and the next empty one.
- **A plug-in with its own screen opens on PANEL**, the first page under **B1**.
- **Time Correct's B2 is GROOVE.** Hold **B2** and press a pad to take the groove from that pattern (**PAD BANK** for the next bank, **−** / **+** for another track's patterns). **ALL** moved to **SHIFT** + **B2**. The pads play the track's sounds.

### New

#### Sequencer

- **SEQ + pad for sequences** (#50): in Settings > **SEQ**, set **Seq + pad** to **Sequences**. **SEQ** + pad then launches a sequence and **SEQ** + **SHIFT** + pad a pattern.
- **Time Correct APPLY** (#55): **B1** (**APPLY**) moves the notes to where Time Correct plays them, with the velocities you hear, and sets Time Correct back to off. Roll and Step Edit then show the notes on the grid. One undo step.

#### Song (#60)

- **All tracks**: **PAD BANK** shows the 16 tracks at once. Press it again for the selected track's lanes.
- **Loop range**: press **LOOP** at the first bar, move the cursor and press **LOOP** at the last bar, over clips or empty time. Press it twice on a clip without moving to loop that clip. With a loop on, a press turns it off.
- **Copy, paste and take out sections**: with a loop range, **SHIFT** + **ERASE** copies its bars, and Push pastes them at the cursor. What is there moves on to make room; push again for another copy after it. **ERASE** takes the range's bars out and closes the gap. With all tracks shown, this works on every track and moves markers and tempo points too.

#### Kits and instruments

- **Layers chosen by a MIDI CC** (#66): on **LAYERS**, **SHIFT** + **K1** past **Random hold** is **CC 1** to **CC 127**. **K2** / **K3** set the controller values that play each layer, for a hi-hat pedal on CC 4: closed, quarter, half and open. With **SEQ RECORD** on, the controller is recorded as a **CC** lane on Motion.
- **MPC 3 drum kits** (#62): `.xtd` kits from MPC 3 expansions show in the browser with a pads icon and load with **LOAD KIT**, their shared samples and slices included.
- **Track colour** (#69): on the mixer, hold the encoder for the Menu and push **TRACK COLOUR**. The pads light in the 16 colours: press one, or turn the encoder. **B3** (**DONE**) keeps it, **B1** (**BACK**) goes back, **B2** (**DEFAULT**) gives the track the colour of its place. The track's patterns, Song clips, pads and fader light take it.
- **Progression folders** (#65): 16 Levels **FILE** shows the progressions one folder at a time. **B3** (**OPEN**) or Push opens a folder, **B1** (**BACK**) goes up. Subfolders of `Melodee/Progressions` and `Progressions` are read four deep.
- **Preview level** (#39): in the sample browser, the fader sets the preview level, also of the file playing. Melodee keeps it when you restart.

#### MIDI

- **Start and stop from a foot pedal** (#7): in MIDI Config, learn a pedal as **Remote play CC**. A press plays, the next stops. **Remote record CC** does what **SEQ RECORD** does.
- **Program changes to plug-ins**: **Receive program change** set to **Plug-in** loads that preset into the plug-in of the track the message reaches, with Bank Select for more than 128. **Send program change** sends a preset you pick on a plug-in track to the track's MIDI channel.
- **Remote preset CC**: learn a controller knob to step the selected track's presets. Push on a Remote CC row learns it.

#### Screen

- **Screensaver** (#49): in Settings > **SCREEN**, after 1, 2, 5, 10 or 30 minutes at rest, the Melodee logo bounces around the screen and the backlight dims. Off by default.

### Fixed

- **A project from another card** (#72): a project, bank or settings file copied to a new card finds its samples and plug-ins at the same place on the new card.
- **MIDI clock tempo**: the tempo shown and played no longer flickers between two values while following MIDI clock. Changing the tempo while the clock leads shows **Tempo locked to MIDI sync**.
- **MIDI Learn**: a knob learned after a wait that ran out is no longer taken early, and a control just learned does nothing until it rests.
- **Time Correct** draws each note where it plays, with the groove and humanize too. **SWING** shows dim at 0 % strength, where it does nothing, and **ERASE** + a knob sets a value back to its default. **−** / **+** step the pattern. Before, the groove row read 1–16 in every bank and **PAD BANK** did nothing on the page.
- **Song**: played from a bar after the loop range, the song plays on to its end and stops. A Song clip longer than 128 bars keeps its length when the project opens again.

## 0.9.1 (2026-11-27)

### Fixed

- **Step Edit's SLICE card.** On a kit step's **SOUND** cards, **K2** (**SLICE**) shows **PAD**, the slice (**5 / 16**) or **RND**. The panel shows the slices of the step's sample with the step's slice lit, and **B2** (**SLICES**) on the sounds shows them numbered. In 0.9.0, **K2** set the slice but its card, the slice edges and **SLICES** stayed blank.

## 0.9.0 (2026-11-26)

Back up `Melodee/Projects` before you update. A project saved in 0.9.0 plays ten times too slow in 0.8.3 and earlier.

### Changed: read this first

- **960 ticks a quarter.** The sequencer counts 960 ticks a quarter note, as the MPC does (96 before). MPC projects import without rounding. Projects from earlier versions load with their timing kept.
- **A fader target you pick stays** (#57). On the Fader page, the target you choose with the encoder is pinned: a plug-in's **FILTER**, **ECHO** and **SPACE** and the knobs of other pages no longer take the fader. **B2** unpins it. **K1**–**K3** on the Fader page still take it.
- **Chop's pads play through the kit pad.** When a kit pad holds the sample, a slice's pad plays the slice through that kit pad, with its tune, envelopes, filter, Warp, Mono/Poly and Pad FX, as its recorded hits do. Taps overlap.

### New

#### Sequencer

- **Track speed** (#67): ×1/64, ×1/32, ×1/16, ×1/8, ×¼, ×½, ×¾, ×1, ×1½, ×2 or ×4. On Patterns, **SHIFT** + **K3** (**SPEED**). At ×1/64 one bar of the pattern lasts 64 bars. See [Track speed](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/tracks.md#track-speed).
- **Track direction** (#67): on Patterns, **SHIFT** + **LOOP** sets **FWD**, **REV** or **RND**. **REV** plays the last step first. **RND** plays every step once a pass, in a new order each pass; the orders are the same each time the pattern plays from the top. The track list shows ▶, ◀ or a die. See [Track direction](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/tracks.md#track-direction).
- **A step's own sound** (#67): on a kit or keygroup track, **B1** on Step Edit has a fourth card set, **SOUND**.
  - **K1** (**SOUND**): any sample on a kit or keygroup pad of the project. The step plays it through its own pad.
  - **K2** (**SLICE**): a slice of the sample's chop, or **RND**.
  - **K3** (**PLAY**): **FWD**, **REV** or **RND**.
  - Hold a step and press **SAMPLE SELECT** to pick the sound on the pads. **ERASE** + knob sets a card back to **PAD**.
  - See [The pattern editors](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/sequencing.md#the-pattern-editors).
- **Pattern lengths** (#52): **K1** on Patterns sets 1 to 16 bars one bar at a time, then 24, 32, 48, 64, 96 and 128 bars.
- **Up to 128 bars:** Looper lengths 32, 64 and 128 bars; Sample Record's length 24 to 128 bars; a song clip 1 to 128 bars. A loop longer than 64 s gets memory made for it: the first press says *making memory, press again*.

#### Samples

- **Trim before loading.** In the sample browser, turn **K2** (**START**) or **K3** (**END**) to open the trim view: the whole sample and a closer view. The encoder moves the point in finer steps. Push loads the sample onto the last pad you hit with that start and end; **SHIFT** + pad loads it onto the next empty pad. **ERASE** sets both back. The file on the card is not changed. See [Trim a sample before loading it](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/kits-and-sampling.md#trim-a-sample-before-loading-it).
- **Reverse one slice.** On Chop, **SHIFT** + **LOOP** reverses the slice in hand. A reversed slice shows an arrow. **TO PADS** and the **MPC 3** export lay it as a reversed pad. One undo.

#### File sharing

- While Melodee runs, a computer connected with USB-C can open the card as a network drive. Melodee keeps working and keeps the card.
- On a Mac, choose **Melodee** under **Network** in the Finder sidebar, or **Go** > **Connect to Server** and **smb://melodee.local**. On Windows, open `\\192.168.155.1`. Connect as **Guest** and open **SD Card**.
- On until you turn it off in **SETTINGS** > **AUDIO** > **File sharing**. Needs firmware 1.3.0.12; on firmware 1.4 it does not start.
- See [File sharing](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/projects-and-files.md#file-sharing).

#### Projects

- **SAVE AS with Autosave off** (#63): **SHIFT** + **B1** on the Project page.
- **Undo** keeps the last 512 steps (32 before), up to 64 MB.

### Fixed

- **Decay From Start** (#64): a one shot fades to silence over its decay, whatever its sustain. Sustain is for Note On mode. The Stage picture shows it.
- **Copy a pad to another kit** (#48): **SHIFT** + **ERASE**, the source pad, then hold **PAD BANK** and press the other kit's track pad, then the target pad. The copy came from the new track's own pad before.
- **SAMPLE SELECT on an audio track** (#53) no longer crashes Melodee when sounds of your own are saved.
- **Pad colours** (#69): the sixteen colours light the pads as vivid as the screen draws them. On Stage **COLOUR**, **Kit** (the colour the MPC kit brought) comes back at the end of the list after another colour is picked.

### Faster

- Saving a project goes through the sample slots in use only.
- Kit pads played into submixes take less work on every block.

## 0.8.0 to 0.8.3 (2026-11-24 to 2026-11-25)

### Changed: read this first

- **The mixer replaces Tracks and Sends** (0.8.0). **PAD FX** + **KNOB FX** opens it. Stage's **MIX** and **SENDS** are gone: a track's level, pan and sends are on the mixer, and **B3** on Stage is **FX**. On a kit, **B3** still has **SENDS** for the pads' own sends.
- **The mixer opens on the strips** (0.8.2): every channel of the row side by side. Hold **+** for the selected channel's card, hold **−** to go back. **−** / **+** act when you let go.
- **Track speed** is on Patterns, **SHIFT** + **K3** (0.8.0). The metronome level is on Tempo.
- **CPU** shows in the Menu's header (0.8.0).
- **Plug-in tracks have no separate Edit page** (0.8.0). Picking a plug-in opens Stage on **EDIT**; **PANEL** is the plug-in's own screen. Hold **SHIFT** on a **B1** page: **B1** (**PLUG-INS**), **B2** (**PRESETS**), **B3** (**TRANSPORT**).
- **Sequences keep track mutes** (0.8.3). Launching a sequence sets its mutes at the next bar (at once when stopped).

### New

- **Mixer** (0.8.0). Every track plays through its effect chain, then its strip: trim, nine-band EQ, compressor, width; then level and pan. **B1** (**SENDS**, then **FX**), **B2** (**EQ**), **B3** (**COMP**). See [The mixer](https://github.com/keremimo/melodee-sample/blob/main/docs/guide/mixer-and-effects.md#the-mixer).
  - **PAD BANK** shows the second row: eight submixes, buses A–D, the input and the master.
  - In mute mode, a pad mutes its channel and **SHIFT** + pad solos it.
  - With **SEQ RECORD** on, level, pan, sends and strip are recorded as Motion lanes.
- **Pad outputs** (0.8.0): a kit pad plays into the kit or into submix 1–8. Kit's Stage **SENDS**, **SHIFT** + **K2** (**OUTPUT**).
- **USB out** (0.8.0): **SETTINGS** > **AUDIO** > **USB out** sends **Tracks** (1–16, mono) or **Submixes** (1–8, stereo) to inputs 3–18.
- **Pad stems** (0.8.0): **STEMS** writes each kit pad the song plays as a WAV of its own, in a folder for each kit, and one WAV for each submix. When one play of the song cannot write them all, it plays again (*Stems: pass 2 of 3*).
- **MPC 3 export** sends each track to the same submix in MPC 3 (0.8.0). **MPC 3** and **LIVE SET** say *mixer settings left out* when the project has settings they cannot carry.
- **Companion screens** (0.8.0): a VST2 plug-in gets a page of its own from a screen folder in `Melodee/Plugin Screens/`. See [Companion screens](https://github.com/keremimo/melodee-sample/blob/main/docs/PLUGIN_COMPANIONS.md).
- **CLAP presets in folders** (0.8.0): a preset location with `/` in its name lists as folders on **PRESETS**.
- **The browsers start at the top of the card** the first time, then where you left them (0.8.0).
- **Firmware 1.4.0.8** (0.8.0).
- **Copy a sequence** (0.8.3): on Sequences, **SHIFT** + **ERASE**, the sequence, then where it goes. Its patterns are copied too.
- **Clear one lock** (0.8.3): hold a step, hold **ERASE** and turn the lock's knob.
- **Plug-in pages** take **MUTE** and **ERASE** taps (0.8.3).

### Fixed

- **MIDI tracks after Play** (#59, 0.8.3): notes play from the first bar after Play, Locate or MTC.
- **Plug-ins at exit** (0.8.3): a CLAP or VST2 plug-in built with a static C++ runtime no longer crashes Melodee when it is unloaded.
- **Echo and reverb tails** (0.8.1): an effect plug-in keeps running for the tail it reports, up to 10 seconds.
- **Presets on a plug-in at rest** (0.8.1): a preset picked for a sleeping CLAP plug-in takes effect at once.
- **A sustain pedal left down** (0.8.1) no longer keeps a plug-in instrument running: All Sound Off (CC 120) and Reset All Controllers (CC 121) lift it.

### Faster

- The screen copies the kits and the pattern overviews only after a change (0.8.1).
- Plug-ins take less work on every block (0.8.1).
- 32-bit VST2 plug-ins: Melodee sleeps until a slow plug-in answers instead of keeping a core busy (0.8.1).
- The controls' lights no longer go through all 8,192 sample slots every 5 ms (0.8.1).
