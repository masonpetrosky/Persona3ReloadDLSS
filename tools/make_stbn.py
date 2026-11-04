"""Makes the spatiotemporal blue noise the add-on gives Unreal's SSAO (src/stbn_64x64x64.bin).

  python tools/make_stbn.py [--out src/stbn_64x64x64.bin] [--seed N] [--check DIR]

64 x 64 pixels x 64 frames of scalar values 0..255, x fastest, then y, then the frame. Made with void and cluster
(Ulichney 1993) over the whole volume with the energy of Wolfe et al., "Spatiotemporal Blue Noise Masks" (EGSR 2022):
a pixel only repels the pixels of its own frame (a Gaussian of their toroidal distance) and itself in the other frames
(a Gaussian of the toroidal frame distance). Every frame is then a 2D blue noise mask and every pixel's values over
time a 1D blue noise sequence. The ranks become values frame by frame, so each frame holds each value 16 times.

--check DIR (with or without --out) writes pictures of the noise and its spectra to DIR and prints how its spectra
and its average over frames compare with white noise and with the noise Unreal's SSAO uses (see check()).
"""
import argparse
import os
import time

import numpy as np

W = 64            # pixels a side
T = 64            # frames
N = W * W * T
SIGMA = 1.9       # the energy's Gaussians, in pixels and in frames (Wolfe et al. use 1.9 for both)


class Field:
    """A binary volume and its energy: each 1 adds a Gaussian to its frame and to its own pixel in other frames."""

    def __init__(self):
        d = np.minimum(np.arange(W), W - np.arange(W)).astype(np.float64)
        ks = np.exp(-(d[:, None] ** 2 + d[None, :] ** 2) / (2 * SIGMA ** 2))
        dt = np.minimum(np.arange(T), T - np.arange(T)).astype(np.float64)
        kt = np.exp(-dt ** 2 / (2 * SIGMA ** 2))
        kt[0] = 0.0  # the pixel itself counts once, in its own frame
        # Tiled twice, so the kernel around any pixel of the torus is a plain slice.
        self.ks2 = np.tile(ks, (2, 2)).astype(np.float32)
        self.kt2 = np.tile(kt, 2).astype(np.float32)
        self.energy = np.zeros((T, W, W), np.float32)
        self.bits = np.zeros((T, W, W), bool)
        self.at_ones = np.full((T, W, W), -np.inf, np.float32)  # the energy where a 1 is, -inf elsewhere
        self.at_zeros = np.zeros((T, W, W), np.float32)         # the energy where a 0 is, +inf elsewhere

    def copy(self):
        f = Field.__new__(Field)
        f.ks2, f.kt2 = self.ks2, self.kt2
        for name in ("energy", "bits", "at_ones", "at_zeros"):
            setattr(f, name, getattr(self, name).copy())
        return f

    def set(self, index, value):
        t, y, x = np.unravel_index(index, (T, W, W))
        s = np.float32(1.0 if value else -1.0)
        a, b, c = (-y) % W, (-x) % W, (-t) % T
        self.energy[t] += s * self.ks2[a:a + W, b:b + W]
        self.energy[:, y, x] += s * self.kt2[c:c + T]
        self.bits[t, y, x] = value
        bits, e = self.bits[t], self.energy[t]
        self.at_ones[t] = np.where(bits, e, -np.inf)
        self.at_zeros[t] = np.where(bits, np.inf, e)
        bits, e = self.bits[:, y, x], self.energy[:, y, x]
        self.at_ones[:, y, x] = np.where(bits, e, -np.inf)
        self.at_zeros[:, y, x] = np.where(bits, np.inf, e)

    def tightest_cluster(self):
        return int(np.argmax(self.at_ones))

    def largest_void(self):
        return int(np.argmin(self.at_zeros))


