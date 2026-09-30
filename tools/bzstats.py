#!/usr/bin/env python3
"""Sums up the statistics lines dxgi.dll writes into dlssnr.log (StatsLog in dlssnr.ini).
Standard library only.

    python tools/bzstats.py [--by-source] dlssnr.log [more.log ...]

For each log: how the white point and its sources moved, how the frame's brightness lay relative to the white point,
how much went into the shoulder or was compressed hard, how much the model changed and how often the composite
clamped, then whether the luminance median stays put as the white point moves (it does when the white point follows
the game's exposure or the scene), and a timeline of every twentieth line. With --by-source, all of that once for each
white point source the log's lines name (the game's exposure, the scene meter, ...), for comparing them in one game.
"""

import math
import re
import statistics
import sys

# One statistics line, as nr_dx12.cpp's LogSample writes it. A percentile is a signed EV, or black / below / above
# the histogram's range, or "-" for an empty histogram.
NUM = r"([-+]?[0-9.]+(?:e[-+]?[0-9]+)?|black|<-16|>\+8|<-4|>\+4|-|absent)"
LINE = re.compile(
    r"stats: frame (\d+), (RR|SR) (\d+)x(\d+), (linear HDR|tonemapped)"
    r"(?:: W ([0-9.e+-]+) \(WhiteSource (\w+), pre-exposure ([^,]+), exposure ([^,]+), WhiteEV ([-+0-9.]+)\))?"
    r"; largest channel / W in EV: p1 " + NUM + r", p10 " + NUM + r", p50 " + NUM + r", p90 " + NUM +
    r", p99 " + NUM + r", p99.9 " + NUM + r", max ([-+0-9.]+) \(value ([0-9.e+-]+)\)"
    r"; luminance / W: p50 " + NUM + r", p99 " + NUM +
    r"(?:; Shoulder ([0-9.]+): in it ([0-9.]+)%, compressed >3 EV ([0-9.]+)%)?"
    r"; 0 in 8 bits ([0-9.]+)%, black ([0-9.]+)%, negative ([0-9.]+)%, not finite (\d+)"
    r"(?:; model change x DetailStrength ([0-9.]+) in EV: p1 " + NUM + r", p50 " + NUM + r", p99 " + NUM +
    r", beyond MaxGainEV ([0-9.]+): ([0-9.]+)% / ([0-9.]+)%, colour changed ([0-9.]+)%|; the model did not deliver this frame)"
    r"; settings #(\d+)")
EDGES = {"black": -16.0, "<-16": -16.0, ">+8": 8.0, "<-4": -4.0, ">+4": 4.0}


def value(text):
    if text is None:
        return None
    if text in EDGES:
        return EDGES[text]
    try:
        return float(text)
    except ValueError:
        return None  # "-" (an empty histogram) or "absent"


def describe(name, values, unit=""):
    values = sorted(v for v in values if v is not None and not math.isnan(v))
    if not values:
        return "%s: -" % name
    at = lambda f: values[min(len(values) - 1, int(f * len(values)))]
    return "%s: min %.3g, p10 %.3g, median %.3g, p90 %.3g, max %.3g%s (n=%d)" % (
        name, values[0], at(0.10), statistics.median(values), at(0.90), values[-1], unit, len(values))


def shown(v, form):
    return form % v if v is not None else "-"


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.search(raw)
            if not m:
                if "stats:" in raw:
                    print("not understood:", raw.strip()[:200])
                continue
            g = m.groups()
            rows.append({
                "frame": int(g[0]), "feature": g[1], "size": "%sx%s" % (g[2], g[3]), "linear": g[4] == "linear HDR",
                "W": value(g[5]), "source": g[6], "pre": value(g[7]), "exposure": value(g[8]), "whiteEV": value(g[9]),
                "p1": value(g[10]), "p10": value(g[11]), "p50": value(g[12]), "p90": value(g[13]),
                "p99": value(g[14]), "p999": value(g[15]), "max": value(g[16]), "maxValue": value(g[17]),
                "lum50": value(g[18]), "lum99": value(g[19]), "shoulder": value(g[20]), "inShoulder": value(g[21]),
                "heavy": value(g[22]), "dark": value(g[23]), "black": value(g[24]), "negative": value(g[25]),
                "nonfinite": int(g[26]), "detail": value(g[27]), "gain1": value(g[28]), "gain50": value(g[29]),
                "gain99": value(g[30]), "maxGain": value(g[31]), "low": value(g[32]), "high": value(g[33]),
                "colour": value(g[34]), "generation": int(g[35]),
            })
    return rows


