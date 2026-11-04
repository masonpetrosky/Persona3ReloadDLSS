"""Takes development dumps with blue-noise SSAO off and on in one go (the add-on with DevDumps=1, DLSS running).

  python tools/ssao_session.py [--modes 0 1 0 1] [--dumps 3] [--save DIR] [--end-mode 1]

For each mode (0 the game's noise, 1 blue noise) it writes dev\\ssao.txt (the add-on switches within 30 frames),
waits for DLSS's history to settle, then asks for --dumps pairs of consecutive frames (dev\\dump.txt holding 2), about
a second apart, and waits until the add-on has written each pair. At the end the add-on gets --end-mode, and --save
copies dev\\frames to DIR. tools/ssao_noise.py compares the modes.
"""
import argparse
import glob
import os
import shutil
import time

DEV = r"C:\Program Files (x86)\Steam\steamapps\common\P3R\P3R\Binaries\Win64\Persona3ReloadDLSS\dev"


def trigger(path, text, timeout=10.0):
    with open(path, "w") as f:
        f.write(text)
    end = time.time() + timeout
    while os.path.exists(path):
        if time.time() > end:
            raise SystemExit("the add-on did not take %s (is the game running with DevDumps=1?)" % path)
        time.sleep(0.05)


def outputs(frames):
    """DLSS outputs written so far (one a dumped frame)."""
    return len(glob.glob(os.path.join(frames, "*_output_*.bin")))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--modes", type=int, nargs="+", default=[0, 1, 0, 1])
    parser.add_argument("--dumps", type=int, default=3)
    parser.add_argument("--settle", type=float, default=1.5)
    parser.add_argument("--save")
    parser.add_argument("--end-mode", type=int, default=1)
    parser.add_argument("--dev", default=DEV)
    args = parser.parse_args()
    frames = os.path.join(args.dev, "frames")
    for mode in args.modes:
        trigger(os.path.join(args.dev, "ssao.txt"), str(mode))
        time.sleep(args.settle)
        for _ in range(args.dumps):
            before = outputs(frames)
            trigger(os.path.join(args.dev, "dump.txt"), "2")
            end = time.time() + 15
            while outputs(frames) < before + 2:
                if time.time() > end:
                    raise SystemExit("the dump did not arrive (is DLSS running?)")
                time.sleep(0.1)
            time.sleep(0.8)
        print("mode %d: %d pairs" % (mode, args.dumps))
    trigger(os.path.join(args.dev, "ssao.txt"), str(args.end_mode))
    if args.save:
        time.sleep(1.0)  # the last pair's files are complete a few frames after the first appears
        shutil.copytree(frames, args.save, dirs_exist_ok=True)
        print("saved", args.save)


main()
