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
echo "Stopping fprintd..."     ; sudo systemctl stop fprintd; sleep 1
echo "Unbinding spidev..."    ; echo $DEV | sudo tee /sys/bus/spi/drivers/spidev/unbind >/dev/null 2>&1 || true; sleep 1
# gpioset releases the line as soon as it exits: without "-m time" the reset is
# not HELD, it is only brushed — and the sensor does not come back from it.
# A SHORT pulse, not a hold: measured, keeping the line low for two seconds
# prevents the recovery that a pulse allows. The ingredient that actually
# matters here is the spidev unbind and rebind.
echo "Reset..."               ; sudo gpioset gpiochip0 58=0 || true; sleep 2
                                sudo gpioset gpiochip0 58=1 || true; sleep 2
# Load the module before rebinding: without it the bind directory does not
# exist and the rebind fails silently.
echo "Rebinding spidev..."   ; sudo modprobe spidev
                                echo spidev | sudo tee /sys/bus/spi/devices/$DEV/driver_override >/dev/null
                                echo $DEV | sudo tee /sys/bus/spi/drivers/spidev/bind >/dev/null 2>&1 || true; sleep 1
echo "Restarting fprintd..." ; sudo systemctl start fprintd; sleep 2
echo "Done. Check with:  fprintd-verify \$USER"
