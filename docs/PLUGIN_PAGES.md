# Plug-in page files

<!-- SPDX-License-Identifier: MIT -->
<!-- This format reference is MIT licensed, apart from the rest of Melodee, so that plug-in makers and other hosts
     may copy it. -->

Melodee shows a plug-in's parameters on the MPC Sample in pages of rows of
three, one row on **K1**–**K3** at a time. Without a page file the pages come
from the plug-in itself: its CLAP remote-controls pages, an Open MPC skin's
Q-Link tabs, its parameter groups, else its list. A page file sets the pages
by hand: which parameters, in which order, their names on the cards, and a
picture above the row.

## Where Melodee looks

At load, the first file that is for the plug-in wins:

1. The SD card: `Melodee/Plugin Pages/`, any `.json` file in it (read in name
   order). Use this to change pages for a plug-in you have.
2. The plug-in's own package:

   | Format   | Place                                                   |
   |----------|---------------------------------------------------------|
   | VST3     | `<name>.vst3/Contents/Resources/melodee-pages.json`     |
   | LV2      | `<name>.lv2/melodee-pages.json`                         |
   | MPC      | `melodee-pages.json` beside `plugin.json` or `plugin-meta.xml` |
   | CLAP     | `<name>.melodee-pages.json` beside `<name>.clap`        |
   | VST2     | `<name>.melodee-pages.json` beside `<name>.so`          |

3. Melodee's own files, for plug-ins whose makers ship none (Neural Amp
   Modeler).

A file that is not for the plug-in is passed over. When a file on the card or
in a package cannot be used, Melodee writes the reason in
`Melodee/Plugin Pages/problems.txt` (when that folder is on the card), a line
each, and goes on to the next source. Reload the plug-in (pick it again, or
reopen the project) after changing a file.

## The file

```json
{
  "melodee-pages": 1,
  "plugin": { "name": "Melodee Poly" },
  "pages": [
    { "name": "FILTER",
      "params": ["Cutoff", "Resonance", { "name": "Filter Env Amount", "label": "Env Amt" }],
      "picture": { "kind": "filter", "cutoff": "Cutoff", "resonance": "Resonance" } },
    { "name": "AMP",
      "params": ["Amp Attack", "Amp Decay", "Amp Sustain", "Amp Release"],
      "picture": { "kind": "envelope", "attack": "Amp Attack", "decay": "Amp Decay",
                   "sustain": "Amp Sustain", "release": "Amp Release" } },
    { "name": "MORE", "rest": true }
  ]
}
```

- `"melodee-pages": 1`: the format's version. A file of a later version is
  passed over by this Melodee.
- `plugin`: which plug-in, by any of:
  - `clap`: its CLAP id (`"org.surge-synth-team.surge-xt"`);
  - `vst2`: its VST2 unique ID, four letters (`"NAMp"`) or a number;
  - `lv2`: its LV2 URI;
  - `name`: its name as Melodee shows it, with `vendor` where two plug-ins share
    a name. VST3 plug-ins go by name.
- `pages`: in order. Each has a `name` (shown at the top left of the page),
  `params`, and may have a `picture` and `"rest": true`.
- A parameter is its name as the plug-in gives it, exactly, or an object:
  - `name`: the same;
  - `id`: the format's own ID instead of the name (the CLAP param id, the VST3
    ParamID, the VST2 parameter index, the LV2 port symbol), as a string;
  - `label`: its name on the MPC, where the plug-in's is long (`"Gate"` for
    `"NoiseGateActive"`).
- `"rest": true`: after its own parameters, every parameter no page of the
  file names, in the plug-in's order.
- A parameter the plug-in does not have is left out (problems.txt says so).
  Parameters no page names and no `rest` page takes go on pages of the
  plug-in's own groups, where it has them; otherwise they are not shown.
- Text: up to 64 KB. Unknown keys are skipped, so a file can carry more for
  later versions.

## Pictures

Drawn by Melodee above the row in hand, in the track's colours. A part is a
parameter, named as in `params`. A part's colour is its knob's while its row
is in hand.

| `kind`     | Parts                                        | Draws                                   |
|------------|----------------------------------------------|-----------------------------------------|
| `envelope` | `attack`, `decay`, `sustain`, `release`      | the envelope                            |
| `filter`   | `cutoff`, `resonance`                        | a low-pass curve                        |
| `eq`       | `bands`: up to four of `freq`, `gain`, `q`   | the bands' response                     |
| `value`    | `params`: up to three                        | their values, large                     |
| `scope`    | none                                         | the plug-in's sound as it plays         |

