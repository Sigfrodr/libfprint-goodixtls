# libfprint-goodixtls — TOD driver for the Goodix GXFP5187 / GXFP51A7 (SPI)

[libfprint](https://fprint.freedesktop.org/) driver (**TOD** variant, the one
Ubuntu ships) for the **Goodix GXFP5187** and **GXFP51A7** SPI fingerprint
sensors of the Huawei MateBook X Pro (`MACH-WX9`) and MateBook 13 2019
(`WRT-WX9`), unsupported upstream (libfprint issue #112).

The protocol has been fully reverse-engineered. The TLS-PSK channel is
established **without** Intel ME / SGX / IAP: the PSK is read out of the
sensor's RAM through the `0xF2` memory command.

> **Reverse-engineering notes.** This repository is self-sufficient for
> building, installing and using the driver. The detailed reverse-engineering
> notes (protocol frames and sequences, the analysis of the Goodix matching
> engine) are kept separately and are *not* required for that purpose; they are
> not distributed here to avoid shipping captured protocol data. Ask the author
> if you need them for further work on the protocol.

## At a glance

- **Hardware** — Goodix **GXFP5187** (MateBook X Pro `MACH-WX9`, firmware
  `GF3288_ST411SEC_APP_11033`) and Goodix **GXFP51A7** (MateBook 13 2019
  `WRT-WX9`, MilanL chip `0x2205`, firmware `GF3288_ST411SEC_APP_14003`); both
  SPI, TLS-PSK, 132×112.
- **What works** — enrolment and verification through `fprintd` and GNOME
  Settings; session unlock and `sudo`. Open matcher, no NBIS, no Intel ME/SGX.
- **Install** — `sudo ./install.sh` does everything (dependencies, build,
  system settings, spidev bind). See [Building](#building) and the
  [spidev prerequisite](#runtime-prerequisite-spidev-node) for the manual steps.
- **Enrol / verify with on-screen guidance** — `python3 gx-verify.py`, see
  [gx-verify.py](#testing-with-visible-feedback-gx-verifypy).

### Supported models

| ACPI id | Laptop | Backend / firmware | Reset | PSK address |
|---|---|---|---|---|
| `GXFP5187` | MateBook X Pro (`MACH-WX9`) | `GF3288_ST411SEC_APP_11033` | gpiochip0 line 58, active-low | `0x20007f0c` |
| `GXFP51A7` | MateBook 13 2019 (`WRT-WX9`) | MilanL `0x2205`, `GF3288_ST411SEC_APP_14003` | gpiochip0 line 264, active-high | `0x20007f14` |

The backend, reset line and PSK address are selected automatically from the ACPI
id. The reset line / polarity and PSK address can be overridden with
`GOODIXTLS_RESET_LINE`, `GOODIXTLS_RESET_ACTIVE_HIGH` and `GOODIXTLS_PSK_ADDR`,
and `GOODIXTLS_WRITE_GAP_US` (default `2000`) tunes the split-write gap.

**Contents** —
[Why a dedicated matcher](#why-a-dedicated-matcher-and-not-nbis) ·
[Usage](#usage) ·
[Building](#building) ·
[spidev prerequisite](#runtime-prerequisite-spidev-node) ·
[The TLS channel](#the-tls-channel-the-systems-openssl-out-of-spec-record-decrypted-by-hand) ·
[Architecture](#architecture) ·
[Provenance](#provenance-of-the-frozen-data) ·
[Threat model](#threat-model)

## Status

| Step | Status |
|---|---|
| Sensor discovery by libfprint (ACPI id `GXFP5187` / `GXFP51A7`) | ✅ |
| Open / close (`FpDevice` life cycle) | ✅ |
| SPI dialogue from the driver (firmware version read) | ✅ `GF3288_ST411SEC_APP_11033` |
| PSK read from the sensor's RAM (0xF2) | ✅ 48 bytes |
| Hardware GPIO reset (line 58) | ✅ |
| Config upload (`0x90`, opens the gate) | ✅ |
| TLS-PSK handshake (OpenSSL, sensor=client) | ✅ |
| Image capture (FDT/REG/nav → image over TLS → 6→4 decoding) | ✅ |
| Finger detection + background calibration | ✅ (threshold σ>150) |
| `fp_device_capture` → `FpImage` returned to libfprint | ✅ **sharp fingerprint** |
| Full enrolment (`fp_device_enroll`) | ✅ |
| **Matcher based on local descriptors** (`goodix_sift.c`) | ✅ **FAR 0 % / FRR 0 %** under cross-validation |
| `FpImageDevice` → `FpDevice` rework (away from NBIS) | ✅ builds, loads, discovers, opens |
| Enrolment through **GNOME Settings** | ✅ fingerprint recorded (`right-index-finger`) |
| **Verification through `fprintd`** | ✅ `verify-match` on the right finger, rejection on another |

The driver captures, enrols and **verifies** a fingerprint without going
through NBIS.

## Why a dedicated matcher (and not NBIS)

`FpImageDevice` mandates the NBIS pipeline (minutiae + bozorth3), which is
unsuitable here: a ~6×5 mm sensor only yields **~6 minutiae per capture**, far
below the 20-40 required. Measured: bozorth3 score consistently zero.

Analysis of the Goodix engine showed that it does not use classical minutiae but **keypoints carrying a
SIFT-like local descriptor**. `goodix_sift.c` reimplements that approach from
published techniques (Lowe 2004; RootSIFT 2012):

1. keypoints detected where the **ridge orientation changes** (the ridges
   themselves look alike everywhere and discriminate nothing);
2. 128-D RootSIFT descriptor oriented by the local angle (rotation
   invariance), L2-normalised then clipped at 0.2;
3. L2 matching with **Lowe's ratio test**;
4. **geometric validation**: the retained pairs must share the same
   transformation;
5. **view fusion**: the score is the number of keypoints of the capture that at
   least one view of the template explains — and not the best per-view score.
   A press often overlaps several views without covering any of them clearly;
   taking the maximum threw away that distributed evidence. Spurious matches
   from a foreign finger, on the other hand, do not accumulate: they do not
   survive geometric validation.

**Results** (cross-validation over 29 captures of a dense enrolment, against
5 captures of another finger):

| | before | after tuning |
|---|---|---|
| same finger | mean 56.9, minimum **12** | mean 139, minimum **67** |
| other finger | mean 1.8, maximum **3** | **0** everywhere |

The driver decides at threshold **15**. A verification costs ~0.10 s of
computation.

### ⚠️ The threshold depends on how rich the template is

This is the main fragility of this matcher, discovered by widening the impostor
set. The fused score grows with the **extent** of the template — for a foreign
finger just as much as for the right one. Measured with **18 captures of
non-enrolled fingers** (several fingers, two hands):

| template | impostor max |
|---|---|
| 29 views, tight enrolment | **3** |
| 30 views, extent 328×248 (guided enrolment) | **8** |
| 40 views (the same plus 10 acquired) | **8** |

Two lessons. First, **improving coverage also raises the impostor floor**: the
gain on rejections is paid for in security margin. Second, it is the **extent**
and not the number of views that counts — the views acquired by the adaptive
store are near-duplicates of already covered zones, and they did not raise the
floor.

**A threshold of 6 had been calibrated on five captures of a single foreign
finger. It let three false acceptances through (6, 7, 8) as soon as the set was
widened.** Five samples from a single finger are not enough to tune a security
parameter. Any change to enrolment requires **re-measuring** this threshold
against a varied impostor set.

Current distributions, on the template in service (40 views):

| | scores |
|---|---|
| enrolled finger (8 varied presses) | 20, 24, 30, 31, 33, 43, 45, 79 |
| your other fingers (18 captures) | 0 ×13, 3, 5, 6, 7, **8** |
| another person (28 captures) | mostly 0/3, max **9** |
| a foreign finger, first set (5) | max 3 |

The threshold of 15 sits at ~2× the impostor maximum, and 25 % below the
legitimate minimum. Presses that are too far off-centre are discarded upstream
by the quality check, without even being scored.

### ⚠️ A retry must never depend on the score

The threshold above only holds if the decision bears on **one** capture. For a
long time the driver recaptured, with the finger still pressed, as long as the
score stayed below the threshold — keeping the **best** of the three attempts.
Presented as a convenience, it was a security flaw: retrying until you pass
turns a decision into a *maximum over several draws*. And `pam_fprintd`
multiplies that again with its `max-tries=3`: up to **nine draws** to clear a
single threshold.

Measured on a non-enrolled middle finger: **3 on the first capture, 20 on the
second**, hence accepted at the threshold of 15 — where the documented impostor
floor is 8. The same press, scored a single time, never exceeds 8.

| | impostor max (middle finger) |
|---|---|
| 48 views, best of 3 captures | **20** — accepted ✗ |
| 30 views, single capture | **8** — rejected ✓ |

A retry remains legitimate on **image quality** (too few keypoints, finger
lifted too early): those captures are discarded *without being scored*. It must
never be conditioned on how close the score is to the threshold.

Corollary for the adaptive store: its harmlessness rests on the acquisition
threshold (30) being strictly above the decision threshold (15). It is indeed
the acquired views — near-duplicates of already covered zones — that raise
convenience without widening the extent, and therefore without giving a foreign
finger more purchase: from 30 to 40 views, the impostor floor stayed at 8.

### How this tuning was obtained

Parameter sweep over the recorded sets (`test-sift`), not by guesswork. Two
levers carried the whole gain:

- **150 keypoints per image instead of 60.** The true finger's score rises from
  6.5 to 11.7 matches on average without raising that of a foreign finger.
  Beyond 150 the detector saturates: the 132×112 image has no more salient
  points.
- **View fusion** (see above). On deliberately scattered presses, borderline
  presses go from 5 to 18 matches while the foreign finger drops from 3 to 0.

Three avenues were measured then **discarded**:

- **cross-checking** the matches (nearest-neighbour reciprocity) lowers
  legitimate scores from 16 to 12 without gaining anything on the impostor;
- widening the **geometric tolerance** beyond 6 px raises the impostor as much
  as the true finger;
- **pooled voting** — registering the views into a single frame of reference
  (`gx_sift_mosaic`) then voting once on the translation — halved the rejection
  rate on a small set, but faced with the 18 impostor captures it puts **13 of
  them at 5 matches or more**, against 4 for fusion. The code remains
  (`gx_sift_match_pooled`) but is unused; the registration, however, serves the
  enrolment guidance.

**What remains decisive**: a covering enrolment. The quality of the template
depends on the number of **distinct** presses and on the surface swept — not on
the raw number of captures. Presses with no overlap with the template remain
rejected, and no tuning changes that.

*After a driver update that changes extraction, you must **re-enrol*** (a
template with 60 keypoints per view faced with captures at 150 loses half its
acceptances). Enrolment **adds** to the existing views, it does not overwrite
them.

**51 impostor captures in total, maximum 9**, against a legitimate minimum of
20 per verification. Counter-intuitive detail: another person is *easier* to
reject than the wearer's other fingers.

*Caveats*: a single third party remains a thin sample, and all the measurements
come from a single sensor. The threshold is specific to this
sensor/enrolment pair — see the warning about how rich the template is.

## The interface guides the whole cycle

`gx-verify.py` displays three states, where GNOME shows none:

| | |
|---|---|
| 👇 Press your finger | waiting for the press |
| ✋ Hold still | finger detected, read in progress (~1.1 s) |
| 👍 You can lift | capture done |

The driver distinguishes "I still need the finger" from "I have it": it keeps
the `FP_FINGER_STATUS_NEEDED` flag armed during the read and only clears it at
the end. That is the only moment at which one can safely say to lift — the
usual signal drops as soon as *detection* happens, one second too early, and
lifting at that point spoils the capture. Invisible when testing on yourself;
glaring as soon as another person tries.

## Usage

Once installed, the sensor appears in **GNOME Settings → Users → Fingerprint
Login**. Enrolment asks for **15 presses** (the driver captures 2 views per
press, i.e. 30 views in total: the finger shifts slightly from one capture to
the next, which enriches the template without multiplying the gestures).

> **Upgrading from an earlier build? Re-enrol.** A template is only valid for
> the image alignment it was captured with. This driver changed the FDT base
> formula for MilanL (GXFP51A7), so a template enrolled with a previous build
> stops matching however rich it is: measured on one unit, a 28-view pre-change
> template scored **7** where a fresh 26-view enrolment of the same finger
> scored **78** (threshold 15). If verification gets worse after an update,
> delete and re-enrol before suspecting the driver.

**A re-enrolment REPLACES the template, it does not enrich it.** The driver
declares `FP_DEVICE_FEATURE_UPDATE_PRINT` and knows how to take over the views
of an existing template, but that path is never used with `fprintd`: its
`EnrollStart` creates a fresh `FpPrint` and does not pass it the already
recorded fingerprint. GNOME, `fprintd-enroll` and `gx-verify.py` all go through
it. Measured: a 30-view template re-enrolled in 15 presses drops back to 28
views, and no "existing views kept" line appears in the log.

What really accumulates from one session to the next is the **adaptive store**
described below: it acquires views over the course of successful
verifications. A re-enrolment, incidentally, resets it, its views having been
acquired under the authority of the previous template.

To widen the coverage you therefore need **a single well-conducted enrolment** —
hence the zone guidance in `gx-verify.py` — and not several successive passes.

### Automatic template enrichment

`libfprint` offers no way to return an enriched template: `fpi_device_verify_
complete()` takes no `FpPrint`, and `fprintd` only writes a template from
enrolment. The driver therefore keeps **its own store** of acquired views,
alongside `fprintd`'s storage:

    /var/lib/fprint/.goodixtls-adapt/<user>.<finger>.views

It lives *inside* `/var/lib/fprint` because `fprintd`'s systemd unit declares
`ProtectSystem=strict` with `StateDirectory=fprint`: all the rest of the disk
is read-only to it. The name starts with a dot so that it is never mistaken for
a user directory.

**The rule that makes adaptation safe**: the acquisition threshold
(`GX_ADAPT_MIN`, **30**) is strictly above the decision threshold
(`GX_MATCH_THRESHOLD`, **15**). Whoever triggers an acquisition is therefore
already authenticating with twice the deciding margin, and about four times the
best score any foreign finger has reached — adaptation never lowers the bar for
an attacker, it only widens the coverage of someone who already passes. Without
that rule, a doubtful acceptance would write itself durably into the template
(template poisoning).

The ceiling is **relative** (60 % of the capture's keypoints) and not
absolute: the score depends on how rich the template is — measured, a 30-view
template takes ordinary presses from ~12 to ~100 matches — so that a fixed
ceiling stops qualifying anything at all as soon as the template improves.
Beyond that, the pressed zone is already well covered and one more view would
teach nothing. Observed: a press at 118 is not retained, a press at 34 is.

Ceiling of 20 acquired views. The store is **wiped at every enrolment**: the
acquired views had been acquired under the authority of the previous template.

**Deleting a fingerprint.** `fprintd` erases its template itself and never
informs the driver: our fingerprints are stored host-side
(`fpi_print_set_device_stored(…, FALSE)`), and libfprint's `delete` vfunc only
concerns templates kept *inside* the sensor. Without a fallback, deleting a
fingerprint in GNOME would leave its descriptors on the disk indefinitely. The
driver therefore **sweeps orphaned stores at every open**: any store whose
`fprintd` template has disappeared is erased. The cleanup is not instantaneous —
an operation on the sensor is needed to trigger it — but it is the only hook
libfprint leaves to the driver.

*Accepted limitation*: weak presses (9, 10 matches) — precisely the ones the
template lacks — are excluded as being too unreliable. They rise into the
window of their own accord as the store fills out, which grows the coverage
step by step without loosening safety.

**Fingerprint unlock** (session, login screen, `sudo`):

```bash
sudo pam-auth-update --enable fprintd     # the password stays available as a fallback
```

**Keeping `fprintd` resident** (otherwise the fingerprint is not always offered
when the screen wakes): the daemon stops after inactivity, and on wake GNOME
builds the authentication prompt immediately — if the service must first be
reactivated by D-Bus, only the password appears.

```bash
sudo mkdir -p /etc/systemd/system/fprintd.service.d
printf '[Service]\nExecStart=\nExecStart=/usr/libexec/fprintd --no-timeout\n' \
  | sudo tee /etc/systemd/system/fprintd.service.d/no-timeout.conf
sudo systemctl daemon-reload && sudo systemctl restart fprintd
```

### Testing with visible feedback: `gx-verify.py`

```bash
python3 gx-verify.py [user]
```

A small GTK4 (PyGObject) window that drives `fprintd` **over D-Bus**, like
GNOME — hence no conflict over access to the sensor. It displays the
instruction in large type and, above all, **the number of matches**, which
GNOME hides. Reading `fprintd`'s log requires being in the `adm` group (or
root); otherwise the score is simply omitted.

It exists because the console is not suitable for this: the "press your finger"
prompts of a test launched in the background never reach the user's eyes, which
produces attempts where nobody presses a finger — and false diagnoses drawn
from empty captures.

On the command line:

```bash
fprintd-enroll        # enrol a finger
fprintd-verify        # verify
```

## The TLS channel: the system's OpenSSL, out-of-spec record decrypted by hand

The sensor departs from the TLS spec on two points:

1. **its PSK is 48 bytes**, beyond the 32 accepted by default;
2. **it sends the image in ONE SINGLE record of 22240 bytes**, against 16384 at
   the spec's ceiling — verified by walking the headers on the wire:
   `type=17 ver=0303 len=22240`, a single record in the frame.

**The sensor cannot be made to fragment.** In TLS, the sender alone decides on
fragmentation; the only standard way to influence it is the
`max_fragment_length` extension (RFC 6066), proposed by the **client** — but
here the client is the sensor, the host being only a server.

Yet the driver embeds **no modified cryptographic library**. `goodix_tls.c`
lets the system's OpenSSL do the handshake and the writes (the 48-byte PSK goes
through: its limit is 512), then derives the session keys from the master
secret (`SSL_SESSION_get_master_key` and the two randoms, TLS 1.2 PRF) and
**decrypts the large record itself** — AES-128-CBC, HMAC-SHA256, sequence
number. That record never crosses the library, so its ceiling does not apply.

The MAC is verified: a wrong byte or sequence number would make the read fail
instead of silently producing a corrupted image.

**Pitfall encountered**: OpenSSL groups several TLS records into a **single**
BIO write call — the `ServerHello` group comes out as one 121-byte block
containing two records. The sensor requires one record per 0xB0 frame and
silently ignores a frame carrying two. The transport therefore re-splits on the
record headers. It is the same "one frame = one transfer" rule that governs the
whole protocol of this sensor.

*Note on security*: this channel brings none to the host. The key can be read
from the sensor's RAM by anyone who reaches the SPI bus — that is precisely how
it is obtained. It is a vendor obfuscation layer, not a security boundary;
that is also why handling the record layer here does not carry the implications
it would elsewhere.

## Protocol timings: measured, not guessed

A capture costs **1136 ms**. The dominant item is the **drain**: after each
command, the driver waits for a silence to be sure the reply is complete, and
each of the nine commands in the sequence pays for it.

| drain silence | time/capture | result (6-8 captures) |
|---|---|---|
| 6 (original value) | 1319 ms | no error |
| **4 (shipped)** | **1136 ms** | no error |
| 3 | 1062 ms | no error |
| 2 | — | **total failure, sensor stuck** |

4 is shipped and not 3: 3 works but it is the value just before the breaking
point, with no margin for a variation in temperature or load.

**The 30 ms pause between commands is NOT redundant** with the drain, despite
appearances: at 10 ms, 19 failures out of 19 and the sensor stuck. Do not touch
it.

### Getting out of a deep lock-up: `gx-recover.sh`

A desynchronisation puts the sensor into a state where **short** commands still
go through (the firmware version can be read) but where **long** transfers fail
— the PSK read by `0xF2`, hence no more TLS sessions. Neither the driver's
reset pulse (10 ms), nor a one-second hold, nor reopening the spidev node gets
out of it **on its own**. A long reset **and** the detach/reattach of the
`spidev` kernel driver must be combined, as its state too must be reset:

```bash
sudo ./gx-recover.sh
```

The driver applies a long reset by itself from the 2nd PSK read attempt
onwards, which covers the light cases; the script remains necessary for the
deep ones.

## The main loop is no longer blocked

Two steps last close to a second each and cannot be split up: establishing the
session (TLS handshake + background capture) and image capture. Both go through
OpenSSL's **blocking** API over a synchronous SPI descriptor, which cannot be driven by callbacks without
restructuring the whole TLS layer.

They therefore run in a worker thread (`GTask`), the result coming back on the
main loop — as libfprint's `secugen` driver does for its heavy processing. That
is safe here: libfprint only runs one operation at a time and the state machine
waits for the callback, so nothing else touches the sensor in the meantime.

**Measured** — worst response time of `fprintd` on D-Bus during a capture:

| | |
|---|---|
| before | **no reply** (the probe expires after 5 s) |
| after | **6 ms** |

Finger detection stays on the main loop: at ~33 ms per poll it is not felt, and
keeping it there avoids any access to the sensor from two threads.

Waiting for the finger **also responds to cancellation**: without that,
`VerifyStop` expired after 30 s and the sensor stayed busy — visible as soon as
you close the fingerprint prompt to type your password.

## Remaining adjustments

- Widen the evaluation to several sensors: everything is measured on a single
  unit, and the threshold depends on the sensor/enrolment pair.
- Widen the evaluation (several fingers/people) before any serious use.

## Building

```bash
sudo apt install libfprint-2-tod-dev meson ninja-build
meson setup build
ninja -C build
sudo ninja -C build install     # -> /usr/lib/x86_64-linux-gnu/libfprint-2/tod-1/
```

## Runtime prerequisite: spidev node

libfprint enumerates the `spidev` subsystem and associates the driver by the
sysfs path containing `GXFP5187`. The `spidev` driver must therefore be bound
to the sensor:

```bash
DEV=spi-GXFP5187:00
sudo modprobe spidev
echo spidev | sudo tee /sys/bus/spi/devices/$DEV/driver_override
echo $DEV   | sudo tee /sys/bus/spi/drivers/spidev/bind
sudo systemctl restart fprintd     # libfprint only enumerates at startup
```

`install.sh` lays down the udev rule `60-libfprint-2-goodixtls.rules`, which
redoes this bind every time the device appears. (On this hardware the sensor
can also be driven by a separate diagnostic kernel module (not part of this
repository); the two are mutually exclusive on the same SPI device — unbind one
before using the other.)

### "Reader unavailable": look below the driver first

The symptom wrongly blames the driver. It almost always comes from `fprintd`
having **no** device to offer — checkable in one command:

```bash
dbus-send --system --print-reply --dest=net.reactivated.Fprint \
  /net/reactivated/Fprint/Manager net.reactivated.Fprint.Manager.GetDevices
```

An empty array means the chain is broken **below** the driver. The three links,
in the order in which they should be examined:

| check | sign that this is it |
|---|---|
| `lsmod \| grep spidev` | module missing → `/sys/bus/spi/drivers/spidev` does not exist |
| `ls /sys/bus/spi/devices/spi-GXFP5187:00/driver` | no link → device not bound |
| `systemctl show fprintd -p ActiveEnterTimestamp` | daemon started **before** the node existed |

The first link is the trap: `spidev` has no alias for this hardware — which is
exactly why the `driver_override` is needed — so **nothing loads it on its
own**. If it is missing, the udev rule writes into a non-existent bind
directory and the sensor disappears without the slightest message. Hence the
two safeguards: `/etc/modules-load.d/goodixtls-spidev.conf` loads it at boot,
and the udev rule loads it itself (`RUN{builtin}+="kmod load spidev"`) before
attempting the bind, whose failure is now traced by `logger -t goodixtls`
instead of being swallowed.

The third link has its own subtlety: libfprint enumerates devices **when its
context is initialised**, so an `fprintd` started while the node did not yet
exist stays blind even once the bind is restored. It has to be restarted.

## Testing

```bash
gcc test-open.c -o test-open $(pkg-config --cflags --libs libfprint-2)
G_MESSAGES_DEBUG=all ./test-open      # should print the firmware version
```

## Architecture

- `goodixtls.h` — protocol constants, geometry, checksums.
- `goodixtls.c` — driver: discovery (`id_table`), SPI transport (one frame =
  one transfer, a crucial framing rule), TLS-PSK, FDT-based finger detection,
  12-bit 6→4 decoder, and the `FpDevice` operations
  (open/close/enroll/verify).
- `goodix_sift.c/h` — keypoint extraction, RootSIFT descriptors, matching and
  geometric validation; serialisation for storage in an `FpPrint`.
- `test-sift.c` — evaluation bench for the matcher on recorded captures.
- Required TOD entry point: `fpi_tod_shared_driver_get_type()`.

## Provenance of the frozen data

Two elements of the sources are not code written for this project but **data
observed on the bus**, indispensable for interoperability:

| element | where | nature |
|---|---|---|
| configuration blob, 256 bytes | `config_pcap.c` | frame 19 of a Wireshark capture of the Windows driver; it is the one that opens the command *gate* |
| capture sequence, 9 commands | `goodixtls.c` (`seq[]`) | bytes recorded "as observed on the wire" |

No vendor code is reused, decompiled or redistributed: the repository
explicitly excludes it. These two blocks are protocol constants, of the same
order as a register number — but their origin is stated here rather than left
to be discovered, because it is debatable.

## Threat model

Three properties to be aware of before deploying this driver anywhere other
than on a personal machine:

- **The TLS channel does not protect the host.** The PSK is read from the
  sensor's RAM (`0xF2`): anyone who reaches the spidev node obtains it. The
  encryption prevents passive eavesdropping on the bus, nothing more — see
  `goodix_tls.h`.
- **The spidev node is exposed to the session.** The udev rule sets `uaccess`
  and `GROUP="plugdev"`. Any process of the logged-in user can therefore talk
  to the sensor, and in particular read its memory. Restricting that node would
  break the driver, which runs under fprintd; it is an accepted trade-off, not
  an oversight.
- **The authentication decision is taken host-side**, by `goodix_sift.c`, and
  not by the sensor. It depends on the threshold and on the adaptive store,
  both described above. A *match-on-chip* sensor would offer a different
  guarantee; this one is not such a sensor.

## License

LGPL-2.1-or-later — see [`COPYING`](COPYING). Each source file carries an
`SPDX-License-Identifier` header.

Contributions and hardware reports are welcome through the issue tracker. This
is a community driver for a sensor upstream libfprint does not yet cover.
