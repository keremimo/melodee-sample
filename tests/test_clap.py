# SPDX-License-Identifier: AGPL-3.0-only
"""Melodee's CLAP host against a fixture library (tests/clap_host.cpp), linked
with the objects make built. Runs where those are: aarch64 Linux (./build.sh)."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OBJ = ROOT / "build/obj"
OBJECTS = ["melodee/plugin/clap_instance.o", "melodee/plugin/plugin_files.o", "juce_core.o", "juce_core_CompilationTime.o",
           "juce_core_zlib.o", "juce_events.o", "juce_audio_basics.o", "juce_audio_processors_headless.o"]
INCLUDES = ["src", "base/src", "build/deps/JUCE/modules", "build/deps/clap/include"]
DEFINES = ["-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1", "-DJUCE_STANDALONE_APPLICATION=1", "-DJUCE_USE_CURL=0",
           "-DJUCE_WEB_BROWSER=0", "-DNDEBUG", "-DJUCE_PLUGINHOST_VST3=1"]


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "aarch64"
                     and all((OBJ / o).is_file() for o in OBJECTS), "needs make's aarch64 objects")
class ClapTests(unittest.TestCase):
    def test_host_against_fixture(self):
        cxx = os.environ.get("CXX", "g++")
        flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", *["-I" + str(ROOT / i) for i in INCLUDES], *DEFINES]
        with tempfile.TemporaryDirectory() as temp:
            plugin, host = Path(temp) / "Test.clap", Path(temp) / "clap_host"
            subprocess.run([cxx, *flags, "-shared", "-fPIC", str(ROOT / "tests/clap_fixture.cpp"), "-o", str(plugin)], check=True)
            subprocess.run([cxx, *flags, str(ROOT / "tests/clap_host.cpp"), *[str(OBJ / o) for o in OBJECTS],
                            "-ldl", "-pthread", "-lrt", "-o", str(host)], check=True)
            result = subprocess.run([str(host), str(plugin)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("clap ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
