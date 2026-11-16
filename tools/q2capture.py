#!/usr/bin/env python3
r"""Pictures for the closed loop from Quake II RTX: path-traced frames of its demo, in HDR, as PFM files that
tools/nrloop.py feeds through dxgi.dll's NR pass (Quake II RTX itself is Vulkan without DLSS, so Banana-Zero cannot
run inside it; its frames can go through tests/nrhost.cpp instead).

    python tools/q2capture.py [--game <q2rtx.exe>] [--shots 10:night 25:noon ...] [--size 1280x720] [--spp 256]
    python tools/q2capture.py --sequence 12:noon --count 40 [--every 0.25]

Each shot is a second of the bundled demo (q2demo1, 58 s long) under a sun preset (night, dawn, morning, noon,
evening, dusk; map: the map's own). The game runs in a window of the given size with its own home under
build\q2rtx\home, so the person's q2config.cfg and autoexec.cfg are neither read nor written, with HDR output on
(vid_hdr 1: the swap chain is scRGB, linear, 1 = 80 nits) and the HUD hidden. For each shot it seeks the demo, pauses
it, lets the path tracer accumulate --spp samples (photo mode 2: no denoiser, no noise left to speak of, no text), and saves
screenshothdr (Radiance .hdr). The game is driven over rcon on 127.0.0.1 with a random password and closed at the
end. Output: build\q2rtx\frames\<second>-<sun>.pfm, the scRGB values as they are, and <...>.png to look at.

With --sequence it takes --count frames of the demo as it plays from that second instead, one every --every
seconds, with the denoiser as in play rather than photo mode (seqNNN.pfm): the camera moves between them, so they
are what a game hands DLSS a few frames apart.

HDR screenshots need the display in Windows' HDR mode; without it the game turns vid_hdr off and the script stops.
"""

import argparse
import os
import secrets
import shutil
import socket
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nrloop import REPO, write_pfm, srgb_encode  # noqa: E402

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Quake II RTX\q2rtx.exe"
OUT = os.path.join(REPO, "build", "q2rtx")
SUNS = {"map": 0, "night": 3, "dawn": 4, "morning": 5, "noon": 6, "evening": 7, "dusk": 8}


class Rcon:
    """The game's console over loopback rcon (Quake II RTX's backdoor listens without a server running)."""

    def __init__(self, port, password):
        self.password = password
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.connect(("127.0.0.1", port))
        self.sock.settimeout(0.5)

    def send(self, command):
        try:
            self.sock.send(b"\xff\xff\xff\xffrcon " + self.password.encode() + b" " + command.encode() + b"\n\0")
        except OSError:
            pass  # not listening yet

    def ask(self, names, timeout=2.0):
        """The values of console variables or macros, by an echo with a fresh marker; None when no reply came."""
        marker = "BZ" + secrets.token_hex(8)
        self.send("echo " + marker + "|" + "|".join("$" "{%s}" % n for n in names))
        until = time.time() + timeout
        while time.time() < until:
            try:
                data = self.sock.recv(65536)
            except (socket.timeout, ConnectionResetError, OSError):
                continue
            for line in data[4:].decode("latin-1").splitlines():
                if line.strip().startswith(marker + "|"):
                    return dict(zip(names, line.strip().split("|")[1:]))
        return None


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read_hdr(path):
    """A Radiance .hdr (RGBE, run-length scanlines as stb_image_write makes them) as a float array, rows top down."""
    with open(path, "rb") as f:
        data = f.read()
    end = data.index(b"\n\n") + 2
    size_line_end = data.index(b"\n", end)
    parts = data[end:size_line_end].split()
    height, width = int(parts[1]), int(parts[3])
    flip = parts[0] == b"+Y"
    pos = size_line_end + 1
    rgbe = np.zeros((height, width, 4), np.uint8)
    for y in range(height):
        if data[pos] == 2 and data[pos + 1] == 2 and (data[pos + 2] << 8 | data[pos + 3]) == width:
            pos += 4
            for c in range(4):
                x = 0
                while x < width:
                    count = data[pos]
                    pos += 1
                    if count > 128:
                        count -= 128
                        rgbe[y, x:x + count, c] = data[pos]
                        pos += 1
                    else:
                        rgbe[y, x:x + count, c] = np.frombuffer(data, np.uint8, count, pos)
                        pos += count
                    x += count
        else:
            rgbe[y] = np.frombuffer(data, np.uint8, width * 4, pos).reshape(width, 4)
            pos += width * 4
    e = rgbe[..., 3].astype(np.int32)
    scale = np.where(e > 0, np.ldexp(1.0, e - 136), 0.0)
    picture = rgbe[..., :3].astype(np.float64) * scale[..., None]
    return picture[::-1] if flip else picture


