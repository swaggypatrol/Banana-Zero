#!/usr/bin/env python3
r"""The closed loop: pictures through dxgi.dll's NR pass on this PC's GPU, and what it did to them, measured.

tests/nrhost.cpp is the game: DLSS Super Resolution at DLAA through the driver's NGX core, dxgi.dll's NR pass after
it with the model beside it, and a second SR feature on the same inputs that NR does not follow, so that every
captured frame comes out twice, after NR and as DLSS alone. This script makes the pictures, runs it, and measures the
change NR made on regions it knows, from the pictures themselves: dxgi.dll's own "kept" log line is a prediction, the
numbers here are what the Output holds.

    python tools/nrloop.py prepare --model <nvngx_dlssnr.dll> --dlss <nvngx_dlss.dll>
    python tools/nrloop.py identity           the rig: NR off and DetailStrength 0 leave DLSS's picture bit for bit
    python tools/nrloop.py bright [--sdr]     the model's white balance on greys from the mid-tones to the top,
                                              kept (GlobalColour 1) and taken off (the default, GlobalColour 0)
    python tools/nrloop.py night [--sdr]      what the white balance taken off does to true greys when most of the
                                              frame is warm light and little of it is grey
    python tools/nrloop.py frames <pfm>...    real pictures (tools/q2capture.py makes some): the model's colour change
                                              per pixel, dxgi.dll's white balance estimate against a fit that tells
                                              white balance from saturation, and the tint on near-greys by level

prepare copies the model beside dxgi.dll in build\Release and DLSS's DLL into build\Release\snippets (both stay the
user's own files; build is not in the repository). Runs land in build\nrloop\<name>. Needs numpy.

The change at a pixel, between DLSS alone (before) and after NR, both as linear light (SDR decoded from sRGB):
    dR = log2((after.R / after.G) / (before.R / before.G)), dB the same for blue, dY = log2(Y(after) / Y(before)).
A region's number is the mean over its pixels with the tenth at either end left out (8-bit SDR steps are coarser
than the changes measured; the texture under each region dithers them); pixels with a channel at or below 1/4096 in
either picture are left out.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.join(REPO, "build", "Release")
RUNS = os.path.join(REPO, "build", "nrloop")
LUMA = np.array([0.2126, 0.7152, 0.0722])
SHOULDER = 0.70  # dlssnr.ini's default


# ---------------------------------------------------------------------------------------------------------------
# Pictures.

def read_pfm(path):
    with open(path, "rb") as f:
        kind = f.readline().strip()
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4")
    channels = 3 if kind == b"PF" else 1
    picture = data.reshape(height, width, channels)[::-1].astype(np.float64)
    return np.repeat(picture, 3, axis=2) if channels == 1 else picture


def write_pfm(path, picture):
    with open(path, "wb") as f:
        f.write(b"PF\n%d %d\n-1.0\n" % (picture.shape[1], picture.shape[0]))
        f.write(np.ascontiguousarray(picture[::-1], dtype="<f4").tobytes())


def srgb_encode(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(v, 1.0 / 2.4) - 0.055)


def srgb_decode(e):
    e = np.clip(e, 0.0, 1.0)
    return np.where(e <= 0.04045, e / 12.92, np.power((e + 0.055) / 1.055, 2.4))


def shoulder_inverse(y, s=SHOULDER):
    """nr_common.hlsli ShoulderInverse: the frame value (over the white point) the encode turns into y."""
    u = np.clip((y - s) / (1.0 - s), 0.0, 1.0 - 1.0 / 4096.0)
    return np.where(y <= s, y, s + (1.0 - s) * u / (1.0 - u))


def frame_values(light, sdr):
    """The Output's values for a picture of the light the model is to see (linear, 0..1): the encode undone for HDR
    with the white point at 1 (WhiteSource manual, WhiteEV 0), sRGB for SDR."""
    if sdr:
        return srgb_encode(light)
    m = np.maximum(light.max(axis=2, keepdims=True), 1e-12)
    return light * shoulder_inverse(m) / m


# ---------------------------------------------------------------------------------------------------------------
# Running the host.

def file_version(path):
    quoted = path.replace("'", "''")
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-Item -LiteralPath '%s').VersionInfo.FileVersion" % quoted], capture_output=True, text=True)
    return out.stdout.strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def prepare(model, dlss):
    os.makedirs(os.path.join(RELEASE, "snippets"), exist_ok=True)
    for source, target in ((model, os.path.join(RELEASE, "nvngx_dlssnr.dll")),
                           (dlss, os.path.join(RELEASE, "snippets", "nvngx_dlss.dll"))):
        shutil.copyfile(source, target)
        print("%s -> %s (version %s, sha256 %s)" % (source, target, file_version(target), sha256(target)[:16]))


def run(name, pictures, sets=None, sdr=False, frames=90, fps=None, capture="last", exposure=None, cuts=False):
    """Runs nrhost over the pictures (arrays of Output values, or (array, frames) pairs) with the dlssnr.ini lines in
    sets (a dict). Returns {frame: (after, before)} as the Output held them, and the run's dlssnr.log."""
    busy = subprocess.run(["tasklist", "/FI", "IMAGENAME eq nrhost.exe", "/NH"], capture_output=True, text=True).stdout
    if "nrhost.exe" in busy:
        raise RuntimeError("another nrhost is running: runs share dlssnr.ini and dlssnr.log beside it, one at a time")
    folder = os.path.join(RUNS, name)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    arguments = [os.path.join(RELEASE, "nrhost.exe"), "--out", folder, "--frames", str(frames), "--capture", capture]
    if sdr:
        arguments.append("--sdr")
    if fps:
        arguments += ["--fps", str(fps)]
    if exposure is not None:
        arguments += ["--exposure", str(exposure)]
    if cuts:
        arguments.append("--cuts")
    for key, value in (sets or {}).items():
        arguments += ["--set", "%s=%s" % (key, value)]
    for i, item in enumerate(pictures):
        picture, count = item if isinstance(item, tuple) else (item, None)
        path = os.path.join(folder, "in%02d.pfm" % i)
        write_pfm(path, picture)
        arguments.append(path if count is None else "%s*%d" % (path, count))
    done = subprocess.run(arguments, capture_output=True, text=True)
    with open(os.path.join(folder, "nrhost.txt"), "w") as f:
        f.write(done.stdout)
    shutil.copyfile(os.path.join(RELEASE, "dlssnr.log"), os.path.join(folder, "dlssnr.log"))
    with open(os.path.join(folder, "dlssnr.log"), encoding="utf-8", errors="replace") as f:
        log = f.read()
    if done.returncode != 0:
        raise RuntimeError("nrhost exited %d:\n%s" % (done.returncode, done.stdout[-3000:]))
    results = {}
    for entry in sorted(os.listdir(folder)):
        if entry.endswith(".nr.pfm"):
            frame = int(entry.split(".")[0])
            results[frame] = (read_pfm(os.path.join(folder, entry)),
                              read_pfm(os.path.join(folder, "%04d.dlss.pfm" % frame)))
    return results, log


