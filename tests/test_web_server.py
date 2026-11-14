# SPDX-License-Identifier: AGPL-3.0-only
"""The web page's HTTP server (src/melodee/web/web_server.cpp), built for the host (Linux)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith("linux"), "the server uses Linux socket calls")
class WebServerTests(unittest.TestCase):
    def test_refusals_and_bodies(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "web_server"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/web_server.cpp"),
                            str(ROOT / "src/melodee/web/web_server.cpp"), str(ROOT / "src/melodee/web/mdns.cpp"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("web server ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
