"""Summarizes a PresentMon 2.x CSV of the game: frame rate, frame time, GPU busy time, CPU busy time and the present
mode, and whether the GPU is the limit (GPU busy close to the frame time).

Usage: python tools/pm_summary.py <presentmon.csv> [more csv files]
"""
import csv
import statistics
import sys


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100.0 * len(values)))] if values else float("nan")


for path in sys.argv[1:]:
    rows = list(csv.DictReader(open(path, encoding="utf-8-sig")))
    rows = [r for r in rows if r.get("Application", "").lower().startswith("p3r")]
    if not rows:
        print(path, ": no P3R frames")
        continue
    frame = [float(r["MsBetweenPresents"]) for r in rows if r["MsBetweenPresents"] not in ("", "NA")]
    gpu = [float(r["MsGPUBusy"]) for r in rows if r["MsGPUBusy"] not in ("", "NA")]
    cpu = [float(r["MsCPUBusy"]) for r in rows if r["MsCPUBusy"] not in ("", "NA")]
    modes = sorted({r["PresentMode"] for r in rows})
    sync = sorted({r["SyncInterval"] for r in rows})
    tearing = sorted({r["AllowsTearing"] for r in rows})
    med = statistics.median(frame)
    gpu_med = statistics.median(gpu) if gpu else float("nan")
    print("%s: %d frames over %.1f s" % (path, len(rows), sum(frame) / 1000.0))
    print("  fps: median %.1f (frame time median %.2f ms, 95th %.2f ms, 99th %.2f ms)" % (
        1000.0 / med, med, pct(frame, 95), pct(frame, 99)))
    print("  GPU busy: median %.2f ms (%.0f%% of the frame time); CPU busy: median %.2f ms" % (
        gpu_med, 100.0 * gpu_med / med, statistics.median(cpu) if cpu else float("nan")))
    print("  present mode %s, sync interval %s, allows tearing %s" % (", ".join(modes), ", ".join(sync),
                                                                     ", ".join(tearing)))
    print("  -> %s" % ("GPU-bound (GPU busy >= 90% of the frame time)" if gpu_med >= 0.9 * med else
                       "not GPU-bound: the CPU side or a frame rate limit sets the pace"))