Times, frequencies and gains are read from the parameter's value text where
it names its unit (`12 ms`, `1.2 s`, `2 kHz`, `-3 dB`); otherwise from where
the parameter sits in its range (times up to 4 s, 20 Hz–20 kHz, ±18 dB, Q
0.3–10).

```json
{ "kind": "eq", "bands": [ { "freq": "Low Freq", "gain": "Low Gain", "q": "Low Q" },
                           { "freq": "High Freq", "gain": "High Gain", "q": "High Q" } ] }
```

Without a picture in the file, Melodee draws an envelope or a filter where a
page holds parameters named that way (attack, decay, sustain and release of
one prefix; a cutoff with a resonance).

## Skins

A page file may carry a `skin`: the maker's own art for its pages. Melodee
draws it from images and data; nothing in the plug-in runs. A skinned page
takes everything under **B1**–**B3** (320 × 218 pixels). Melodee keeps
**B1**–**B3**, its MENU, notices and questions and draws the page dots at the
top right. Each control is drawn once, by the skin: on the controls
**K1**–**K3** turn Melodee adds only a white dot where the pot sits until it
reaches the value, the fader mark while the fader drives one, and the Shift
mark while Shift reaches its second parameter. A page with no skin page of its
name keeps Melodee's own page. Settings > SCREEN > **Plug-in screens** turns
every skin off.

An example with its art is in
[docs/examples/plugin-skin](examples/plugin-skin/): Melodee Poly's FILTER and
AMP ENV pages.

```json
"skin": {
  "font": "fonts/SpaceGrotesk.ttf",
  "pages": {
    "FILTER": {
      "background": "art/filter.png",
      "controls": [
        { "kind": "curve", "of": "filter", "x": 18, "y": 46, "w": 96, "h": 92, "colour": "FFB347" },
        { "param": "Cutoff", "kind": "knob", "strip": "art/knob-34.png", "frames": 64, "x": 139, "y": 51,
          "label": { "text": "CUTOFF", "x": 126, "y": 92, "w": 60, "colour": "ECE3CF" } },
        { "param": "Cutoff", "kind": "value", "x": 126, "y": 106, "w": 60, "colour": "FFB347" }
      ]
    }
  },
  "strip": { "background": "art/strip.png", "controls": [] }
}
```

- `pages`: a skin page for each page of the file it draws, by the page's
  name. Coordinates are pixels from the area's top left.
- `background`: a PNG, 320 × 218.
- `strip`: an effect's skin in a track's FX chain, everything under the
  chain's row of slots, 320 × 144 (no **K1**–**K3** cards under it).
- `font`: a TTF or OTF for labels and values; without one, Melodee's.
- Paths are relative to the page file's folder and stay inside it. Images
  are PNG; a skin's images come to 4 MB at most once decoded (RGBA), or
  Melodee draws its own page.
- Control kinds:

  | `kind`   | Shows                                                      | Needs                         |
  |----------|------------------------------------------------------------|-------------------------------|
  | `knob`   | a filmstrip frame picked by the parameter's value          | `param`, `strip`, `frames`    |
  | `switch` | the same, a frame a step                                   | `param`, `strip`, `frames`    |
  | `slider` | a track image and its `cap` moved by the value             | `param`, `image`, `cap`       |
  | `meter`  | a filmstrip frame picked by the sound's level              | `strip`, `frames`, `of`: `in` or `out` |
  | `value`  | the parameter's value as the plug-in writes it             | `param`                       |
  | `label`  | `text`                                                     | `text`                        |
  | `image`  | a picture                                                  | `image`                       |
  | `curve`  | the page's picture (`of`: `filter`, `envelope`, `eq`)      | a `picture` on the page       |
  | `scope`  | the plug-in's sound as it plays                            |                               |

- `x`, `y`: the control's top left. `w`, `h`: its size; without them a
  filmstrip's frame size or the image's (a value or label: 100 wide).
  `size`: a knob's square, or a label's or value's text height (11 px and up:
  smaller is drawn at 11).
- `colour`: six hex digits, for labels, values, curves and the scope.
  `align`: `left`, `center` or `right`. A slider is upright unless
  `"orientation": "horizontal"`.
- A control's `label` object is a label of its own, placed beside it.
- Kinds this Melodee does not know are skipped, so a skin can carry later
  ones.

### Making a filmstrip

Draw the control once for each value from its lowest to its highest, every
frame the same size, and stack the frames top to bottom in one PNG: a 34 ×
34 knob of 64 frames is 34 × 2176. Melodee shows frame
`round(value × (frames − 1))`. Filmstrips made for other hosts (KnobMan, the
MPC desktop software's Open MPC skins) work as they are when they are
stacked top to bottom; scale them to the size the page wants.

The MPC's panel shows 16-bit colour: gradients look smoother dithered.
