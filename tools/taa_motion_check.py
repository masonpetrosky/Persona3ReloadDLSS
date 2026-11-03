"""Checks DLSS's inputs against the next frame's image, from the dumps of tools/rdc_taa_inputs.py.

The scene color is rendered with the camera jitter, so a surface that sits at unjittered pixel position s is drawn
at s + k * jitter, where the jitter is in pixels as the add-on hands it to DLSS (x right, y down, from
View.TemporalAAJitter) and k is +1 or -1 depending on the convention. A pixel p of the later frame B therefore
shows what the earlier frame A shows at p + mv + k * (jitter A - jitter B), with mv the motion vector the add-on makes
(src/motion_vectors.hlsl: the previous position minus this one, in pixels, x right and y down).

1. Static pixels (no velocity written; the camera did not move in these captures) find k: the sign whose shift
   matches the earlier image best.
2. Pixels with velocity inside a rectangle (such as a ceiling fan) then test the motion vectors: as made, and wrong
   variants (none, mirrored, half, double). The right one has the smallest color error.

Also prints the stencil values of the whole frame, to show the dump holds the stencil plane.

Usage: python tools/taa_motion_check.py <dump dir> <earlier capture> <later capture>
           [--rect x0 y0 x1 y1] [--jitter ax ay bx by]
"""
import argparse
import json
import os

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("folder")
parser.add_argument("earlier")
parser.add_argument("later")
parser.add_argument("--rect", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"))
parser.add_argument("--jitter", type=float, nargs=4, default=[0, 0, 0, 0], metavar=("AX", "AY", "BX", "BY"),
                    help="each frame's jitter in pixels, x right and y down, as the add-on computes it")
args = parser.parse_args()


def load(name, slot):
    meta = json.load(open(os.path.join(args.folder, name + ".json")))["inputs"][slot]
    raw = np.fromfile(os.path.join(args.folder, "%s_%s.bin" % (name, slot)), dtype=np.uint8)
    per_pixel = meta["bytes"] // (meta["width"] * meta["height"])
    return raw.reshape(meta["height"], meta["width"], per_pixel)


def luma(color):
    rgb = color.view(np.float16).astype(np.float32)[..., :3]
    return rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def sample(image, x, y):
    """Bilinear sample of a 2D image at pixel positions (pixel centers at integer + 0.5)."""
    h, w = image.shape
    fx, fy = x - 0.5, y - 0.5
    x0 = np.clip(np.floor(fx).astype(np.int64), 0, w - 2)
    y0 = np.clip(np.floor(fy).astype(np.int64), 0, h - 2)
    tx = np.clip(fx - x0, 0.0, 1.0)
    ty = np.clip(fy - y0, 0.0, 1.0)
    top = image[y0, x0] * (1 - tx) + image[y0, x0 + 1] * tx
    bottom = image[y0 + 1, x0] * (1 - tx) + image[y0 + 1, x0 + 1] * tx
    return top * (1 - ty) + bottom * ty


def errors(prev, cur, xs, ys, dx, dy):
    """Color error of each pixel against the earlier image at the shifted position, relative to the median luma."""
    here = cur[ys, xs]
    there = sample(prev, xs + 0.5 + dx, ys + 0.5 + dy)
    return np.abs(there - here) / (np.median(here) + 1e-6)


vel = load(args.later, "t3").view(np.uint16).astype(np.float32) / 65535.0
height, width = vel.shape[:2]
x0, y0, x1, y1 = args.rect if args.rect else (0, 0, width, height)

ds = load(args.later, "t2")
values, counts = np.unique(ds[..., 4], return_counts=True)
print("whole-frame stencil values (byte 4 of 8):", {int(a): int(b) for a, b in zip(values, counts)})

cur = luma(load(args.later, "t1"))
prev = luma(load(args.earlier, "t1"))
ax, ay, bx, by = args.jitter
jdx, jdy = ax - bx, ay - by

# 1. The jitter's sign, from static pixels with some detail (where a half-pixel shift changes the color).
written = vel[..., 0] > 0
gx = np.abs(np.diff(cur, axis=1, prepend=cur[:, :1]))
gy = np.abs(np.diff(cur, axis=0, prepend=cur[:1, :]))
detail = (gx + gy) > 0.25 * (cur + 1e-3)
ys, xs = np.nonzero(~written & detail)
ys, xs = ys[4:-4], xs[4:-4]
keep = (xs > 4) & (xs < width - 5) & (ys > 4) & (ys < height - 5)
ys, xs = ys[keep], xs[keep]
print("%s -> %s: jitter A (%+.3f, %+.3f) B (%+.3f, %+.3f) px; %d static pixels with detail" % (
    args.earlier, args.later, ax, ay, bx, by, len(xs)))
best_k, best = 0, None
for k in (+1, 0, -1):
    e = errors(prev, cur, xs, ys, k * jdx, k * jdy)
    print("  static, shift %+d x jitter difference: median color error %.4f, mean %.4f" % (k, np.median(e), e.mean()))
    if best is None or np.median(e) < best:
        best_k, best = k, np.median(e)
print("  -> the image moves by %+d x the jitter" % best_k)

# 2. The motion vectors where an object wrote velocity.
ys, xs = np.nonzero(written[y0:y1, x0:x1])
ys, xs = ys + y0, xs + x0
v = (vel[ys, xs, :2] - 32767.0 / 65535.0) / (0.499 * 0.5)  # screen position now minus before, clip units, y up
mv = np.stack([-v[:, 0] * width / 2, v[:, 1] * height / 2], axis=1)  # before minus now, pixels, y down
speed = np.hypot(mv[:, 0], mv[:, 1])
print("%d,%d-%d,%d: %d pixels with velocity, motion median %.2f px, max %.2f px" % (
    x0, y0, x1, y1, len(xs), np.median(speed) if len(xs) else 0.0, speed.max() if len(xs) else 0.0))
if len(xs):
    variants = [("as made", 1, 1, 1.0), ("none", 0, 0, 1.0), ("x mirrored", -1, 1, 1.0), ("y mirrored", 1, -1, 1.0),
                ("both mirrored", -1, -1, 1.0), ("half", 1, 1, 0.5), ("double", 1, 1, 2.0)]
    for name, sx, sy, k in variants:
        e = errors(prev, cur, xs, ys, sx * k * mv[:, 0] + best_k * jdx, sy * k * mv[:, 1] + best_k * jdy)
        print("  %-13s median color error %.4f, mean %.4f" % (name, np.median(e), e.mean()))
