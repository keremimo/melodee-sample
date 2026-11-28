#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Deezer's Spleeter 4-stem model (a TensorFlow checkpoint, MIT) as the file the stem separator reads
(tools/stems/stems_engine.c): the four networks' weights laid out as SpleeterRT's network code wants them
(its spleeterCoeff), as 16-bit floats.

    python3 tools/stems/convert_model.py build/deps/spleeter-4stems build/stems/spleeter-4stems.f16

The checkpoint is read here, without TensorFlow: model.index is a table of each tensor's place in
model.data-00000-of-00001. The checkpoint keeps one U-Net per stem, its layers numbered on from the stem before:
vocals, drums, bass, other (Spleeter's 4stems configuration). For each:

  a convolution's kernel [h][w][in][out]             ->  [out][in][h][w]
  a transposed convolution's kernel [h][w][out][in]  ->  [in][out][h][w]
  a batch normalisation (gamma, beta, moving mean and variance, epsilon 0.001)
                                                     ->  an offset for each channel, then a scale for each
  the sixth convolution's batch normalisation is left out: Spleeter's decoder starts from the convolution itself

The file: "MLST", then as 32-bit words the version (1), the networks (4) and the weights of one (9822725), then
each network's weights as IEEE 754 half floats, little-endian.

--check MODEL_C compares the drums network with the one SpleeterRT's own offline program carries (its
Executable/model.c, first network), which was made from the same checkpoint: the layout above is right when they
agree to a half float's step.
"""
from array import array
from pathlib import Path
import struct
import sys

ENCODER = [(2, 16), (16, 32), (32, 64), (64, 128), (128, 256), (256, 512)]      # in, out
DECODER = [(512, 256), (512, 128), (256, 64), (128, 32), (64, 16), (32, 1)]     # in (with the skip), out
STEMS = 4
WEIGHTS = 9822725
EPSILON = 0.001


def varint(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7f) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def block(data, offset, size):
    """The entries of a table block: keys share a prefix with the one before."""
    if data[offset + size] != 0:
        raise SystemExit("the checkpoint's index is compressed")
    raw = data[offset:offset + size]
    end = len(raw) - 4 - 4 * struct.unpack_from("<I", raw, len(raw) - 4)[0]
    pos, key, out = 0, b"", []
    while pos < end:
        shared, pos = varint(raw, pos)
        unshared, pos = varint(raw, pos)
        length, pos = varint(raw, pos)
        key = key[:shared] + raw[pos:pos + unshared]
        pos += unshared
        out.append((key, raw[pos:pos + length]))
        pos += length
    return out


def proto(buf):
    """A protocol buffer message's fields, {number: [values]}: varints as integers, the rest as bytes."""
    pos, out = 0, {}
    while pos < len(buf):
        tag, pos = varint(buf, pos)
        wire = tag & 7
        if wire == 0:
            value, pos = varint(buf, pos)
        elif wire == 2:
            n, pos = varint(buf, pos)
            value = buf[pos:pos + n]
            pos += n
        elif wire in (1, 5):
            n = 8 if wire == 1 else 4
            value = buf[pos:pos + n]
            pos += n
        else:
            raise SystemExit("the checkpoint's index is not as expected")
        out.setdefault(tag >> 3, []).append(value)
    return out


def tensors(index):
    """{name: (shape, offset, bytes)} of every tensor in the checkpoint."""
    data = index.read_bytes()
    footer = data[-48:]
    if len(footer) < 48 or struct.unpack_from("<Q", footer, 40)[0] != 0xdb4775248b80fb57:
        raise SystemExit(f"{index} is not a checkpoint's index")
    _, pos = varint(footer, 0)
    _, pos = varint(footer, pos)
    index_offset, pos = varint(footer, pos)
    index_size, pos = varint(footer, pos)
    out = {}
    for _, handle in block(data, index_offset, index_size):
        offset, p = varint(handle, 0)
        size, p = varint(handle, p)
        for key, value in block(data, offset, size):
            if not key:
                continue  # the bundle's header
            entry = proto(value)
            shape = [proto(dim).get(1, [0])[0] for dims in entry.get(2, []) for dim in proto(dims).get(2, [])]
            out[key.decode()] = (shape, entry.get(4, [0])[0], entry.get(5, [0])[0])
    return out


class Checkpoint:
    def __init__(self, folder):
        self.found = tensors(folder / "model.index")
        self.data = (folder / "model.data-00000-of-00001").open("rb")

    def read(self, name, shape):
        if name not in self.found:
            raise SystemExit(f"the checkpoint has no {name}")
        have, offset, size = self.found[name]
        if have != list(shape):
            raise SystemExit(f"{name} is {have}, not {list(shape)}")
        self.data.seek(offset)
        values = array("f")
        values.frombytes(self.data.read(size))
        if sys.byteorder != "little":
            values.byteswap()
        return values


