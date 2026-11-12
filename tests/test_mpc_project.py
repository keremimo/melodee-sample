# SPDX-License-Identifier: AGPL-3.0-only
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
        self.assertIn("pattern 0 1 length=768 notes=2 first=0:36:127", out)
        self.assertIn("pattern 0 0 length=768 notes=1 first=0:37:127", out)

    def test_not_a_project(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Bad.xpj"
            path.write_bytes(gzip.compress(b"hello\n"))
            result = self.run_tool(path)
        self.assertEqual(result.returncode, 1)
        self.assertIn("error=Not an MPC Sample project", result.stdout)


if __name__ == "__main__":
    unittest.main()
