# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""An MPC Sample project (.xpj) read by the built app."""
import gzip
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/melodee"
HEADER = "ACVS\n1.3.0.12\nSerialisableProjectData\njson\nLinux\n"


def wav(rate):
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    data = b"\0\0" * 100
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def sliced_wav(rate, frames, slices):
    """A mono 16-bit WAV with the MPC's slices in it: the "atem" JSON and the "slic" chunk, as the stock app saves."""
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    def slice_info(start, end):
        return {"Start": start, "End": end, "LoopStart": start, "LoopMode": 0, "PulsePosition": 0, "LoopCrossfadeLength": 0,
                "LoopCrossfadeType": 0, "TailLength": 0.0, "TailLoopPosition": 0.5}
    value = {"defaultSlice": slice_info(0, frames - 1), "numBars": 2, "Num slices": len(slices)}
    for n, (start, end) in enumerate(slices):
        value["Slice %d" % n] = slice_info(start, end)
    atem = json.dumps({"version": 1, "value0": value, "value1": {"version": 1, "note": "F", "scale": "Natural Minor"}}, indent=4).encode()
    slic = struct.pack("<HHH", 1, len(slices), 0) + b"".join(struct.pack("<IIIH", s, e, s, 0) for s, e in slices)
    data = b"\0\0" * frames
    def chunk(name, body):
        return name + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"atem", atem) + chunk(b"slic", slic) + chunk(b"data", data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def chunks(path):
    """A WAV's chunks by name."""
    raw, out, at = Path(path).read_bytes(), {}, 12
    while at + 8 <= len(raw):
        name, size = raw[at:at + 4].decode("latin1"), struct.unpack("<I", raw[at + 4:at + 8])[0]
        out[name] = raw[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    return out


def envelope(one_shot=True):
    def v(x):
        return {"value0": x}
    return {"Attack": v(0.0), "Decay": v(0.05), "Sustain": v(1.0), "Release": v(0.0), "DecayFromEnd": v(True), "OneShot": v(one_shot)}


def instrument(sample=None, start=0, end=0, volume=0.7079457640647888, filter_type=29, cutoff=1.0, one_shot=True, group=0):
    return {"coarseTune": 0, "fineTune": 0, "monophonic": True, "whichMuteGroup": group, "loopLock": True, "warpEnable": False, "bpmLock": True,
            "synthSection": {"ampEnvelope": envelope(one_shot), "velocitySensitivity": 1.0,
                             "filterData": {"value0": {"filterType": filter_type, "filterCutoff": cutoff, "filterResonance": 0.0, "filterEnvelopeAmount": 0.0}}},
            "mixable": {"volume": volume, "pan": 0.5},
            "layersv": [{"active": True, "sampleFile": sample or "", "coarseTune": 0, "fineTune": 0, "direction": 0, "loopMode": 0,
                         "layerLoopModeOverridesSliceLoopMode": True, "sliceInfo": {"Start": start, "End": end, "LoopStart": start, "LoopMode": 1}}]}


def note(time, key, velocity=1.0, length=240, muted=False):
    return {"time": time, "type": 3, "muted": muted, "note": {"note": key, "velocity": velocity, "length": length, "probability": 100}}


def sequence(bpm, name, events):
    clip = {"eventList": {"events": events}}
    return {"name": "Sequence", "bpm": bpm, "lengthPulses": 7680,
            "timeSignatureTrack": {"timeSignatures": [{"beatsPerBar": 3, "beatLength": 960, "barStart": 0}]},
            "trackClipMaps": [[{"key": name, "value": clip}, {"key": "Out 1/2", "value": {"eventList": {"events": []}}}]]}


def project(pads):
    notes = {"noteForPad": {"value%d" % n: 36 + n for n in range(128)}}
    drum = {"name": "Drum 001", "volume": 0.7079, "mute": False,
            "program": {"type": 0, "padNoteMap": notes, "mixable": {"volume": 1.0, "pan": 0.75, "mute": False, "solo": False},
                        "drum": {"instruments": pads + [instrument() for _ in range(128 - len(pads))]}}}
    output = {"name": "Out 1/2", "program": {"type": 9}}
    return {"data": {"version": 28, "masterTempoEnabled": False, "masterTempo": 128.0, "currentSequence": 1,
                     "tracks": [output, drum],
                     # out of order, as the stock app leaves them; 128 has no pattern to go to (127 is the last)
                     "sequences": [{"key": 1, "value": sequence(72.5, "Drum 001", [note(960, 37, 0.5), note(0, 36), note(9999, 36), note(480, 38, muted=True)])},
                                   {"key": 0, "value": sequence(105.0, "Drum 001", [note(0, 37)])},
                                   {"key": 128, "value": sequence(90.0, "Drum 001", [])}]}}


def write(folder, name, document):
    path = Path(folder) / (name + ".xpj")
    path.write_bytes(gzip.compress((HEADER + json.dumps(document)).encode()))
    return path


@unittest.skipUnless(BINARY.is_file(), "build/melodee is not built")
class MpcProject(unittest.TestCase):
    def run_tool(self, path):
        return subprocess.run([str(BINARY), "--mpc-import", str(path)], capture_output=True, text=True, timeout=60)

    def test_project(self):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Song_[ProjectData]"
            data.mkdir()
            (data / "Kick.wav").write_bytes(wav(44100))
            (data / "Snare.wav").write_bytes(wav(22050))
            path = write(folder, "Song", project([instrument("Kick.wav", 10, 90, group=2),
                                                  instrument("Snare.wav", 10, 40, volume=48 / 127, filter_type=3, cutoff=0.5, one_shot=False),
                                                  instrument("Gone.wav", 0, 50)]))
            before = path.read_bytes()
            result = self.run_tool(path)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            out = result.stdout
            self.assertEqual(path.read_bytes(), before)
        self.assertIn("name=Song tracks=1 sequences=2 pads=3 notes=3 over_limit=1 missing=1 tempo=725 seq_tempo=1 signature=3/4", out)
        self.assertIn("track 0 name=Drum 001 level=127 pan=25 mute=0 pattern=1", out)
        self.assertIn("pad A01 file=Kick start=10 end=90 level=100 pan=0 pitch=0 play=1 mono=1 group=2 filter=Off", out)
        # 22.05 kHz frames double at the model's 44.1 kHz; 48/127 is -10.9 dB
        self.assertIn("pad A02 file=Snare start=20 end=80 level=51 pan=0 pitch=0 play=0 mono=1 group=0 filter=LPF4 cutoff=50", out)
        self.assertIn("pattern 0 1 length=7680 notes=2 first=0:36:127", out)
        self.assertIn("pattern 0 0 length=7680 notes=1 first=0:37:127", out)

    def test_song(self):
        # The first song with steps becomes Song: a clip a step on tracks with notes, a marker a step, and the tempo
        # lane where the sequences' own tempos change; the second song is left out.
        document = project([instrument()])
        data = document["data"]
        data["sequences"][1]["value"]["name"] = "Hook Part Two"
        data["songs"] = [{"name": "(unnamed)", "ignoreTempo": False, "items": []},
                         {"name": "(unnamed)", "ignoreTempo": False,
                          "items": [{"item.sequenceIndex": 0, "item.repeats": 2}, {"item.sequenceIndex": 1, "item.repeats": 1},
                                    {"item.sequenceIndex": 5, "item.repeats": 1}]},
                         {"name": "(unnamed)", "ignoreTempo": False, "items": [{"item.sequenceIndex": 1, "item.repeats": 1}]}]
        with tempfile.TemporaryDirectory() as folder:
            out = self.run_tool(write(folder, "Song", document)).stdout
            data["songs"][1]["ignoreTempo"] = True
            ignored = self.run_tool(write(folder, "Song", document)).stdout
        self.assertIn("scene 0 name=Hook Part\n", out)
        self.assertIn("song steps=2 steps_left_out=1 songs_left_out=1 clips=2 markers=2 tempo_points=2 bars=9", out)
        self.assertIn("clip track=0 pattern=0 bar=0 bars=6\nclip track=0 pattern=1 bar=6 bars=3", out)
        self.assertIn("marker bar=0 name=Hook Part\nmarker bar=6 name=SEQ 2", out)
        self.assertIn("tempo bar=0 tempo=1050 ramp=0\ntempo bar=6 tempo=725 ramp=0", out)
        self.assertIn("clips=2 markers=2 tempo_points=0", ignored)

    def test_keygroup_track(self):
        # An MPC 3 keygroup track: a KEYGROUP track playing it from the project ("#k<track>"), its zones read from
        # the data folder, its notes MIDI notes.
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Keys_[ProjectData]"
            data.mkdir()
            for name in ("Kick.wav", "Low.wav", "High.wav"):
                (data / name).write_bytes(wav(44100))
            document = project([instrument("Kick.wav", 10, 90)])
            low, high = instrument("Low.wav", 0, 100, one_shot=False), instrument("High.wav", 0, 100, one_shot=False)
            low.update(lowNote=0, highNote=59); low["layersv"][0]["rootNote"] = 49
            high.update(lowNote=60, highNote=127); high["layersv"][0]["rootNote"] = 73
            keys = {"name": "Keys", "program": {"type": 1, "mixable": {"volume": 0.7079457640647888, "pan": 0.5},
                                                  "keygroup": {"numKeygroups": 2},
                                                  "drum": {"monophonic": False, "instruments": [low, high] + [instrument() for _ in range(126)]}}}
            document["data"]["tracks"].append(keys)
            for entry in document["data"]["sequences"]:
                if entry["key"] == 1:
                    entry["value"]["trackClipMaps"][0].append({"key": "Keys", "value": {"eventList": {"events": [note(0, 64), note(480, 52, 0.5)]}}})
            path = write(folder, "Keys", document)
            result = self.run_tool(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        out = result.stdout
        self.assertIn("track 1 name=Keys level=100 pan=0 mute=0 pattern=1 engine=9", out)
        self.assertIn("keygroup zones=2", out)
        self.assertIn("pattern 1 1 length=7680 notes=2 first=0:64:127", out)

    def test_wav_slices(self):
        # A sample chopped on the MPC keeps its slices in the WAV: they open as the file's chop (Manual), each slice its
        # own start and end, a gap between them kept; at 22.05 kHz their frames double at the model's 44.1 kHz.
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Chops_[ProjectData]"
            data.mkdir()
            (data / "Break.wav").write_bytes(sliced_wav(44100, 120000, [(29153, 74097), (74097, 95962)]))
            (data / "Half.wav").write_bytes(sliced_wav(22050, 50000, [(1000, 20000), (30000, 49999)]))
            path = write(folder, "Chops", project([instrument("Break.wav", 74097, 95962), instrument("Half.wav", 0, 50000)]))
            result = self.run_tool(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("chop file=Break from=0 to=0 slices=2 29153-74097 74097-95962", result.stdout)
        self.assertIn("chop file=Half from=0 to=0 slices=2 2000-40000 60000-0", result.stdout)

    def test_mpc3_drum_kit(self):
        # Issue 62: an MPC 3 kit (.xtd, JSON as an .xty) whose pads share samples chained in one WAV beside it in
        # Samples/ ("../Samples/<file>" from its data folder), each layer one slice of it by sliceIndex; a layer that
        # names only the folder plays nothing.
        with tempfile.TemporaryDirectory() as folder:
            kits = Path(folder) / "Drum Kits"
            (kits / "Samples").mkdir(parents=True)
            (kits / "Samples" / "Chain.wav").write_bytes(sliced_wav(44100, 1000, [(0, 300), (300, 650), (650, 1000)]))
            layer = {"active": True, "sampleFile": "../Samples/Chain.wav", "sliceIndex": 2, "sliceInfo": {"Start": 0, "End": 999},
                     "velocityStart": 0, "velocityEnd": 127}
            kit = {"program": {"type": 0, "drum": {"instruments": [{"layersv": [layer, {"active": True, "sampleFile": "../Samples/"}]}]}}}
            path = kits / "Kit.xtd"
            path.write_bytes(gzip.compress(("ACVS\n3.6.0.134\nSerialisableTrackData\njson\nLinux\n" + json.dumps({"data": kit})).encode()))
            result = self.run_tool(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("missing=0", result.stdout)
        self.assertIn("pad A01 file=Chain start=650", result.stdout)

    def test_two_members(self):
        # The factory kits in /content/Projects/Kits: the header in one gzip member, the JSON in the next.
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Kit_[ProjectData]"
            data.mkdir()
            (data / "Kick.wav").write_bytes(wav(44100))
            path = Path(folder) / "Kit.xpj"
            path.write_bytes(gzip.compress(HEADER.encode()) + gzip.compress(json.dumps(project([instrument("Kick.wav", 10, 90)])).encode()))
            result = self.run_tool(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("name=Kit tracks=1", result.stdout)
        self.assertIn("pad A01 file=Kick start=10 end=90", result.stdout)

    def test_not_a_project(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Bad.xpj"
            path.write_bytes(gzip.compress(b"hello\n"))
            result = self.run_tool(path)
        self.assertEqual(result.returncode, 1)
        self.assertIn("error=Not an MPC Sample project", result.stdout)


if __name__ == "__main__":
    unittest.main()
