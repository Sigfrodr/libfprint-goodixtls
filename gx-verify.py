#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Goodix GXFP5187 SPI (TLS-PSK) driver for libfprint
#
# Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
"""GTK utility for the Goodix GXFP5187 fingerprint reader.

Two modes: verify a finger, and enroll it while showing the coverage being
built up. It exists because neither the console nor GNOME fits the job:

  - a "put your finger down" prompt printed by a script started in the
    background never reaches the user's eyes;
  - GNOME's enrollment only shows a counter, without saying where to put the
    finger or what is missing -- yet on a 6x5 mm sensor COVERAGE is the
    primary factor in the recognition rate.

On each touch the driver logs the position of the view in the common frame of
reference and the extent reached; this window reads them and draws them.

    python3 gx-verify.py [user]
"""
import gi, os, sys, re, subprocess, gettext

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Gio

# Translations. The source strings are English and act as the msgids; the
# catalogues live in po/<lang>/LC_MESSAGES/gx-verify.mo.
#
# fallback=True is what keeps this tool usable straight from a clone: with no
# catalogue compiled, or an unknown locale, gettext returns the English source
# strings instead of raising. Translation is therefore a bonus, never a
# prerequisite for running.
#
# The local directory is tried first so that a translation can be tested
# without installing anything; the system path is the one that matters once
# install.sh has run.
_LOCALEDIRS = [os.path.join(os.path.dirname(os.path.abspath(__file__)), "po"),
               "/usr/share/locale"]
for _d in _LOCALEDIRS:
    _t = gettext.translation("gx-verify", _d, fallback=True)
    if isinstance(_t, gettext.GNUTranslations):
        break
_ = _t.gettext

USER = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("USER", "")
BUS = "net.reactivated.Fprint"
DEV_IFACE = BUS + ".Device"
MGR_PATH = "/net/reactivated/Fprint/Manager"
FINGER = "right-index-finger"

# Sensor geometry, in image pixels.
SENSOR_W, SENSOR_H = 132, 112

# Zones to cover, in order. Prescribing the zone rather than deriving a
# direction from the registration avoids a sign trap: when the finger moves to
# the right, the observed window moves to the LEFT in the finger's frame of
# reference. An instruction computed from the offsets would be inverted if that
# sign were set wrong; a named instruction is interpreted by the user, who
# cannot get the frame of reference wrong.
ZONES = [
    (_("the centre of the pad"),        _("firm touch, finger flat")),
    (_("the centre, slightly offset"),  _("1 to 2 mm to the side")),
    (_("towards the TIP of the finger"), _("roll the finger towards the nail")),
    (_("towards the TIP, further"),     _("without lifting the pad")),
    (_("towards the BASE of the finger"), _("move back towards the first knuckle")),
    (_("towards the BASE, further"),    _("still flat")),
    (_("the LEFT edge"),                _("tilt the finger slightly to the left")),
    (_("the LEFT edge, further"),       _("roll a little more")),
    (_("the RIGHT edge"),               _("tilt the finger slightly to the right")),
    (_("the RIGHT edge, further"),      _("roll a little more")),
    (_("TIP + left edge"),              _("corner of the finger")),
    (_("TIP + right edge"),             _("corner of the finger")),
    (_("BASE + left edge"),             _("corner of the finger")),
    (_("BASE + right edge"),            _("corner of the finger")),
    (_("the centre, to finish"),        _("firm touch")),
]