def main(path, bySource):
    rows = load(path)
    print("%s: %d statistics lines" % (path, len(rows)))
    if not rows:
        return
    if not bySource:
        summarize(rows)
        return
    for source in sorted({r["source"] or "-" for r in rows}):
        part = [r for r in rows if (r["source"] or "-") == source]
        print("-- WhiteSource %s: %d statistics lines" % (source, len(part)))
        summarize(part)


def summarize(rows):
    kinds = sorted({"%s %s %s" % (r["feature"], r["size"], "linear HDR" if r["linear"] else "tonemapped") for r in rows})
    print("  frames: %s; white point sources: %s; settings snapshots: %s" % (
        ", ".join(kinds), ", ".join(sorted({r["source"] for r in rows if r["source"]})) or "-",
        ", ".join(str(n) for n in sorted({r["generation"] for r in rows}))))
    for key, name in (("W", "W"), ("pre", "pre-exposure"), ("exposure", "exposure texture"),
                      ("lum50", "luminance / W p50, EV"), ("lum99", "luminance / W p99, EV"),
                      ("p50", "largest channel / W p50, EV"), ("p90", "largest channel / W p90, EV"),
                      ("p99", "largest channel / W p99, EV"), ("p999", "largest channel / W p99.9, EV"),
                      ("max", "largest channel / W max, EV"), ("maxValue", "largest value"),
                      ("inShoulder", "in the shoulder, %"), ("heavy", "compressed > 3 EV, %"),
                      ("dark", "0 in 8 bits, %"), ("negative", "negative, %"), ("gain1", "model change p1, EV"),
                      ("gain50", "model change p50, EV"), ("gain99", "model change p99, EV"),
                      ("low", "clamped at -MaxGainEV, %"), ("high", "clamped at +MaxGainEV, %"),
                      ("colour", "colour changed, %")):
        print("  " + describe(name, [r[key] for r in rows]))
    print("  not finite, all lines together: %d; lines without a model result: %d" % (
        sum(r["nonfinite"] for r in rows), sum(1 for r in rows if r["detail"] is None)))
    # A white point that follows the game's exposure keeps the luminance median where it is as W moves.
    pairs = [(math.log2(r["W"]), r["lum50"]) for r in rows
             if r["W"] and r["W"] > 0 and r["lum50"] is not None and r["lum50"] > -16.0]
    if len(pairs) > 2:
        xs, ys = zip(*pairs)
        if max(xs) - min(xs) > 1e-6:
            mx, my = statistics.mean(xs), statistics.mean(ys)
            slope = sum((x - mx) * (y - my) for x, y in pairs) / sum((x - mx) ** 2 for x in xs)
            print("  W spans %.2f EV; luminance median against log2 W: slope %.2f (0: W follows the scene, "
                  "-1: W moves on its own)" % (max(xs) - min(xs), slope))
        else:
            print("  W never moves")
    print("  timeline (every twentieth line): frame, W, luminance p50, largest p99, in shoulder %, "
          "compressed %, model change p1 / p99")
    for r in rows[::max(1, len(rows) // 20)]:
        print("    %8d  W %-10s lum50 %6s  p99 %6s  shoulder %5s  heavy %5s  change %s / %s" % (
            r["frame"], shown(r["W"], "%.4g"), shown(r["lum50"], "%+.2f"), shown(r["p99"], "%+.2f"),
            shown(r["inShoulder"], "%.2f"), shown(r["heavy"], "%.2f"), shown(r["gain1"], "%+.2f"),
            shown(r["gain99"], "%+.2f")))


if __name__ == "__main__":
    bySource = "--by-source" in sys.argv[1:]
    paths = [a for a in sys.argv[1:] if a != "--by-source"]
    if not paths:
        sys.exit(__doc__)
    for p in paths:
        main(p, bySource)
