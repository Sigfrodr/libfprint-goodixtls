/* TLS interoperability tests with an OpenSSL client and synthetic PSK.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>
#include "goodix_tls.h"

typedef struct
{
  SSL_CTX *ctx;
  SSL *ssl;
  GxTls *server;
  gboolean gcm;
} Peer;

typedef struct
{
  const gchar *cipher;
  gboolean legacy_hello;
} TestCase;

static const guint8 test_psk[48] = { 0x42, 0x19, 0x87 };

static unsigned int
client_psk (SSL *ssl, const char *hint, char *identity,
            unsigned int identity_cap, unsigned char *psk, unsigned int psk_cap)
{
  g_assert_cmpuint (identity_cap, >, strlen ("Client_identity"));
  g_assert_cmpuint (psk_cap, >=, sizeof test_psk);
  strcpy (identity, "Client_identity");
  memcpy (psk, test_psk, sizeof test_psk);
  return sizeof test_psk;
}

static void
advance_client (Peer *p)
{
  int r = SSL_do_handshake (p->ssl);
  if (r != 1)
    g_assert_cmpint (SSL_get_error (p->ssl, r), ==, SSL_ERROR_WANT_READ);
}

static int
send_to_client (gpointer user, const guint8 *buf, gsize len)
{
  Peer *p = user;
  int n = BIO_write (SSL_get_rbio (p->ssl), buf, len);
  g_assert_cmpint (n, ==, len);
  advance_client (p);
  return n;
}

static int
recv_from_client (gpointer user, guint8 *buf, gsize cap)
{
  Peer *p = user;
  return BIO_read (SSL_get_wbio (p->ssl), buf, cap);
}

static void
peer_init (Peer *p, const TestCase *test)
{
  g_autoptr(GError) error = NULL;
  const gchar *cipher = test->cipher;
  g_autofree gchar *list = g_strconcat (cipher, ":@SECLEVEL=0", NULL);
  unsigned int session_id_len;
  p->gcm = strstr (cipher, "GCM") != NULL;
  p->ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (p->ctx);
  g_assert_cmpint (SSL_CTX_set_min_proto_version (p->ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_max_proto_version (p->ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_cipher_list (p->ctx, list), ==, 1);
  /* The sensor uses MAC-then-encrypt for CBC. Test both a modern client
   * offering tickets/EMS and a legacy hello like the sensor's. */
  SSL_CTX_set_options (p->ctx, SSL_OP_NO_ENCRYPT_THEN_MAC);
  if (test->legacy_hello)
    SSL_CTX_set_options (p->ctx,
                         SSL_OP_NO_TICKET | SSL_OP_NO_EXTENDED_MASTER_SECRET);
  SSL_CTX_set_psk_client_callback (p->ctx, client_psk);
  p->ssl = SSL_new (p->ctx);
  g_assert_nonnull (p->ssl);
  SSL_set_bio (p->ssl, BIO_new (BIO_s_mem ()), BIO_new (BIO_s_mem ()));
  SSL_set_connect_state (p->ssl);
  p->server = gx_tls_new (test_psk, sizeof test_psk, "Client_identity",
                          send_to_client, recv_from_client, p);
  g_assert_nonnull (p->server);
  advance_client (p);
  g_assert_true (gx_tls_handshake_run (p->server, &error));
  g_assert_no_error (error);
  g_assert_true (SSL_is_init_finished (p->ssl));
  g_assert_cmpstr (gx_tls_ciphersuite (p->server), ==, cipher);
  SSL_SESSION_get_id (SSL_get_session (p->ssl), &session_id_len);
  g_assert_cmpuint (session_id_len, ==, 0);
  g_assert_false (SSL_SESSION_has_ticket (SSL_get_session (p->ssl)));
}

/* OpenSSL normally fragments at 16 KiB. Build the sensor's oversized record
 * using OpenSSL's independent TLS1-PRF implementation and the CLIENT session. */
