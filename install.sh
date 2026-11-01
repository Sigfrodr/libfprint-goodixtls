#!/bin/bash
# Goodix GXFP5187 SPI (TLS-PSK) driver for libfprint
#
# Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
# Installs the libfprint driver for the Goodix GXFP5187 (SPI) sensor.
#
# Gathers the steps that used to be manual: dependencies, build, system
# settings and binding the spidev node.
#
# No patched cryptographic library is needed: the driver uses the system
# OpenSSL. The sensor does send the image in an out-of-spec TLS record
# (~22 kB against 16384) that no conforming library will accept — goodix_tls.c
# decrypts that one itself.
#
#   ./install.sh              install everything
#   ./install.sh --uninstall  remove what was installed
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# The same driver serves two ACPI ids; pick whichever is present.
SPI_DEV=
for d in spi-GXFP51A7:00 spi-GXFP5187:00; do
  [ -d "/sys/bus/spi/devices/$d" ] && { SPI_DEV=$d; break; }
done
SPI_DEV=${SPI_DEV:-spi-GXFP5187:00}
DROPIN=/etc/systemd/system/fprintd.service.d/goodixtls.conf

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
warn() { printf '\033[33m!! %s\033[0m\n' "$*"; }
die()  { printf '\033[31m!! %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run this with sudo"

# --- uninstall -------------------------------------------------------
if [ "${1:-}" = "--uninstall" ]; then
  say "Uninstalling"
  rm -f /usr/lib/*/libfprint-2/tod-1/libgoodixtls-tod-driver.so
  rm -f /etc/modprobe.d/goodixtls-spidev.conf
  rm -f /etc/modules-load.d/goodixtls-spidev.conf
  rm -f /etc/udev/rules.d/60-libfprint-2-goodixtls.rules
  rm -f /usr/share/locale/*/LC_MESSAGES/gx-verify.mo
  rm -f /usr/lib/udev/rules.d/60-libfprint-2-goodixtls.rules
  udevadm control --reload 2>/dev/null || true
  rm -f "$DROPIN"
  systemctl daemon-reload
  systemctl try-restart fprintd || true
  echo "Done. Enrolled fingerprints are kept; to erase them:"
  echo "  rm -rf /var/lib/fprint/*/goodixtls /var/lib/fprint/.goodixtls-adapt"
  rm -f /var/lib/fprint/.goodixtls-timing
  exit 0
fi

# --- 1. dependencies -------------------------------------------------
say "Dependencies"
MISSING=()
for p in libfprint-2-tod-dev meson ninja-build build-essential pkg-config \
         libssl-dev libglib2.0-dev libgudev-1.0-dev python3-gi \
         python3-gi-cairo gir1.2-gtk-4.0; do
  dpkg -s "$p" >/dev/null 2>&1 || MISSING+=("$p")
done
if [ ${#MISSING[@]} -gt 0 ]; then
  echo "Installing: ${MISSING[*]}"
  apt-get update -qq && apt-get install -y "${MISSING[@]}"
else
  echo "Already present."
fi

# --- 2. le pilote ----------------------------------------------------
say "Building and installing the driver"
cd "$HERE"
[ -d build ] || meson setup build >/dev/null
ninja -C build >/dev/null
ninja -C build install >/dev/null
echo "Installed into $(pkg-config --variable=tod_driversdir libfprint-2-tod-1)"

# --- 3. system settings ----------------------------------------------
# spidev truncates at 4096 bytes by default, which cuts off the navigation
# frame (4765 B) and the image (22 kB): this was THE main blocker of the project.
say "System settings"
echo "options spidev bufsiz=65536" > /etc/modprobe.d/goodixtls-spidev.conf

# spidev has no alias for this hardware: nothing loads it on its own, and
# without it /sys/bus/spi/drivers/spidev does not exist — the udev rule's bind
# then fails and the sensor simply vanishes from fprintd.
echo spidev > /etc/modules-load.d/goodixtls-spidev.conf

modprobe -r spidev 2>/dev/null || true
modprobe spidev bufsiz=65536

# The udev rule redoes this bind every time the SPI device appears (boot, or
# a rebind after gx-recover.sh).
install -m 0644 "$HERE/60-libfprint-2-goodixtls.rules" \
        /etc/udev/rules.d/60-libfprint-2-goodixtls.rules
udevadm control --reload

# Translation catalogues for gx-verify. Compiled here rather than shipped as
# binaries: .mo files are build artefacts. msgfmt comes with gettext, which is
# not worth making a hard dependency — without it the tool simply runs in
# English, which its gettext fallback already handles.
if command -v msgfmt >/dev/null 2>&1; then
  for po in "$HERE"/po/*.po; do
    [ -e "$po" ] || continue
    lang=$(basename "$po" .po)
    install -d "/usr/share/locale/$lang/LC_MESSAGES"
    msgfmt -o "/usr/share/locale/$lang/LC_MESSAGES/gx-verify.mo" "$po"
    echo "Translation installed: $lang"
  done
else
  warn "msgfmt absent (gettext): gx-verify will run in English."
fi

# fprintd stops after being idle; on screen wake GNOME builds the prompt
# straight away and offers only the password if the service has to be started
# first. RuntimeDirectory is for the driver's runtime files, ProtectSystem=strict
# forbidding it to write anywhere else.
mkdir -p "$(dirname "$DROPIN")"
cat > "$DROPIN" <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/libexec/fprintd --no-timeout
# fprintd.service already declares DeviceAllow= entries. Once any is present
# systemd enforces a closed device policy, and cgroup device filtering is not
# bypassable by root: gx_gpio_reset() then gets EPERM on /dev/gpiochip0 and
# silently skips the reset. SPI keeps working, so the symptom looks like a
# protocol-timing bug rather than a missing permission.
DeviceAllow=/dev/gpiochip0 rw
RuntimeDirectory=goodixtls
RuntimeDirectoryMode=0755
RuntimeDirectoryPreserve=yes
EOF
systemctl daemon-reload

# --- 4. binding the spidev node ---------------------------------------
say "Binding the sensor to spidev"
if [ ! -d "/sys/bus/spi/devices/$SPI_DEV" ]; then
  warn "No supported sensor present on the SPI bus (looked for GXFP51A7, GXFP5187)."
  warn "Check the hardware really is a Goodix GXFP51A7 or GXFP5187 (ACPI path \\_SB_.SPBA)."
else
  echo spidev > "/sys/bus/spi/devices/$SPI_DEV/driver_override"
  # Do not swallow the error: a failed bind shows up nowhere else, and fprintd
  # then simply answers "reader unavailable".
  if [ -e "/sys/bus/spi/devices/$SPI_DEV/driver" ]; then
    echo "Already bound to $(basename "$(readlink -f "/sys/bus/spi/devices/$SPI_DEV/driver")")."
  elif ! echo "$SPI_DEV" > /sys/bus/spi/drivers/spidev/bind 2>/tmp/gx-bind.err; then
    warn "Cannot bind $SPI_DEV to spidev: $(cat /tmp/gx-bind.err)"
  fi
  rm -f /tmp/gx-bind.err
  node=$(ls /dev/spidev* 2>/dev/null | head -1)
  [ -n "$node" ] && echo "Node $node ready." || warn "spidev node missing."
fi

systemctl restart fprintd
say "Done"
cat <<EOF
Enrol a finger, with zone-by-zone guidance:
    python3 $HERE/gx-verify.py

Or through GNOME: Settings → Users → Fingerprint Login.

Session unlock and sudo (the password stays available as a fallback):
    sudo pam-auth-update --enable fprintd

If the sensor stops responding (deep lock-up):
    sudo $HERE/gx-recover.sh
EOF
