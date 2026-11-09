# SPDX-License-Identifier: AGPL-3.0-only
"""Fetch Melodee's pinned typeface, Rubik (SIL OFL 1.1), into ignored build storage.

The build embeds build/deps/rubik/Rubik-VF.ttf in the binary
(src/melodee/ui/rubik_data.cpp); every weight comes from its wght axis.
"""
from pathlib import Path
import hashlib
import urllib.request

REVISION = "5bb28733cec313b63fbd415b574b5ae35c8aa8b8"  # google/fonts
BASE = f"https://raw.githubusercontent.com/google/fonts/{REVISION}/ofl/rubik/"
FILES = {
    "Rubik-VF.ttf": ("Rubik%5Bwght%5D.ttf", "1b3a7437ba2af80e465e773ed60c5036d1ba6ace492d89046dbcf18fb31e4e88"),
    "OFL.txt": ("OFL.txt", "472cbe7c25441df63e9c7864b43eb3c0f4b3df950c66a76224e6cfe1eae843fb"),
}


def prepare(root):
    folder = Path(root) / "build/deps/rubik"
    folder.mkdir(parents=True, exist_ok=True)
    for name, (remote, digest) in FILES.items():
        path = folder / name
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            with urllib.request.urlopen(BASE + remote, timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != digest:
                raise RuntimeError("Rubik checksum mismatch for " + name)
            path.write_bytes(data)