def nr_frames(log):
    """NR's own count of the frames the model delivered, from the totals at exit."""
    marker = "; NR "
    for line in reversed(log.splitlines()):
        if "process ending; totals" in line and marker in line:
            return int(line.split(marker, 1)[1].split()[0])
    return 0


def kept_lines(log):
    """dxgi.dll's prediction of a grey's colour after the composite (LogSample), from its stats lines."""
    found = []
    for line in log.splitlines():
        if "predicted after the composite" in line:
            tail = line.split("predicted after the composite", 1)[1]
            rg = float(tail.split("R/G x", 1)[1].split()[0].rstrip(",;"))
            bg = float(tail.split("B/G x", 1)[1].split()[0].rstrip(",;"))
            greys = float(line.split("fitted on ", 1)[1].split("%", 1)[0])
            found.append({"rg": rg, "bg": bg, "greyShare": greys})
    return found


# ---------------------------------------------------------------------------------------------------------------
# Measuring.

def linear(picture, sdr):
    return srgb_decode(picture) if sdr else picture


def trimmed_mean(values, cut=0.1):
    values = np.sort(values)
    k = int(len(values) * cut)
    return float(np.mean(values[k:len(values) - k] if len(values) > 2 * k else values))


def change(after, before, region, sdr):
    """dR, dB, dY over a region (a pair of slices; a boolean mask also works), and how many pixels counted."""
    a = linear(after[region], sdr).reshape(-1, 3)
    b = linear(before[region], sdr).reshape(-1, 3)
    ok = np.all(np.isfinite(a), axis=1) & np.all(np.isfinite(b), axis=1)
    ok &= np.all(a > 1.0 / 4096.0, axis=1) & np.all(b > 1.0 / 4096.0, axis=1)
    a, b = a[ok], b[ok]
    if len(a) == 0:
        return None
    dr = np.log2((a[:, 0] / a[:, 1]) / (b[:, 0] / b[:, 1]))
    db = np.log2((a[:, 2] / a[:, 1]) / (b[:, 2] / b[:, 1]))
    dy = np.log2((a @ LUMA) / (b @ LUMA))
    return {"dR": trimmed_mean(dr), "dB": trimmed_mean(db), "dY": trimmed_mean(dy), "pixels": int(len(a))}