class Coverage(Gtk.DrawingArea):
    """Map of the finger areas already seen: one rectangle per touch."""

    def __init__(self):
        super().__init__()
        self.rects = []          # (dx, dy) of each placed view
        self.orphans = 0         # touches with no usable overlap
        self.set_content_height(150)
        self.set_draw_func(self.draw)

    def reset(self):
        self.rects, self.orphans = [], 0
        self.queue_draw()

    def add(self, dx, dy):
        self.rects.append((dx, dy))
        self.queue_draw()

    def draw(self, _area, cr, width, height):
        cr.set_source_rgb(0.13, 0.13, 0.15)
        cr.paint()
        if not self.rects:
            return

        xs = [d[0] for d in self.rects]
        ys = [d[1] for d in self.rects]
        x0, x1 = min(xs), max(xs) + SENSOR_W
        y0, y1 = min(ys), max(ys) + SENSOR_H
        margin = 12
        scale = min((width - 2 * margin) / max(x1 - x0, 1),
                    (height - 2 * margin) / max(y1 - y0, 1))
        ox = (width - (x1 - x0) * scale) / 2
        oy = (height - (y1 - y0) * scale) / 2

        # Translucent stacking: the more often an area is seen, the brighter it gets.
        for (dx, dy) in self.rects:
            cr.set_source_rgba(0.21, 0.52, 0.89, 0.28)
            cr.rectangle(ox + (dx - x0) * scale, oy + (dy - y0) * scale,
                         SENSOR_W * scale, SENSOR_H * scale)
            cr.fill()
        for (dx, dy) in self.rects:
            cr.set_source_rgba(0.4, 0.7, 1.0, 0.5)
            cr.set_line_width(1)
            cr.rectangle(ox + (dx - x0) * scale, oy + (dy - y0) * scale,
                         SENSOR_W * scale, SENSOR_H * scale)
            cr.stroke()


