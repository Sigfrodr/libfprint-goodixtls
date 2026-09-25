#!/bin/bash
# Goodix GXFP5187 SPI (TLS-PSK) driver for libfprint
#
# Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
# Recovers the sensor from a deep lock-up.
#
# After losing synchronisation — typically from protocol delays that were too
# short — the sensor ends up in a state where SHORT commands still go through
# (the firmware version reads back fine) but LONG transfers fail: the PSK read
# through 0xF2 in particular, so no TLS session can be established at all.
#
# Measured: neither the driver's reset pulse nor reopening the spidev node gets
# it out on its own. What matters is unbinding and rebinding the spidev kernel
# driver, whose state has to be reset. Holding the reset line down for a long
# time is not merely useless, it is counter-productive: recovery fails where a
# short pulse succeeds.
set -e
DEV=spi-GXFP5187:00

# libgpiod 2.x moved the chip behind -c and holds lines until the process
# exits, so a pulse needs "-t 0"; libgpiod 1.x takes the chip positionally and
# releases on exit. Both forms are tried so recovery works with either.
# Never call this through "|| true": a missing or failing gpioset must be
# visible, because a silent failure here is indistinguishable from a sensor
# that will not come back.
gpio_pulse() {  # line assert release
  local line=$1 assert=$2 release=$3
  if gpioset --version 2>/dev/null | grep -qE 'v?2\.'; then
    sudo gpioset -c gpiochip0 -t 0 "$line=$assert" &&
    sudo gpioset -c gpiochip0 -t 0 "$line=$release"
  else
    sudo gpioset gpiochip0 "$line=$assert" &&
    sudo gpioset gpiochip0 "$line=$release"
  fi
}
echo "Stopping fprintd..."     ; sudo systemctl stop fprintd; sleep 1
echo "Unbinding spidev..."    ; echo $DEV | sudo tee /sys/bus/spi/drivers/spidev/unbind >/dev/null 2>&1 || true; sleep 1
# A SHORT pulse, not a hold: measured, keeping the line low for two seconds
# prevents the recovery that a pulse allows. The ingredient that actually
# matters here is the spidev unbind and rebind.
echo "Reset..."
if ! gpio_pulse 58 0 1; then
  echo "WARNING: gpioset failed (libgpiod installed? permission denied?)." >&2
  echo "         The spidev unbind/rebind below is what clears the lock-up." >&2
fi
sleep 2
# Load the module before rebinding: without it the bind directory does not
# exist and the rebind fails silently.
echo "Rebinding spidev..."   ; sudo modprobe spidev
                                echo spidev | sudo tee /sys/bus/spi/devices/$DEV/driver_override >/dev/null
                                echo $DEV | sudo tee /sys/bus/spi/drivers/spidev/bind >/dev/null 2>&1 || true; sleep 1
echo "Restarting fprintd..." ; sudo systemctl start fprintd; sleep 2
echo "Done. Check with:  fprintd-verify \$USER"