def void_and_cluster(seed):
    rng = np.random.default_rng(seed)
    field = Field()
    start = N // 10
    for index in rng.choice(N, start, replace=False):
        field.set(index, True)
    # The initial pattern: move the tightest cluster's 1 into the largest void until that is where it came from.
    swaps = 0
    while True:
        cluster = field.tightest_cluster()
        field.set(cluster, False)
        void = field.largest_void()
        field.set(void, True)
        if void == cluster:
            break
        swaps += 1
    print("initial pattern: %d ones, %d swaps" % (start, swaps))
    ranks = np.full(N, -1, np.int64)
    # Phase 1: the initial pattern's 1s, tightest cluster first, get the ranks below it, counting down.
    work = field.copy()
    for rank in range(start - 1, -1, -1):
        cluster = work.tightest_cluster()
        work.set(cluster, False)
        ranks[cluster] = rank
    # Phases 2 and 3: the largest void gets the next rank, counting up. Past half full, the tightest cluster of 0s is
    # the 0 with the least energy from the 1s (the energy of all pixels is the same everywhere on the torus), so the
    # same choice serves both phases.
    t0 = time.time()
    for rank in range(start, N):
        void = field.largest_void()
        field.set(void, True)
        ranks[void] = rank
        if rank % 32768 == 0:
            print("  rank %d of %d (%.0f s)" % (rank, N, time.time() - t0))
    assert (ranks >= 0).all() and len(np.unique(ranks)) == N
    return ranks


