# SPDX-License-Identifier: AGPL-3.0-only
"""Melodee's plug-in hosts against fixture plug-ins, linked with the objects
make built: the CLAP host (tests/clap_host.cpp) and JUCE's LV2 host as the
rack uses it (tests/lv2_host.cpp). Runs where those objects are: aarch64
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
INCLUDES = ["src", "base/src", "build/deps/JUCE/modules", "build/deps/clap/include"]
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
