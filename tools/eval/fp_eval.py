#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# fp_eval.py — shared, local, privacy-preserving matcher evaluation for small
# host-processing fingerprint sensors (the Goodix Milan-SPI family and friends).
#
# WHY THIS EXISTS
# ---------------
# Several out-of-tree drivers for these ~6 mm sensors independently found that
# libfprint's default NBIS path (mindtct + bozorth3) is chance-level here, and
# each shipped a host-side descriptor matcher instead. To make that case to
# upstream we need a FAR/FRR measured the same way across units and users — but
# fingerprint captures are biometric data that must not leave anyone's machine.
#
# This tool squares that circle: every contributor runs it LOCALLY on their own
# captures and posts ONLY the aggregate numbers it prints (score distributions,
# EER, FAR/FRR at fixed operating points). It never writes an image or a
# template, and the report contains no per-capture data and no file names — just
# counts and statistics. Pool the reports, not the fingerprints.
#
# USAGE
# -----
#   python3 fp_eval.py --captures DIR --sensor "GXFP5187 132x112" [options]
#
# Capture directory layout — one sub-directory per DISTINCT FINGER (identity),
# each holding several views (grayscale PGM/PNG/BMP/TIFF) of that finger:
#
#   captures/
#     right-index/   view01.png view02.png ...      (>= enroll+1 views)
#     right-middle/  ...
#     left-thumb/    ...
#
# Two or more fingers are required (the others act as impostors for each). More
# fingers and more users (each running the tool and posting numbers) give a real
# multi-user picture without anyone sharing a capture.
#
# Backends are auto-detected: a dependency-free numpy matcher always runs; cv2
# (SIFT/ORB) and NBIS (mindtct/bozorth3) are used too when installed.

import argparse
import glob
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

try:
    from PIL import Image
    _HAVE_PIL = True
except Exception:
    _HAVE_PIL = False

try:
    import cv2  # optional, for the SIFT/ORB backends
    _HAVE_CV2 = True
except Exception:
    _HAVE_CV2 = False

_IMG_EXT = (".pgm", ".png", ".bmp", ".tif", ".tiff", ".jpg", ".jpeg")

# --------------------------------------------------------------------------- #
# Image loading
# --------------------------------------------------------------------------- #

def load_gray(path, upscale=1):
    """Load an image as a float32 grayscale array in [0, 255]."""
    if _HAVE_PIL:
        im = Image.open(path).convert("L")
        if upscale != 1:
            im = im.resize((im.width * upscale, im.height * upscale),
                           Image.BICUBIC)
        return np.asarray(im, dtype=np.float32)
    # Minimal PGM fallback if Pillow is missing.
    if path.lower().endswith(".pgm"):
        return _load_pgm(path, upscale)
    raise RuntimeError("Pillow is required for non-PGM images (pip install pillow)")


def _load_pgm(path, upscale):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P5"):
        raise RuntimeError("only binary (P5) PGM supported without Pillow")
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    w, h, _maxv = vals
    idx += 1
    buf = np.frombuffer(data[idx:idx + w * h], dtype=np.uint8).astype(np.float32)
    img = buf.reshape(h, w)
    if upscale != 1:
        img = np.repeat(np.repeat(img, upscale, axis=0), upscale, axis=1)
    return img

# --------------------------------------------------------------------------- #
# numpy reference matcher: Harris corners + BRIEF-256 + RANSAC affine.
# This mirrors the FAST/BRIEF + RANSAC recipe the family's host matchers use
# (SIGFM-style); it is a neutral reference, not any one project's code.
# --------------------------------------------------------------------------- #

_MAX_PTS = 150          # cf. the drivers' keypoint cap
_PATCH = 31             # BRIEF sampling window (odd)
_NBITS = 256
_LOWE = 0.8
_RANSAC_TOL = 4.0
_RANSAC_ITERS = 300

_POPCOUNT = np.array([bin(i).count("1") for i in range(256)], dtype=np.int32)