def values_per_frame(ranks):
    """Each frame's pixels in the order of their ranks, as values 0..255 (16 pixels each)."""
    r = ranks.reshape(T, W * W)
    order = np.argsort(r, axis=1)
    local = np.empty_like(order)
    np.put_along_axis(local, order, np.broadcast_to(np.arange(W * W), (T, W * W)), axis=1)
    return (local * 256 // (W * W)).astype(np.uint8).reshape(T, W, W)


# --- Unreal's SSAO noise (FSystemTextures, SSAORandomization; PostProcessAmbientOcclusion) ----------------------

UE_REORDER = [0, 11, 7, 3, 10, 4, 15, 12, 6, 8, 1, 14, 13, 2, 9, 5]


def ue_tile_u():
    """The 4x4 tile's rotations as fractions of half a turn (the angle is pi * u)."""
    return np.array(UE_REORDER, np.float64).reshape(4, 4) / 16.0


def ue_offset(k):
    """The texel shift of TemporalOffset = k * (2.48, 7.52) / 64 with point sampling, at pixel centers."""
    return int(np.floor(0.5 + k * 2.48)), int(np.floor(0.5 + k * 7.52))


def screen(method, frame, size, stbn, rng):
    """The rotation u of every pixel of a size x size screen in a frame, for one of the noises compared."""
    y, x = np.mgrid[0:size, 0:size]
    if method in ("unreal", "random step"):
        k = frame % 8 if method == "unreal" else int(rng.integers(8))
        sx, sy = ue_offset(k)
        return ue_tile_u()[(y + sy) % 4, (x + sx) % 4]
    if method == "stbn":
        return (stbn[frame % T][y % W, x % W] + 0.5) / 256.0
    if method == "white":
        return rng.random((size, size))
    raise ValueError(method)


def blur(img, sigma):
    """A Gaussian blur that wraps around the edges, like the noise's tiling."""
    fy = np.fft.fftfreq(img.shape[0])[:, None]
    fx = np.fft.fftfreq(img.shape[1])[None, :]
    return np.real(np.fft.ifft2(np.fft.fft2(img) * np.exp(-2 * (np.pi * sigma) ** 2 * (fx ** 2 + fy ** 2))))


def radial(power):
    h, w = power.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    r = np.sqrt(fx ** 2 + fy ** 2)
    bins = np.linspace(0, 0.5, 9)
    return [float(power[(r >= lo) & (r < hi)].mean()) for lo, hi in zip(bins[:-1], bins[1:])], bins


def check(values, out):
    import cv2
    os.makedirs(out, exist_ok=True)
    v = values.astype(np.float64) / 255.0
    rng = np.random.default_rng(7)
    white = rng.random(v.shape)
    print("\nSpectra, power per frequency band divided by white noise's (low bands near 0 = blue):")
    sp = lambda a: np.mean(np.abs(np.fft.fft2(a - a.mean(axis=(1, 2), keepdims=True))) ** 2, axis=0)
    tp = lambda a: np.mean(np.abs(np.fft.fft(a - a.mean(axis=0, keepdims=True), axis=0)) ** 2, axis=(1, 2))
    s_noise, bins = radial(sp(v))
    s_white, _ = radial(sp(white))
    print("  frames, cycles/pixel " + " ".join("%.2f-%.2f" % (a, b) for a, b in zip(bins[:-1], bins[1:])))
    print("  frames               " + " ".join("%9.2f" % (a / b) for a, b in zip(s_noise, s_white)))
    t_noise, t_white = tp(v), tp(white)
    f = np.abs(np.fft.fftfreq(T))
    tb = np.linspace(0, 0.5, 5)
    print("  time, cycles/frame   " + " ".join("%.3f-%.3f" % (a, b) for a, b in zip(tb[:-1], tb[1:])))
    print("  time                 " + " ".join(
        "%11.2f" % (t_noise[(f >= a) & (f <= b) & (f > 0)].mean() / t_white[(f >= a) & (f <= b) & (f > 0)].mean())
        for a, b in zip(tb[:-1], tb[1:])))
    hist_frame = np.array([np.bincount(values[t].ravel(), minlength=256) for t in range(T)])
    print("  every frame holds each value %d..%d times" % (hist_frame.min(), hist_frame.max()))
    per_pixel = values.reshape(T, -1).astype(np.float64) / 255.0
    print("  each pixel's mean over the 64 frames: %.3f..%.3f (white noise: %.3f..%.3f)" % (
        per_pixel.mean(axis=0).min(), per_pixel.mean(axis=0).max(),
        white.reshape(T, -1).mean(axis=0).min(), white.reshape(T, -1).mean(axis=0).max()))

    # Pictures: frame 0 at 4x, four frames in a row, a pixel row over time (x across, frames down).
    z = lambda a: cv2.resize(a, (a.shape[1] * 4, a.shape[0] * 4), interpolation=cv2.INTER_NEAREST)
    cv2.imwrite(os.path.join(out, "frame0.png"), z(values[0]))
    cv2.imwrite(os.path.join(out, "frames0-3.png"), z(np.concatenate([values[t] for t in range(4)], axis=1)))
    cv2.imwrite(os.path.join(out, "row0_over_time.png"), z(values[:, 0, :]))
    spec = np.fft.fftshift(sp(v))
    spec = np.log1p(spec / spec.mean())
    cv2.imwrite(os.path.join(out, "frame_spectrum.png"), z((255 * spec / spec.max()).astype(np.uint8)))

    # How the noises average out: the SSAO term's first harmonic in the rotation, cos(2 pi u) (the angle is pi u),
    # is 0 on average over all rotations. Each method's frames go through an exponential average over time (a TAA
    # or DLSS history, weight alpha for the new frame) and a slight blur (the reconstruction); what is left is error.
    # "unreal" is the game's noise as its TAA sees it, the step of TemporalOffset following the frame (k = frame % 8);
    # "random step" is the add-on's first fix; "grid" is the share of the error's power at the 4x4 tile's
    # frequencies (multiples of 1/4 cycle per pixel), the grid on Junpei's cheek.
    size = 128
    fy = np.fft.fftfreq(size)[:, None] * 4
    fx = np.fft.fftfreq(size)[None, :] * 4
    grid = (np.abs(fy - np.round(fy)) < 1e-9) & (np.abs(fx - np.round(fx)) < 1e-9)
    grid[0, 0] = False
    print("\nAveraged over frames (cos(2 pi u); error RMS after the average, after the average and a 0.7 px blur, "
          "and the grid's share of the error):")
    for alpha in (0.1, 0.05):
        print("  alpha %.2f:" % alpha)
        for method in ("unreal", "random step", "stbn", "white"):
            mrng = np.random.default_rng(11)
            hist = None
            errs = []
            for frame in range(512):
                img = np.cos(2 * np.pi * screen(method, frame, size, values, mrng))
                hist = img if hist is None else hist + alpha * (img - hist)
                if frame >= 256:
                    errs.append(hist.copy())
            errs = np.array(errs)
            rms = np.sqrt(np.mean(errs ** 2))
            blurred = np.array([blur(e, 0.7) for e in errs[::16]])
            rms_b = np.sqrt(np.mean(blurred ** 2))
            power = np.mean([np.abs(np.fft.fft2(e)) ** 2 for e in errs[::16]], axis=0)
            share = power[grid].sum() / max(power.sum(), 1e-30)
            print("    %-12s %.4f  %.4f  grid %5.1f%%" % (method, rms, rms_b, 100 * share))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out")
    parser.add_argument("--seed", type=int, default=1418)
    parser.add_argument("--check")
    parser.add_argument("--load", help="check an existing file instead of making one")
    args = parser.parse_args()
    if args.load:
        values = np.fromfile(args.load, np.uint8).reshape(T, W, W)
    else:
        t0 = time.time()
        values = values_per_frame(void_and_cluster(args.seed))
        print("made in %.0f s" % (time.time() - t0))
        if args.out:
            values.tofile(args.out)
            print("wrote", args.out, values.size, "bytes")
    if args.check:
        check(values, args.check)


main()
