#!/usr/bin/env python3
"""Turns the frame dumps dxgi.dll writes (DumpEvery in dlssnr.ini) into pictures and numbers.
Standard library only.

    python tools/bzdump.py [dump.bzdump | folder] ...

With no argument it reads every .bzdump in %LOCALAPPDATA%\\Banana-Zero\\dumps. For each dump it writes, beside it:

    <name>.frame.png    the frame before the composite, through the encode's own curve (white point, shoulder, sRGB)
    <name>.proxy.png    what the model saw, striped where the encode compressed (magenta) or compressed hard (red)
    <name>.model.png    what the model made
    <name>.result.png   the frame after the composite, through the same curve as .frame.png
    <name>.gain.png     the composite's brightness change: red brighter, blue darker, white unchanged (1 EV = full)

from the whole frame reduced 4x, and the same five from the full-resolution crop at the centre (<name>.crop.*.png),
then prints what the dump says about the frame: the white point and its sources, how the brightness lies relative to
it, how much went into the shoulder, and how much the composite changed.
"""

import math
import os
import struct
import sys
import zlib

HEADER = struct.Struct("<8sIIQ16I8f64s")  # nr_dx12.cpp DumpHeader, 184 bytes
FLAG_LINEAR = 0x01
FLAG_EXPOSURE = 0x02
GPU_HEADER = 64  # NR_DUMP_HEADER_BYTES
LUMA = (0.2126, 0.7152, 0.0722)