def _sobel(img):
    kx = np.array([[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]], dtype=np.float32)
    ky = kx.T
    return _conv2(img, kx), _conv2(img, ky)


def _conv2(img, k):
    from numpy.lib.stride_tricks import sliding_window_view
    kh, kw = k.shape
    pad = np.pad(img, ((kh // 2, kh // 2), (kw // 2, kw // 2)), mode="reflect")
    win = sliding_window_view(pad, (kh, kw))
    return np.einsum("ijkl,kl->ij", win, k).astype(np.float32)


def _gauss(img, sigma=1.0):
    r = max(1, int(3 * sigma))
    x = np.arange(-r, r + 1, dtype=np.float32)
    g = np.exp(-(x ** 2) / (2 * sigma ** 2))
    g /= g.sum()
    tmp = _conv2(img, g[None, :])
    return _conv2(tmp, g[:, None])


def _harris_corners(img, max_pts=_MAX_PTS, k=0.04):
    g = _gauss(img, 1.0)
    Ix, Iy = _sobel(g)
    Sxx = _gauss(Ix * Ix, 1.5)
    Syy = _gauss(Iy * Iy, 1.5)
    Sxy = _gauss(Ix * Iy, 1.5)
    det = Sxx * Syy - Sxy * Sxy
    tr = Sxx + Syy
    R = det - k * tr * tr
    b = _PATCH // 2 + 1
    R[:b, :] = R[-b:, :] = R[:, :b] = R[:, -b:] = -np.inf
    # non-max suppression on a 3x3 neighbourhood
    from numpy.lib.stride_tricks import sliding_window_view
    pad = np.pad(R, 1, mode="constant", constant_values=-np.inf)
    mx = sliding_window_view(pad, (3, 3)).max(axis=(2, 3))
    peaks = (R == mx) & np.isfinite(R) & (R > 0)
    ys, xs = np.nonzero(peaks)
    if len(xs) == 0:
        return np.empty((0, 2), np.int32)
    order = np.argsort(R[ys, xs])[::-1][:max_pts]
    return np.stack([xs[order], ys[order]], axis=1).astype(np.int32)


def _brief_pattern(seed=1234):
    rng = np.random.RandomState(seed)
    half = _PATCH // 2
    p = rng.randint(-half, half + 1, size=(_NBITS, 4))
    return p  # each row: (x1, y1, x2, y2)


_PATTERN = _brief_pattern()


def _brief_descriptors(img, pts):
    g = _gauss(img, 1.0)
    descs = np.zeros((len(pts), _NBITS // 8), dtype=np.uint8)
    x1 = _PATTERN[:, 0]; y1 = _PATTERN[:, 1]
    x2 = _PATTERN[:, 2]; y2 = _PATTERN[:, 3]
    for i, (px, py) in enumerate(pts):
        a = g[py + y1, px + x1]
        b = g[py + y2, px + x2]
        bits = (a < b).astype(np.uint8)
        descs[i] = np.packbits(bits)
    return descs


def _features_numpy(img):
    pts = _harris_corners(img)
    if len(pts) == 0:
        return np.empty((0, 2), np.int32), np.empty((0, _NBITS // 8), np.uint8)
    return pts, _brief_descriptors(img, pts)


def _hamming_matrix(dq, dg):
    # dq: (N,32) uint8, dg: (M,32) uint8 -> (N,M) int distances
    x = dq[:, None, :] ^ dg[None, :, :]
    return _POPCOUNT[x].sum(axis=2)


def _ransac_affine_inliers(src, dst, tol=_RANSAC_TOL, iters=_RANSAC_ITERS):
    """Batched RANSAC affine fit; returns the best inlier count (vectorised)."""
    n = len(src)
    if n < 3:
        return 0
    rng = np.random.RandomState(0)
    src1 = np.concatenate([src, np.ones((n, 1))], axis=1).astype(np.float64)
    idx = rng.randint(0, n, size=(iters, 3))          # (iters, 3) sample triplets
    A = src1[idx]                                      # (iters, 3, 3)
    B = dst[idx].astype(np.float64)                    # (iters, 3, 2)
    valid = np.abs(np.linalg.det(A)) > 1e-6
    if not valid.any():
        return 0
    X = np.linalg.solve(A[valid], B[valid])           # (k, 3, 2) affine models
    pred = np.einsum("ni,kij->knj", src1, X)          # (k, n, 2)
    err = np.sqrt(((pred - dst[None]) ** 2).sum(axis=2))
    return int((err < tol).sum(axis=1).max())


def score_numpy(fa, fb):
    """Inlier count between two views' (points, descriptors)."""
    (pa, da), (pb, db) = fa, fb
    if len(da) < 3 or len(db) < 3:
        return 0.0
    D = _hamming_matrix(da, db)
    order = np.argsort(D, axis=1)
    best = D[np.arange(len(D)), order[:, 0]]
    second = D[np.arange(len(D)), order[:, 1]]
    keep = best < _LOWE * np.maximum(second, 1)
    if keep.sum() < 3:
        return 0.0
    src = pa[keep].astype(np.float64)
    dst = pb[order[keep, 0]].astype(np.float64)
    return float(_ransac_affine_inliers(src, dst))

# --------------------------------------------------------------------------- #
# Optional cv2 backends (SIFT = RootSIFT-style; ORB = FAST+BRIEF, SIGFM-like)
# --------------------------------------------------------------------------- #

def _features_cv2_sift(img):
    sift = cv2.SIFT_create(nfeatures=_MAX_PTS)
    kp, des = sift.detectAndCompute(img.astype(np.uint8), None)
    if des is None:
        return None
    des = des / (des.sum(axis=1, keepdims=True) + 1e-7)   # RootSIFT: L1 then sqrt
    des = np.sqrt(des)
    return kp, des.astype(np.float32)


def _features_cv2_orb(img):
    orb = cv2.ORB_create(nfeatures=_MAX_PTS)
    kp, des = orb.detectAndCompute(img.astype(np.uint8), None)
    if des is None:
        return None
    return kp, des


def _score_cv2(fa, fb, norm):
    if fa is None or fb is None:
        return 0.0
    (ka, da), (kb, db) = fa, fb
    if len(da) < 3 or len(db) < 3:
        return 0.0
    bf = cv2.BFMatcher(norm)
    raw = bf.knnMatch(da, db, k=2)
    good = [m for m, n in (p for p in raw if len(p) == 2)
            if m.distance < _LOWE * n.distance]
    if len(good) < 4:
        return float(len(good))
    src = np.float32([ka[m.queryIdx].pt for m in good])
    dst = np.float32([kb[m.trainIdx].pt for m in good])
    _H, mask = cv2.findHomography(src, dst, cv2.RANSAC, _RANSAC_TOL)
    return float(int(mask.sum()) if mask is not None else 0)

# --------------------------------------------------------------------------- #
# Optional NBIS backend (mindtct + bozorth3)
# --------------------------------------------------------------------------- #

_HAVE_NBIS = bool(shutil.which("mindtct") and shutil.which("bozorth3"))


def _nbis_xyt(img, workdir, tag):
    p = os.path.join(workdir, tag)
    if _HAVE_PIL:
        Image.fromarray(img.astype(np.uint8)).save(p + ".png")
        src = p + ".png"
    else:
        with open(p + ".pgm", "wb") as f:
            h, w = img.shape
            f.write(b"P5\n%d %d\n255\n" % (w, h))
            f.write(img.astype(np.uint8).tobytes())
        src = p + ".pgm"
    subprocess.run(["mindtct", src, p], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p + ".xyt"


def _score_nbis(xyt_probe, xyt_gallery):
    out = subprocess.run(["bozorth3", xyt_probe, xyt_gallery],
                         capture_output=True, text=True, check=True)
    return float(out.stdout.strip() or 0)

# --------------------------------------------------------------------------- #
# Metrics
# --------------------------------------------------------------------------- #

def _rates(gen, imp, thr):
    gen = np.asarray(gen, float); imp = np.asarray(imp, float)
    frr = float((gen < thr).mean()) if len(gen) else float("nan")   # genuine rejected
    far = float((imp >= thr).mean()) if len(imp) else float("nan")  # impostor accepted
    return far, frr


def _eer(gen, imp):
    gen = np.asarray(gen, float); imp = np.asarray(imp, float)
    if not len(gen) or not len(imp):
        return float("nan"), float("nan")
    thrs = np.unique(np.concatenate([gen, imp]))
    thrs = np.concatenate([[thrs[0] - 1], thrs, [thrs[-1] + 1]])
    best = None
    for t in thrs:
        far, frr = _rates(gen, imp, t)
        d = abs(far - frr)
        if best is None or d < best[0]:
            best = (d, (far + frr) / 2, t)
    return best[1], best[2]


def _far_at_frr(gen, imp, target_frr):
    if not len(gen) or not len(imp):
        return float("nan"), float("nan")
    # include a threshold below every genuine score (FRR=0) as a candidate
    thrs = np.concatenate([[np.min(gen) - 1], np.unique(np.asarray(gen, float))])
    chosen = None
    for t in sorted(thrs):
        _far, frr = _rates(gen, imp, t)
        if frr <= target_frr:
            chosen = t
    if chosen is None:
        return float("nan"), float("nan")
    far, frr = _rates(gen, imp, chosen)
    return far, chosen


def _frr_at_far(gen, imp, target_far):
    if not len(gen) or not len(imp):
        return float("nan"), float("nan")
    # include a threshold above every impostor score (FAR=0) as a candidate
    thrs = np.concatenate([np.unique(np.asarray(imp, float)), [np.max(imp) + 1]])
    chosen = None
    for t in sorted(thrs, reverse=True):
        far, _frr = _rates(gen, imp, t)
        if far <= target_far:
            chosen = t
    if chosen is None:
        return float("nan"), float("nan")
    far, frr = _rates(gen, imp, chosen)
    return frr, chosen


def _dprime(gen, imp):
    gen = np.asarray(gen, float); imp = np.asarray(imp, float)
    if not len(gen) or not len(imp):
        return float("nan")
    v = (gen.var() + imp.var()) / 2
    if v <= 0:
        return float("inf") if gen.mean() != imp.mean() else 0.0
    return float((gen.mean() - imp.mean()) / math.sqrt(v))


def _summ(a):
    a = np.asarray(a, float)
    if not len(a):
        return {}
    pct = {f"p{p}": float(np.percentile(a, p)) for p in (5, 25, 50, 75, 95)}
    return dict(n=int(len(a)), min=float(a.min()), max=float(a.max()),
                mean=float(a.mean()), std=float(a.std()), **pct)


def _hist(gen, imp, bins=20):
    both = np.concatenate([gen, imp]) if len(gen) and len(imp) else np.asarray(gen or imp, float)
    if not len(both):
        return {}
    edges = np.linspace(float(both.min()), float(both.max()) + 1e-9, bins + 1)
    gh, _ = np.histogram(gen, bins=edges)
    ih, _ = np.histogram(imp, bins=edges)
    return dict(edges=[round(float(e), 3) for e in edges],
                genuine=gh.tolist(), impostor=ih.tolist())

# --------------------------------------------------------------------------- #
# Evaluation protocol
# --------------------------------------------------------------------------- #

def _load_dataset(root, upscale):
    fingers = {}
    for d in sorted(os.listdir(root)):
        fd = os.path.join(root, d)
        if not os.path.isdir(fd):
            continue
        views = sorted(p for p in glob.glob(os.path.join(fd, "*"))
                       if p.lower().endswith(_IMG_EXT))
        if views:
            fingers[d] = views
    if len(fingers) < 2:
        sys.exit("error: need at least 2 finger sub-directories with images")
    imgs = {f: [load_gray(p, upscale) for p in v] for f, v in fingers.items()}
    return imgs


def _split(n_views, enroll, rng):
    idx = list(range(n_views))
    rng.shuffle(idx)
    e = min(enroll, n_views - 1)
    return idx[:e], idx[e:]


def run_backend(name, feat_fn, score_fn, imgs, enroll, repeats):
    # Pre-extract features once per view.
    feats = {f: [feat_fn(im) for im in views] for f, views in imgs.items()}
    gen, imp = [], []
    geom_ok = True
    for rep in range(repeats):
        rng = np.random.RandomState(1000 + rep)
        for fid, views in feats.items():
            e_idx, p_idx = _split(len(views), enroll, rng)
            if not p_idx:
                continue
            gallery = [views[i] for i in e_idx]
            # genuine: held-out probes of this finger vs its own gallery
            for pi in p_idx:
                s = max((score_fn(views[pi], g) for g in gallery), default=0.0)
                gen.append(s)
            # impostor: every other finger's views vs this gallery
            for ofid, oviews in feats.items():
                if ofid == fid:
                    continue
                for ov in oviews:
                    s = max((score_fn(ov, g) for g in gallery), default=0.0)
                    imp.append(s)
    return gen, imp


def run_nbis(imgs, enroll, repeats):
    if not _HAVE_NBIS:
        return None
    with tempfile.TemporaryDirectory() as wd:
        xyt = {}
        for fid, views in imgs.items():
            xyt[fid] = []
            for i, im in enumerate(views):
                try:
                    xyt[fid].append(_nbis_xyt(im, wd, f"{fid}_{i}"))
                except Exception:
                    xyt[fid].append(None)
        gen, imp = [], []
        for rep in range(repeats):
            rng = np.random.RandomState(1000 + rep)
            for fid, xs in xyt.items():
                e_idx, p_idx = _split(len(xs), enroll, rng)
                gallery = [xs[i] for i in e_idx if xs[i]]
                if not gallery:
                    continue
                for pi in p_idx:
                    if not xs[pi]:
                        continue
                    gen.append(max((_score_nbis(xs[pi], g) for g in gallery), default=0.0))
                for ofid, oxs in xyt.items():
                    if ofid == fid:
                        continue
                    for ox in oxs:
                        if not ox:
                            continue
                        imp.append(max((_score_nbis(ox, g) for g in gallery), default=0.0))
        return gen, imp


def evaluate(gen, imp):
    eer, eer_thr = _eer(gen, imp)
    far1, _ = _far_at_frr(gen, imp, 0.01)
    far5, _ = _far_at_frr(gen, imp, 0.05)
    frr01, _ = _frr_at_far(gen, imp, 0.001)
    frr1, _ = _frr_at_far(gen, imp, 0.01)
    return dict(
        genuine=_summ(gen), impostor=_summ(imp),
        eer=eer, eer_threshold=eer_thr, dprime=_dprime(gen, imp),
        far_at_frr_1pct=far1, far_at_frr_5pct=far5,
        frr_at_far_0p1pct=frr01, frr_at_far_1pct=frr1,
        histogram=_hist(gen, imp),
    )

# --------------------------------------------------------------------------- #
# Report
# --------------------------------------------------------------------------- #

def _fmt_pct(x):
    return "n/a" if x != x else f"{100 * x:.2f}%"


def print_report(report):
    m = report["meta"]
    print("=" * 64)
    print("fingerprint matcher evaluation — aggregates only, no biometric data")
    print("=" * 64)
    print(f"sensor        : {m['sensor']}")
    print(f"fingers       : {m['n_fingers']}   views/finger: {m['views_per_finger']}")
    print(f"enroll views  : {m['enroll']}   repeats: {m['repeats']}   upscale: {m['upscale']}")
    print(f"backends run  : {', '.join(m['backends'])}")
    print()
    for name, r in report["results"].items():
        g, i = r["genuine"], r["impostor"]
        print(f"--- {name} " + "-" * (58 - len(name)))
        print(f"  genuine  n={g.get('n',0):5d}  mean={g.get('mean',float('nan')):8.3f}  "
              f"median={g.get('p50',float('nan')):8.3f}  max={g.get('max',float('nan')):8.3f}")
        print(f"  impostor n={i.get('n',0):5d}  mean={i.get('mean',float('nan')):8.3f}  "
              f"median={i.get('p50',float('nan')):8.3f}  max={i.get('max',float('nan')):8.3f}")
        print(f"  EER = {_fmt_pct(r['eer'])}   d' = {r['dprime']:.2f}   "
              f"(threshold at EER = {r['eer_threshold']:.3f})")
        print(f"  FAR @ FRR=1% : {_fmt_pct(r['far_at_frr_1pct'])}     "
              f"FRR @ FAR=0.1% : {_fmt_pct(r['frr_at_far_0p1pct'])}")
        print()
    print("Post the block above (or the --json file) on the issue. It contains")
    print("only counts and statistics — no images, templates or file names.")


def main():
    ap = argparse.ArgumentParser(
        description="Local, privacy-preserving fingerprint matcher evaluation.")
    ap.add_argument("--captures", required=True,
                    help="directory with one sub-dir per finger (see header)")
    ap.add_argument("--sensor", default="unspecified",
                    help='label for the report, e.g. "GXFP5187 132x112"')
    ap.add_argument("--enroll", type=int, default=20,
                    help="enrollment views per finger (default 20)")
    ap.add_argument("--repeats", type=int, default=3,
                    help="random enroll/probe splits to average (default 3)")
    ap.add_argument("--upscale", type=int, default=1,
                    help="integer upscale before matching (small sensors: try 3)")
    ap.add_argument("--backends", default="auto",
                    help="comma list of numpy,sift,orb,nbis (default: auto)")
    ap.add_argument("--json", help="also write the full report to this file")
    args = ap.parse_args()

    want = args.backends.split(",") if args.backends != "auto" else \
        ["numpy"] + (["sift", "orb"] if _HAVE_CV2 else []) + (["nbis"] if _HAVE_NBIS else [])

    imgs = _load_dataset(args.captures, args.upscale)
    vpf = {f: len(v) for f, v in imgs.items()}

    results = {}
    for b in want:
        b = b.strip()
        if b == "numpy":
            gen, imp = run_backend("numpy-brief", _features_numpy, score_numpy,
                                   imgs, args.enroll, args.repeats)
        elif b == "sift":
            if not _HAVE_CV2:
                print("skip sift: opencv not installed", file=sys.stderr); continue
            gen, imp = run_backend("sift", _features_cv2_sift,
                                   lambda a, c: _score_cv2(a, c, cv2.NORM_L2),
                                   imgs, args.enroll, args.repeats)
        elif b == "orb":
            if not _HAVE_CV2:
                print("skip orb: opencv not installed", file=sys.stderr); continue
            gen, imp = run_backend("orb", _features_cv2_orb,
                                   lambda a, c: _score_cv2(a, c, cv2.NORM_HAMMING),
                                   imgs, args.enroll, args.repeats)
        elif b == "nbis":
            out = run_nbis(imgs, args.enroll, args.repeats)
            if out is None:
                print("skip nbis: mindtct/bozorth3 not installed", file=sys.stderr); continue
            gen, imp = out
        else:
            print(f"unknown backend: {b}", file=sys.stderr); continue
        results[b] = evaluate(gen, imp)

    report = dict(
        meta=dict(sensor=args.sensor, n_fingers=len(imgs),
                  views_per_finger=vpf, enroll=args.enroll,
                  repeats=args.repeats, upscale=args.upscale,
                  backends=list(results.keys()),
                  tool="fp_eval.py", note="aggregates only; no biometric data emitted"),
        results=results)

    print_report(report)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
