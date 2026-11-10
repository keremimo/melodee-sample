# SPDX-License-Identifier: AGPL-3.0-only
"""The release zip's autorun is signed, its files are checked and its boot
hands the melodee folder to the base's SD boot scripts."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import make_release  # noqa: E402


@unittest.skipUnless((ROOT / "build/deps/rubik/OFL.txt").is_file(), "dependencies not fetched")
class ReleaseTests(unittest.TestCase):
    def collect(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "melodee"
            binary.write_bytes(b"\x7fELF not really")
            return make_release.collect(binary, "v0.1.0-alpha.1", "0123456789ab")

    def test_autorun_is_signed_and_starts_the_melodee_folder(self):
        files = self.collect()
        script, manifest = files["autorun/autorun.sh"].decode(), files["autorun/autorun.json"].decode()
        self.assertEqual(files["autorun/autorun.check"].decode(), make_release.base_device().autorun_check(manifest, script))
        self.assertIn('sd-boot.sh" "$D/melodee"', script)
        self.assertTrue(script.startswith("#!/bin/sh\n"))

    def test_checksums_cover_what_sd_worker_copies(self):
        files = self.collect()
        listed = {}
        for line in files["autorun/melodee/SHA256SUMS"].decode().splitlines():
            digest, name = line.split("  ")
            listed[name] = digest
        self.assertEqual(set(listed), {"sample-base", "restore-stock.sh", "sd-boot.sh", "sd-worker.sh"})
        for name, digest in listed.items():
            self.assertEqual(hashlib.sha256(files["autorun/melodee/" + name]).hexdigest(), digest)
        self.assertEqual(files["autorun/melodee/app"], b"melodee\n")

    def test_nothing_private_ships(self):
        for name, data in self.collect().items():
            make_release.scan(name, data)
        self.assertIn(b"v0.1.0-alpha.1", self.collect()["README.txt"])


if __name__ == "__main__":
    unittest.main()