def read_dump(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER.size or not data.startswith(b"BZDUMP1"):
        raise ValueError("not a frame dump")
    fields = HEADER.unpack_from(data)
    names = ("magic headerBytes blockBytes frame width height scale reducedWidth reducedHeight cropWidth cropHeight "
             "cropX cropY pictures flags outputFormat feature generation inputType whiteSource preExposure whiteScale "
             "whiteEV shoulder detail colour maxGain highlight exe").split()
    h = dict(zip(names, fields))
    h["exe"] = h["exe"].split(b"\0", 1)[0].decode("utf-8", "replace")
    block = data[h["headerBytes"]:h["headerBytes"] + h["blockBytes"]]
    if len(block) != h["blockBytes"]:
        raise ValueError("cut short")
    tag, white, exposure = struct.unpack_from("<Iff", block, 0)
    rw, rh, cw, ch = h["reducedWidth"], h["reducedHeight"], h["cropWidth"], h["cropHeight"]
    end = GPU_HEADER + h["pictures"] * (rw * rh + cw * ch) * 8
    closing, = struct.unpack_from("<I", block, end)
    if tag != closing or tag != (h["frame"] + 1) & 0xFFFFFFFF:
        raise ValueError("the frame stamps do not match (%d, %d)" % (tag, closing))
    h["white"], h["exposure"] = white, exposure

    def picture(part, index):
        w, hh = (rw, rh) if part == 0 else (cw, ch)
        start = GPU_HEADER + (index * rw * rh if part == 0 else h["pictures"] * rw * rh + index * cw * ch) * 8
        values = struct.unpack_from("<%de" % (w * hh * 4), block, start)
        return w, hh, values

    return h, picture


def write_png(path, width, height, rgb):
    """rgb: bytes, 3 per pixel, rows top to bottom."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def srgb(v):
    if not v > 0.0:
        return 0
    if v >= 1.0:
        return 255
    e = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1.0 / 2.4) - 0.055
    return int(e * 255.0 + 0.5)


def shoulder(m, s):
    if m <= s:
        return m
    t = (m - s) / (1.0 - s)
    return s + (1.0 - s) * t / (1.0 + t)


def finite(*values):
    return all(v == v and abs(v) != math.inf for v in values)


def through_curve(values, h):
    """The frame as the model would see it: over the white point, the shoulder, sRGB."""
    linear = h["flags"] & FLAG_LINEAR
    white, s = h["white"], h["shoulder"]
    out = bytearray()
    for i in range(0, len(values), 4):
        r, g, b = values[i], values[i + 1], values[i + 2]
        if not finite(r, g, b):
            out += b"\xff\x00\xff"  # not a number: magenta
            continue
        if linear:
            r, g, b = max(r, 0.0) / white, max(g, 0.0) / white, max(b, 0.0) / white
            m = max(r, g, b)
            if m > s:
                k = shoulder(m, s) / m
                r, g, b = r * k, g * k, b * k
            out += bytes((srgb(r), srgb(g), srgb(b)))
        else:
            out += bytes((min(255, max(0, int(r * 255 + 0.5))), min(255, max(0, int(g * 255 + 0.5))),
                          min(255, max(0, int(b * 255 + 0.5)))))
    return out


def encoded(values, width, zebra):
    """A picture that is display-encoded already (the proxy, the model's output); zebra stripes from the alpha."""
    out = bytearray()
    for i in range(0, len(values), 4):
        r, g, b, a = values[i:i + 4]
        p = i // 4
        x, y = p % width, p // width
        if zebra and a >= 0.5 and ((x + y) // 4) % 2 == 0:
            out += b"\xff\x26\x26" if a >= 1.5 else b"\xf2\x4d\xf2"
            continue
        out += bytes(min(255, max(0, int((c if c == c else 0.0) * 255 + 0.5))) for c in (r, g, b))
    return out


def luma(values, i):
    return LUMA[0] * values[i] + LUMA[1] * values[i + 1] + LUMA[2] * values[i + 2]


def gain_map(before, after, floor):
    out = bytearray()
    gains = []
    for i in range(0, len(before), 4):
        a, b = luma(before, i), luma(after, i)
        if not finite(a, b):
            out += b"\x00\x00\x00"
            continue
        g = math.log2((max(b, 0.0) + floor) / (max(a, 0.0) + floor))
        gains.append(g)
        t = max(-1.0, min(1.0, g))
        fade = int(255 * (1.0 - abs(t)) + 0.5)
        out += bytes((255, fade, fade)) if t >= 0 else bytes((fade, fade, 255))
    return out, gains


def percentile(sorted_values, fraction):
    if not sorted_values:
        return float("nan")
    return sorted_values[min(len(sorted_values) - 1, int(fraction * len(sorted_values)))]


def ev(v):
    return "black" if not v > 0 else "%+.2f" % math.log2(v)


def one(path):
    h, picture = read_dump(path)
    stem = path[:-len(".bzdump")] if path.endswith(".bzdump") else path
    floor = h["white"] / 4096.0
    summary = {}
    for part, tag in ((0, ""), (1, ".crop")):
        w, hh, frame = picture(part, 0)
        _, _, proxy = picture(part, 1)
        _, _, model = picture(part, 2)
        _, _, result = picture(part, 3)
        write_png(stem + tag + ".frame.png", w, hh, through_curve(frame, h))
        write_png(stem + tag + ".proxy.png", w, hh, encoded(proxy, w, True))
        write_png(stem + tag + ".model.png", w, hh, encoded(model, w, False))
        write_png(stem + tag + ".result.png", w, hh, through_curve(result, h))
        gains_png, gains = gain_map(frame, result, floor)
        write_png(stem + tag + ".gain.png", w, hh, gains_png)
        if part == 1:  # full resolution: the numbers come from the crop
            peaks = sorted(max(max(frame[i], frame[i + 1], frame[i + 2]), 0.0) / h["white"]
                           for i in range(0, len(frame), 4) if finite(frame[i], frame[i + 1], frame[i + 2]))
            zebra = [proxy[i + 3] for i in range(0, len(proxy), 4)]
            gains.sort()
            summary = {
                "peaks": [ev(percentile(peaks, f)) for f in (0.01, 0.10, 0.50, 0.90, 0.99, 0.999)],
                "shoulder": 100.0 * sum(1 for z in zebra if z >= 0.5) / max(1, len(zebra)),
                "heavy": 100.0 * sum(1 for z in zebra if z >= 1.5) / max(1, len(zebra)),
                "gain": ["%+.2f" % percentile(gains, f) for f in (0.01, 0.50, 0.99)],
            }
    pre = "absent" if h["preExposure"] != h["preExposure"] else "%.4g" % h["preExposure"]
    # The block's exposure is whatever t3 held: the game's exposure texture, or with the scene white point our own.
    source = {0: "exposure", 1: "manual", 2: "scene"}.get(h["whiteSource"], str(h["whiteSource"]))
    exposure = "%.4g" % h["exposure"] if h["flags"] & FLAG_EXPOSURE and source == "exposure" else "-"
    print("%s: %s frame %d, %s %dx%d, %s; W %.4g (%s, pre-exposure %s, exposure %s, WhiteEV %+.2f), Shoulder %.2f, "
          "DetailStrength %.2f, ColourStrength %.2f, MaxGainEV %.2f, HighlightRestore %.2f; centre crop %dx%d: largest "
          "channel / W in EV p1 %s p10 %s p50 %s p90 %s p99 %s p99.9 %s; in the shoulder %.2f%%, compressed >3 EV "
          "%.2f%%; composite change in EV p1 %s p50 %s p99 %s" % (
              os.path.basename(path), h["exe"], h["frame"], "RR" if h["feature"] == 13 else "SR", h["width"],
              h["height"], "linear HDR" if h["flags"] & FLAG_LINEAR else "display-encoded", h["white"],
              source, pre, exposure, h["whiteEV"], h["shoulder"],
              h["detail"], h["colour"], h["maxGain"], h["highlight"], h["cropWidth"], h["cropHeight"],
              *summary["peaks"], summary["shoulder"], summary["heavy"], *summary["gain"]))


def main(arguments):
    paths = []
    if not arguments:
        arguments = [os.path.join(os.environ.get("LOCALAPPDATA", "."), "Banana-Zero", "dumps")]
    for argument in arguments:
        if os.path.isdir(argument):
            paths += sorted(os.path.join(argument, n) for n in os.listdir(argument) if n.endswith(".bzdump"))
        else:
            paths.append(argument)
    if not paths:
        print("no .bzdump files in", ", ".join(arguments))
        return 1
    failed = 0
    for path in paths:
        try:
            one(path)
        except (OSError, ValueError, struct.error) as problem:
            print("%s: %s" % (path, problem))
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
