"""Converts ReShade's HDR10 screenshots (16-bit PNG, cICP BT.2020 + PQ) to SDR sRGB for web pages.

  python tools/hdr_screenshot_to_sdr.py <out folder> <png> [<png> ...] [--white 203] [--peak 1000]

PQ to nits, BT.2020 to BT.709 primaries, then nits / --white (RenoDX's ToneMapGameNits, the paper white that becomes
SDR white) with an extended-Reinhard shoulder on luminance that reaches 1.0 at --peak nits (RenoDX's
ToneMapPeakNits), so highlights roll off instead of clipping; colors keep their ratios; sRGB curve, 8-bit PNG. The
same mapping for every file keeps A/B comparisons fair.
"""
import argparse
import os

import cv2
import numpy as np

M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
BT2020_TO_BT709 = np.array([[1.6605, -0.5876, -0.0728],
                            [-0.1246, 1.1329, -0.0083],
                            [-0.0182, -0.1006, 1.1187]], dtype=np.float32)


def pq_to_nits(e):
    p = np.power(np.clip(e, 0, 1), 1 / M2)
    return 10000 * np.power(np.maximum(p - C1, 0) / (C2 - C3 * p), 1 / M1)


def to_srgb8(linear):
    linear = np.clip(linear, 0, 1)
    srgb = np.where(linear <= 0.0031308, 12.92 * linear, 1.055 * np.power(linear, 1 / 2.4) - 0.055)
    return np.round(srgb * 255).astype(np.uint8)


def convert(path, white, peak):
    bgr = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if bgr is None or bgr.dtype != np.uint16:
        raise SystemExit("%s: not a 16-bit PNG" % path)
    rgb2020 = pq_to_nits(bgr[..., ::-1].astype(np.float32) / 65535)
    rgb = np.maximum(rgb2020 @ BT2020_TO_BT709.T, 0) / white
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    lw = peak / white
    mapped = lum * (1 + lum / (lw * lw)) / (1 + lum)
    scale = np.where(lum > 1e-6, mapped / np.maximum(lum, 1e-6), 1)
    return to_srgb8(rgb * scale[..., None])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("pngs", nargs="+")
    parser.add_argument("--white", type=float, default=203)
    parser.add_argument("--peak", type=float, default=1000)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for path in args.pngs:
        sdr = convert(path, args.white, args.peak)
        out = os.path.join(args.out, os.path.splitext(os.path.basename(path))[0] + " sdr.png")
        cv2.imwrite(out, sdr[..., ::-1])
        print(out)


main()