static GByteArray *
oversized_record (Peer *p, const guint8 *data, gsize len, guint64 seq)
{
  guint8 master[48], sr[32], cr[32], kb[96], aad[13];
  gsize kb_len = p->gcm ? 40 : 96;
  EVP_PKEY_CTX *kdf = EVP_PKEY_CTX_new_id (EVP_PKEY_TLS1_PRF, NULL);
  g_assert_nonnull (kdf);
  g_assert_cmpuint (SSL_SESSION_get_master_key (SSL_get_session (p->ssl),
                                               master, sizeof master), ==, 48);
  g_assert_cmpuint (SSL_get_server_random (p->ssl, sr, sizeof sr), ==, 32);
  g_assert_cmpuint (SSL_get_client_random (p->ssl, cr, sizeof cr), ==, 32);
  g_assert_cmpint (EVP_PKEY_derive_init (kdf), ==, 1);
  g_assert_cmpint (EVP_PKEY_CTX_set_tls1_prf_md (kdf, EVP_sha256 ()), ==, 1);
  g_assert_cmpint (EVP_PKEY_CTX_set1_tls1_prf_secret (kdf, master, sizeof master), ==, 1);
  g_assert_cmpint (EVP_PKEY_CTX_add1_tls1_prf_seed (kdf,
                                                 (const guint8 *) "key expansion",
                                                 13), ==, 1);
  g_assert_cmpint (EVP_PKEY_CTX_add1_tls1_prf_seed (kdf, sr, sizeof sr), ==, 1);
  g_assert_cmpint (EVP_PKEY_CTX_add1_tls1_prf_seed (kdf, cr, sizeof cr), ==, 1);
  g_assert_cmpint (EVP_PKEY_derive (kdf, kb, &kb_len), ==, 1);
  EVP_PKEY_CTX_free (kdf);

  for (int i = 0; i < 8; i++)
    aad[i] = seq >> (56 - 8 * i);
  aad[8] = 23;
  aad[9] = aad[10] = 3;
  aad[11] = len >> 8;
  aad[12] = len;

  GByteArray *record = g_byte_array_sized_new (len + 128);
  g_byte_array_set_size (record, len + 128);
  guint8 *body = record->data + 5;
  EVP_CIPHER_CTX *enc = EVP_CIPHER_CTX_new ();
  g_assert_nonnull (enc);
  int n = 0, tail = 0;
  gsize body_len;

  if (p->gcm)
    {
      guint8 nonce[12];
      memcpy (nonce, kb + 32, 4);
      /* Deliberately different from the sequence number: explicit nonces
       * are not required to equal the TLS record sequence. */
      memset (nonce + 4, 0xa7, 8);
      memcpy (body, nonce + 4, 8);
      g_assert_cmpint (EVP_EncryptInit_ex (enc, EVP_aes_128_gcm (), NULL, kb, nonce), ==, 1);
      g_assert_cmpint (EVP_EncryptUpdate (enc, NULL, &n, aad, sizeof aad), ==, 1);
      g_assert_cmpint (EVP_EncryptUpdate (enc, body + 8, &n, data, len), ==, 1);
      g_assert_cmpint (EVP_EncryptFinal_ex (enc, body + 8 + n, &tail), ==, 1);
      g_assert_cmpint (n + tail, ==, len);
      g_assert_cmpint (EVP_CIPHER_CTX_ctrl (enc, EVP_CTRL_GCM_GET_TAG, 16, body + 8 + len), ==, 1);
      body_len = 8 + len + 16;
    }
  else
    {
      g_autofree guint8 *msg = g_malloc (sizeof aad + len);
      g_autofree guint8 *plain = g_malloc (len + 32 + 16);
      guint8 mac[32];
      unsigned int mac_len = 0;
      memcpy (msg, aad, sizeof aad);
      memcpy (msg + sizeof aad, data, len);
      g_assert_nonnull (HMAC (EVP_sha256 (), kb, 32, msg, sizeof aad + len, mac, &mac_len));
      g_assert_cmpuint (mac_len, ==, 32);
      gsize pad = 16 - (len + 32) % 16;
      memcpy (plain, data, len);
      memcpy (plain + len, mac, 32);
      memset (plain + len + 32, pad - 1, pad);
      memset (body, 0x3c, 16);
      g_assert_cmpint (EVP_EncryptInit_ex (enc, EVP_aes_128_cbc (), NULL, kb + 64, body), ==, 1);
      g_assert_cmpint (EVP_CIPHER_CTX_set_padding (enc, 0), ==, 1);
      g_assert_cmpint (EVP_EncryptUpdate (enc, body + 16, &n, plain, len + 32 + pad), ==, 1);
      g_assert_cmpint (EVP_EncryptFinal_ex (enc, body + 16 + n, &tail), ==, 1);
      body_len = 16 + n + tail;
    }
  EVP_CIPHER_CTX_free (enc);
  record->data[0] = 23;
  record->data[1] = record->data[2] = 3;
  record->data[3] = body_len >> 8;
  record->data[4] = body_len;
  g_byte_array_set_size (record, body_len + 5);
  OPENSSL_cleanse (master, sizeof master);
  OPENSSL_cleanse (kb, sizeof kb);
  return record;
}

