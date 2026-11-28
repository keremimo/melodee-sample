# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Melodee's plug-in hosts against fixture plug-ins, linked with the objects
make built: the CLAP host (tests/clap_host.cpp), the VST2 host
(tests/vst2_host.cpp) and JUCE's LV2 host as the rack uses it
(tests/lv2_host.cpp). Runs where those objects are: aarch64
Linux (./build.sh)."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OBJ = ROOT / "build/obj"
OBJECTS = ["melodee/plugin/plugin_files.o", "juce_core.o", "juce_core_CompilationTime.o", "juce_core_zlib.o",
           "juce_events.o", "juce_audio_basics.o", "juce_audio_processors_headless.o",
           "juce_audio_processors_headless_lv2_libs.o"]
LV2_SDK = "build/deps/JUCE/modules/juce_audio_processors_headless/format_types/LV2_SDK"
INCLUDES = ["src", "core/src", "base/src", "build/deps/JUCE/modules", "build/deps/clap/include", "sdk/melodee-screen"]
DEFINES = ["-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1", "-DJUCE_STANDALONE_APPLICATION=1", "-DJUCE_USE_CURL=0",
           "-DJUCE_WEB_BROWSER=0", "-DNDEBUG", "-DJUCE_PLUGINHOST_VST3=1", "-DJUCE_PLUGINHOST_LV2=1"]
FLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", *["-I" + str(ROOT / i) for i in INCLUDES],
         "-isystem", str(ROOT / LV2_SDK / "lv2"), *DEFINES]  # the LV2 headers as JUCE ships them: their warnings are not ours


def compile_and_run(test, temp, plugin, host_source, extra_objects, expected):
    cxx = os.environ.get("CXX", "g++")
    host = Path(temp) / "host"
    subprocess.run([cxx, *FLAGS, str(ROOT / host_source), *[str(OBJ / o) for o in extra_objects + OBJECTS],
                    "-ldl", "-pthread", "-lrt", "-o", str(host)], check=True)
    result = subprocess.run([str(host), str(plugin)], capture_output=True, text=True, timeout=60,
                            env={**os.environ, "HOME": temp})
    test.assertEqual(result.returncode, 0, result.stderr)
    test.assertIn(expected, result.stdout)


class IdleGateTests(unittest.TestCase):
    def test_idle_gate(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / "idle_gate"
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/idle_gate.cpp"), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("idle ok", result.stdout)


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "aarch64"
                     and all((OBJ / o).is_file() for o in OBJECTS + ["melodee/plugin/clap_instance.o"]),
                     "needs make's aarch64 objects")
class PluginHostTests(unittest.TestCase):
    def test_clap_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.clap"
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-shared", "-fPIC",
                            str(ROOT / "tests/clap_fixture.cpp"), "-o", str(plugin)], check=True)
            compile_and_run(self, temp, plugin, "tests/clap_host.cpp", ["melodee/plugin/clap_instance.o"], "clap ok")

    @unittest.skipUnless(shutil.which("zig"), "needs zig")
    def test_clap_host_exits_after_a_static_runtime_plugin(self):
        """A CLAP with zig's static libc++ crashed the host at exit once dlclose unmapped it."""
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.clap"
            subprocess.run(["zig", "c++", "-target", "aarch64-linux-gnu.2.28", "-Wno-nullability-completeness",
                            "-std=c++20", "-O2", "-fPIC", "-DNDEBUG", "-shared",
                            "-I" + str(ROOT / "build/deps/clap/include"), "-I" + str(ROOT / "sdk/melodee-screen"),
                            str(ROOT / "tests/clap_fixture.cpp"), "-o", str(plugin)], check=True)
            compile_and_run(self, temp, plugin, "tests/clap_host.cpp", ["melodee/plugin/clap_instance.o"], "clap ok")

    def test_vst2_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.so"
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-shared", "-fPIC",
                            str(ROOT / "tests/vst2_fixture.cpp"), "-o", str(plugin)], check=True)
            compile_and_run(self, temp, plugin, "tests/vst2_host.cpp",
                            ["melodee/plugin/vst2_instance.o", "melodee/plugin/vst2_bridge_instance.o"], "vst2 ok")

    def test_screen_example(self):
        """sdk/melodee-screen/example builds with both entry points, and Melodee draws its page as Edit shows it."""
        example = ROOT / "sdk/melodee-screen/example"
        melodee = ROOT / "build/melodee"
        with tempfile.TemporaryDirectory() as temp:
            library = Path(temp) / "screen-example.so"
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-fPIC", "-shared", "-fvisibility=hidden",
                            "-Wall", "-Wextra", "-Werror", "-I" + str(example.parent), "-I" + str(ROOT / "build/deps/clap/include"),
                            str(example / "screen_example.cpp"), "-o", str(library)], check=True)
            symbols = subprocess.run(["nm", "-D", "--defined-only", str(library)], capture_output=True, text=True, check=True).stdout
            self.assertIn("clap_entry", symbols)
            self.assertIn("VSTPluginMain", symbols)
            if not melodee.is_file():
                return
            clap = Path(temp) / "screen-example.clap"
            shutil.copy(library, clap)
            for plugin in (clap, library):
                out = Path(temp) / ("png-" + plugin.suffix[1:])
                result = subprocess.run([str(melodee), "--melodee-plugin-screen", str(plugin), str(out),
                                         "wheel:20", "plus", "wheel:-10", "push", "k3:40"],
                                        capture_output=True, text=True, timeout=60, env={**os.environ, "HOME": temp})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("written=6", result.stdout)
                self.assertEqual(len(list(out.glob("*.png"))), 6)

    def test_lv2_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            bundle = Path(temp) / "Tone.lv2"
            bundle.mkdir()
            for name in ("manifest.ttl", "tone.ttl", "presets.ttl"):
                shutil.copy(ROOT / "tests/lv2_fixture" / name, bundle / name)
            # -Wno-parentheses: LV2's atom macros, expanded in the fixture's own code.
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-Wno-parentheses", "-shared", "-fPIC",
                            str(ROOT / "tests/lv2_fixture/tone.cpp"), "-o", str(bundle / "tone.so")], check=True)
            compile_and_run(self, temp, bundle, "tests/lv2_host.cpp", [], "lv2 ok")


if __name__ == "__main__":
    unittest.main()
