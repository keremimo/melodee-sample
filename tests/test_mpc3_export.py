# SPDX-License-Identifier: AGPL-3.0-only
"""A project written as an MPC 3 project by the built app, then read back."""
import gzip
import json
import math
import re
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from test_mpc_project import BINARY, chunks, instrument, note, project, sliced_wav, wav, write


def float_wav(path, rate, channels, frames):
    """A print as Melodee writes one: 32-bit float, each channel's sample its number."""
    data = b"".join(struct.pack("<f", float(c + 1)) for _ in range(frames) for c in range(channels))
    fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def read(path):
    lines = gzip.decompress(Path(path).read_bytes()).decode().split("\n", 5)
    return lines[:5], json.loads(lines[5])["data"]


@unittest.skipUnless(BINARY.is_file(), "build/melodee is not built")
class Mpc3Export(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=120)

    def export(self, folder, *changes, pads=None):
        source = Path(folder) / "in"
        data = source / "Song_[ProjectData]"
        data.mkdir(parents=True)
        (data / "Kick.wav").write_bytes(wav(44100))
        (data / "Snare.wav").write_bytes(wav(22050))
        path = write(source, "Song", project(pads or [instrument("Kick.wav", 10, 90, group=2),
                                                      instrument("Snare.wav", 10, 40, volume=48 / 127, filter_type=3, cutoff=0.5, one_shot=False)]))
        out = Path(folder) / "out"
        out.mkdir()
        result = self.run_tool("--mpc3-export", path, out, "Beat", *[c.replace("FOLDER", str(folder)) for c in changes])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout, out

    def test_submix_routes(self):
        # The mixer: a track sent to a submix plays into MPC 3's (destination 1, the submix's index; 2 is Out 1/2), that
        # submix gets its level, pan and mute on its bus track and in the mixer's list; a drum pad goes into its program
        # (destination 0), as MPC 3's own kits' pads.
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder, "O:0=3", "U:2=64,-20,1")
            _, data = read(out / "Beat.xpj")
            drums = [t for t in data["tracks"] if t["program"]["type"] == 0]
            self.assertTrue(drums)
            route = drums[0]["program"]["mixable"]["audioRoute"]
            self.assertEqual((route["destination"], route["audioRouteSubIndex"]), (1, 2))
            pads = drums[0]["program"]["drum"]["instruments"]
            self.assertTrue(all(p["mixable"]["audioRoute"]["destination"] == 0 for p in pads))
            bus = next(t for t in data["tracks"] if t["name"] == "Submix 3")
            self.assertTrue(bus["mute"] and bus["program"]["mixable"]["mute"])
            self.assertLess(bus["program"]["mixable"]["pan"], 0.5)
            listed = data["mixer"]["submixes"][2]["mixable"]
            self.assertEqual(listed["volume"], bus["program"]["mixable"]["volume"])
            self.assertLess(listed["volume"], next(t for t in data["tracks"] if t["name"] == "Submix 1")["program"]["mixable"]["volume"])
            other = next(t for t in data["tracks"] if t["name"] == "Submix 1")
            self.assertEqual(other["program"]["mixable"]["audioRoute"]["destination"], 2)

    def test_wav_slices(self):
        # A file's chop goes into the copied WAV as the MPC keeps slices ("atem" and "slic"), its key kept, and reads
        # back as the same slices.
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "in"
            data = source / "Song_[ProjectData]"
            data.mkdir(parents=True)
            (data / "Break.wav").write_bytes(sliced_wav(44100, 120000, [(29153, 74097), (74097, 95962)]))
            path = write(source, "Song", project([instrument("Break.wav", 0, 120000)]))
            out = Path(folder) / "out"
            out.mkdir()
            result = self.run_tool("--mpc3-export", path, out, "Beat")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("chops=1", result.stdout)
            written = chunks(out / "Beat_[ProjectData]" / "Break.wav")
            atem = json.loads(written["atem"].rstrip(b"\0"))
            self.assertEqual(atem["value0"]["Num slices"], 2)
            self.assertEqual((atem["value0"]["Slice 1"]["Start"], atem["value0"]["Slice 1"]["End"]), (74097, 95962))
            self.assertEqual(atem["value1"]["note"], "F")
            self.assertEqual(struct.unpack("<HHH", written["slic"][:6]), (1, 2, 0))
            self.assertEqual(struct.unpack("<IIIH", written["slic"][6:20]), (29153, 74097, 29153, 0))
            self.assertEqual(len(written["data"]), 240000)
            back = self.run_tool("--mpc-import", out / "Beat.xpj")
        self.assertIn("chop file=Break from=0 to=0 slices=2 29153-74097 74097-95962", back.stdout)

    def test_document(self):
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder)
            self.assertIn("exported tracks=1 keygroups=0 audio=0 regions=0 midi=0 sequences=2 song=0 pads=2 notes=3 samples=2 skipped_tracks=0 skipped_pads=0 missing=0", report)
            header, data = read(out / "Beat.xpj")
            self.assertEqual(header[0], "ACVS")
            self.assertTrue(header[1].startswith("3."), header)
            self.assertEqual(header[2:4], ["SerialisableProjectData", "json"])
            self.assertEqual(data["version"], 30)
            self.assertEqual(sorted((out / "Beat_[ProjectData]").iterdir()), [out / "Beat_[ProjectData]" / "Kick.wav", out / "Beat_[ProjectData]" / "Snare.wav"])
            self.assertEqual(sorted(s["path"] for s in data["samples"]), ["Kick.wav", "Snare.wav"])

            kinds = [(t["name"], t["program"]["type"]) for t in data["tracks"]]
            self.assertEqual(kinds[0], ("Drum 001", 0))
            self.assertEqual(sum(1 for _, k in kinds if k == 9), 16)  # the Out pairs come with the template
            drum = data["tracks"][0]["program"]
            pads = drum["drum"]["instruments"]
            self.assertEqual(len(pads), 128)
            self.assertEqual(pads[0]["layersv"][0]["sampleFile"], "Kick.wav")
            self.assertEqual(pads[0]["whichMuteGroup"], 2)
            # 22.05 kHz: Melodee's 44.1 kHz frames 20..80 are the file's 10..40 again
            self.assertEqual((pads[1]["layersv"][0]["sliceInfo"]["Start"], pads[1]["layersv"][0]["sliceInfo"]["End"]), (10, 40))
            self.assertEqual(pads[1]["synthSection"]["filterData"]["value0"]["filterType"], 3)
            self.assertIsInstance(pads[1]["mixable"]["volume"], float)
            self.assertEqual((pads[0]["monophonic"], pads[0]["polyphony"]), (True, 1))

            # Assignments name only tracks the project has (the template's own are gone)
            names = {t["name"] for t in data["tracks"]}
            def targets(node):
                if isinstance(node, dict):
                    for key, value in node.items():
                        if key == "track" and isinstance(value, str):
                            yield value
                        else:
                            yield from targets(value)
                elif isinstance(node, list):
                    for value in node:
                        yield from targets(value)
            self.assertTrue(all(not t or t in names for t in targets(data)))

            self.assertEqual(len(data["sequences"]), 128)
            self.assertEqual(data["sequences"][0]["key"], 127)
            second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
            self.assertEqual(second["bpm"], 72.5)
            self.assertEqual(second["timeSignatureTrack"]["timeSignatures"][0]["beatsPerBar"], 3)
            clips = second["trackClipMaps"][0]
            self.assertEqual([c["key"] for c in clips], sorted(c["key"] for c in clips))
            events = next(c["value"] for c in clips if c["key"] == "Drum 001")["eventList"]["events"]
            self.assertEqual([(e["time"], e["note"]["note"]) for e in events], [(0, 36), (960, 37)])
            self.assertEqual(data["masterTempoEnabled"], False)
            self.assertEqual(len(data["clipPlayerData"]["trackClipTransportMap"]), len(clips))

    def test_missing_sample_left_out(self):
        # A pad whose file is not there is left out, not named: MPC 3 stops on a project naming a sample it cannot find.
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder, pads=[instrument("Kick.wav", 10, 90), instrument("Gone.wav", 0, 50)])
            self.assertIn("pads=1", report)
            self.assertIn("missing=1", report)
            _, data = read(out / "Beat.xpj")
        named = json.dumps(data)
        self.assertNotIn("Gone", named)
        self.assertEqual(data["tracks"][0]["program"]["drum"]["instruments"][1]["layersv"][0]["sampleFile"], "")

    def test_read_back(self):
        # What the export wrote reads back as what went in (shell/mpc_project.cpp reads MPC 3's projects too).
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder)
            result = self.run_tool("--mpc-import", out / "Beat.xpj")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = result.stdout
        self.assertIn("name=Beat tracks=1 sequences=128 pads=2 notes=3 over_limit=0 missing=0 tempo=725 seq_tempo=1 signature=3/4", text)
        self.assertIn("pad A01 file=Kick start=10 end=90 level=100 pan=0 pitch=0 play=1 mono=1 group=2 filter=Off", text)
        self.assertIn("pad A02 file=Snare start=20 end=80 level=51 pan=0 pitch=0 play=0 mono=1 group=0 filter=LPF4 cutoff=50", text)
        self.assertIn("pattern 0 1 length=7680 notes=2 first=0:36:127", text)
        self.assertIn("pattern 0 0 length=7680 notes=1 first=0:37:127", text)

    def test_keygroup_instrument(self):
        # An SFZ of two key zones on the Snare's pad: a Keygroup track of its own, the pad's notes with it at
        # 60 + their pitch; the kit keeps the Kick.
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Piano"
            keys.mkdir()
            (keys / "low.wav").write_bytes(wav(44100))
            (keys / "high.wav").write_bytes(wav(48000))
            (keys / "Piano.sfz").write_text("<region> sample=low.wav lokey=0 hikey=59 pitch_keycenter=48\n"
                                            "<region> sample=high.wav lokey=60 hikey=127 pitch_keycenter=72 tune=10 volume=-6\n")
            report, out = self.export(folder, "0.1=FOLDER/Piano/Piano.sfz", "0.1^5")
            self.assertIn("exported tracks=1 keygroups=1 audio=0 regions=0 midi=0 sequences=2 song=0 pads=1 notes=3 samples=3", report)
            _, data = read(out / "Beat.xpj")
        tracks = {t["name"]: t for t in data["tracks"]}
        piano = tracks["Piano"]["program"]
        self.assertEqual((piano["type"], piano["keygroup"]["numKeygroups"]), (1, 2))
        groups = [i for i in piano["drum"]["instruments"] if i["layersv"][0]["sampleFile"]]
        self.assertEqual([(g["lowNote"], g["highNote"], g["layersv"][0]["rootNote"], g["layersv"][0]["sampleFile"]) for g in groups],
                         [(0, 59, 49, "low.wav"), (60, 127, 73, "high.wav")])
        self.assertEqual(groups[1]["layersv"][0]["fineTune"], 10)
        self.assertAlmostEqual(groups[1]["layersv"][0]["volume"]["gainCoefficient"], 10 ** (-6 / 20), places=4)
        self.assertEqual(tracks["Drum 001"]["program"]["drum"]["instruments"][1]["layersv"][0]["sampleFile"], "")
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        clips = {c["key"]: c["value"]["eventList"]["events"] for c in second["trackClipMaps"][0]}
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clips["Piano"]], [(960, 65)])
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clips["Drum 001"]], [(0, 36)])

    def test_keygroup_track(self):
        # A KEYGROUP track: an MPC keygroup track of its instrument, its notes their MIDI notes (36 and 37 here).
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Piano"
            keys.mkdir()
            (keys / "low.wav").write_bytes(wav(44100))
            (keys / "Piano.sfz").write_text("<region> sample=low.wav lokey=0 hikey=127 pitch_keycenter=48\n")
            report, out = self.export(folder, "K:0=FOLDER/Piano/Piano.sfz")
            self.assertIn("exported tracks=0 keygroups=1", report)
            _, data = read(out / "Beat.xpj")
        track = data["tracks"][0]
        self.assertEqual((track["name"], track["program"]["type"]), ("Drum 001", 1))
        group = track["program"]["drum"]["instruments"][0]
        self.assertEqual((group["lowNote"], group["highNote"], group["layersv"][0]["rootNote"]), (0, 127, 49))
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        events = next(c["value"] for c in second["trackClipMaps"][0] if c["key"] == "Drum 001")["eventList"]["events"]
        self.assertEqual([(e["time"], e["note"]["note"]) for e in events], [(0, 36), (960, 37)])

    def test_keygroup_portamento(self):
        # A mono program that glides (an auto sampled 303's): its Mono and Portamento go to the MPC 3 program.
        time = 1 + math.log(0.06 / 30) / 7  # 60 ms on MPC 3's curve, 30 s x e^(7 (v - 1))
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Bass"
            keys.mkdir()
            (keys / "Bass-C3.wav").write_bytes(wav(44100))
            layer = ("<Layer number=\"1\"><Active>True</Active><Volume>1.0</Volume><Pan>0.5</Pan><VelStart>0</VelStart><VelEnd>127</VelEnd>"
                     "<RootNote>49</RootNote><SampleName>Bass-C3</SampleName><SliceEnd>44099</SliceEnd></Layer>")
            (keys / "Bass.xpm").write_text(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPCVObject><Program type=\"Keygroup\"><ProgramName>Bass</ProgramName>"
                f"<Mono>True</Mono><PortamentoTime>{time:.6f}</PortamentoTime><PortamentoLegato>True</PortamentoLegato>"
                "<KeygroupNumKeygroups>1</KeygroupNumKeygroups><Instruments><Instrument number=\"1\"><LowNote>0</LowNote><HighNote>127</HighNote>"
                f"<Layers>{layer}</Layers></Instrument></Instruments></Program></MPCVObject>")
            report, out = self.export(folder, "K:0=FOLDER/Bass/Bass.xpm")
            self.assertIn("exported tracks=0 keygroups=1", report)
            _, data = read(out / "Beat.xpj")
        voices = data["tracks"][0]["program"]["drum"]
        self.assertTrue(voices["monophonic"])
        self.assertTrue(voices["portamentoLegato"])
        self.assertAlmostEqual(voices["portamentoTime"], time, places=5)
        self.assertFalse(voices["monoRetrigger"])  # no tag for it: Legato carries the note on

    def test_keygroup_mono_retrigger(self):
        # A mono program whose keys start the note again when played legato (an auto sampled pluck's): MPC 3's Mono
        # Retrig, from the .xpm's tag.
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Pluck"
            keys.mkdir()
            (keys / "Pluck-C3.wav").write_bytes(wav(44100))
            layer = ("<Layer number=\"1\"><Active>True</Active><Volume>1.0</Volume><Pan>0.5</Pan><VelStart>0</VelStart><VelEnd>127</VelEnd>"
                     "<RootNote>49</RootNote><SampleName>Pluck-C3</SampleName><SliceEnd>44099</SliceEnd></Layer>")
            (keys / "Pluck.xpm").write_text(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPCVObject><Program type=\"Keygroup\"><ProgramName>Pluck</ProgramName>"
                "<Mono>True</Mono><PortamentoTime>0.000000</PortamentoTime><PortamentoLegato>True</PortamentoLegato>"
                "<MonoRetrigger>True</MonoRetrigger>"
                "<KeygroupNumKeygroups>1</KeygroupNumKeygroups><Instruments><Instrument number=\"1\"><LowNote>0</LowNote><HighNote>127</HighNote>"
                f"<Layers>{layer}</Layers></Instrument></Instruments></Program></MPCVObject>")
            report, out = self.export(folder, "K:0=FOLDER/Pluck/Pluck.xpm")
            self.assertIn("exported tracks=0 keygroups=1", report)
            _, data = read(out / "Beat.xpj")
        voices = data["tracks"][0]["program"]["drum"]
        self.assertTrue(voices["monophonic"] and voices["monoRetrigger"] and voices["portamentoLegato"])

    def test_track_file(self):
        # A keygroup program in "<name>_[TrackData]" as MPC 3's track file beside that folder (the auto sampler's
        # layout): a track's header and gzip around one Keygroup track, its samples named as they lie in the folder,
        # nothing copied, Mono Retrig from the program's tag; read back, it is the program the .xpm is.
        time = 1 + math.log(0.03 / 30) / 7
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Pluck_[TrackData]"
            data.mkdir()
            for name in ("Pluck-C3", "Pluck-C4"):
                (data / f"{name}.wav").write_bytes(wav(44100))

            def keygroup(number, low, high, name, root):
                return (f"<Instrument number=\"{number}\"><LowNote>{low}</LowNote><HighNote>{high}</HighNote><TriggerMode>2</TriggerMode>"
                        "<VolumeRelease>0.5</VolumeRelease><Layers><Layer number=\"1\"><Active>True</Active><Volume>1.0</Volume><Pan>0.5</Pan>"
                        f"<VelStart>0</VelStart><VelEnd>127</VelEnd><RootNote>{root + 1}</RootNote><SampleName>{name}</SampleName>"
                        f"<SampleFile>{name}.WAV</SampleFile>"  # as an MPC names it; the file is .wav
                        "<SliceStart>0</SliceStart><SliceEnd>40000</SliceEnd><SliceLoop>1</SliceLoop><SliceLoopStart>1000</SliceLoopStart>"
                        "</Layer></Layers></Instrument>")

            (data / "Pluck.xpm").write_text(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPCVObject><Program type=\"Keygroup\"><ProgramName>Pluck</ProgramName>"
                f"<Mono>True</Mono><PortamentoTime>{time:.6f}</PortamentoTime><PortamentoLegato>True</PortamentoLegato>"
                "<PortamentoQuantized>False</PortamentoQuantized><MonoRetrigger>True</MonoRetrigger>"
                "<KeygroupNumKeygroups>2</KeygroupNumKeygroups><Instruments>"
                + keygroup(1, 0, 59, "Pluck-C3", 48) + keygroup(2, 60, 127, "Pluck-C4", 60) + "</Instruments></Program></MPCVObject>")
            track_file = Path(folder) / "Pluck.xty"
            result = self.run_tool("--mpc-track", data / "Pluck.xpm", track_file)
            self.assertEqual(result.stdout.strip(), "written", result.stdout + result.stderr)
            header, track = read(track_file)
            self.assertEqual((header[0], header[2], header[3]), ("ACVS", "SerialisableTrackData", "json"))
            program = track["program"]
            self.assertEqual((track["name"], program["name"], program["type"]), ("Pluck", "Pluck", 1))
            voices = program["drum"]
            self.assertTrue(voices["monophonic"] and voices["monoRetrigger"] and voices["portamentoLegato"])
            self.assertAlmostEqual(voices["portamentoTime"], time, places=5)
            self.assertEqual(program["keygroup"]["numKeygroups"], 2)
            self.assertEqual([s["path"] for s in program["samples"]], ["Pluck-C3.wav", "Pluck-C4.wav"])
            first = voices["instruments"][0]
            self.assertEqual((first["lowNote"], first["highNote"], first["monophonic"], first["triggerMode"]), (0, 59, False, 2))
            layer = first["layersv"][0]
            self.assertEqual((layer["sampleName"], layer["sampleFile"], layer["rootNote"]), ("Pluck-C3", "Pluck-C3.wav", 49))
            self.assertEqual((layer["sliceInfo"]["LoopStart"], layer["sliceInfo"]["End"], layer["sliceInfo"]["LoopMode"]), (1000, 40000, 1))
            self.assertEqual(len(voices["instruments"]), 128)
            # nothing copied, nothing left behind
            self.assertEqual(sorted(p.name for p in data.iterdir()), ["Pluck-C3.wav", "Pluck-C4.wav", "Pluck.xpm"])
            self.assertEqual(sorted(p.name for p in Path(folder).iterdir()), ["Pluck.xty", "Pluck_[TrackData]"])
            # read back as the sample library reads it: the same program
            from_xpm = self.run_tool("--instrument-read", data / "Pluck.xpm").stdout
            from_track = self.run_tool("--instrument-read", track_file).stdout
            self.assertIn("mono=1 legato=1 tied=0 glide=0.030", from_xpm)
            # (its files named from the track file's folder; the release through MPC 3's 0..1 and back, within 2 %)
            release = [float(re.search(r"release=([0-9.]+)", text).group(1)) for text in (from_xpm, from_track)]
            self.assertAlmostEqual(release[0], release[1], delta=0.02 * release[0])
            same = [re.sub(r"release=[0-9.]+", "", text).replace("Pluck_[TrackData]/", "").lower() for text in (from_xpm, from_track)]
            self.assertEqual(same[0], same[1])
            # a program that is not in the track file's folder: not written
            elsewhere = self.run_tool("--mpc-track", data / "Pluck.xpm", Path(folder) / "Other.xty")
            self.assertIn("error=", elsewhere.stdout)
            self.assertFalse((Path(folder) / "Other.xty").exists())

    def test_track_file_eight_layers(self):
        # The auto sampler's program with eight velocity layers (written whole for this, an .xpm for MPC 2 keeps four):
        # MPC 3's track file holds all eight in the keygroup, each with its velocities; read back, it is the program.
        tops = [24, 40, 56, 64, 80, 96, 112, 127]
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Keys_[TrackData]"
            data.mkdir()
            layers = ""
            for n, top in enumerate(tops):
                name = f"Keys-C4-V{top}"
                (data / f"{name}.wav").write_bytes(wav(44100))
                layers += (f"<Layer number=\"{n + 1}\"><Active>True</Active><Volume>1.0</Volume><Pan>0.5</Pan>"
                           f"<VelStart>{tops[n - 1] + 1 if n else 0}</VelStart><VelEnd>{top}</VelEnd><RootNote>61</RootNote>"
                           f"<SampleName>{name}</SampleName><SliceStart>0</SliceStart><SliceEnd>40000</SliceEnd></Layer>")
            (data / "Keys.all.xpm").write_text(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPCVObject><Program type=\"Keygroup\"><ProgramName>Keys</ProgramName>"
                "<Mono>False</Mono><MonoRetrigger>False</MonoRetrigger><KeygroupNumKeygroups>1</KeygroupNumKeygroups><Instruments><Instrument number=\"1\"><LowNote>0</LowNote>"
                "<HighNote>127</HighNote><TriggerMode>2</TriggerMode><VelocitySensitivity>0.5</VelocitySensitivity><Layers>"
                + layers + "</Layers></Instrument></Instruments></Program></MPCVObject>")
            track_file = Path(folder) / "Keys.xty"
            result = self.run_tool("--mpc-track", data / "Keys.all.xpm", track_file)
            self.assertEqual(result.stdout.strip(), "written", result.stdout + result.stderr)
            _, track = read(track_file)
            first = track["program"]["drum"]["instruments"][0]
            written = [layer for layer in first["layersv"] if layer.get("active") and layer.get("sampleFile")]
            self.assertEqual([layer["sampleName"] for layer in written], [f"Keys-C4-V{top}" for top in tops])
            self.assertEqual([layer["velocityEnd"] for layer in written], tops)
            self.assertEqual([layer["velocityStart"] for layer in written], [0] + [top + 1 for top in tops[:-1]])
            from_xpm = self.run_tool("--instrument-read", data / "Keys.all.xpm").stdout
            from_track = self.run_tool("--instrument-read", track_file).stdout
            same = [re.sub(r"release=[0-9.]+", "", text).replace("Keys_[TrackData]/", "").lower() for text in (from_xpm, from_track)]
            self.assertEqual(same[0], same[1])

    def test_keygroup_split(self):
        # A split's mono side (a program that is not Mono, the side's keygroups Mono in one mute group) and each side's
        # own Velocity: MPC 3's program stays poly, the side's keygroups monophonic in mute group 1.
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "Split_[TrackData]"
            data.mkdir()
            for name in ("Split-C3", "Split-C5"):
                (data / f"{name}.wav").write_bytes(wav(44100))

            def keygroup(number, low, high, name, root, mono, velocity):
                return (f"<Instrument number=\"{number}\"><LowNote>{low}</LowNote><HighNote>{high}</HighNote><TriggerMode>2</TriggerMode>"
                        f"<VelocitySensitivity>{velocity}</VelocitySensitivity><Mono>{mono}</Mono><Polyphony>{1 if mono == 'True' else 0}</Polyphony>"
                        f"<MuteGroup>{1 if mono == 'True' else 0}</MuteGroup><Layers><Layer number=\"1\"><Active>True</Active><Volume>1.0</Volume>"
                        f"<Pan>0.5</Pan><VelStart>0</VelStart><VelEnd>127</VelEnd><RootNote>{root + 1}</RootNote><SampleName>{name}</SampleName>"
                        "<SliceStart>0</SliceStart><SliceEnd>40000</SliceEnd></Layer></Layers></Instrument>")

            (data / "Split.xpm").write_text(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPCVObject><Program type=\"Keygroup\"><ProgramName>Split</ProgramName>"
                "<Mono>False</Mono><PortamentoTime>0.000000</PortamentoTime><PortamentoLegato>False</PortamentoLegato>"
                "<MonoRetrigger>True</MonoRetrigger><MonoSidePortamentoTime>0.000000</MonoSidePortamentoTime>"
                "<MonoSidePortamentoLegato>True</MonoSidePortamentoLegato><KeygroupNumKeygroups>2</KeygroupNumKeygroups><Instruments>"
                + keygroup(1, 0, 59, "Split-C3", 48, "True", "0.000000") + keygroup(2, 60, 127, "Split-C5", 72, "False", "0.800000")
                + "</Instruments></Program></MPCVObject>")
            read_back = self.run_tool("--instrument-read", data / "Split.xpm").stdout
            self.assertIn("mono=1 legato=1 tied=0 glide=0.000 mono_keys=0-59", read_back)
            result = self.run_tool("--mpc-track", data / "Split.xpm", Path(folder) / "Split.xty")
            self.assertEqual(result.stdout.strip(), "written", result.stdout + result.stderr)
            _, track = read(Path(folder) / "Split.xty")
            voices = track["program"]["drum"]
            self.assertFalse(voices["monophonic"])
            under, over = voices["instruments"][0], voices["instruments"][1]
            self.assertEqual((under["monophonic"], under["polyphony"], under["whichMuteGroup"]), (True, 1, 1))
            self.assertEqual((over["monophonic"], over["polyphony"], over["whichMuteGroup"]), (False, 0, 0))
            self.assertAlmostEqual(under["synthSection"]["velocitySensitivity"], 0.0, places=3)
            self.assertAlmostEqual(over["synthSection"]["velocitySensitivity"], 0.8, places=3)
            # and read back from the track file: the same side
            self.assertIn("mono_keys=0-59", self.run_tool("--instrument-read", Path(folder) / "Split.xty").stdout)

    def test_keygroup_two_stage_release(self):
        # An auto sampled sound that drops and rings on: two keygroups over the same keys and sample, one with most of
        # the level and a 50 ms release, one a fifth of it with 1.5 s. They stay two in the MPC 3 program, each with
        # its level and its release as the MPC's 0..1 (1 ms to 100 s, by ratio).
        def stage(seconds):
            return math.log10(seconds * 1000) / 5
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Pad"
            keys.mkdir()
            (keys / "Pad-C3.wav").write_bytes(wav(44100))
            def group(number, volume, release):
                layer = ("<Layer number=\"1\"><Active>True</Active><Pan>0.5</Pan><VelStart>0</VelStart><VelEnd>127</VelEnd>"
                         f"<Volume>{volume}</Volume><RootNote>49</RootNote><SampleName>Pad-C3</SampleName>"
                         "<SliceEnd>29999</SliceEnd><SliceLoop>1</SliceLoop><SliceLoopStart>20000</SliceLoopStart></Layer>")
                return (f"<Instrument number=\"{number}\"><LowNote>0</LowNote><HighNote>127</HighNote><TriggerMode>2</TriggerMode>"
                        f"<VolumeRelease>{stage(release):.6f}</VolumeRelease><Layers>{layer}</Layers></Instrument>")
            (keys / "Pad.xpm").write_text(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPCVObject><Program type=\"Keygroup\"><ProgramName>Pad</ProgramName>"
                "<KeygroupNumKeygroups>2</KeygroupNumKeygroups><Instruments>"
                + group(1, 0.8, 0.05) + group(2, 0.2, 1.5) + "</Instruments></Program></MPCVObject>")
            report, out = self.export(folder, "K:0=FOLDER/Pad/Pad.xpm")
            self.assertIn("exported tracks=0 keygroups=1", report)
            _, data = read(out / "Beat.xpj")
        program = data["tracks"][0]["program"]
        self.assertEqual(program["keygroup"]["numKeygroups"], 2)
        groups = [i for i in program["drum"]["instruments"] if i["layersv"][0]["sampleFile"]]
        self.assertEqual([g["triggerMode"] for g in groups], [2, 2])
        self.assertEqual([g["layersv"][0]["loopMode"] for g in groups], [1, 1])
        self.assertEqual(groups[1]["layersv"][0]["sampleFile"], groups[0]["layersv"][0]["sampleFile"])
        levels = [g["layersv"][0]["volume"]["gainCoefficient"] for g in groups]
        self.assertAlmostEqual(levels[0], 0.8, places=2)
        self.assertAlmostEqual(levels[1], 0.2, places=2)
        releases = [g["synthSection"]["ampEnvelope"]["Release"]["value0"] for g in groups]
        # Melodee's release steps are a tenth apart by ratio: within that of what the program asked for
        self.assertAlmostEqual(releases[0], stage(0.05), delta=0.02)
        self.assertAlmostEqual(releases[1], stage(1.5), delta=0.02)

    def test_pitched_wav_pad(self):
        # A WAV pad its notes play 7 semitones up (16 Levels, Keys): one keygroup over every key, its own pitch at 60.
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder, "0.0^7")
            self.assertIn("exported tracks=1 keygroups=1", report)
            _, data = read(out / "Beat.xpj")
        tracks = {t["name"]: t for t in data["tracks"]}
        kick = tracks["Kick"]["program"]
        group = kick["drum"]["instruments"][0]
        self.assertEqual((group["lowNote"], group["highNote"], group["layersv"][0]["rootNote"], group["layersv"][0]["sampleFile"]), (0, 127, 61, "Kick.wav"))
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        clips = {c["key"]: c["value"]["eventList"]["events"] for c in second["trackClipMaps"][0]}
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clips["Kick"]], [(0, 67)])

    def test_layered_pad_stays_a_pad(self):
        # A pad of two velocity layers (an MPC pad's, read as an instrument) is still a pad of the kit.
        with tempfile.TemporaryDirectory() as folder:
            pad = instrument("Kick.wav", 10, 90)
            soft = dict(pad["layersv"][0], sampleFile="Snare.wav", velocityStart=0, velocityEnd=63)
            pad["layersv"][0] = dict(pad["layersv"][0], velocityStart=64, velocityEnd=127)
            pad["layersv"].append(soft)
            report, out = self.export(folder, pads=[pad])
            self.assertIn("exported tracks=1 keygroups=0", report)
            _, data = read(out / "Beat.xpj")
        layers = [l for l in data["tracks"][0]["program"]["drum"]["instruments"][0]["layersv"] if l["sampleFile"]]
        self.assertEqual(sorted(l["sampleFile"] for l in layers), ["Kick.wav", "Snare.wav"])

    def test_prints_as_audio_track(self):
        # A plug-in track's prints: an Audio track, each printed pattern a region from its sequence's start; a print
        # with pad sends' channels made stereo.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 48000, 2, 1000)
            float_wav(prints / "T02 A2.wav", 48000, 4, 500)
            report, out = self.export(folder, f"1@0={prints}/T02 A1.wav", f"1@1={prints}/T02 A2.wav")
            self.assertIn("exported tracks=1 keygroups=0 audio=1 regions=2 midi=0 sequences=2 song=0", report)
            _, data = read(out / "Beat.xpj")
            stereo = (out / "Beat_[ProjectData]" / "Synth A2.wav").read_bytes()
        tracks = {t["name"]: t for t in data["tracks"]}
        self.assertEqual(tracks["Synth"]["program"]["type"], 6)
        self.assertEqual(sorted(s["path"] for s in tracks["Synth"]["program"]["samples"]), ["Synth A1.wav", "Synth A2.wav"])
        channels, rate = struct.unpack("<HI", stereo[22:28])
        self.assertEqual((channels, rate), (2, 48000))
        self.assertEqual(struct.unpack("<ff", stereo[44:52]), (1.0, 2.0))
        self.assertEqual(len(stereo), 44 + 500 * 8)
        for key, file, frames in ((0, "Synth A1.wav", 1000), (1, "Synth A2.wav", 500)):
            sequence = next(s["value"] for s in data["sequences"] if s["key"] == key)
            events = next(c["value"] for c in sequence["trackClipMaps"][0] if c["key"] == "Synth")["eventList"]["events"]
            self.assertEqual([(e["type"], e["time"], e["audio"]["sample"]["path"], e["audio"]["sliceEnd"]) for e in events][0], (4, 0, file, frames))

    def test_audio_track_takes(self):
        # Freeze plan F3: an audio track's pattern's take: an Audio track of MPC 3's own with the take in its program,
        # the take from the sequence's start, again each time the pattern loops, at the length it plays.
        with tempfile.TemporaryDirectory() as folder:
            takes = Path(folder) / "Song Audio"
            takes.mkdir()
            (takes / "Take 004.wav").write_bytes(wav(48000))
            report, out = self.export(folder, f"A:3.0={takes}/Take 004.wav")
            self.assertIn("audio=1", report)
            _, data = read(out / "Beat.xpj")
            copied = (out / "Beat_[ProjectData]" / "Take 004.wav").is_file()
        self.assertTrue(copied)
        tracks = {t["name"]: t for t in data["tracks"]}
        self.assertEqual(tracks["Vox"]["program"]["type"], 6)
        self.assertEqual([s["path"] for s in tracks["Vox"]["program"]["samples"]], ["Take 004.wav"])
        sequence = next(s["value"] for s in data["sequences"] if s["key"] == 0)
        events = next(c["value"] for c in sequence["trackClipMaps"][0] if c["key"] == "Vox")["eventList"]["events"]
        self.assertTrue(events)
        first = events[0]
        self.assertEqual((first["type"], first["time"], first["audio"]["sample"]["path"], first["audio"]["sliceStart"]), (4, 0, "Take 004.wav", 0))
        self.assertFalse(first["audio"]["warpEnable"])
        self.assertGreater(first["audio"]["tempo"], 0)  # the tempo it was recorded at
        self.assertEqual(first["audio"]["sliceEnd"], 100)  # the whole take (100 frames)

    def test_audio_track_reads_back(self):
        # Freeze plan F3: an exported audio track's take reads back as an audio track's take, from its file.
        with tempfile.TemporaryDirectory() as folder:
            takes = Path(folder) / "Song Audio"
            takes.mkdir()
            (takes / "Take 004.wav").write_bytes(wav(48000))
            self.export(folder, f"A:3.0={takes}/Take 004.wav")
            result = self.run_tool("--mpc-import", Path(folder) / "out" / "Beat.xpj")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        line = next(l for l in result.stdout.splitlines() if l.startswith("take "))
        self.assertRegex(line, r"take \d+ 0 take=1 file=Take 004.wav offset=0 frames=92 tempo=\d+ gain=0 warp=0 semi=0")
        self.assertIn("regions=1 regions_left_out=0", result.stdout)

    def test_print_loops_in_a_longer_sequence(self):
        # A one-bar print in a two-bar sequence (the kit's pattern is longer): the clip spans the sequence, the print
        # once each bar, as Melodee loops it.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 44100, 2, 100)
            _, out = self.export(folder, f"1@0={prints}/T02 A1.wav")
            _, data = read(out / "Beat.xpj")
        sequence = next(s["value"] for s in data["sequences"] if s["key"] == 0)
        clip = next(c["value"] for c in sequence["trackClipMaps"][0] if c["key"] == "Synth")
        self.assertEqual(clip["endPulses"], sequence["lengthPulses"])
        times = [(e["time"], e["audio"]["length"]) for e in clip["eventList"]["events"]]
        self.assertEqual(times[0], (0, 3840))
        self.assertEqual(sum(length for _, length in times), sequence["lengthPulses"])

    def test_song(self):
        # The song as one sequence "Song" in the first free pattern number (2): pattern 1 (two notes in a 7680-tick
        # pattern) at bar 0 for 4 bars of 3/4 plays twice and a little; a print's clip from bar 2; it plays first.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 44100, 2, 100)
            report, out = self.export(folder, f"1@0={prints}/T02 A1.wav", "S:0.1@0+4", "S:1.0@2+2")
            self.assertIn("sequences=2 song=4 pads=", report)
            _, data = read(out / "Beat.xpj")
        song = next(s["value"] for s in data["sequences"] if s["key"] == 2)
        self.assertEqual((song["name"], song["lengthBars"], song["lengthPulses"]), ("Song", 4, 4 * 2880))
        self.assertEqual(data["currentSequence"], 2)
        clips = {c["key"]: c["value"] for c in song["trackClipMaps"][0]}
        drum = [(e["time"], e["note"]["note"]) for e in clips["Drum 001"]["eventList"]["events"]]
        self.assertEqual(drum, [(0, 36), (960, 37), (7680, 36), (8640, 37)])
        self.assertEqual(clips["Drum 001"]["endPulses"], 4 * 2880)
        audio = [(e["time"], e["audio"]["length"]) for e in clips["Synth"]["eventList"]["events"]]
        self.assertEqual(audio[0][0], 2 * 2880)
        self.assertEqual(sum(length for _, length in audio), 2 * 2880)

    def test_tracks_follow_the_sequence(self):
        # A one-bar pattern in a two-bar sequence plays twice, written out; every track as long as the sequence
        # (MPC 3's drum tracks were two bars of their own and looped them).
        with tempfile.TemporaryDirectory() as folder:
            # Drum 001's pattern 1 cut to 3840 ticks (its notes at 0 and 960 stay); the plug-in track's pattern 1 is 7680
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A2.wav", 44100, 2, 100)
            _, out = self.export(folder, "L:0.1=3840", f"1@1={prints}/T02 A2.wav", "L:1.1=7680")
            _, data = read(out / "Beat.xpj")
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        self.assertEqual(second["lengthPulses"], 7680)
        clip = next(c["value"] for c in second["trackClipMaps"][0] if c["key"] == "Drum 001")
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clip["eventList"]["events"]], [(0, 36), (960, 37), (3840, 36), (4800, 37)])
        self.assertEqual(clip["endPulses"], 7680)
        for track in data["tracks"]:
            if track["program"]["type"] in (0, 1, 6):
                self.assertEqual((track["length"], track["lengthFollowsSequenceLength"]), (0, True))

    def test_midi_track(self):
        # A MIDI track: MPC 3's own (type 4) on its channel, with its program, its notes as they are.
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder, "M:0=5,11")
            self.assertIn("exported tracks=0 keygroups=0 audio=0 regions=0 midi=1", report)
            _, data = read(out / "Beat.xpj")
        track = data["tracks"][0]
        self.assertEqual((track["name"], track["program"]["type"], track["midiOutputRoute"]["outputChannel"]), ("MIDI OUT", 4, 4))  # channel 5, stored from 0
        sound = track["midiBankAndProgramNumber"]
        self.assertEqual((sound["midiProgramNumberEnable"], sound["midiProgramNumber"], sound["midiBankEnable"]), (True, 10, False))
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        events = next(c["value"] for c in second["trackClipMaps"][0] if c["key"] == "MIDI OUT")["eventList"]["events"]
        self.assertEqual([(e["time"], e["note"]["note"]) for e in events], [(0, 36), (960, 37)])

    def test_warped_pad(self):
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder, "0.0~1")
            _, data = read(out / "Beat.xpj")
            back = self.run_tool("--mpc-import", out / "Beat.xpj").stdout
        pad = data["tracks"][0]["program"]["drum"]["instruments"][0]
        self.assertEqual((pad["warpEnable"], pad["bpmLock"], pad["tempo"]), (True, True, 72.5))
        self.assertIn("pad A01 file=Kick start=10 end=90 level=100 pan=0 pitch=0 play=1 mono=1 group=2 filter=Off cutoff=100 warp=1", back)

    def test_nothing_to_export(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Empty.mldp"
            path.write_bytes(b"")
            result = self.run_tool("--mpc3-export", path, folder, "X")
        self.assertEqual(result.returncode, 1)
        self.assertIn("error=", result.stdout)


if __name__ == "__main__":
    unittest.main()