static void
check_record (Peer *p, guint8 *raw, gsize raw_len, const guint8 *plain, gsize len)
{
  g_autofree guint8 *out = g_malloc (len + 1);
  memset (out, 0xa5, len + 1);
  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len - 1, out, len), ==, -1);
  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len, out, len - 1), ==, -1);
  raw[0] ^= 1;
  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len, out, len), ==, -1);
  raw[0] ^= 1;
  raw[2] ^= 1;
  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len, out, len), ==, -1);
  raw[2] ^= 1;
  raw[raw_len - 1] ^= 1;
  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len, out, len), ==, -1);
  raw[raw_len - 1] ^= 1;
  for (gsize i = 0; i <= len; i++)
    g_assert_cmpuint (out[i], ==, 0xa5);

  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len, out, len), ==, len);
  g_assert_cmpmem (out, len, plain, len);
  g_assert_cmpuint (out[len], ==, 0xa5);
  memset (out, 0xa5, len + 1);
  g_assert_cmpint (gx_tls_decrypt_record (p->server, raw, raw_len, out, len), ==, -1);
  for (gsize i = 0; i <= len; i++)
    g_assert_cmpuint (out[i], ==, 0xa5);
}

static void
test_cipher (gconstpointer data)
{
  Peer p = { 0 };
  const gsize sizes[] = { 1, 15, 16, 17, 1024, 16384 };
  guint8 plain[22200], raw[23000];
  for (gsize i = 0; i < sizeof plain; i++)
    plain[i] = i * 37 + 11;
  peer_init (&p, data);
  for (gsize i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
      g_assert_cmpint (SSL_write (p.ssl, plain, sizes[i]), ==, sizes[i]);
      int n = BIO_read (SSL_get_wbio (p.ssl), raw, sizeof raw);
      g_assert_cmpint (n, >, 0);
      g_assert_cmpint (BIO_pending (SSL_get_wbio (p.ssl)), ==, 0);
      check_record (&p, raw, n, plain, sizes[i]);
    }
  g_autoptr(GByteArray) big = oversized_record (&p, plain, sizeof plain, 1 + G_N_ELEMENTS (sizes));
  check_record (&p, big->data, big->len, plain, sizeof plain);
  gx_tls_free (p.server);
  SSL_free (p.ssl);
  SSL_CTX_free (p.ctx);
}

int
main (int argc, char **argv)
{
  static const TestCase cases[] = {
    { "PSK-AES128-CBC-SHA256", FALSE },
    { "PSK-AES128-GCM-SHA256", FALSE },
    { "PSK-AES128-CBC-SHA256", TRUE },
    { "PSK-AES128-GCM-SHA256", TRUE },
  };

  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/tls/cbc", &cases[0], test_cipher);
  g_test_add_data_func ("/tls/gcm", &cases[1], test_cipher);
  g_test_add_data_func ("/tls/cbc-legacy-hello", &cases[2], test_cipher);
  g_test_add_data_func ("/tls/gcm-legacy-hello", &cases[3], test_cipher);
  return g_test_run ();
}
