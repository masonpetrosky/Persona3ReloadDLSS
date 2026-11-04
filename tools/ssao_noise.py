"""Compares the SSAO noise modes in the add-on's development dumps (DevDumps=1, Persona3ReloadDLSS\\dev\\frames).

  python tools/ssao_noise.py <frames dir> <out dir> [--crop X Y W H] [--zoom N]

Frames go in groups by the SSAO noise they had (frames.jsonl: ssao_blue_noise and ssao_blue; test builds' dumps have
ssao_noise 0, 1 for a random step or 2 for blue noise). For every SSAO pass output
(<frame>_ao_<w>x<h>.bin, R8 at the render size) it prints, for the high-passed AO (each pixel minus a blur): the share
of its power at the game's 4x4 tile frequencies (multiples of 1/4 cycle a pixel, the grid), the share below 1/8 cycle
a pixel (clumps), and how much the AO changes from a frame to the next one. With --crop (render pixels) it writes per
group a sheet of the AO crops and of the DLSS output crops (the same area at the output size), gray and contrast
stretched, and prints the output crops' grid share (the tile's frequencies scaled to the output size).
"""
import argparse
import json
import os
import re
from collections import defaultdict

import cv2
import numpy as np


def high_pass(img, sigma=2.0):
    return img - cv2.GaussianBlur(img, (0, 0), sigma)


def shares(res, period):
    """The power share at the frequencies of a `period`-pixel tile, and below 1/8 cycle a pixel."""
    h, w = res.shape
    h, w = h - h % period, w - w % period
    res = res[:h, :w]
    power = np.abs(np.fft.fft2(res - res.mean())) ** 2
    fy = np.fft.fftfreq(h)[:, None] * period
    fx = np.fft.fftfreq(w)[None, :] * period
    grid = (np.abs(fy - np.round(fy)) < 1e-6) & (np.abs(fx - np.round(fx)) < 1e-6)
    grid[0, 0] = False
    r = np.hypot(np.fft.fftfreq(h)[:, None], np.fft.fftfreq(w)[None, :])
    low = (r > 0) & (r < 0.125)
    total = power.sum()
    return power[grid].sum() / total, power[low].sum() / total


def gray_sheet(tiles, zoom):
    """Tiles side by side, each stretched to its 1st..99th percentile, enlarged without smoothing."""
    out = []
    for t in tiles:
        lo, hi = np.percentile(t, 1), np.percentile(t, 99)
        g = np.clip((t - lo) / max(hi - lo, 1e-6) * 255, 0, 255).astype(np.uint8)
        g = cv2.resize(g, (g.shape[1] * zoom, g.shape[0] * zoom), interpolation=cv2.INTER_NEAREST)
        out += [g, np.full((g.shape[0], 6), 255, np.uint8)]
    return np.concatenate(out[:-1], axis=1)


def label(m):
    if "ssao_blue_noise" in m:
        return ("blue noise" if m.get("ssao_blue") else "blue noise (not active)") if m["ssao_blue_noise"] else "game"
    if m.get("ssao_noise") == 2:  # test builds with three modes
        return "blue noise" if m.get("ssao_blue") else "blue noise (not active)"
    return {0: "game", 1: "random step"}.get(m.get("ssao_noise"), "?")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("frames")
    parser.add_argument("out")
    parser.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "W", "H"))
    parser.add_argument("--zoom", type=int, default=4)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    meta = {}
    for line in open(os.path.join(args.frames, "frames.jsonl"), encoding="utf-8"):
        if line.strip():
            m = json.loads(line)
            meta[m["frame"]] = m
    files = defaultdict(dict)
    for name in os.listdir(args.frames):
        m = re.match(r"(\d+)_(color|output|ao)_(\d+)x(\d+)\.bin$", name)
        if m:
            files[int(m.group(1))][m.group(2)] = (os.path.join(args.frames, name), int(m.group(3)), int(m.group(4)))
    groups = defaultdict(list)
    for f in sorted(files):
        if f in meta and "ao" in files[f]:
            groups[label(meta[f])].append(f)
    lum = np.array([0.2126, 0.7152, 0.0722], np.float32)
    for name, frames in groups.items():
        print("\n%s: %d frames" % (name, len(frames)))
        prev = None
        ao_tiles, out_tiles = [], []
        for f in frames:
            path, w, h = files[f]["ao"]
            ao = np.fromfile(path, np.uint8).reshape(h, w).astype(np.float32) / 255.0
            res = high_pass(ao)
            grid, low = shares(res, 4)
            change = ""
            if prev is not None and prev[0] == f - 1:
                change = ", change from the frame before %.4f" % np.sqrt(np.mean((ao - prev[1]) ** 2))
            prev = (f, ao)
            step = meta[f].get("ssao_step")
            print("  frame %d (step %s, taa index %s): AO grid share %.2f%%, low %.2f%%%s" % (
                f, step, meta[f].get("taa_params", [None])[0], 100 * grid, 100 * low, change))
            if args.crop:
                x, y, cw, ch = args.crop
                ao_tiles.append(ao[y:y + ch, x:x + cw])
                cg, cl = shares(high_pass(ao)[y:y + ch, x:x + cw], 4)
                print("    AO crop grid share %.2f%%, low %.2f%%, residual RMS %.4f" % (
                    100 * cg, 100 * cl, high_pass(ao)[y:y + ch, x:x + cw].std()))
                if "output" in files[f]:
                    opath, ow, oh = files[f]["output"]
                    out = np.fromfile(opath, np.float16).reshape(oh, ow, 4).astype(np.float32)
                    s = ow / float(w)
                    ox, oy, ocw, och = (int(round(v * s)) for v in (x, y, cw, ch))
                    o = out[oy:oy + och, ox:ox + ocw, :3] @ lum
                    o = o / (1 + o)
                    og, _ = shares(high_pass(o, 2.0 * s), int(round(4 * s)))
                    print("    output crop grid share %.2f%%" % (100 * og))
                    out_tiles.append(o)
        if args.crop and ao_tiles:
            slug = name.replace(" ", "_").replace("(", "").replace(")", "")
            cv2.imwrite(os.path.join(args.out, "ao_%s.png" % slug), gray_sheet(ao_tiles, args.zoom))
            if out_tiles:
                z = max(1, int(round(args.zoom * ao_tiles[0].shape[1] / out_tiles[0].shape[1])))
                cv2.imwrite(os.path.join(args.out, "output_%s.png" % slug), gray_sheet(out_tiles, z))


main()
