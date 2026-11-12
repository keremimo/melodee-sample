# SPDX-License-Identifier: AGPL-3.0-only
"""The release zip's autorun is signed, its files are checked and its boot
hands the melodee folder to the base's SD boot scripts. Its file list is
signed with the key in src/melodee/shell/update.h."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
        self.assertEqual(set(listed), {"sample-base", "restore-stock.sh", "sd-boot.sh", "sd-worker.sh", "sd-update.sh"})
        for name, digest in listed.items():
            self.assertEqual(hashlib.sha256(files["autorun/melodee/" + name]).hexdigest(), digest)
        self.assertEqual(files["autorun/melodee/app"], b"melodee\n")

    def test_nothing_private_ships(self):
        for name, data in self.collect().items():
            make_release.scan(name, data)
        self.assertIn(b"v0.1.0-alpha.1", self.collect()["autorun/melodee/README.txt"])

    def test_zip_is_the_autorun_folder_alone(self):
        files = self.collect()
        self.assertEqual({name.split("/")[0] for name in files}, {"autorun"})
        self.assertIn("autorun/melodee/licenses/LICENSE-AGPL-3.0.txt", files)

    def test_release_list_covers_every_file_and_is_signed(self):
        try:
            tool = make_release.openssl()
        except RuntimeError:
            self.skipTest("no OpenSSL 3")
        files = self.collect()
        with tempfile.TemporaryDirectory() as temp:
            key, public = Path(temp) / "key.pem", Path(temp) / "public.pem"
            subprocess.run([tool, "genpkey", "-algorithm", "ed25519", "-out", str(key)], check=True, capture_output=True)
            subprocess.run([tool, "pkey", "-in", str(key), "-pubout", "-out", str(public)], check=True)
            # A key that does not match update.h is rejected.
            with self.assertRaises(RuntimeError) as raised:
                make_release.sign(dict(files), key)
            self.assertIn("update.h", str(raised.exception))
            with mock.patch.object(make_release, "public_key", return_value=public.read_text()):
                signed = make_release.sign(dict(files), key)
            listing = signed["autorun/melodee/RELEASE.sha256"].decode().splitlines()
            self.assertEqual([line.split("  ")[1] for line in listing], sorted(files))
            for line in listing:
                digest, name = line.split("  ")
                self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest)
            (Path(temp) / "list").write_bytes(signed["autorun/melodee/RELEASE.sha256"])
            (Path(temp) / "list.sig").write_bytes(signed["autorun/melodee/RELEASE.sha256.sig"])
            subprocess.run([tool, "pkeyutl", "-verify", "-pubin", "-inkey", str(public), "-rawin",
                            "-in", str(Path(temp) / "list"), "-sigfile", str(Path(temp) / "list.sig")],
                           check=True, capture_output=True)
        with self.assertRaises(RuntimeError) as raised:
            make_release.sign(dict(files), Path("/nonexistent/key.pem"))
        self.assertIn("openssl genpkey", str(raised.exception))

    def test_public_key_matches_update_h(self):
        key = make_release.public_key()
        self.assertTrue(key.startswith("-----BEGIN PUBLIC KEY-----\n") and key.endswith("-----END PUBLIC KEY-----\n"))
        self.assertEqual(len(key.splitlines()), 3)


if __name__ == "__main__":
    unittest.main()
