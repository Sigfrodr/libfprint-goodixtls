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

#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>

#include "goodix_tls.h"

/* TLS_PSK_WITH_AES_128_CBC_SHA256 key material. */
#define MAC_KEY_LEN  32
#define ENC_KEY_LEN  16
#define IV_LEN       16
#define MAC_LEN      32
#define KEY_BLOCK_LEN (2 * MAC_KEY_LEN + 2 * ENC_KEY_LEN)

struct _GxTls
{
  SSL_CTX   *ctx;
  SSL       *ssl;
  BIO_METHOD *biom;

  GxTlsSend  send;
  GxTlsRecv  recv;
  gpointer   user;

  guint8     psk[64];
  gsize      psk_len;
  gchar     *identity;

  /* Derived once the handshake completes. The sensor is the client, so its
   * records are protected with the client_write_* material. */
  guint8     client_mac[MAC_KEY_LEN];
  guint8     client_key[ENC_KEY_LEN];
  guint8     server_mac[MAC_KEY_LEN];
  guint8     server_key[ENC_KEY_LEN];
  guint64    read_seq;
  gboolean   keys_ready;
};

/* ------------------------------------------------------------------ */
/*  Transport glue                                                     */
/* ------------------------------------------------------------------ */

static int
bio_write_cb (BIO *b, const char *buf, int len)
{
  GxTls *t = BIO_get_data (b);
  int r = t->send (t->user, (const guint8 *) buf, len);

  BIO_clear_retry_flags (b);
  return r > 0 ? r : -1;
}

static int
bio_read_cb (BIO *b, char *buf, int len)
{
  GxTls *t = BIO_get_data (b);
  int r = t->recv (t->user, (guint8 *) buf, len);

  BIO_clear_retry_flags (b);
  return r > 0 ? r : -1;
}

static long
bio_ctrl_cb (BIO *b, int cmd, long num, void *ptr)
{
  return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

static int
bio_create_cb (BIO *b)
{
  BIO_set_init (b, 1);
  return 1;
}

/* ------------------------------------------------------------------ */
/*  Key derivation (TLS 1.2 PRF with SHA-256)                          */
/* ------------------------------------------------------------------ */

/* P_hash from RFC 5246 section 5: A(0) = seed, A(i) = HMAC(secret, A(i-1)),
 * output = HMAC(secret, A(1) || seed) || HMAC(secret, A(2) || seed) || ... */
static void
tls_prf (const guint8 *secret, gsize secret_len,
         const gchar *label, const guint8 *seed, gsize seed_len,
         guint8 *out, gsize out_len)
{
  gsize label_len = strlen (label);
  g_autofree guint8 *ls = g_malloc (label_len + seed_len);
  guint8 a[EVP_MAX_MD_SIZE];
  unsigned int a_len = 0;
  gsize done = 0;

  memcpy (ls, label, label_len);
  memcpy (ls + label_len, seed, seed_len);

  /* A(1) */
  HMAC (EVP_sha256 (), secret, secret_len, ls, label_len + seed_len, a, &a_len);

  while (done < out_len)
    {
      guint8 block[EVP_MAX_MD_SIZE];
      unsigned int block_len = 0;
      g_autofree guint8 *tmp = g_malloc (a_len + label_len + seed_len);
      gsize take;

      memcpy (tmp, a, a_len);
      memcpy (tmp + a_len, ls, label_len + seed_len);
      HMAC (EVP_sha256 (), secret, secret_len, tmp,
            a_len + label_len + seed_len, block, &block_len);

      take = MIN (block_len, out_len - done);
      memcpy (out + done, block, take);
      done += take;

      /* A(i+1) = HMAC(secret, A(i)) */
      HMAC (EVP_sha256 (), secret, secret_len, a, a_len, a, &a_len);
    }
}

static gboolean
derive_keys (GxTls *t, GError **error)
{
  SSL_SESSION *sess = SSL_get_session (t->ssl);
  guint8 master[48], cr[32], sr[32], seed[64], kb[KEY_BLOCK_LEN];
  gsize n;

  if (!sess)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no TLS session");
      return FALSE;
    }
  n = SSL_SESSION_get_master_key (sess, master, sizeof master);
  if (n != sizeof master)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "unexpected master secret length %zu", n);
      return FALSE;
    }
  if (SSL_get_client_random (t->ssl, cr, sizeof cr) != sizeof cr ||
      SSL_get_server_random (t->ssl, sr, sizeof sr) != sizeof sr)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read randoms");
      return FALSE;
    }

  /* key_block = PRF(master_secret, "key expansion",
   *                 server_random + client_random)  — note the order. */
  memcpy (seed, sr, 32);
  memcpy (seed + 32, cr, 32);
  tls_prf (master, sizeof master, "key expansion", seed, sizeof seed,
           kb, sizeof kb);

  memcpy (t->client_mac, kb, MAC_KEY_LEN);
  memcpy (t->server_mac, kb + MAC_KEY_LEN, MAC_KEY_LEN);
  memcpy (t->client_key, kb + 2 * MAC_KEY_LEN, ENC_KEY_LEN);
  memcpy (t->server_key, kb + 2 * MAC_KEY_LEN + ENC_KEY_LEN, ENC_KEY_LEN);

  /* Sequence numbers restart at zero when the cipher state changes, and the
   * Finished message is the first record under the new keys. Application data
   * from the sensor therefore starts at 1. The MAC check below will tell us
   * loudly if that assumption ever stops holding. */
  t->read_seq = 1;
  t->keys_ready = TRUE;

  OPENSSL_cleanse (master, sizeof master);
  OPENSSL_cleanse (kb, sizeof kb);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Handshake                                                          */
