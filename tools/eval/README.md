# fp_eval — shared, local, privacy-preserving matcher evaluation

A single script to measure **how well a matcher tells your finger from someone
else's** on a small host-processing fingerprint sensor, and to compare that
against libfprint's default **NBIS** path (`mindtct` + `bozorth3`) on the *same*
captures.

It exists so the Goodix Milan-SPI family (5187 / 51A0 / 51C0 …) can put a real
**FAR/FRR** behind the "NBIS is chance-level here, host-side matching isn't"
claim — across several units and users — **without anyone sharing biometric
data**.

## The privacy contract

- You run it **on your own machine, on your own captures.**
- It **never writes an image or a template.** Its only output is aggregate
  numbers: score distributions, EER, FAR/FRR at fixed operating points, a
  histogram. No per-capture rows, no file names.
- You post **that output**. Everyone pools numbers; nobody uploads a
  fingerprint.

If you can read the block it prints and see only counts and statistics, that is
by design — that is the whole point.

## Install

```
pip install numpy pillow
# optional, enables the SIFT and ORB backends:
pip install opencv-python-headless
# optional, enables the NBIS backend: install NBIS so `mindtct` and `bozorth3`
# are on your PATH (e.g. the `nbis` package, or build from the NIST source).
```

The `numpy` backend always runs with just numpy + pillow, so the tool works even
with nothing optional installed.

## Capture layout

One sub-directory per **distinct finger** (an identity), each with several views
of that finger. Grayscale PGM / PNG / BMP / TIFF.

```
captures/
  right-index/   view01.png view02.png ...   (>= enroll+1 views)
  right-middle/  ...
  left-thumb/    ...
```

Two fingers is the minimum (each finger's captures act as impostors for the
others). More fingers, and more people each running the tool, give a real
multi-user picture. Capture the way you'd really enrol and unlock — same finger
placements, same conditions.

## Run

```
python3 fp_eval.py --captures ./captures --sensor "GXFP5187 132x112" \
                   --enroll 20 --repeats 3 --json report.json
```

- `--enroll N` — enrolled views per finger (rest are held-out probes). The
  enrol/probe split is **disjoint**, so the threshold is never chosen on the data
  it's scored on.
- `--repeats R` — averages over R random enrol/probe splits.
- `--upscale K` — integer upscale before matching; on a ~6 mm sensor `--upscale 3`
  is often worth trying.
- `--backends numpy,sift,orb,nbis` — defaults to every backend available;
  `none` runs only the `--backend-so` plug-ins.
- `--at-threshold T` — also report FAR and FRR at your driver's own decision
  threshold (repeatable), not only at the EER point.

## What each backend is

| backend | what it is | stands in for |
|---|---|---|
| `numpy` | Harris corners + BRIEF-256 + rigid RANSAC, pure numpy | the family's FAST/BRIEF + RANSAC (SIGFM-style) host matchers |
| `sift`  | OpenCV RootSIFT + Lowe ratio + RANSAC homography | descriptor matchers like this driver's |
| `orb`   | OpenCV ORB (FAST + BRIEF) + RANSAC | the SIGFM recipe, via a standard impl |
| `nbis`  | `mindtct` → `bozorth3`, best-of-gallery | libfprint's default `FpImageDevice` path |

The backends are neutral reference implementations, not any project's shipped
code. The goal isn't to crown a matcher — it's one defensible FAR/FRR per
backend on identical data, so the NBIS-vs-host-matching gap is measured the same
way everywhere.

## Scoring your driver's REAL matcher (`--backend-so`)

The reference backends deliberately under-sell a real, tuned matcher — on a
GXFP51A0 set the numpy backend measured EER ≈ 18 % where the shipped SIGFM matcher
was ≈ 4.8 % on the *same* images. So to describe the matcher people actually run,
plug it in directly:

```
python3 fp_eval.py --captures ./captures --backend-so ./mymatcher.so
```

Write a thin adapter implementing the four-function ABI in
[`plugin/fp_eval_plugin.h`](plugin/fp_eval_plugin.h), compile it together with
your matcher into a `.so`, and pass it above (repeat `--backend-so` for several).
Your matcher then runs under the *same* held-out split, metrics and
aggregates-only report as every other backend. A complete, compilable example is
in [`plugin/example_ncc.c`](plugin/example_ncc.c):

```
cc -O2 -shared -fPIC -o example_ncc.so plugin/example_ncc.c -lm
python3 fp_eval.py --captures ./captures --backend-so ./example_ncc.so
```

This is how each of us scores our own shipped matcher on our own data, so the
pooled numbers describe the drivers people really run — not stand-ins.

A real-world example is [`plugin/gq_sigfm.c`](plugin/gq_sigfm.c) (contributed by
@szlukabence on issue #5): a ~30-line adapter over GodsQuantum's GXFP51A0 matcher.
It isn't self-contained — you build it against that driver's sources (see the
header) — but it shows the pattern. Measured on a GXFP51A0 it put the real matcher
at EER ≈ 4.7 % where NBIS on the same 80×64 captures was 50 % (chance).

If your matcher decides on the whole enrolled gallery rather than its best single
view, also export the optional `fpeval_score_gallery()`; the harness then uses it
in place of the best-of-gallery rule.

## This driver's matcher (GXFP5187)

[`plugin/goodix_rootsift.c`](plugin/goodix_rootsift.c) wraps `goodix_sift.c`, the
matcher the driver ships, including its view fusion (`fpeval_score_gallery`). Its
decision threshold follows the template's size (15 up to 15 views, 18 up to 20,
20 up to 30). Verify retries and adapted views are left out, so the
FRR it reports is a pessimistic bound.

[`collect-goodix.sh`](collect-goodix.sh) does the whole run on a GXFP5187: it
enables the driver's capture dump, enrols each test finger twice with
`fprintd-enroll` (roughly 30 usable captures per finger once empty second
views are dropped), deletes each test enrolment again, converts the dumps with
[`goodix_dump.py`](goodix_dump.py) (the driver's own preprocessing and
no-finger rejection), builds the plug-in and runs it alone with `--enroll 15`,
reporting FAR/FRR at the driver's threshold for a 20-view enrolment
(`--at-threshold 18`):

```
./collect-goodix.sh ~/fp-eval-goodix left-index-finger left-middle-finger \
                    right-middle-finger right-ring-finger
```

Use fingers you do not unlock with; the script refuses one that is enrolled. The
captures stay in that directory; only the printed report is meant to be shared.

## Reading the result

```
--- numpy ---
  genuine  n= 144  mean= 62.486  median= 61.500  max= 77.000
  impostor n= 864  mean=  4.772  median=  4.000  max= 11.000
  EER = 0.00%   d' = 12.48   (threshold at EER = 51.000)
  FAR @ FRR=1% : 0.00%     FRR @ FAR=0.1% : 0.00%
```

- **EER** — equal-error rate; lower is better, ~50 % is chance.
- **d'** — separation between the genuine and impostor score distributions.
- **FAR @ FRR=1%** — how often a stranger is accepted when you tune to reject
  genuine fingers 1 % of the time (and vice-versa).

With small in-house sets these numbers are indicative, not certified — the value
is in **many** of them, from different sensors and people, measured identically.
Post the printed block (or `report.json`) on the matching thread.
