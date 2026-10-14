#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Convert goodixtls capture dumps into fp_eval images.

The driver dumps every capture to /run/goodixtls/dump when that directory
exists: pNNN.bin is the raw 132x112 frame as 16-bit samples, pNNN.bin.bg the
background it was taken against. This applies the driver's own preprocessing
(gx_preprocess: background minus frame, then the row and column medians
removed), drops the captures the driver itself rejects as "no finger"
(contrast below GOODIX_FINGER_STD / 3), and writes 8-bit PGMs scaled over each
image's full range. The matcher is invariant to intensity scale and offset, so
the scaling costs only quantisation.

    goodix_dump.py RAW_DIR OUT_DIR

RAW_DIR holds one sub-directory per finger, each with pNNN.bin/.bin.bg pairs
(collect-goodix.sh lays them out that way); OUT_DIR gets the same finger
sub-directories, ready for fp_eval.py --captures OUT_DIR.
"""
import os
import sys

import numpy as np

W, H = 132, 112
FINGER_STD = 150.0          # GOODIX_FINGER_STD in goodixtls.c


def preprocess(frame, bg):
    d = bg.astype(np.float64) - frame.astype(np.float64)
    d -= np.median(d, axis=1, keepdims=True)    # per row
    d -= np.median(d, axis=0, keepdims=True)    # per column
    return d


def to_pgm(path, img):
    lo, hi = float(img.min()), float(img.max())
    u8 = np.zeros_like(img, dtype=np.uint8) if hi <= lo else \
        np.round((img - lo) * 255.0 / (hi - lo)).astype(np.uint8)
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (W, H))
        f.write(u8.tobytes())


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    raw, out = sys.argv[1], sys.argv[2]
    for finger in sorted(os.listdir(raw)):
        src = os.path.join(raw, finger)
        if not os.path.isdir(src):
            continue
        dst = os.path.join(out, finger)
        os.makedirs(dst, exist_ok=True)
        kept = dropped = 0
        for name in sorted(os.listdir(src)):
            if not name.endswith(".bin"):
                continue
            p = os.path.join(src, name)
            try:
                frame = np.fromfile(p, dtype=np.uint16).reshape(H, W)
                bg = np.fromfile(p + ".bg", dtype=np.uint16).reshape(H, W)
            except (OSError, ValueError):
                dropped += 1
                continue
            img = preprocess(frame, bg)
            if img.std() < FINGER_STD / 3.0:
                dropped += 1                    # the driver rejects these too
                continue
            to_pgm(os.path.join(dst, name[:-4] + ".pgm"), img)
            kept += 1
        print(f"{finger}: {kept} captures kept, {dropped} dropped")


if __name__ == "__main__":
    main()