/* ------------------------------------------------------------------ */

static unsigned int
psk_server_cb (SSL *ssl, const char *identity, unsigned char *psk,
               unsigned int max_psk_len)
{
  GxTls *t = SSL_get_ex_data (ssl, 0);

  if (!t || t->psk_len > max_psk_len)
    return 0;
  memcpy (psk, t->psk, t->psk_len);
  return (unsigned int) t->psk_len;
}

GxTls *
gx_tls_new (const guint8 *psk, gsize psk_len, const gchar *identity,
            GxTlsSend send, GxTlsRecv recv, gpointer user)
{
  GxTls *t;

  if (!psk || psk_len == 0 || psk_len > sizeof t->psk)
    return NULL;

  t = g_new0 (GxTls, 1);
  memcpy (t->psk, psk, psk_len);
  t->psk_len = psk_len;
  t->identity = g_strdup (identity);
  t->send = send;
  t->recv = recv;
  t->user = user;
  return t;
}

gboolean
gx_tls_handshake_run (GxTls *t, GError **error)
{
  BIO *bio;
  int r;

  t->ctx = SSL_CTX_new (TLS_server_method ());
  if (!t->ctx)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "SSL_CTX_new failed");
      return FALSE;
    }

  /* The sensor only speaks TLS 1.2 with a PSK CBC suite. Those are below
   * OpenSSL's default security level, hence SECLEVEL=0 — appropriate here,
   * where the cipher choice is the peer's and not ours to improve. */
  SSL_CTX_set_min_proto_version (t->ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (t->ctx, TLS1_2_VERSION);
  if (!SSL_CTX_set_cipher_list (t->ctx, "PSK-AES128-CBC-SHA256:@SECLEVEL=0"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "PSK-AES128-CBC-SHA256 unavailable in this OpenSSL build");
      return FALSE;
    }
  SSL_CTX_set_psk_server_callback (t->ctx, psk_server_cb);
  if (t->identity)
    SSL_CTX_use_psk_identity_hint (t->ctx, t->identity);

  t->ssl = SSL_new (t->ctx);
  if (!t->ssl)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "SSL_new failed");
      return FALSE;
    }
  SSL_set_ex_data (t->ssl, 0, t);

  t->biom = BIO_meth_new (BIO_get_new_index () | BIO_TYPE_SOURCE_SINK,
                          "goodix-spi");
  BIO_meth_set_write (t->biom, bio_write_cb);
  BIO_meth_set_read (t->biom, bio_read_cb);
  BIO_meth_set_ctrl (t->biom, bio_ctrl_cb);
  BIO_meth_set_create (t->biom, bio_create_cb);

  bio = BIO_new (t->biom);
  BIO_set_data (bio, t);
  SSL_set_bio (t->ssl, bio, bio);          /* SSL takes ownership */

  r = SSL_accept (t->ssl);
  if (r != 1)
    {
      unsigned long e = ERR_get_error ();
      char buf[256] = "";

      if (e)
        ERR_error_string_n (e, buf, sizeof buf);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "TLS handshake failed (%d/%d) %s", r,
                   SSL_get_error (t->ssl, r), buf);
      return FALSE;
    }
  return derive_keys (t, error);
}