# ---------------------------------------------------------------------------------------------------------------
# Scenes.

GREY_LEVELS = (0.05, 0.10, 0.18, 0.30, 0.45, 0.60, 0.66, 0.72, 0.78, 0.85, 0.92, 0.97)
COLOURS = {"red": (0.80, 0.08, 0.06), "orange": (0.85, 0.40, 0.08), "green": (0.10, 0.55, 0.12),
           "cyan": (0.08, 0.50, 0.60), "blue": (0.06, 0.10, 0.70), "skin": (0.62, 0.40, 0.30)}
CASTS = {"neutral": (1.0, 1.0, 1.0), "warm": (1.0, 0.86, 0.66), "cool": (0.70, 0.86, 1.0)}


def chart(cast=(1.0, 1.0, 1.0), width=1280, height=720, background=0.18):
    """The light the model is to see (linear, 0..1): a textured background of mid grey under the cast, a row of grey
    patches at GREY_LEVELS (the level is the patch's largest channel, so a cast never pushes it past the top), and a
    row of coloured ones. Returns the picture and {name: region}."""
    cast = np.array(cast, dtype=np.float64) / max(cast)
    y, x = np.mgrid[0:height, 0:width].astype(np.float64)
    rng = np.random.default_rng(7)
    texture = 1.0 + 0.04 * np.sin(x / 7.3) * np.cos(y / 5.1) + 0.02 * rng.standard_normal((height, width))
    light = background * texture[..., None] * cast
    regions = {}
    side = width // (len(GREY_LEVELS) + 2)
    gap = side // 6
    inset = side // 5
    for i, level in enumerate(GREY_LEVELS):
        x0, y0 = side + i * side, height // 6
        patch = (slice(y0, y0 + side - gap), slice(x0, x0 + side - gap))
        light[patch] = level * cast * texture[patch][..., None]
        regions["grey %.2f" % level] = (slice(y0 + inset, y0 + side - gap - inset),
                                        slice(x0 + inset, x0 + side - gap - inset))
    for i, (name, colour) in enumerate(COLOURS.items()):
        x0, y0 = side + 2 * i * side, height // 6 + 2 * side
        patch = (slice(y0, y0 + side - gap), slice(x0, x0 + 2 * side - gap))
        light[patch] = np.array(colour) * cast * texture[patch][..., None]
        regions[name] = (slice(y0 + inset, y0 + side - gap - inset), slice(x0 + inset, x0 + 2 * side - gap - inset))
    regions["background"] = (slice(height - height // 5, height - 8), slice(8, width - 8))
    return np.clip(light, 0.0, 0.999), regions


def night(width=1280, height=720, lit=(1.0, 0.62, 0.30), haze=(1.0, 0.78, 0.55), greys=True, ramp=False, road=0.0):
    """A night street, as light the model is to see: a dark textured ground, most of the frame under warm (sodium)
    light in two saturations at a range of levels, small true greys (road paint, a kerb, a sign) and two neon signs;
    with ramp, a row of small grey patches at GREY_LEVELS too; with road, the lower half of the frame a neutral road at
    that level (dusk: much of the frame grey, under the same lights). Returns the picture and {name: region}; the true
    greys are the regions whose names start with "grey", "ramp" or "road"."""
    y, x = np.mgrid[0:height, 0:width].astype(np.float64)
    rng = np.random.default_rng(11)
    texture = 1.0 + 0.05 * np.sin(x / 6.1) * np.cos(y / 4.3) + 0.03 * rng.standard_normal((height, width))
    light = 0.012 * texture[..., None] * np.array([0.9, 0.95, 1.0])
    regions = {}
    # Pools of light: a smooth falloff of the lamp's colour from each lamp down the frame.
    lamps = ((0.2, 0.25), (0.5, 0.2), (0.8, 0.3))
    pool = np.zeros((height, width))
    for lx, ly in lamps:
        d2 = ((x - lx * width) / (0.22 * width)) ** 2 + ((y - ly * height) / (0.45 * height)) ** 2
        pool += 0.45 * np.exp(-d2)
    pool = np.minimum(pool, 0.9)
    if road > 0.0:
        light[height // 2:] = road * texture[height // 2:, :, None]
        regions["road"] = (slice(int(height * 0.88), height - 8), slice(8, width - 8))
    lit_mask = x < width * 0.62
    light += np.where(lit_mask[..., None], pool[..., None] * np.array(lit), pool[..., None] * np.array(haze)) * texture[..., None]
    regions["sodium"] = (slice(int(height * 0.3), int(height * 0.6)), slice(int(width * 0.1), int(width * 0.55)))
    regions["haze"] = (slice(int(height * 0.3), int(height * 0.6)), slice(int(width * 0.66), int(width * 0.95)))
    if greys:
        for name, (x0, y0, w, h, level) in {"grey paint": (0.05, 0.80, 0.25, 0.05, 0.25),
                                            "grey kerb": (0.40, 0.88, 0.30, 0.04, 0.12),
                                            "grey sign": (0.75, 0.70, 0.07, 0.12, 0.40)}.items():
            patch = (slice(int(y0 * height), int((y0 + h) * height)), slice(int(x0 * width), int((x0 + w) * width)))
            light[patch] = level * texture[patch][..., None]
            regions[name] = patch
    if ramp:
        side = width // (len(GREY_LEVELS) + 2)
        for i, level in enumerate(GREY_LEVELS):
            x0, y0 = side + i * side, int(height * 0.62)
            patch = (slice(y0, y0 + side // 2), slice(x0, x0 + side - side // 6))
            light[patch] = level * texture[patch][..., None]
            inner = (slice(y0 + side // 10, y0 + side // 2 - side // 10), slice(x0 + side // 8, x0 + side - side // 6 - side // 8))
            regions["ramp %.2f" % level] = inner
    for name, (x0, colour) in {"neon blue": (0.30, (0.05, 0.15, 0.8)), "neon pink": (0.88, (0.8, 0.08, 0.5))}.items():
        patch = (slice(int(0.05 * height), int(0.12 * height)), slice(int(x0 * width), int((x0 + 0.08) * width)))
        light[patch] = np.array(colour) * texture[patch][..., None]
        regions[name] = patch
    return np.clip(light, 0.0, 0.999), regions


# ---------------------------------------------------------------------------------------------------------------
# Experiments.

BASE = {"WhiteSource": "manual", "WhiteEV": 0, "DetailStrength": 1, "ColourStrength": 1}


def identity(args):
    """The rig before anything is measured with it: with Enabled 0 and with DetailStrength 0 the two features'
    pictures are identical (so DLSS is the same in both, and NR leaves its frame alone), and with the defaults NR
    ran and changed the picture."""
    light, _ = chart(CASTS["warm"])
    failures = 0
    for sdr in (False, True):
        picture = frame_values(light, sdr)
        for label, sets, same in (("Enabled 0", {"Enabled": 0}, True), ("DetailStrength 0", {"DetailStrength": 0}, True),
                                  ("defaults", {}, False)):
            results, log = run("identity-%s-%s" % ("sdr" if sdr else "hdr", label.split()[0].lower()), [picture],
                               dict(BASE, **sets), sdr=sdr, frames=args.frames)
            after, before = results[max(results)]
            identical = np.array_equal(after, before)
            ok = identical == same and (same or nr_frames(log) > 0)
            failures += not ok
            print("%s %s %-17s pictures %s, NR delivered %d frames" % ("ok  " if ok else "FAIL", "SDR" if sdr else "HDR",
                  label, "identical" if identical else "differ (max %.4g)" % np.abs(after - before).max(),
                  nr_frames(log)))
    return failures


def bright(args):
    """What the model does to a grey's colour from the mid-tones to the top of its input, and what the composite
    leaves of it: per grey patch, the change with the model's white balance kept (GlobalColour 1, HighlightColour 6:
    the model's own, as the composite adds a grey's colour change at its own light) and with it taken off (the
    default). A white balance the model applies to bright greys as to dark ones, but that the composite takes off
    less of near the top (TakeCastOff's room), shows as a residual that grows towards the top."""
    report = {}
    for cast_name in args.casts:
        light, regions = chart(CASTS[cast_name])
        picture = frame_values(light, args.sdr)
        mode = "sdr" if args.sdr else "hdr"
        rows = {}
        for label, sets in (("model", {"GlobalColour": 1, "HighlightColour": 6}), ("default", {"GlobalColour": 0})):
            results, log = run("bright-%s-%s-%s" % (mode, cast_name, label), [picture],
                               dict(BASE, StatsLog=0.5, **sets), sdr=args.sdr, frames=args.frames, fps=60)
            after, before = results[max(results)]
            for name, region in regions.items():
                rows.setdefault(name, {})[label] = change(after, before, region, args.sdr)
            kept = kept_lines(log)
            rows.setdefault("_log", {})[label] = kept[-1] if kept else None
        report[cast_name] = rows
        print("\n%s, %s cast: change in EV (median over each patch); model = GlobalColour 1, default = GlobalColour 0"
              % (mode.upper(), cast_name))
        print("%-12s %8s %8s %8s | %8s %8s %8s | %s" % ("region", "model dR", "dB", "dY", "def. dR", "dB", "dY",
                                                          "residual share of the model's tint (R, B)"))
        for name, row in rows.items():
            if name == "_log":
                continue
            m, d = row["model"], row["default"]
            if m is None or d is None:
                print("%-12s (no pixels)" % name)
                continue
            share = lambda r, t: "%5.0f%%" % (100.0 * r / t) if abs(t) > 0.01 else "    -"
            print("%-12s %+8.4f %+8.4f %+8.4f | %+8.4f %+8.4f %+8.4f | %s %s" % (
                name, m["dR"], m["dB"], m["dY"], d["dR"], d["dB"], d["dY"], share(d["dR"], m["dR"]),
                share(d["dB"], m["dB"])))
        for label, kept in rows["_log"].items():
            if kept:
                print("dxgi.dll's prediction (%s run): grey R/G x%.4f B/G x%.4f (dR %+.4f, dB %+.4f), greys %.1f%%" % (
                    label, kept["rg"], kept["bg"], np.log2(kept["rg"]), np.log2(kept["bg"]), kept["greyShare"]))
    with open(os.path.join(RUNS, "bright-%s.json" % ("sdr" if args.sdr else "hdr")), "w") as f:
        json.dump(report, f, indent=1)
    return 0


def night_experiment(args):
    """The white balance taken off where the frame holds little grey: at each strength, the change on every region
    with the model's white balance kept (GlobalColour 1) and taken off (GlobalColour 0), and what dxgi.dll measured
    as the model's white balance. On a true grey, GlobalColour 0 should leave no more colour change than the model
    made; a white balance measured on the warm light's own change in saturation shows instead as a cast the other
    way on the greys (blue for warm light)."""
    ambient = {"warm": {}, "cool": {"lit": (0.30, 0.62, 1.0), "haze": (0.55, 0.78, 1.0)}}[args.ambient]
    light, regions = night(ramp=args.ramp, road=args.road, **ambient)
    picture = frame_values(light, args.sdr)
    mode = ("sdr" if args.sdr else "hdr") + ("-ramp" if args.ramp else "") + "-" + args.ambient + (
        "-road%g" % args.road if args.road else "")
    for detail, colour in ((1.0, 1.0), (1.5, 1.25))[:1 if args.ramp else 2]:
        rows, logs = {}, {}
        for label, gc in (("model", 1), ("default", 0)):
            results, log = run("night-%s-%g-%g-%s" % (mode, detail, colour, label), [picture],
                               dict(BASE, DetailStrength=detail, ColourStrength=colour, GlobalColour=gc, StatsLog=0.5),
                               sdr=args.sdr, frames=args.frames, fps=60)
            after, before = results[max(results)]
            for name, region in regions.items():
                rows.setdefault(name, {})[label] = change(after, before, region, args.sdr)
            logs[label] = [l for l in log.splitlines() if "white balance R/G" in l]
        print("\n%s night, DetailStrength %.2f x ColourStrength %.2f: change in EV" % (mode.upper(), detail, colour))
        print("%-12s %8s %8s %8s | %8s %8s %8s" % ("region", "model dR", "dB", "dY", "def. dR", "dB", "dY"))
        for name, row in rows.items():
            m, d = row["model"], row["default"]
            if m and d:
                print("%-12s %+8.4f %+8.4f %+8.4f | %+8.4f %+8.4f %+8.4f" % (name, m["dR"], m["dB"], m["dY"], d["dR"],
                                                                         d["dB"], d["dY"]))
        line = logs["default"][-1] if logs["default"] else ""
        if "white balance R/G" in line:
            print("dxgi.dll: " + line.split("the model's cast at", 1)[1].split("; its mean", 1)[0].strip())
    return 0


def white_of(picture):
    """The white point the scene meter aims at: the frame's middle luminance 2.32 EV below it."""
    return float(np.median(picture @ LUMA)) * 2.0 ** 2.32


def encode_linear(frame, s=SHOULDER):
    """nr_common.hlsli EncodeLinear with the white point at 1, capped at 1 (ProxyLight)."""
    x = np.maximum(np.nan_to_num(frame), 0.0)
    m = np.maximum(x.max(axis=2, keepdims=True), 1e-20)
    t = (m - s) / (1.0 - s)
    shoulder = np.where(m > s, s + (1.0 - s) * t / (1.0 + t), m)
    return np.minimum(np.where(m > s, x * shoulder / m, x), 1.0)


def model_change(after, before):
    """Per pixel, from a run with the model's colour kept whole (GlobalColour 1, HighlightColour 6): the light the
    model saw (p), the model's colour change as log2 of its gain on red and on blue over its gain on green (t, what
    nr_common.hlsli's Tint measures), and the input's own log chroma (c). The composite adds a pixel's colour change
    at its own light, so the result's chromaticity is the model's output's, at any level."""
    p = encode_linear(before)
    a = np.maximum(after, 1e-20)
    b = np.maximum(before, 1e-20)
    t = np.stack([np.log2((a[..., 0] * b[..., 1]) / (b[..., 0] * a[..., 1])),
                  np.log2((a[..., 2] * b[..., 1]) / (b[..., 2] * a[..., 1]))], axis=-1)
    pc = np.maximum(p, 1e-20)
    c = np.stack([np.log2(pc[..., 0] / pc[..., 1]), np.log2(pc[..., 2] / pc[..., 1])], axis=-1)
    return p, np.clip(t, -2.0, 2.0), c


def greyness(p, s=SHOULDER):
    """nr_common.hlsli Greyness on the model's input alone (the model output's own floor is left out)."""
    lo, hi = p.min(axis=2), p.max(axis=2)
    share = np.where(hi > 0, lo / np.maximum(hi, 1e-20), 0.0)
    return np.where((lo > 1.0 / 1024.0) & (hi < s), share * share, 0.0)


def estimates(p, t, c):
    """dxgi.dll's white balance (cast_tint, before its shrink) beside v0.3.0's mean tint over the greyish pixels, the
    grey share, and d^2."""
    w = greyness(p)
    mean = (w.reshape(-1)[:, None] * t.reshape(-1, 2)).sum(axis=0) / max(w.sum(), 1e-20)
    fit, d2 = cast_tint(w, t, c, 0.0)
    return mean, fit, w.sum() / w.size, d2


REACH = 128.0  # NR_GREY_REACH: how far the fit is trusted to reach grey, d^2 at which it counts half (nr.hlsl's CastTint)
SPREAD = 0.1  # NR_GREY_SPREAD: the ridge on the slope, as a spread of chroma in EV


def cast_tint(w, t, c, prior, reach=REACH, spread=SPREAD):
    """nr.hlsl's CastTint in doubles: the intercept of tint = b + A chroma over the greyish pixels by weighted least
    squares (centered moments, a ridge of spread^2 on A), its slope's share of it weighed by reach / (reach + d^2),
    where d^2 is how far grey lies from the greyish pixels' mean chroma in units of their spread (a fit that has to
    reach far from its pixels to find grey is not trusted to), then shrunk by grey / (grey + prior). The ridge also
    takes in the rounding of the sums, half a 1/64 step per 32x32 group of the cast pass over the greyness (w is the
    picture's own 2-D weights, for its size). reach None: the whole slope and no rounding (v0.3.1). Returns the white
    balance (R, B in EV) and d^2."""
    groups = np.ceil(w.shape[0] / 32.0) * np.ceil(w.shape[1] / 32.0)
    w = w.reshape(-1)
    t = t.reshape(-1, 2)
    c = c.reshape(-1, 2)
    s = w.sum()
    if s <= 0:
        return np.zeros(2), 0.0
    mc = (w[:, None] * c).sum(axis=0) / s
    mt = (w[:, None] * t).sum(axis=0) / s
    dc, dt = c - mc, t - mt
    rounding = 0.0 if reach is None else 0.5 * groups / (s * 64.0)
    cov = (w[:, None, None] * dc[:, :, None] * dc[:, None, :]).sum(axis=0) / s + (spread * spread + rounding) * np.eye(2)
    cct = (w[:, None, None] * dc[:, :, None] * dt[:, None, :]).sum(axis=0) / s
    m = np.linalg.solve(cov, mc)
    d2 = float(m @ mc)
    trust = 1.0 if reach is None else reach / (reach + d2)
    return (mt - trust * (m @ cct)) * s / (s + prior), d2


def removal_candidates(p, t, c, use):
    """What each candidate would divide out at every pixel (R, B tint in EV), estimated from the pixels in use (a mask),
    each faded by room above the shoulder as TakeCastOff does: "v0.3.0 mean", the weighted mean tint over greyish
    pixels with NR_GREY_PRIOR; "v0.3.1 fit", the intercept with its whole slope; "fit reach N", the intercept with the
    slope weighed by N / (N + d^2); "default", what the shipped TakeCastOff does (REACH)."""
    w = greyness(p) * use
    level = p.max(axis=2)
    room = np.clip((1.0 - level) / (1.0 - SHOULDER), 0.0, 1.0)[..., None]
    prior = use.sum() / 256.0
    out = {"v0.3.0 mean": room * ((w[..., None] * t).sum(axis=(0, 1)) / (w.sum() + prior)),
           "v0.3.1 fit": room * cast_tint(w, t, c, prior, None)[0]}
    for reach in (32.0, 512.0):
        out["fit reach %g" % reach] = room * cast_tint(w, t, c, prior, reach)[0]
    out["default"] = room * cast_tint(w, t, c, prior)[0]
    return out


def frames_experiment(args):
    """Real pictures through the rig, each normalised to the white point the scene meter aims at and shown for
    --frames frames after a Reset (the model's history thrown away: each is a frame of its own), once with the
    model's colour kept whole (GlobalColour 1, HighlightColour 6) and once as the default composite (GlobalColour 0).
    Then offline, per picture: v0.3.0's mean tint and dxgi.dll's white balance (cast_tint) with its d^2, how much
    tint each candidate of removal_candidates leaves on near-grey pixels (smallest channel over largest above
    0.8; estimated on the even pixels of a checkerboard, scored on the odd ones), the default composite's measured
    residual next to the prediction of it by the algorithm it runs ("default"), and, over the whole list, how far each estimate moves
    from one picture to the next (for a sequence, the swing a player sees)."""
    names = [os.path.splitext(os.path.basename(path))[0] for path in args.pictures]
    pictures = []
    for path in args.pictures:
        picture = read_pfm(path)
        pictures.append(picture / white_of(picture))
    shown = [(picture, args.frames) for picture in pictures]
    capture = ",".join(str((i + 1) * args.frames - 1) for i in range(len(pictures)))
    kept, _ = run("frames-model", shown, dict(BASE, GlobalColour=1, HighlightColour=6), frames=args.frames,
                  capture=capture, cuts=True)
    default, _ = run("frames-default", shown, dict(BASE, GlobalColour=0), frames=args.frames, capture=capture,
                     cuts=True)
    rows, series = [], {"mean": [], "fit": []}
    for i, name in enumerate(names):
        frame = (i + 1) * args.frames - 1
        after, before = kept[frame]
        p, t, c = model_change(after, before)
        mean, b, share, d2 = estimates(p, t, c)
        series["mean"].append(mean)
        series["fit"].append(b)
        left_after, left_before = default[frame]
        _, left, _ = model_change(left_after, left_before)
        lo, hi = p.min(axis=2), p.max(axis=2)
        near = (lo > 0.004) & (lo / np.maximum(hi, 1e-20) > 0.8)
        yy, xx = np.mgrid[0:p.shape[0], 0:p.shape[1]]
        even = ((yy + xx) % 2 == 0).astype(np.float64)
        odd = near & (even == 0)
        predicted = t - removal_candidates(p, t, c, np.ones_like(even))["default"]
        scores = {"measured default": np.abs(left[near]).mean(axis=0),
                  "predicted default": np.abs(predicted[near]).mean(axis=0),
                  "model alone": np.abs(t[odd]).mean(axis=0)}
        signed = {"model alone": t[odd].mean(axis=0)}
        for key, removal in removal_candidates(p, t, c, even).items():
            scores[key] = np.abs((t - removal)[odd]).mean(axis=0)
            signed[key] = (t - removal)[odd].mean(axis=0)
        signed["measured default"] = left[near].mean(axis=0)
        # Colours: how far each candidate leaves saturated pixels from the game's own colour, against the model alone.
        colourful = (lo / np.maximum(hi, 1e-20) < 0.5) & (hi > 0.05) & (even == 0)
        for key, removal in removal_candidates(p, t, c, even).items():
            series.setdefault("colours " + key, []).append(np.abs((t - removal)[colourful]).mean(axis=0))
        series.setdefault("colours model alone", []).append(np.abs(t[colourful]).mean(axis=0))
        rows.append((name, share, mean, b, d2, int(odd.sum()), scores))
        for key, value in signed.items():
            series.setdefault("greys " + key, []).append(value)
    print("%-10s %6s | %15s | %15s | %6s" % ("picture", "greys", "mean R B", "fit R B", "d^2"))
    for name, share, mean, b, d2, _, _ in rows:
        print("%-10s %5.1f%% | %+7.4f %+7.4f | %+7.4f %+7.4f | %6.2f" % (
            name, 100 * share, mean[0], mean[1], b[0], b[1], d2))
    keys = list(rows[0][6])
    print("\nmean |tint| left on near-grey pixels (R, B in EV)")
    print("%-10s %6s " % ("picture", "pixels") + " ".join("%17s" % k for k in keys))
    for name, _, _, _, _, n, scores in rows:
        print("%-10s %6d " % (name, n) + " ".join("%8.4f %8.4f" % tuple(scores[k]) for k in keys))
    total = {k: np.mean([r[6][k] for r in rows], axis=0) for k in keys}
    print("%-10s %6s " % ("mean", "") + " ".join("%8.4f %8.4f" % tuple(total[k]) for k in keys))
    if len(rows) > 2:
        print("\nhow far each estimate, and the mean tint left on the near-greys, moves from one picture to the next (R, B"
              " in EV): mean |step|, largest |step|, standard deviation")
        for key, values in series.items():
            if key.startswith("colours"):
                continue
            v = np.array(values)
            step = np.abs(np.diff(v, axis=0))
            print("%-24s %.4f %.4f | %.4f %.4f | %.4f %.4f" % (key, *step.mean(axis=0), *step.max(axis=0), *v.std(axis=0)))
    print("\nmean |tint| between the result and the game's own colour on saturated pixels (R, B in EV), over all pictures")
    for key, values in series.items():
        if key.startswith("colours"):
            print("%-24s %.4f %.4f" % (key, *np.mean(values, axis=0)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--model", required=True, help="the user's nvngx_dlssnr.dll")
    p.add_argument("--dlss", required=True, help="an nvngx_dlss.dll (DLSS Super Resolution)")
    p = sub.add_parser("identity")
    p.add_argument("--frames", type=int, default=90)
    p = sub.add_parser("night")
    p.add_argument("--sdr", action="store_true")
    p.add_argument("--ramp", action="store_true", help="grey patches at every level, to see the model's tint by level")
    p.add_argument("--ambient", default="warm", choices=["warm", "cool"], help="the street lights' colour")
    p.add_argument("--road", type=float, default=0.0, help="a neutral road over the lower half at this level")
    p.add_argument("--frames", type=int, default=150)
    p = sub.add_parser("frames")
    p.add_argument("pictures", nargs="+")
    p.add_argument("--frames", type=int, default=20, help="frames each picture is shown for, after a Reset")
    p = sub.add_parser("bright")
    p.add_argument("--sdr", action="store_true")
    p.add_argument("--frames", type=int, default=150)
    p.add_argument("--casts", nargs="+", default=list(CASTS), choices=list(CASTS))
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.model, args.dlss)
        return 0
    os.makedirs(RUNS, exist_ok=True)
    return {"identity": identity, "bright": bright, "night": night_experiment, "frames": frames_experiment}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
