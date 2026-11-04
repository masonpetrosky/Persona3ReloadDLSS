"""Looks at the add-on's development frame dumps (DevDumps=1: Persona3ReloadDLSS\\dev\\frames).

  python tools/dev_frames.py <frames dir> <out dir> [--crop X Y W H] [--zoom N]

Writes for every dumped frame a preview of the TAA input color (render size) and of DLSS's output (simple Reinhard
on the pre-exposed values, sRGB), and with --crop (render pixels; the output crop is the same area scaled to the
output size): a sheet of the crops of all frames zoomed --zoom times (input above, output below), a sheet of their
high-pass residuals (each pixel minus a blur, gray = 0), and per frame the strongest periods of the input crop's
residual (FFT) and how much each frame's residual pattern matches the first frame's (correlation: near 1 = a still
pattern, near 0 = one that changes every frame).
"""
import argparse
import json
import os
import re

import cv2
import numpy as np


def load(path, width, height):
    return np.fromfile(path, dtype=np.float16).reshape(height, width, 4).astype(np.float32)


def preview(rgba):
    rgb = np.maximum(rgba[..., :3], 0)
    rgb = rgb / (1 + rgb)
    srgb = np.where(rgb <= 0.0031308, 12.92 * rgb, 1.055 * np.power(rgb, 1 / 2.4) - 0.055)
    return np.round(np.clip(srgb, 0, 1) * 255).astype(np.uint8)[..., ::-1]


def residual(rgba):
    lum = rgba[..., :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    lum = lum / (1 + lum)
    return lum - cv2.GaussianBlur(lum, (0, 0), 2.0)


def periods(res, count=4):
    h, w = res.shape
    window = np.outer(np.hanning(h), np.hanning(w))
    power = np.abs(np.fft.fftshift(np.fft.fft2(res * window))) ** 2
    power[h // 2, w // 2] = 0
    out = []
    for k in np.argsort(power.ravel())[::-1]:
        fy, fx = (k // w - h // 2) / h, (k % w - w // 2) / w
        if fy < 0 or (fy == 0 and fx < 0):
            continue  # the spectrum is symmetric: keep one of each pair
        out.append("(%.3f, %.3f)/px" % (fx, fy))
        if len(out) == count:
            break
    return out


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
    files = {}
    for name in os.listdir(args.frames):
        m = re.match(r"(\d+)_(color|output)_(\d+)x(\d+)\.bin$", name)
        if m:
            files.setdefault(int(m.group(1)), {})[m.group(2)] = (os.path.join(args.frames, name), int(m.group(3)),
                                                                 int(m.group(4)))
    frames = sorted(files)
    inputs, outputs = [], []
    for f in frames:
        info = meta.get(f, {})
        color = load(*files[f]["color"])
        output = load(*files[f]["output"]) if "output" in files[f] else None
        cv2.imwrite(os.path.join(args.out, "%06d_color.png" % f), preview(color))
        if output is not None:
            cv2.imwrite(os.path.join(args.out, "%06d_output.png" % f), preview(output))
        print("frame %d: render %s, jitter %s, phases %s, reset %s" % (f, info.get("render"), info.get("jitter"),
                                                                        info.get("phases"), info.get("reset")))
        inputs.append(color)
        outputs.append(output)
    if not args.crop:
        return
    x, y, w, h = args.crop
    scale = outputs[0].shape[1] / float(inputs[0].shape[1]) if outputs[0] is not None else 1.0
    ox, oy, ow, oh = (int(round(v * scale)) for v in (x, y, w, h))
    z = args.zoom
    tiles_in, tiles_out, tiles_res = [], [], []
    first = None
    for f, color, output in zip(frames, inputs, outputs):
        crop = color[y:y + h, x:x + w]
        res = residual(crop)
        if first is None:
            first = res
        corr = float(np.corrcoef(first.ravel(), res.ravel())[0, 1])
        print("frame %d crop: residual rms %.4f, periods %s, pattern match with the first frame %.2f"
              % (f, res.std(), ", ".join(periods(res)), corr))
        tiles_in.append(cv2.resize(preview(crop), (w * z, h * z), interpolation=cv2.INTER_NEAREST))
        shown = np.clip(128 + res / (4 * max(first.std(), 1e-6)) * 64, 0, 255).astype(np.uint8)
        tiles_res.append(cv2.resize(cv2.cvtColor(shown, cv2.COLOR_GRAY2BGR), (w * z, h * z),
                                    interpolation=cv2.INTER_NEAREST))
        if output is not None:
            ocrop = output[oy:oy + oh, ox:ox + ow]
            tiles_out.append(cv2.resize(preview(ocrop), (w * z, h * z), interpolation=cv2.INTER_NEAREST))
    sep = lambda t: np.full((t.shape[0], 6, 3), 255, np.uint8)
    rows = [np.concatenate([np.concatenate([t, sep(t)], 1) for t in tiles], 1) for tiles in
            (tiles_in, tiles_out) if tiles]
    cv2.imwrite(os.path.join(args.out, "crops.png"), np.concatenate(rows, 0))
    cv2.imwrite(os.path.join(args.out, "residuals.png"),
                np.concatenate([np.concatenate([t, sep(t)], 1) for t in tiles_res], 1))


main()