def numbered(name, n):
    return name if n == 0 else f"{name}_{n}"


def convolution(ck, n, inputs, outputs, size=5):
    """Kernel [h][w][in][out] -> [out][in][h][w], then the bias."""
    name = numbered("conv2d", n)
    kernel = ck.read(name + "/kernel", (size, size, inputs, outputs))
    taps = size * size
    out = array("f", bytes(4 * len(kernel)))
    for k in range(taps):
        for i in range(inputs):
            at = (k * inputs + i) * outputs
            out[i * taps + k::inputs * taps] = kernel[at:at + outputs]
    return out + ck.read(name + "/bias", (outputs,))


def transposed(ck, n, inputs, outputs, size=5):
    """Kernel [h][w][out][in] -> [in][out][h][w], then the bias."""
    name = numbered("conv2d_transpose", n)
    kernel = ck.read(name + "/kernel", (size, size, outputs, inputs))
    taps = size * size
    out = array("f", bytes(4 * len(kernel)))
    for k in range(taps):
        for o in range(outputs):
            at = (k * outputs + o) * inputs
            out[o * taps + k::outputs * taps] = kernel[at:at + inputs]
    return out + ck.read(name + "/bias", (outputs,))


def normalisation(ck, n, channels):
    """An offset for each channel, then a scale for each: scale * x + offset is the layer."""
    name = numbered("batch_normalization", n)
    gamma, beta = ck.read(name + "/gamma", (channels,)), ck.read(name + "/beta", (channels,))
    mean, variance = ck.read(name + "/moving_mean", (channels,)), ck.read(name + "/moving_variance", (channels,))
    scale = [g / (v + EPSILON) ** 0.5 for g, v in zip(gamma, variance)]
    return array("f", [b - m * s for b, m, s in zip(beta, mean, scale)]) + array("f", scale)


def network(ck, stem):
    """Stem `stem`'s weights in the order of SpleeterRT's spleeterCoeff."""
    out = array("f")
    for n, (inputs, outputs) in enumerate(ENCODER):
        out += convolution(ck, 7 * stem + n, inputs, outputs)
        if n < 5:
            out += normalisation(ck, 12 * stem + n, outputs)
    for n, (inputs, outputs) in enumerate(DECODER):
        out += transposed(ck, 6 * stem + n, inputs, outputs)
        out += normalisation(ck, 12 * stem + 6 + n, outputs)
    out += convolution(ck, 7 * stem + 6, 1, 2, size=4)
    if len(out) != WEIGHTS:
        raise SystemExit(f"a network came to {len(out)} weights, not {WEIGHTS}")
    return out


def halves(values):
    """Half floats, little-endian. SpleeterRT reads a half without its denormals: they are written as zero."""
    out = bytearray()
    for at in range(0, len(values), 1 << 16):
        piece = values[at:at + (1 << 16)]
        out += struct.pack(f"<{len(piece)}e", *piece)
    return bytes(out)


def spleeterrt_network(model_c, which):
    """A network of SpleeterRT's own model.c (an array of 32-bit words holding its half floats), as floats."""
    text = model_c.read_text()
    words = array("i", map(int, text[text.index("{") + 1:text.rindex("}")].replace("\n", "").split(",")))
    raw = words.tobytes()[2 * WEIGHTS * which:2 * WEIGHTS * (which + 1)]
    return struct.unpack(f"<{WEIGHTS}e", raw)


def check(ck, model_c):
    ours, theirs = network(ck, 1), spleeterrt_network(model_c, 0)  # drums; SpleeterRT's first network
    apart = 0
    for a, b in zip(ours, theirs):
        # A half float's step at that size; under 6.2e-5 (a half's denormals) SpleeterRT's file holds zero.
        if abs(a - b) > max(abs(a) / 1024, 6.2e-5):
            apart += 1
    print(f"drums against SpleeterRT's: {apart} of {WEIGHTS} weights apart")
    return apart == 0


def main():
    arguments = sys.argv[1:]
    model_c = None
    if "--check" in arguments:
        at = arguments.index("--check")
        model_c = Path(arguments[at + 1])
        del arguments[at:at + 2]
    if len(arguments) != 2:
        raise SystemExit(__doc__)
    ck = Checkpoint(Path(arguments[0]))
    if model_c and not check(ck, model_c):
        raise SystemExit("the layout does not match SpleeterRT's")
    target = Path(arguments[1])
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("wb") as out:
        out.write(b"MLST" + struct.pack("<III", 1, STEMS, WEIGHTS))
        for stem in range(STEMS):
            out.write(halves(network(ck, stem)))
    print(f"{target}: {target.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
