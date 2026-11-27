# SPDX-License-Identifier: AGPL-3.0-only
"""A project written as an Ableton Live Set by the built app (core/src/melodee/shell/live_export.h), then read back."""
import gzip
from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from test_mpc_project import BINARY, instrument, project, wav, write
from test_mpc3_export import float_wav


def live_set(path):
    """A Live Set's XML: the .als is gzip around it."""
    return ET.fromstring(gzip.decompress(Path(path).read_bytes()))


def value(node, path):
    """The Value of the element at `path` ("Name/EffectiveName") under `node`."""
    found = node.find(path)
    return None if found is None else found.get("Value")


def name_of(track):
    return value(track, "Name/EffectiveName")


def slots(track):
    """A track's Session View clip slots, one a scene."""
    return track.findall("DeviceChain/MainSequencer/ClipSlotList/ClipSlot")


def notes(clip):
    """A MIDI clip's notes as (time in beats, key)."""
    out = []
    for key_track in clip.findall("Notes/KeyTracks/KeyTrack"):
        key = int(value(key_track, "MidiKey"))
        out += [(float(n.get("Time")), key) for n in key_track.findall("Notes/MidiNoteEvent")]
    return sorted(out)


@unittest.skipUnless(BINARY.is_file(), "build/melodee is not built")
class LiveExport(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=120)

    def export(self, folder, *changes, pads=None):
        """The MPC Sample project of test_mpc_project (a kit of Kick and Snare, 3/4, A1 and A2) as "Beat Project"."""
        source = Path(folder) / "in"
        data = source / "Song_[ProjectData]"
        data.mkdir(parents=True)
        (data / "Kick.wav").write_bytes(wav(44100))
        (data / "Snare.wav").write_bytes(wav(22050))
        path = write(source, "Song", project(pads or [instrument("Kick.wav", 10, 90, group=2),
                                                      instrument("Snare.wav", 10, 40, volume=48 / 127, filter_type=3, cutoff=0.5, one_shot=False)]))
        out = Path(folder) / "out"
        out.mkdir()
        result = self.run_tool("--live-export", path, out, "Beat", *[c.replace("FOLDER", str(folder)) for c in changes])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout, out / "Beat Project"

    def tracks(self, root, tag="MidiTrack"):
        return root.findall("LiveSet/Tracks/" + tag)

    def test_document(self):
        # The kit as a Drum Rack of two Simplers, its two patterns as clips in scenes A1 and A2, the samples copied
        # beside the set and named by their path in it.
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder)
            self.assertIn("exported tracks=1 clips=2 arranged=0 scenes=2 notes=3 samples=2 missing=0 skipped=0", report)
            root = live_set(out / "Beat.als")
            copied = sorted(p.name for p in (out / "Samples" / "Imported").iterdir())
        self.assertEqual(copied, ["Kick.wav", "Snare.wav"])
        self.assertEqual(root.tag, "Ableton")
        self.assertTrue(root.get("MinorVersion").startswith("12."))
        self.assertEqual(root.get("Creator"), "Melodee")
        tracks = self.tracks(root)
        self.assertEqual([name_of(t) for t in tracks], ["Drum 001"])
        kit = tracks[0]
        self.assertEqual(value(kit, "Color"), "28")  # track 1's Coral as Live's salmon
        self.assertEqual(len(kit.findall(".//DrumGroupDevice")), 1)
        branches = kit.findall(".//DrumBranch")
        self.assertEqual(len(branches), 2)
        # Pad 1 on C1 (Live keeps 128 - the note), its mute group a choke group, one shot; pad 2 gate.
        self.assertEqual([value(b, "BranchInfo/ReceivingNote") for b in branches], ["92", "91"])
        self.assertEqual([value(b, "BranchInfo/ChokeGroup") for b in branches], ["2", "0"])
        simplers = [b.find("DeviceChain/MidiToAudioDeviceChain/Devices/OriginalSimpler") for b in branches]
        self.assertEqual([value(s, "Globals/PlaybackMode") for s in simplers], ["1", "0"])
        kick, snare = (s.find("Player/MultiSampleMap/SampleParts/MultiSamplePart") for s in simplers)
        self.assertEqual(value(kick, "IsActive"), "true")
        self.assertEqual((value(kick, "SampleStart"), value(kick, "SampleEnd")), ("10", "90"))
        # 22.05 kHz: Melodee's 44.1 kHz frames 20..80 are the file's 10..40
        self.assertEqual((value(snare, "SampleStart"), value(snare, "SampleEnd")), ("10", "40"))
        self.assertEqual(value(kick, "SampleRef/FileRef/RelativePath"), "Samples/Imported/Kick.wav")
        self.assertEqual(value(kick, "SampleRef/FileRef/RelativePathType"), "1")
        # Each pattern a clip in the scene of its number, as long as the pattern (7680 ticks: 8 beats).
        self.assertEqual(len(slots(kit)), 2)
        a1, a2 = (s.find("ClipSlot/Value/MidiClip") for s in slots(kit))
        self.assertEqual((value(a1, "Name"), value(a2, "Name")), ("A1", "A2"))
        self.assertEqual(value(a2, "Loop/LoopEnd"), "8")
        self.assertEqual(notes(a1), [(0.0, 37)])
        self.assertEqual(notes(a2), [(0.0, 36), (1.0, 37)])
        # The tempo, 3/4, and each scene at its pattern's tempo.
        self.assertEqual(value(root, "LiveSet/MainTrack/DeviceChain/Mixer/Tempo/Manual"), "72.5")
        self.assertEqual(value(root, "LiveSet/MainTrack/DeviceChain/Mixer/TimeSignature/Manual"), "200")
        scenes = root.findall("LiveSet/Scenes/Scene")
        self.assertEqual([(value(s, "Name"), value(s, "Tempo"), value(s, "IsTempoEnabled")) for s in scenes],
                         [("A1", "105", "true"), ("A2", "72.5", "true")])
        self.assertEqual(root.findall("LiveSet/Tracks/ReturnTrack"), [])

    def test_pitched_pad_track_of_its_own(self):
        # A WAV pad its notes play five semitones up: a track of its own with a Simpler, its notes 60 + their pitch;
        # the Drum Rack keeps the Snare.
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder, "0.0^5")
            root = live_set(out / "Beat.als")
        tracks = self.tracks(root)
        self.assertEqual([name_of(t) for t in tracks], ["Drum 001", "Drum 001 A01"])
        self.assertEqual(len(tracks[0].findall(".//DrumBranch")), 1)
        self.assertEqual(len(tracks[1].findall("DeviceChain/DeviceChain/Devices/OriginalSimpler")), 1)
        a2 = slots(tracks[1])[1].find("ClipSlot/Value/MidiClip")
        self.assertEqual(notes(a2), [(0.0, 65)])

    def test_instrument_pad_as_sampler(self):
        # An SFZ of two key zones on the Snare's pad, played up: a Sampler of both zones on a track of its own.
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Piano"
            keys.mkdir()
            (keys / "low.wav").write_bytes(wav(44100))
            (keys / "high.wav").write_bytes(wav(48000))
            (keys / "Piano.sfz").write_text("<region> sample=low.wav lokey=0 hikey=59 pitch_keycenter=48\n"
                                            "<region> sample=high.wav lokey=60 hikey=127 pitch_keycenter=72 tune=10 volume=-6\n")
            report, out = self.export(folder, "0.1=FOLDER/Piano/Piano.sfz", "0.1^5")
            self.assertIn("samples=3", report)
            root = live_set(out / "Beat.als")
        tracks = self.tracks(root)
        self.assertEqual([name_of(t) for t in tracks], ["Drum 001", "Drum 001 A02"])
        parts = tracks[1].findall("DeviceChain/DeviceChain/Devices/MultiSampler/Player/MultiSampleMap/SampleParts/MultiSamplePart")
        self.assertEqual([(value(p, "KeyRange/Min"), value(p, "KeyRange/Max"), value(p, "RootKey"), value(p, "Detune")) for p in parts],
                         [("0", "59", "48", "0"), ("60", "127", "72", "10")])
        self.assertAlmostEqual(float(value(parts[1], "Volume")), 10 ** (-6 / 20), places=4)
        a2 = slots(tracks[1])[1].find("ClipSlot/Value/MidiClip")
        self.assertEqual(notes(a2), [(1.0, 65)])

    def test_keygroup_track(self):
        # A KEYGROUP track of one zone: a Simpler over every key, its notes as they are.
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Piano"
            keys.mkdir()
            (keys / "low.wav").write_bytes(wav(44100))
            (keys / "Piano.sfz").write_text("<region> sample=low.wav lokey=0 hikey=127 pitch_keycenter=48\n")
            _, out = self.export(folder, "K:0=FOLDER/Piano/Piano.sfz")
            root = live_set(out / "Beat.als")
        tracks = self.tracks(root)
        self.assertEqual([name_of(t) for t in tracks], ["Drum 001"])
        part = tracks[0].find("DeviceChain/DeviceChain/Devices/OriginalSimpler/Player/MultiSampleMap/SampleParts/MultiSamplePart")
        self.assertEqual((value(part, "KeyRange/Min"), value(part, "KeyRange/Max"), value(part, "RootKey")), ("0", "127", "48"))
        a2 = slots(tracks[0])[1].find("ClipSlot/Value/MidiClip")
        self.assertEqual(notes(a2), [(0.0, 36), (1.0, 37)])

    def test_midi_track(self):
        # A MIDI track: its notes as they are, its program sent by each clip.
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder, "M:0=5,11")
            root = live_set(out / "Beat.als")
        tracks = self.tracks(root)
        self.assertEqual([name_of(t) for t in tracks], ["MIDI OUT"])
        self.assertEqual(tracks[0].findall("DeviceChain/DeviceChain/Devices/*"), [])
        a2 = slots(tracks[0])[1].find("ClipSlot/Value/MidiClip")
        self.assertEqual(value(a2, "ProgramChange"), "10")  # program 11, stored from 0
        self.assertEqual(notes(a2), [(0.0, 36), (1.0, 37)])

    def test_prints_beside_plugin_track(self):
        # A plug-in track: a MIDI track of its notes, no instrument, and an audio track of its prints beside it.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 48000, 2, 1000)
            report, out = self.export(folder, f"1@0={prints}/T02 A1.wav")
            self.assertIn("samples=3", report)
            root = live_set(out / "Beat.als")
            copied = (out / "Samples" / "Imported" / "Synth Print A1.wav").is_file()
        self.assertTrue(copied)
        self.assertEqual([name_of(t) for t in self.tracks(root)], ["Drum 001", "Synth"])
        audio = self.tracks(root, "AudioTrack")
        self.assertEqual([name_of(t) for t in audio], ["Synth Print"])
        clip = slots(audio[0])[0].find("ClipSlot/Value/AudioClip")
        self.assertEqual(value(clip, "SampleRef/FileRef/RelativePath"), "Samples/Imported/Synth Print A1.wav")
        self.assertEqual(value(clip, "IsWarped"), "true")
        self.assertIsNone(slots(audio[0])[1].find("ClipSlot/Value/AudioClip"))

    def test_audio_track_takes(self):
        # An audio track's take: an audio clip in its pattern's scene, warped to the tempo it was recorded at.
        with tempfile.TemporaryDirectory() as folder:
            takes = Path(folder) / "Song Audio"
            takes.mkdir()
            (takes / "Take 004.wav").write_bytes(wav(48000))
            _, out = self.export(folder, f"A:3.0={takes}/Take 004.wav")
            root = live_set(out / "Beat.als")
            copied = (out / "Samples" / "Imported" / "Take 004.wav").is_file()
        self.assertTrue(copied)
        audio = self.tracks(root, "AudioTrack")
        self.assertEqual([name_of(t) for t in audio], ["Vox"])
        clip = slots(audio[0])[0].find("ClipSlot/Value/AudioClip")
        self.assertEqual(value(clip, "SampleRef/FileRef/RelativePath"), "Samples/Imported/Take 004.wav")
        self.assertEqual(len(clip.findall("WarpMarkers/WarpMarker")), 2)

    def test_song(self):
        # The song in Arrangement View: pattern A2 on the kit's lane for four bars of 3/4, the print from bar 3.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 44100, 2, 100)
            report, out = self.export(folder, f"1@0={prints}/T02 A1.wav", "S:0.1@0+4", "S:1.0@2+2")
            self.assertIn("arranged=3", report)  # the kit's clip, and the plug-in track's on its MIDI track and its prints
            root = live_set(out / "Beat.als")
        kit = self.tracks(root)[0]
        arranged = kit.findall("DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events/MidiClip")
        self.assertEqual([(c.get("Time"), value(c, "CurrentEnd"), value(c, "Loop/LoopEnd")) for c in arranged], [("0", "12", "8")])
        audio = self.tracks(root, "AudioTrack")[0]
        printed = audio.findall("DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events/AudioClip")
        self.assertEqual([(c.get("Time"), value(c, "CurrentEnd")) for c in printed], [("6", "12")])

    def test_missing_sample_counted(self):
        with tempfile.TemporaryDirectory() as folder:
            report, _ = self.export(folder, pads=[instrument("Kick.wav", 10, 90), instrument("Gone.wav", 0, 50)])
        self.assertIn("samples=1 missing=1", report)

    def test_nothing_to_export(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Empty.mldp"
            path.write_bytes(b"")
            result = self.run_tool("--live-export", path, folder, "X")
        self.assertEqual(result.returncode, 1)
        self.assertIn("error=", result.stdout)


if __name__ == "__main__":
    unittest.main()
