"""Checks DLSS's inputs against the next frame's image, from the dumps of tools/rdc_taa_inputs.py.

The scene color is rendered with the camera jitter, so a surface that sits at unjittered pixel position s is drawn
at s + k * jitter, where the jitter is in pixels as the add-on hands it to DLSS (x right, y down, from
View.TemporalAAJitter) and k is +1 or -1 depending on the convention. A pixel p of the later frame B therefore
shows what the earlier frame A shows at p + mv + k * (jitter A - jitter B), with mv the motion vector the add-on makes
(src/motion_vectors.hlsl: the previous position minus this one, in pixels, x right and y down): the velocity where an
object wrote it, otherwise camera motion from depth and View.ClipToPrevClip.

1. Pixels without velocity and with some detail find k and test the camera motion: as made (from depth and
   ClipToPrevClip, as the shader computes it), none, mirrored, half and double.
2. Pixels with velocity inside a rectangle (such as a ceiling fan) test the velocity the same way.
The right variant has the smallest color error. Also prints the stencil values of the whole frame.

The jitter and ClipToPrevClip come from the dumps' .json (rdc_taa_inputs.py keeps the View values); --jitter
overrides the jitter for older dumps without them.

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
parser.add_argument("--jitter", type=float, nargs=4, metavar=("AX", "AY", "BX", "BY"),
                    help="each frame's jitter in pixels, x right and y down, as the add-on computes it")
args = parser.parse_args()


def info(name):
    return json.load(open(os.path.join(args.folder, name + ".json")))


def load(name, slot):
    meta = info(name)["inputs"][slot]
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
    """Color error of each pixel against the earlier image at the shifted position, relative to the median luma.
    Positions that land outside the image are left out (NaN)."""
    here = cur[ys, xs]
    px, py = xs + 0.5 + dx, ys + 0.5 + dy
    there = sample(prev, px, py)
    e = np.abs(there - here) / (np.median(here) + 1e-6)
    h, w = prev.shape
    e[(px < 1) | (py < 1) | (px > w - 1) | (py > h - 1)] = np.nan
    return e


def report(label, e):
    print("  %-34s median color error %.4f, mean %.4f" % (label, np.nanmedian(e), np.nanmean(e)))
    return np.nanmedian(e)


vel = load(args.later, "t3").view(np.uint16).astype(np.float32) / 65535.0
height, width = vel.shape[:2]
x0, y0, x1, y1 = args.rect if args.rect else (0, 0, width, height)

ds = load(args.later, "t2")
depth = ds[..., 0:4].copy().view(np.float32)[..., 0]
values, counts = np.unique(ds[..., 4], return_counts=True)
print("whole-frame stencil values (byte 4 of 8):", {int(a): int(b) for a, b in zip(values, counts)})

view_a, view_b = info(args.earlier).get("view"), info(args.later).get("view")
if args.jitter:
    ax, ay, bx, by = args.jitter
elif view_a and view_b:
    ja, jb = view_a["temporal_aa_jitter"], view_b["temporal_aa_jitter"]
    ax, ay = ja[0] * width / 2, -ja[1] * height / 2
    bx, by = jb[0] * width / 2, -jb[1] * height / 2
else:
    ax = ay = bx = by = 0.0
jdx, jdy = ax - bx, ay - by
cur = luma(load(args.later, "t1"))
prev = luma(load(args.earlier, "t1"))
print("%s -> %s: jitter A (%+.3f, %+.3f) B (%+.3f, %+.3f) px" % (args.earlier, args.later, ax, ay, bx, by))


def camera_motion(xs, ys):
    """motion_vectors.hlsl's camera motion: screen position from the pixel center, device depth, ClipToPrevClip."""
    m = np.array(view_b["clip_to_prev_clip"], dtype=np.float64).reshape(4, 4)  # Unreal's rows
    size = view_b["view_size_and_inv_size"]
    sx = (xs + 0.5) * size[2] * 2.0 - 1.0
    sy = 1.0 - (ys + 0.5) * size[3] * 2.0
    d = depth[ys, xs].astype(np.float64)
    p = np.outer(sx, m[0]) + np.outer(sy, m[1]) + np.outer(d, m[2]) + m[3]
    vx, vy = sx - p[:, 0] / p[:, 3], sy - p[:, 1] / p[:, 3]
    return -vx * size[0] / 2, vy * size[1] / 2  # before minus now, pixels, y down


# 1. Pixels without velocity and with some detail (where a sub-pixel shift changes the color).
written = vel[..., 0] > 0
gx = np.abs(np.diff(cur, axis=1, prepend=cur[:, :1]))
gy = np.abs(np.diff(cur, axis=0, prepend=cur[:1, :]))
detail = (gx + gy) > 0.25 * (cur + 1e-3)
ys, xs = np.nonzero(~written & detail)
keep = (xs > 4) & (xs < width - 5) & (ys > 4) & (ys < height - 5)
ys, xs = ys[keep], xs[keep]
cx, cy = camera_motion(xs, ys) if view_b else (np.zeros(len(xs)), np.zeros(len(xs)))
speed = np.hypot(cx, cy)
print("%d pixels without velocity, with detail; camera motion median %.2f px, 90%% %.2f px, max %.2f px" % (
    len(xs), np.median(speed), np.percentile(speed, 90), speed.max()))
best_k, best = 0, None
for k in (+1, 0, -1):
    m = report("camera motion, %+d x jitter difference" % k, errors(prev, cur, xs, ys, cx + k * jdx, cy + k * jdy))
    if best is None or m < best:
        best_k, best = k, m
print("  -> the image moves by %+d x the jitter" % best_k)
if view_b:
    for label, sx, sy, k in (("no camera motion", 0, 0, 1.0), ("camera x mirrored", -1, 1, 1.0),
                             ("camera y mirrored", 1, -1, 1.0), ("camera half", 1, 1, 0.5),
                             ("camera double", 1, 1, 2.0)):
        report(label, errors(prev, cur, xs, ys, sx * k * cx + best_k * jdx, sy * k * cy + best_k * jdy))

# 2. The velocity, where an object wrote it.
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
        report(name, errors(prev, cur, xs, ys, sx * k * mv[:, 0] + best_k * jdx, sy * k * mv[:, 1] + best_k * jdy))
