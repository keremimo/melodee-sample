# SPDX-License-Identifier: AGPL-3.0-only
"""Check the versioned metadata and stereo trace without JUCE or device I/O."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CustomPluginGuiTests(unittest.TestCase):
    def test_metadata_and_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "package"
            package.mkdir()
            binary = Path(directory) / "test"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-Isrc", "tests/custom_plugin_gui.cpp", "src/melodee/plugin/custom_gui.cpp",
                            "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary), str(ROOT / "docs/examples/second-spin"), str(package)],
                           check=True, timeout=10)
