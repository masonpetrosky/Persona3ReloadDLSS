"""Strength of a 4-pixel-periodic pattern (an ordered dither or an aliased screentone) in a patch of the add-on's
development frame dumps: the patch's pixels averaged per (x mod 4, y mod 4) phase, the 4x4 cell's spread over the
patch's mean luminance (0 = no such pattern).

  python tools/skin_pattern.py <frames dir> X Y W H [frame ...]
"""
import json
import os
import re
import sys

import numpy as np

d, x, y, w, h = sys.argv[1], *map(int, sys.argv[2:6])
want = set(map(int, sys.argv[6:]))
meta = {json.loads(l)["frame"]: json.loads(l) for l in open(os.path.join(d, "frames.jsonl")) if l.strip()}
for name in sorted(os.listdir(d)):
    m = re.match(r"(\d+)_color_(\d+)x(\d+)\.bin$", name)
    if not m or (want and int(m.group(1)) not in want):
        continue
    f, cw, ch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    c = np.fromfile(os.path.join(d, name), dtype=np.float16).reshape(ch, cw, 4).astype(np.float32)
    lum = c[y:y + h, x:x + w, :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    yy, xx = np.mgrid[y:y + h, x:x + w]
    cell = np.array([[lum[(yy % 4 == j) & (xx % 4 == i)].mean() for i in range(4)] for j in range(4)])
    print("frame %d jitter %s: 4x4 pattern %.1f%% of the mean" % (f, meta.get(f, {}).get("jitter"),
                                                                 100 * cell.std() / lum.mean()))