def write_png(path, picture):
    import struct
    import zlib
    height, width = picture.shape[:2]
    rows = (np.clip(picture, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    raw = b"".join(b"\0" + rows[y].tobytes() for y in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def tail(path, lines=15):
    try:
        with open(path, encoding="latin-1") as f:
            return "".join(f.readlines()[-lines:])
    except OSError:
        return "(no console log)"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default=GAME)
    parser.add_argument("--shots", nargs="+", default=["3:noon", "8:noon", "8:night", "15:dusk", "22:noon", "22:night", "30:map", "38:evening", "45:night", "52:noon"],
                        help="second:sun, the sun one of " + ", ".join(SUNS))
    parser.add_argument("--size", default="1280x720")
    parser.add_argument("--spp", type=int, default=256, help="samples the path tracer accumulates for each shot")
    parser.add_argument("--sequence", help="second:sun to play from, taking --count frames")
    parser.add_argument("--count", type=int, default=40)
    parser.add_argument("--every", type=float, default=0.25)
    args = parser.parse_args()
    shots = []
    for shot in args.shots:
        second, sun = shot.split(":")
        shots.append((float(second), sun))
    home = os.path.join(OUT, "home")
    frames = os.path.join(OUT, "sequence" if args.sequence else "frames")
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(os.path.join(home, "baseq2"))
    os.makedirs(frames, exist_ok=True)
    for name in ("q2config.cfg", "autoexec.cfg"):
        with open(os.path.join(home, "baseq2", name), "w") as f:
            f.write("// tools/q2capture.py: an isolated home\n")

    port, password = free_port(), secrets.token_hex(32)
    sets = {"basedir": os.path.dirname(args.game), "homedir": home, "net_ip": "127.0.0.1", "net_enable_ipv6": "0",
            "net_port": str(port), "rcon_password": password, "backdoor": "1", "vid_rtx": "1", "vid_hdr": "1",
            "vid_fullscreen": "0", "vid_geometry": args.size, "vid_vsync": "0", "r_maxfps": "0", "cl_autopause": "0",
            "in_grab": "0", "s_enable": "0", "cl_cinematics": "0", "drs_enable": "0", "viewsize": "100",
            "scr_draw2d": "0", "scr_showpause": "0", "scr_fps": "0", "con_notifytime": "0", "cl_demowait": "1",
            "pt_accumulation_rendering": "2", "cl_demobar": "0", "pt_accumulation_rendering_framenum": str(args.spp), "pt_dof": "0",
            "gl_screenshot_async": "0", "gl_screenshot_message": "1", "logfile": "1", "logfile_flush": "1"}
    command = [args.game]
    for name, value in sets.items():
        command += ["+set", name, value]
    command += ["+demo", "q2demo1"]
    game = subprocess.Popen(command, cwd=os.path.dirname(args.game))
    rcon = Rcon(port, password)
    log = os.path.join(home, "baseq2", "logs", "console.log")
    try:
        # Until the demo plays: the map loaded, its clock moving.
        first, state, until = None, None, time.time() + 120
        while time.time() < until:
            if game.poll() is not None:
                raise RuntimeError("the game exited (%s) before the demo played" % game.returncode)
            state = rcon.ask(["cl_demopos", "vid_hdr", "cl_mapname"])
            if state and state["cl_mapname"] and ":" in state["cl_demopos"]:
                if state["vid_hdr"] != "1":
                    raise RuntimeError("vid_hdr is %s: the display is not in HDR mode, so there are no HDR "
                                       "screenshots" % state["vid_hdr"])
                if first is None:
                    first = state["cl_demopos"]
                elif state["cl_demopos"] != first:
                    break
            time.sleep(0.5)
        else:
            raise RuntimeError("the demo did not start playing within 120 s")
        print("the demo plays on %s" % state["cl_mapname"])
        if args.sequence:
            second, sun = args.sequence.split(":")
            rcon.send("set pt_accumulation_rendering 0")
            rcon.send("set sun_preset %d" % SUNS[sun])
            rcon.send("seek %d" % int(float(second)))
            time.sleep(2.0)
            for i in range(args.count):
                started = time.time()
                name = "seq%03d" % i
                rcon.send("screenshothdr " + name)
                path = os.path.join(home, "baseq2", "screenshots", name + ".hdr")
                for _ in range(80):
                    if os.path.exists(path) and os.path.getsize(path) > 0:
                        time.sleep(0.1)
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError("no screenshot %s; the console says:\n%s" % (path, tail(log)))
                picture = read_hdr(path)
                write_pfm(os.path.join(frames, name + ".pfm"), picture)
                if i % 10 == 0:
                    write_png(os.path.join(frames, name + ".png"),
                              srgb_encode(picture / max(np.percentile(picture, 99.5), 1e-6)))
                time.sleep(max(0.0, args.every - (time.time() - started)))
            print("%d frames in %s" % (args.count, frames))
            shots = []
        paused = False
        for second, sun in shots:
            if paused:
                rcon.send("pause")
                paused = False
            rcon.send("set sun_preset %d" % SUNS[sun])
            rcon.send("seek %d" % int(second))
            time.sleep(1.5)  # the seek lands and the frame after it renders
            rcon.send("pause")
            paused = True
            time.sleep(3.0 + args.spp / 60.0)  # the accumulation, at no fewer than 60 frames a second
            name = "%02d-%s" % (int(second), sun)
            rcon.send("screenshothdr " + name)
            path = os.path.join(home, "baseq2", "screenshots", name + ".hdr")
            for _ in range(40):
                if os.path.exists(path) and os.path.getsize(path) > 0:
                    time.sleep(0.3)
                    break
                time.sleep(0.25)
            else:
                raise RuntimeError("no screenshot %s; the console says:\n%s" % (path, tail(log)))
            picture = read_hdr(path)
            write_pfm(os.path.join(frames, name + ".pfm"), picture)
            write_png(os.path.join(frames, name + ".png"), srgb_encode(picture / max(np.percentile(picture, 99.5), 1e-6)))
            print("%s: %dx%d, scRGB max %.2f, p50 luminance %.4f" % (name, picture.shape[1], picture.shape[0],
                  picture.max(), np.median(picture @ np.array([0.2126, 0.7152, 0.0722]))))
    finally:
        rcon.send("quit")
        try:
            game.wait(10)
        except subprocess.TimeoutExpired:
            game.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
