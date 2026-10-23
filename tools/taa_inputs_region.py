"""Velocity and stencil statistics of the main TAA inputs over a screen rectangle (such as a ceiling fan), from the
dumps of tools/rdc_taa_inputs.py. Usage: python tools/taa_inputs_region.py <dump dir> <capture name> x0 y0 x1 y1
"""
import json
import os
import sys

import numpy as np

folder, name = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
info = json.load(open(os.path.join(folder, name + ".json")))
inputs = info["inputs"]


def load(slot):
    meta = inputs[slot]
    raw = np.fromfile(os.path.join(folder, "%s_%s.bin" % (name, slot)), dtype=np.uint8)
    per_pixel = meta["bytes"] // (meta["width"] * meta["height"])
    return meta, raw.reshape(meta["height"], meta["width"], per_pixel)


vel_meta, vel = load("t3")
vel = vel.view(np.uint16).astype(np.float32) / 65535.0  # R16G16B16A16_UNORM
ds_meta, ds = load("t2")
print("velocity", vel_meta["format"], vel.shape, "depth", ds_meta["format"], ds.shape)
depth = ds[..., 0:4].copy().view(np.float32)[..., 0]
stencil = ds[..., 4] if ds.shape[2] >= 5 else None

region = (slice(y0, y1), slice(x0, x1))
v = vel[region]
written = v[..., 0] > 0
print("region %d,%d-%d,%d: %d pixels, velocity written on %.1f%%" % (x0, y0, x1, y1, written.size,
                                                                       100.0 * written.mean()))
if written.any():
    enc = v[written][:, :2]
    dec = (enc - 32767.0 / 65535.0) / (0.499 * 0.5)
    px = np.stack([-dec[:, 0] * 3840 / 2, dec[:, 1] * 2160 / 2], axis=1)
    mag = np.hypot(px[:, 0], px[:, 1])
    print("  written velocity in pixels: median %.2f, 90%% %.2f, max %.2f" % (np.median(mag), np.percentile(mag, 90),
                                                                             mag.max()))
if stencil is not None:
    s = stencil[region]
    values, counts = np.unique(s, return_counts=True)
    print("  stencil values:", {int(a): int(b) for a, b in zip(values, counts)})
    print("  responsive AA bit (8) on %.1f%%" % (100.0 * ((s & 8) != 0).mean()))
d = depth[region]
print("  depth min %.6g max %.6g" % (d.min(), d.max()))

# A coarse map: one character per 16x16 block. '#' velocity written, 'R' responsive AA only, '+' both, '.' neither.
for by in range(y0, y1, 16):
    row = ""
    for bx in range(x0, x1, 16):
        w = (vel[by:by + 16, bx:bx + 16, 0] > 0).mean() > 0.25
        r = stencil is not None and ((stencil[by:by + 16, bx:bx + 16] & 8) != 0).mean() > 0.25
        row += "+" if w and r else "#" if w else "R" if r else "."
    print(row)
