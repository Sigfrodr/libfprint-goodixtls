/*
 * Goodix GXFP5187 SPI (TLS-PSK) driver for libfprint
 *
 * Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "drivers_api.h"

/* Sensor geometry, cross-checked against the vendor driver's own logs. */
#define GOODIX_IMG_WIDTH   132
#define GOODIX_IMG_HEIGHT  112
#define GOODIX_IMG_PIXELS  (GOODIX_IMG_WIDTH * GOODIX_IMG_HEIGHT)

/* SPI framing: 0xA0 is a cleartext message, 0xB0 carries a TLS record. */
#define GOODIX_PKT_PLAIN   0xA0
#define GOODIX_PKT_TLS     0xB0

/*
 * Command table. The first body byte is (cmd0 << 4) | (cmd1 << 1), with
 * cmd0 being: 0x2 image, 0x3 finger detect, 0x5 navigation, 0x8 register,
 * 0x9 chip, 0xA MCU, 0xD TLS connection, 0xF firmware update / memory.
 */
#define GOODIX_CMD_IMAGE       0x20
#define GOODIX_CMD_FDT_DOWN    0x36
#define GOODIX_CMD_NAV         0x50
#define GOODIX_CMD_REG         0x82
#define GOODIX_CMD_ENABLE_CHIP 0x96
#define GOODIX_CMD_MCU_STATE   0xAE
#define GOODIX_CMD_UPLOAD_CFG  0x90
#define GOODIX_CMD_REQUEST_TLS 0xD0
#define GOODIX_CMD_TLS_OK      0xD4
#define GOODIX_CMD_FW_VERSION  0xA8
#define GOODIX_CMD_MEM_READ    0xF2   /* arbitrary memory read: [addr32][len32] */

/*
 * The pre-shared key is a 48-byte PMK that the sensor decrypts into its own RAM
 * at boot; the driver reads it back with the memory-read command above. This is
 * what lets the TLS channel be established on Linux without Intel ME, SGX or
 * the in-application-programming mode that other approaches rely on — and it is
 * non-destructive, unlike rewriting the key through IAP.
 *
 * TLS identity is "Client_identity", ciphersuite TLS-PSK-WITH-AES-128-CBC-SHA256.
 * Note the roles are reversed from what one might expect: the SENSOR is the TLS
 * client and the host is the server.
 */
#define GOODIX_PSK_ADDR   0x20007f0c
#define GOODIX_PSK_LEN    48
#define GOODIX_TLS_IDENTITY "Client_identity"

/* Finger detection reads twelve 16-bit values, which a finger pulls DOWN.
 *
 * Measured in use, logging every poll: idle sits at a mean of 363 with a drop
 * of 0, while a finger gives a mean of 266 to 314 and a drop of 48 to 86. The
 * original thresholds of 50 and 320 sat right at the finger end of that range,
 * so a light or slightly offset press produced a drop of about 45 at a mean of
 * about 325 and fired NEITHER test — the finger simply went unnoticed until
 * the user lifted and pressed again.
 *
 * They are now placed between the two populations instead. Loosening them is
 * cheap: a spurious detection costs one wasted capture, which the contrast
 * check then rejects, whereas a missed detection is a visible failure. */
#define GOODIX_FDT_DROP 30

/* Absolute threshold on the mean of those twelve values. Needed because the
 * relative threshold above is useless when the baseline itself was taken with
 * a finger resting on the sensor — exactly when detection would otherwise fail
 * silently.
 *
 * This is NOT used as a fixed number at runtime: it is the fallback the driver
 * starts from, then recomputes per unit as (measured idle mean − margin) once
 * it has a baseline. On the author's sensor idle sits near 364, so the derived
 * floor lands near 340 — the value this used to be hard-coded to. On another
 * unit whose idle sits elsewhere, a fixed 340 was the portability bug behind
 * the "huge amount of failed attempts" report: a sensor idling below it reads
 * as a finger on every poll and captures noise. See self->fdt_abs. */
#define GOODIX_FDT_ABS  340

/* How far below the measured idle mean the per-unit absolute floor sits.
 * Chosen so idle≈364 − margin reproduces the previous 340 on the author's
 * unit; it is the gap between the idle population and the weakest finger
 * press, kept roughly centred between the two. */
#define GOODIX_FDT_ABS_MARGIN 24

/* Frame checksum. */
static inline guint8
goodix_body_cksum (const guint8 *b, gsize n)
{
  guint8 s = 0;

  while (n--)
    s += *b++;
  return (guint8) (0xAA - s);
}
