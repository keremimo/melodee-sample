# SPDX-License-Identifier: AGPL-3.0-only
"""Fetch Melodee's pinned dependencies into ignored build storage: its typeface,
Rubik (SIL OFL 1.1), and the CLAP plug-in headers (MIT).

The build embeds build/deps/rubik/Rubik-VF.ttf in the binary
(src/melodee/ui/rubik_data.cpp); every weight comes from its wght axis.
The CLAP host (src/melodee/plugin/clap_instance.cpp) includes
build/deps/clap/include.
"""
from pathlib import Path
import hashlib
import subprocess
import urllib.request

REVISION = "5bb28733cec313b63fbd415b574b5ae35c8aa8b8"  # google/fonts
BASE = f"https://raw.githubusercontent.com/google/fonts/{REVISION}/ofl/rubik/"
FILES = {
    "Rubik-VF.ttf": ("Rubik%5Bwght%5D.ttf", "1b3a7437ba2af80e465e773ed60c5036d1ba6ace492d89046dbcf18fb31e4e88"),
    "OFL.txt": ("OFL.txt", "472cbe7c25441df63e9c7864b43eb3c0f4b3df950c66a76224e6cfe1eae843fb"),
}


CLAP_TAG = "1.2.9"
CLAP_REVISION = "cd94482ba5941ae410809b6fbaed3bc851044270"  # free-audio/clap 1.2.9


def prepare_clap(root):
    path = Path(root) / "build/deps/clap"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "--depth", "1", "--branch", CLAP_TAG,
                        "https://github.com/free-audio/clap.git", str(path)], check=True)
    revision = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if revision != CLAP_REVISION:
        raise RuntimeError("CLAP revision mismatch; remove build/deps/clap and rebuild")


def prepare(root):
    prepare_clap(root)
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