const gchar *
gx_tls_ciphersuite (GxTls *t)
{
  return t && t->ssl ? SSL_get_cipher (t->ssl) : "none";
}

gboolean
gx_tls_write (GxTls *t, const guint8 *buf, gsize len)
{
  return t && t->ssl && SSL_write (t->ssl, buf, (int) len) == (int) len;
}

/* ------------------------------------------------------------------ */
/*  Record decryption                                                  */
/* ------------------------------------------------------------------ */

gssize
gx_tls_decrypt_record (GxTls *t, const guint8 *raw, gsize raw_len,
                       guint8 *out, gsize out_cap)
{
  EVP_CIPHER_CTX *c;
  const guint8 *body;
  gsize body_len;
  int plain_len = 0, tmp = 0;
  g_autofree guint8 *plain = NULL;
  guint8 mac[MAC_LEN], want[EVP_MAX_MD_SIZE], hdr[13];
  unsigned int want_len = 0;
  gsize content_len, pad;

  if (!t || !t->keys_ready || raw_len < 5 + IV_LEN + MAC_LEN)
    return -1;
  if (raw[0] != 23)                         /* application data */
    return -1;

  body = raw + 5;
  body_len = ((gsize) raw[3] << 8) | raw[4];
  if (body_len + 5 > raw_len || body_len < IV_LEN + MAC_LEN ||
      (body_len - IV_LEN) % IV_LEN != 0)
    return -1;

  /* AES-128-CBC, explicit IV in front of the ciphertext (TLS 1.2). */
  plain = g_malloc (body_len);
  c = EVP_CIPHER_CTX_new ();
  if (!c)
    return -1;
  EVP_DecryptInit_ex (c, EVP_aes_128_cbc (), NULL, t->client_key, body);
  EVP_CIPHER_CTX_set_padding (c, 0);        /* TLS padding, not PKCS#7 */
  if (!EVP_DecryptUpdate (c, plain, &plain_len, body + IV_LEN,
                          (int) (body_len - IV_LEN)) ||
      !EVP_DecryptFinal_ex (c, plain + plain_len, &tmp))
    {
      EVP_CIPHER_CTX_free (c);
      return -1;
    }
  EVP_CIPHER_CTX_free (c);
  plain_len += tmp;

  /* plaintext = content || MAC || padding, where every padding byte carries
   * the padding length and the final byte is that length. */
  pad = (gsize) plain[plain_len - 1] + 1;
  if ((gsize) plain_len < pad + MAC_LEN)
    return -1;
  content_len = (gsize) plain_len - pad - MAC_LEN;
  if (content_len > out_cap)
    return -1;
  memcpy (mac, plain + content_len, MAC_LEN);

  /* MAC covers seq_num || type || version || length || content. Verifying it
   * also confirms the sequence number is in step; a mismatch here is the loud
   * failure that would otherwise show up as silently corrupt images. */
  for (int i = 0; i < 8; i++)
    hdr[i] = (guint8) (t->read_seq >> (56 - 8 * i));
  hdr[8] = raw[0];
  hdr[9] = raw[1];
  hdr[10] = raw[2];
  hdr[11] = (guint8) (content_len >> 8);
  hdr[12] = (guint8) content_len;

  {
    g_autofree guint8 *msg = g_malloc (sizeof hdr + content_len);

    memcpy (msg, hdr, sizeof hdr);
    memcpy (msg + sizeof hdr, plain, content_len);
    HMAC (EVP_sha256 (), t->client_mac, MAC_KEY_LEN, msg,
          sizeof hdr + content_len, want, &want_len);
  }
  if (want_len != MAC_LEN || CRYPTO_memcmp (want, mac, MAC_LEN) != 0)
    return -1;

  memcpy (out, plain, content_len);
  t->read_seq++;
  return (gssize) content_len;
}

void
gx_tls_close (GxTls *t)
{
  if (t && t->ssl)
    SSL_shutdown (t->ssl);
}

void
gx_tls_free (GxTls *t)
{
  if (!t)
    return;
  if (t->ssl)
    SSL_free (t->ssl);                      /* frees the BIO too */
  if (t->ctx)
    SSL_CTX_free (t->ctx);
  if (t->biom)
    BIO_meth_free (t->biom);
  OPENSSL_cleanse (t->psk, sizeof t->psk);
  OPENSSL_cleanse (t->client_key, sizeof t->client_key);
  OPENSSL_cleanse (t->client_mac, sizeof t->client_mac);
  g_free (t->identity);
  g_free (t);
}
