# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Fetch Melodee's pinned dependencies into ignored build storage: its typefaces,
Rubik and the faces Settings' Font offers (SIL OFL 1.1), the CLAP plug-in headers (MIT), and the
stem separator's sources: SpleeterRT (GPL-3.0), OpenBLAS (BSD-3-Clause) and Deezer's Spleeter 4-stem model (MIT).

The build embeds build/deps/rubik/Rubik-VF.ttf and build/deps/fonts/*/ in the binary
(src/melodee/ui/font_data.cpp); every weight comes from a face's wght axis.
The CLAP host (src/melodee/plugin/clap_instance.cpp) includes
build/deps/clap/include. tools/stems/build.py builds the stem separator from
build/deps/spleeterrt, build/deps/openblas and build/deps/spleeter-4stems.
"""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import tarfile
import urllib.request

REVISION = "5bb28733cec313b63fbd415b574b5ae35c8aa8b8"  # google/fonts
BASE = f"https://raw.githubusercontent.com/google/fonts/{REVISION}/ofl/rubik/"
FILES = {
    "Rubik-VF.ttf": ("Rubik%5Bwght%5D.ttf", "1b3a7437ba2af80e465e773ed60c5036d1ba6ace492d89046dbcf18fb31e4e88"),
    "OFL.txt": ("OFL.txt", "472cbe7c25441df63e9c7864b43eb3c0f4b3df950c66a76224e6cfe1eae843fb"),
}

# Settings > SCREEN > Font's other faces: folder, the google/fonts path, the font's and its OFL.txt's SHA-256.
FACES = {
    "intertight": ("InterTight%5Bwght%5D.ttf", "b81b73dcb64df3c230cabade7df6c5773bf863233f24c9ee51087519f1f88b6f",
                   "50240ab035cf1b6b3307940235481d515c4b6de3ab1fa843dbe59e7892cb9d58"),
    "atkinsonhyperlegiblenext": ("AtkinsonHyperlegibleNext%5Bwght%5D.ttf", "5a455d1cfa099b601ab70751bb9673e8fe1854dc4500c80e1a220d0d75e31745",
                                 "aca6a428580965d2297d1b718042dd427c2a9443ece3b0d02d758e161e0c4030"),
    "spacegrotesk": ("SpaceGrotesk%5Bwght%5D.ttf", "acad6de1fc93436f5c0f1f4137751ef04f1aea3063e7036535970ffcfbd79f72",
                     "564ce565c371c5e5bbf286006565a7c9aa55a9f56e7ca58d56e05d649dd61a72"),
    "ibmplexsans": ("IBMPlexSans%5Bwdth,wght%5D.ttf", "3b031aa4216174205bd8471f88a49b91f093169e9e87bd5262242bc5967fe2e3",
                    "7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"),
    "manrope": ("Manrope%5Bwght%5D.ttf", "3ae11c49db0455a3cc33e37d380f20fdb8c7f8b41dc07625c177e3d87a9d6ae6",
                "58172e0c0fac2cda8a37b348164bb55e44b0e69051e557e92b1d3f6910141f7b"),
    "vt323": ("VT323-Regular.ttf", "cf4de751ada78ceac033dbe16a687742939995b77bc2a052ae17a4957958594d",
              "27d9af34210253e7ca1251fbace86c6f65b40031d6ce1a75493a1b2093631298"),
}


def fetch(url, path, digest, what):
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
        return
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError(what + " checksum mismatch")
    path.write_bytes(data)


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


# The stem separator (tools/stems/build.py, src/melodee/shell/stems.h): the network and STFT code of
# james34602/SpleeterRT, OpenMathLib/OpenBLAS for its matrix products, and Deezer's Spleeter 4-stem model, a
# TensorFlow checkpoint in a release of deezer/spleeter (tools/stems/convert_model.py lays its weights out anew).
SPLEETERRT_REVISION = "d818fb9ca826d31cb7dda60409f3f14e6e789597"
OPENBLAS_TAG = "v0.3.28"
OPENBLAS_REVISION = "5ef8b1964658f9cb6a6324a06f6a1a022609b0c5"
SPLEETER_MODEL = "https://github.com/deezer/spleeter/releases/download/v1.4.0/4stems.tar.gz"
SPLEETER_MODEL_SHA256 = "3adb4a50ad4eb18c7c4d65fcf4cf2367a07d48408a5eb7d03cd20067429dfaa8"
SPLEETER_MODEL_FILES = ("model.index", "model.data-00000-of-00001")


def file_digest(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for piece in iter(lambda: file.read(1 << 20), b""):
            digest.update(piece)
    return digest.hexdigest()


def head_of(path):
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def prepare_stems(root):
    quiet = ["git", "-c", "advice.detachedHead=false"]
    spleeter = Path(root) / "build/deps/spleeterrt"
    if not (spleeter / "MELODEE_REVISION").is_file():
        if not spleeter.exists(): # it has no tags: the commit itself is fetched
            spleeter.mkdir(parents=True)
            subprocess.run(["git", "init", "-q", str(spleeter)], check=True)
            subprocess.run(["git", "-C", str(spleeter), "remote", "add", "origin", "https://github.com/james34602/SpleeterRT.git"], check=True)
            subprocess.run(["git", "-C", str(spleeter), "fetch", "-q", "--depth", "1", "origin", SPLEETERRT_REVISION], check=True)
            subprocess.run([*quiet, "-C", str(spleeter), "checkout", "-q", "FETCH_HEAD"], check=True)
        if head_of(spleeter) != SPLEETERRT_REVISION:
            raise RuntimeError("SpleeterRT revision mismatch; remove build/deps/spleeterrt and rebuild")
        (spleeter / "MELODEE_REVISION").write_text(SPLEETERRT_REVISION + "\n")
    openblas = Path(root) / "build/deps/openblas"
    if not (openblas / "MELODEE_REVISION").is_file():
        if not openblas.exists():
            subprocess.run([*quiet, "clone", "-q", "--depth", "1", "--branch", OPENBLAS_TAG,
                            "https://github.com/OpenMathLib/OpenBLAS.git", str(openblas)], check=True)
        if head_of(openblas) != OPENBLAS_REVISION:
            raise RuntimeError("OpenBLAS revision mismatch; remove build/deps/openblas and rebuild")
        (openblas / "MELODEE_REVISION").write_text(OPENBLAS_REVISION + "\n")
    # The model: the archive checked against its digest, then the checkpoint's two files taken out of it by name.
    model = Path(root) / "build/deps/spleeter-4stems"
    marker = model / "MELODEE_REVISION"
    if not (marker.is_file() and marker.read_text().strip() == SPLEETER_MODEL_SHA256 and all((model / name).is_file() for name in SPLEETER_MODEL_FILES)):
        model.mkdir(parents=True, exist_ok=True)
        archive = model / "4stems.tar.gz"
        if not archive.is_file() or file_digest(archive) != SPLEETER_MODEL_SHA256:
            with urllib.request.urlopen(SPLEETER_MODEL, timeout=120) as response, open(archive, "wb") as file:
                shutil.copyfileobj(response, file)
        if file_digest(archive) != SPLEETER_MODEL_SHA256:
            raise RuntimeError("Spleeter 4stems.tar.gz checksum mismatch")
        with tarfile.open(archive) as tar:
            for name in SPLEETER_MODEL_FILES:
                member = tar.extractfile(tar.getmember(name))
                with open(model / name, "wb") as file:
                    shutil.copyfileobj(member, file)
        marker.write_text(SPLEETER_MODEL_SHA256 + "\n")


def prepare(root):
    prepare_clap(root)
    prepare_stems(root)
    folder = Path(root) / "build/deps/rubik"
    folder.mkdir(parents=True, exist_ok=True)
    for name, (remote, digest) in FILES.items():
        fetch(BASE + remote, folder / name, digest, "Rubik " + name)
    for face, (remote, digest, licence) in FACES.items():
        folder = Path(root) / "build/deps/fonts" / face
        folder.mkdir(parents=True, exist_ok=True)
        base = f"https://raw.githubusercontent.com/google/fonts/{REVISION}/ofl/{face}/"
        fetch(base + remote, folder / "face.ttf", digest, face)
        fetch(base + "OFL.txt", folder / "OFL.txt", licence, face + " OFL.txt")
    # Melodee Pixel, Melodee's own, drawn from src/melodee/ui/fonts/melodee_pixel.txt.
    spec = importlib.util.spec_from_file_location("gen_pixel_font", Path(root) / "tools/gen_pixel_font.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.prepare(root)