class App(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="org.goodixtls.verify")
        self.dev = None
        self.mode = None          # "verify" | "enroll"
        self.cursor = None        # journal read position
        self.stages = 0
        self.nstages = 15         # replaced by the driver's value
        self.points = []          # points accumulated after each touch
        self.prompt_head = _("Put your finger down")
        self.prompt_detail = ""

    # --- interface ---------------------------------------------------
    def do_activate(self):
        self.win = Gtk.ApplicationWindow(application=self,
                                         title="Fingerprint reader")
        self.win.set_default_size(480, 560)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        for f in ("set_margin_top", "set_margin_bottom",
                  "set_margin_start", "set_margin_end"):
            getattr(box, f)(24)
        self.win.set_child(box)

        self.icon = Gtk.Label()
        self.icon.set_markup("<span size='56000'>👆</span>")
        box.append(self.icon)

        self.msg = Gtk.Label(label="Connecting to the reader…")
        self.msg.set_wrap(True)
        self.msg.add_css_class("title-2")
        box.append(self.msg)

        self.detail = Gtk.Label(label="")
        self.detail.set_wrap(True)
        self.detail.add_css_class("dim-label")
        box.append(self.detail)

        self.bar = Gtk.ProgressBar()
        self.bar.set_show_text(True)
        self.bar.set_visible(False)
        box.append(self.bar)

        self.cov = Coverage()
        self.cov.set_visible(False)
        box.append(self.cov)


        self.covtxt = Gtk.Label(label="")
        self.covtxt.add_css_class("dim-label")
        self.covtxt.set_visible(False)
        box.append(self.covtxt)

        btns = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        btns.set_halign(Gtk.Align.CENTER)
        box.append(btns)

        self.b_verify = Gtk.Button(label=_("Verify"))
        self.b_verify.add_css_class("suggested-action")
        self.b_verify.add_css_class("pill")
        self.b_verify.connect("clicked", lambda _b: self.start("verify"))
        btns.append(self.b_verify)

        self.b_enroll = Gtk.Button(label=_("Enroll this finger"))
        self.b_enroll.add_css_class("pill")
        self.b_enroll.connect("clicked", lambda _b: self.start("enroll"))
        btns.append(self.b_enroll)

        self.b_stop = Gtk.Button(label=_("Stop"))
        self.b_stop.add_css_class("pill")
        self.b_stop.connect("clicked", lambda _b: self.stop())
        self.b_stop.set_visible(False)
        btns.append(self.b_stop)

        self.set_buttons(False)
        self.win.present()
        self.connect_device()

    def set_buttons(self, ready):
        self.b_verify.set_sensitive(ready)
        self.b_enroll.set_sensitive(ready)

    # --- connection ---------------------------------------------------
    def connect_device(self):
        try:
            bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
            mgr = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE, None,
                                         BUS, MGR_PATH, BUS + ".Manager", None)
            path = mgr.call_sync("GetDefaultDevice", None,
                                 Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
            self.dev = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE,
                                              None, BUS, path, DEV_IFACE, None)
            self.dev.connect("g-signal", self.on_signal)
            self.dev.connect("g-properties-changed", self.on_props)
            n = self.dev.get_cached_property("num-enroll-stages")
            if n:
                self.nstages = max(int(n.unpack()), 1)
            self.ensure_claimed()
        except Exception as e:
            return self.state("error", _("Reader unavailable"), str(e))
        self.state("idle", _("Ready"),
                   _("User: {user}").format(user=USER or _("(current)")))
        self.set_buttons(True)

    # --- claiming -------------------------------------------------
    def ensure_claimed(self):
        """Claim the device before an operation.

        fprintd releases it at the end of EVERY verification (journal:
        "released device 0"), so claiming once at startup is not enough: the
        next VerifyStart would fail with "device not claimed". We therefore
        re-claim before each operation. An "already claimed" (AlreadyInUse) is
        not an error: the device is still ours from startup.
        """
        try:
            self.dev.call_sync("Claim", GLib.Variant("(s)", (USER,)),
                               Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error as e:
            if "AlreadyInUse" not in e.message:
                raise

    # --- start ---------------------------------------------------
    def start(self, mode):
        if not self.dev or self.mode:
            return
        self.mode = mode
        self.set_buttons(False)
        self.b_stop.set_visible(True)
        self.mark_journal()
        try:
            self.ensure_claimed()
            if mode == "verify":
                self.bar.set_visible(False)
                self.cov.set_visible(False)
                self.covtxt.set_visible(False)
                self.state("wait", _("Put your finger down"),
                           _("flat, and HOLD IT until the result "
                             "(the capture takes ~1.5 s)"))
                self.hold_prompt()
                self.dev.call_sync("VerifyStart", GLib.Variant("(s)", ("any",)),
                                   Gio.DBusCallFlags.NONE, -1, None)
            else:
                self.stages = 0
                self.points = []
                self.cov.reset()
                self.bar.set_visible(True)
                self.bar.set_fraction(0)
                self.bar.set_text(_("0 touches"))
                self.cov.set_visible(True)
                self.covtxt.set_visible(True)
                self.covtxt.set_text("")
                self.say_zone()
                self.hold_prompt()
                self.dev.call_sync("EnrollStart", GLib.Variant("(s)", (FINGER,)),
                                   Gio.DBusCallFlags.NONE, -1, None)
            # Safety net: if the driver was already asking for the finger, its
            # property does not change and no PropertiesChanged arrives -- the
            # screen would stay on "Preparing". So we re-read the state once.
            GLib.timeout_add(300, self.sync_prompt)
        except Exception as e:
            self.finish("error", _("Failed to start"), str(e))

    def sync_prompt(self):
        """Re-read finger-needed/present and bring the screen back in sync."""
        if not self.mode or not self.dev:
            return False
        need = self.dev.get_cached_property("finger-needed")
        pres = self.dev.get_cached_property("finger-present")
        if need and bool(need.unpack()) and not (pres and bool(pres.unpack())):
            self.state("wait", self.prompt_head, self.prompt_detail)
        return False

    def hold_prompt(self):
        """Hide the prompt until the driver has asked for the finger.

        The driver calibrates the background at the start of each operation,
        which takes 1 to 7 s depending on the state of the sensor, and only
        raises "finger-needed" afterwards. Showing "Put your finger down"
        before that is not merely inaccurate: a finger placed DURING the
        calibration is taken for the background, hence an image with no
        contrast and a collapsed score (measured: 9 instead of 100, with
        "background retaken, finger was still down" in the journal). The real
        prompt is redisplayed by on_props, which finds it back in
        prompt_head/prompt_detail.
        """
        self.state("prep", _("Preparing the sensor…"),
                   _("do not put your finger down yet"))

    def stop(self):
        self.finish("idle", _("Interrupted"), "")

    def finish(self, kind, message, detail):
        for m in ("VerifyStop", "EnrollStop"):
            try:
                self.dev.call_sync(m, None, Gio.DBusCallFlags.NONE, -1, None)
            except Exception:
                pass
        self.mode = None
        self.b_stop.set_visible(False)
        self.set_buttons(True)
        self.state(kind, message, detail)

    # --- fprintd signals ---------------------------------------------
    def on_signal(self, _p, _s, signal, params):
        if signal == "VerifyStatus":
            self.on_verify(*params.unpack())
        elif signal == "EnrollStatus":
            self.on_enroll(*params.unpack())

    def on_props(self, _proxy, changed, _inval):
        """The driver distinguishes "I still need the finger" from "I have it".

        finger-needed drops back to false as soon as the capture is done: that
        is the only moment at which one can say to lift without being wrong.
        Without that feedback the user has to guess -- and lifting too early
        spoils the capture, since the read goes on for ~1.1 s after detection.
        """
        d = changed.unpack() if changed else {}
        if not self.mode or ("finger-needed" not in d and "finger-present" not in d):
            return
        need = self.dev.get_cached_property("finger-needed")
        pres = self.dev.get_cached_property("finger-present")
        need = bool(need.unpack()) if need else False
        pres = bool(pres.unpack()) if pres else False

        if pres and need:
            self.state("hold", _("Do not move"), _("reading in progress…"))
        elif pres and not need:
            self.state("lift", _("You can lift your finger"), "")
        elif need and not pres:
            self.state("wait", self.prompt_head, self.prompt_detail)

    def on_verify(self, result, done):
        if result == "verify-match":
            self.finish("match", _("Finger recognised ✓"), self.read_score())
        elif result == "verify-no-match":
            self.finish("nomatch", _("Finger not recognised"), self.read_score())
        elif result == "verify-disconnected":
            # The sensor did not answer at all -- typically no TLS session. Say
            # so, rather than letting the user believe the finger was badly
            # placed: it is the fprintd journal that carries the exact cause.
            self.finish("error", _("The sensor is not responding"),
                        _("Session failed — run  sudo gx-recover.sh"))
        elif done:
            self.finish("error", _("Verification interrupted"), result)
        else:
            self.state("wait", _("Put your finger down again"), {
                "verify-swipe-too-short":
                    _("Finger removed too early — HOLD IT for two seconds"),
                "verify-finger-not-centered": _("Finger off-centre"),
                "verify-remove-and-retry": _("Remove your finger, then put it back"),
            }.get(result, _("Imperfect capture — try again")))

    def on_enroll(self, result, done):
        if result == "enroll-completed":
            self.read_coverage()
            self.stages = self.nstages     # fprintd does not emit the last one
            self.bar.set_text(_("{done} / {total} touches").format(
                done=self.stages, total=self.nstages))
            self.bar.set_fraction(1.0)
            return self.finish("match", _("Enrollment complete ✓"),
                               self.coverage_text())
        if result == "enroll-failed":
            return self.finish("error", _("Enrollment failed"), "")
        if result == "enroll-stage-passed":
            self.read_coverage()
            self.bar.set_text(f"{self.stages} / {self.nstages} touches")
            self.bar.set_fraction(min(self.stages / float(self.nstages), 1.0))
            self.covtxt.set_text(self.coverage_text())
            self.say_zone()
        elif not done:
            self.state("wait", _("Put your finger down again"), {
                "enroll-swipe-too-short":
                    _("Finger removed too early — HOLD IT for two seconds"),
                "enroll-finger-not-centered": _("Finger off-centre"),
                "enroll-remove-and-retry": _("Remove your finger, then put it back"),
            }.get(result, _("Imperfect capture — try again")))
        elif done:
            self.finish("error", _("Enrollment interrupted"), result)

    # --- driver journal -------------------------------------------
    def mark_journal(self):
        try:
            out = subprocess.run(
                ["journalctl", "-u", "fprintd", "-n", "1", "-o", "cat",
                 "--show-cursor"], capture_output=True, text=True,
                timeout=2).stdout
            m = re.search(r"-- cursor: (\S+)", out)
            self.cursor = m.group(1) if m else None
        except Exception:
            self.cursor = None

    def journal_since(self):
        cmd = ["journalctl", "-u", "fprintd", "-o", "cat"]
        cmd += (["--after-cursor", self.cursor] if self.cursor else ["-n", "80"])
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=2).stdout
        except Exception:
            return ""

    def read_score(self):
        out = self.journal_since()
        hits = re.findall(r"verify: (\d+) matches in (\d+) attempt", out)
        if not hits:
            return ""
        n, tries = hits[-1]
        s = _("{n} matches (attempt {tries})").format(n=n, tries=tries)
        return s + _(" — view added to the template") if "view acquired" in out else s

    def read_coverage(self):
        """Positions of the placed views, as the driver registered them."""
        out = self.journal_since()
        self.cov.reset()
        last = 0
        for m in re.finditer(r"enroll: stage=(\d+) at=(-?\d+),(-?\d+)", out):
            self.cov.add(int(m.group(2)), int(m.group(3)))
            last = max(last, int(m.group(1)))
        for m in re.finditer(r"enroll: stage=(\d+) at=\?", out):
            last = max(last, int(m.group(1)))
        if last:
            self.stages = last
        self.cov.orphans = len(re.findall(r"disjoint area", out))
        self.points = [int(m) for m in re.findall(r"points=(\d+)", out)]
        self.last_extent = None
        ex = re.findall(r"extent=(\d+)x(\d+) points=(\d+)", out)
        if ex:
            self.last_extent = ex[-1]

    def say_zone(self):
        """Announce the zone to place, and report if the previous one brought
        nothing -- it is the only feedback that prevents repeating the same
        placement."""
        i = min(self.stages, len(ZONES) - 1)
        zone, how = ZONES[i]
        head = _("Place {zone}").format(zone=zone)
        if self.stages == 0:
            self.state("wait", head,
                       how + _(" — lift your finger between each touch"))
            return
        gained = self.gain()
        if gained is not None and gained < 12:
            # The zone name is inserted as it stands, not lower-cased. Zones
            # capitalise the part that matters — TIP, BASE, LEFT, RIGHT — and
            # folding the case erased exactly the word the user needs to read.
            # The whole sentence is one msgid so the translator controls the
            # wording, rather than the code changing case on its behalf: case
            # folding is language-dependent and has no business here.
            self.state("nomatch", _("Too close to the previous placement"),
                       _("Try again: place {zone} — {how}").format(
                           zone=zone, how=how))
        else:
            self.state("wait", head, how)

    def gain(self):
        """Points added by the last touch, or None if unknown."""
        if len(self.points) < 2:
            return None
        return self.points[-1] - self.points[-2]

    def coverage_text(self):
        e = getattr(self, "last_extent", None)
        if not e:
            return ""
        w, h, n = int(e[0]), int(e[1]), int(e[2])
        area = (w + SENSOR_W) * (h + SENSOR_H)
        ratio = area / float(SENSOR_W * SENSOR_H)
        s = _("Area covered: {ratio:.1f}× one touch — {n} points").format(
            ratio=ratio, n=n)
        if self.cov.orphans:
            s += _(" ({n} touch(es) with no overlap)").format(n=self.cov.orphans)
        return s

    # --- rendering -------------------------------------------------------
    def state(self, kind, message, detail):
        if kind == "wait":
            self.prompt_head, self.prompt_detail = message, detail
        icons = {"idle": "👆", "wait": "👇", "hold": "✋", "lift": "👍",
                 "prep": "⏳", "match": "✅", "nomatch": "❌", "error": "⚠️"}
        colors = {"match": "#26a269", "nomatch": "#c01c28",
                  "error": "#e5a50a", "wait": "#3584e4",
                  "prep": "#77767b", "hold": "#e5a50a", "lift": "#26a269"}
        self.icon.set_markup(f"<span size='56000'>{icons.get(kind, '👆')}</span>")
        c = colors.get(kind)
        if c:
            self.msg.set_markup(
                f"<span foreground='{c}'>{GLib.markup_escape_text(message)}</span>")
        else:
            self.msg.set_text(message)
        self.detail.set_text(detail or "")

    def do_shutdown(self):
        if self.dev:
            for m in ("VerifyStop", "EnrollStop", "Release"):
                try:
                    self.dev.call_sync(m, None, Gio.DBusCallFlags.NONE, -1, None)
                except Exception:
                    pass
        Gtk.Application.do_shutdown(self)


if __name__ == "__main__":
    App().run(None)
