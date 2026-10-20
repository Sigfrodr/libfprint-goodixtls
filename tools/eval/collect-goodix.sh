#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Collect GXFP5187 captures for fp_eval, then score the driver's own matcher.
#
# For each test finger it runs fprintd-enroll twice (about 40 captures: one
# per press, a gallery the size of the driver's plus held-out probes), files
# the dumped frames under that finger, and deletes the test enrolment again.
# Fingers already enrolled for real are never touched: pick test fingers you
# do NOT use to unlock.
#
#     ./collect-goodix.sh [DIR] [finger ...]
#
# Captures are biometric data. They stay in DIR on this machine; only the
# aggregate report fp_eval prints is meant to be shared.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-$HOME/fp-eval-goodix}
shift || true
FINGERS=("$@")
[ ${#FINGERS[@]} -gt 0 ] || FINGERS=(left-index-finger left-middle-finger
                                     right-middle-finger right-ring-finger)
DUMP=/run/goodixtls/dump
RUNS=2

enrolled=$(fprintd-list "$USER" 2>/dev/null | sed -n 's/^ - #[0-9]*: //p')
for f in "${FINGERS[@]}"; do
  if grep -qx "$f" <<<"$enrolled"; then
    echo "$f is enrolled for real; pick another test finger." >&2
    exit 1
  fi
done

mkdir -p "$DIR/raw"
sudo mkdir -p "$DUMP"
trap 'sudo rm -rf "$DUMP"' EXIT

for f in "${FINGERS[@]}"; do
  mkdir -p "$DIR/raw/$f"
  for run in $(seq 1 $RUNS); do
    echo
    echo "=== $f — enrolment $run/$RUNS: vary the placement between presses ==="
    sudo find "$DUMP" -type f -delete
    fprintd-enroll -f "$f" || true
    fprintd-delete "$USER" -f "$f" >/dev/null 2>&1 || true
    n=$(sudo find "$DUMP" -name 'p*.bin' | wc -l)
    for p in $(sudo find "$DUMP" -name 'p*.bin' | sort); do
      b=$(basename "$p" .bin)
      sudo cp "$p" "$DIR/raw/$f/r${run}-$b.bin"
      sudo cp "$p.bg" "$DIR/raw/$f/r${run}-$b.bin.bg"
    done
    echo "$f run $run: $n captures"
  done
done
sudo chown -R "$USER:" "$DIR/raw"
chmod -R go-rwx "$DIR"

python3 "$HERE/goodix_dump.py" "$DIR/raw" "$DIR/captures"

SO="$DIR/goodix_rootsift.so"
cc -O2 -shared -fPIC -o "$SO" "$HERE/plugin/goodix_rootsift.c" \
   "$HERE/../../goodix_sift.c" -I"$HERE/../.." \
   $(pkg-config --cflags --libs glib-2.0) -lm

python3 "$HERE/fp_eval.py" --captures "$DIR/captures" \
        --sensor "GXFP5187 132x112" --enroll 20 --repeats 5 \
        --backends none --backend-so "$SO" --at-threshold 18 \
        --json "$DIR/report.json"
echo
echo "Aggregate report: $DIR/report.json (no images, safe to share)."
