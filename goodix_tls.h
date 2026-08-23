/*
 * TLS-PSK channel for the Goodix GXFP5187 sensor
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

/*
 * The sensor speaks TLS 1.2 with TLS_PSK_WITH_AES_128_CBC_SHA256, and — with
 * the roles reversed from what one might expect — it is the CLIENT while the
 * host is the server.
 *
 * It also departs from the spec in one way that matters: it sends the whole
 * fingerprint image in a SINGLE record of about 22 kB, against the 16384-byte
 * plaintext ceiling TLS sets. Verified by walking the record headers on the
 * wire (type=17 version=0303 length=22240). Nothing on the receiving side can
 * change that: in TLS the sender alone decides how to fragment, and the only
 * standard lever — the max_fragment_length extension of RFC 6066 — is offered
 * by the client, which here is the sensor.
 *
 * Every compliant TLS library therefore refuses to read that record. Rather
 * than ship a patched crypto library, which no distribution would package and
 * no upstream would accept, this module lets a stock OpenSSL perform the
 * handshake and the (small) writes, then derives the connection keys from the
 * negotiated master secret and decrypts the oversized record itself.
 *
 * A note on what this does and does not protect: the channel provides no
 * security to the host. The pre-shared key is read out of the sensor's own RAM
 * over the same SPI bus, so anyone able to talk to the sensor can obtain it.
 * This is vendor obfuscation, not a trust boundary — which is also why
 * handling the record layer here is not the hazard it would normally be.
 */

#pragma once

#include <gio/gio.h>

typedef struct _GxTls GxTls;

/* Transport hooks: both must block until they have moved some bytes, and
 * return the count, or a negative value on failure. */
typedef int (*GxTlsSend) (gpointer user, const guint8 *buf, gsize len);
typedef int (*GxTlsRecv) (gpointer user, guint8 *buf, gsize cap);

GxTls *gx_tls_new (const guint8 *psk, gsize psk_len, const gchar *identity,
                   GxTlsSend send, GxTlsRecv recv, gpointer user);
void   gx_tls_free (GxTls *tls);

/* Runs the server side of the handshake and derives the connection keys.
 * Returns FALSE and sets @error on failure. */
gboolean gx_tls_handshake_run (GxTls *tls, GError **error);

/* Name of the negotiated ciphersuite, for logging. */
const gchar *gx_tls_ciphersuite (GxTls *tls);

/* Sends application data. Small commands only; goes through OpenSSL. */
gboolean gx_tls_write (GxTls *tls, const guint8 *buf, gsize len);

/* Reads ONE application-data record and decrypts it into @out.
 *
 * This deliberately bypasses OpenSSL, which would reject the oversized image
 * record. @raw is the record exactly as it came off the wire, header included.
 * Returns the plaintext length, or -1 on failure. */
gssize gx_tls_decrypt_record (GxTls *tls, const guint8 *raw, gsize raw_len,
                              guint8 *out, gsize out_cap);

void gx_tls_close (GxTls *tls);
