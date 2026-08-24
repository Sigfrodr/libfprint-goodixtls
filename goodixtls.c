/*
 * Goodix GXFP5187 SPI (TLS-PSK) driver for libfprint
 *
 * Copyright (C) 2026 Benjamin Allègre (https://github.com/Sigfrodr)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Driver for the SPI fingerprint sensor found in the Huawei MateBook X Pro,
 * whose protocol was reverse-engineered for this work. The chain is: SPI
 * dialogue where one frame must be exactly one transfer, opening the command
 * gate by uploading a configuration blob, a TLS-PSK channel whose key is read
 * out of the sensor's own RAM, and a 132x112 image in packed 12-bit samples
 * (six bytes carry four interleaved pixels) calibrated by subtracting a
 * background frame.
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
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#define FP_COMPONENT "goodixtls"
#define _GNU_SOURCE

#include <errno.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>

#include "goodixtls.h"
#include "goodix_sift.h"
#include "goodix_tls.h"

/* Frames averaged for background and finger: a noise/latency trade-off. */
#define GOODIX_BG_FRAMES     1
#define GOODIX_FINGER_FRAMES 3
/* Finger detection threshold on the standard deviation of the
 * background-minus-frame difference. */
#define GOODIX_FINGER_STD    150.0
/* Largest frame body we ever receive: the image, at about 22 kB. */
#define GOODIX_RX_MAX        24000

struct _FpiDeviceGoodixTls
{
  FpDevice parent;

  int           spi_fd;
  FpiSsm       *task_ssm;

  /* TLS stack. Note the sensor is the client and we are the server. */
  GxTls        *tls;
  gboolean      tls_up;

  guint8        psk[GOODIX_PSK_LEN];

  /* Reassembly buffer for TLS records, drained by the BIO recv callback. */
  guint8        tls_rx[GOODIX_RX_MAX];
  int           tls_rxlen, tls_rxpos;

  guint16      *bg_frame;    /* averaged background, NULL until calibrated */
  guint         poll_id;     /* finger-detection timeout source */
  int           poll_count;  /* poll iterations, bounded to avoid hanging */
  GPtrArray    *enroll_feats;/* descriptor sets accumulated during enrolment */
  int           fdt_base[12];/* FDT baseline (finger absent) */
  gboolean      have_fdt;    /* is fdt_base populated? */
  int           fdt_abs;     /* per-unit absolute floor, derived from baseline */
  int           timing_scale;/* protocol-delay multiplier in %, grows on desync */
  int           timing_saved;/* last value persisted to disk, to avoid rewrites */
  gboolean      bg_dirty;    /* background taken with a finger down */
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls, fpi_device_goodixtls, FPI,
                      DEVICE_GOODIXTLS, FpDevice)
G_DEFINE_TYPE (FpiDeviceGoodixTls, fpi_device_goodixtls, FP_TYPE_DEVICE)

G_MODULE_EXPORT GType
fpi_tod_shared_driver_get_type (void)
{
  return fpi_device_goodixtls_get_type ();
}

static const FpIdEntry goodixtls_id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_id = "GXFP5187" },
  { .udev_types = 0 }
};

/* Fixed startup commands, byte-for-byte as validated against the sensor. */
static const guint8 GX_AMORCE[] = { 0, 5, 0, 0, 0, 0, 0, 0x88 };
static const guint8 GX_ENABLE[] = { 0x96, 0x03, 0x00, 0x01, 0x00, 0x10 };
static const guint8 GX_REQTLS[] = { 0xd0, 0x03, 0x00, 0x00, 0x00, 0xd7 };

/* ------------------------------------------------------------------ */
/*  SPI transport: one frame is exactly one transfer                   */
/* ------------------------------------------------------------------ */

static gboolean
gx_write_frame (FpiDeviceGoodixTls *self, guint8 type,
                const guint8 *body, gsize n)
{
  g_autofree guint8 *buf = g_malloc (n + 4);
  struct spi_ioc_transfer xfer = { 0 };

  buf[0] = type;
  buf[1] = n & 0xFF;
  buf[2] = (n >> 8) & 0xFF;
  buf[3] = buf[0] + buf[1] + buf[2];
  memcpy (buf + 4, body, n);

  xfer.tx_buf = (unsigned long) buf;
  xfer.len = n + 4;
  return ioctl (self->spi_fd, SPI_IOC_MESSAGE (1), &xfer) >= 1;
}

/* Reads one frame: a 4-byte header then the body. Returns the body length,
 * or -1 on failure. */
static int
gx_read_frame (FpiDeviceGoodixTls *self, guint8 *out_type,
               guint8 *rx, gsize rx_cap)
{
  guint8 hdr[4] = { 0 };
  struct spi_ioc_transfer xh = { 0 }, xb = { 0 };
  guint16 n;

  xh.rx_buf = (unsigned long) hdr;
  xh.len = 4;
  if (ioctl (self->spi_fd, SPI_IOC_MESSAGE (1), &xh) < 1)
    return -1;
  if (hdr[0] != GOODIX_PKT_PLAIN && hdr[0] != GOODIX_PKT_TLS)
    return -1;
  *out_type = hdr[0];
  n = hdr[1] | (hdr[2] << 8);
  if (n == 0 || n > rx_cap)
    return -1;

  g_usleep (200);                    /* let the sensor stage the body */
  xb.rx_buf = (unsigned long) rx;
  xb.len = n;
  if (ioctl (self->spi_fd, SPI_IOC_MESSAGE (1), &xb) < 1)
    return -1;
  return n;
}

/* Sends the preamble (A0), then a plaintext command (A0). */
static gboolean
gx_send_plain_raw (FpiDeviceGoodixTls *self, const guint8 *body, gsize n)
{
  if (!gx_write_frame (self, GOODIX_PKT_PLAIN, GX_AMORCE, sizeof GX_AMORCE))
    return FALSE;
  g_usleep (8000);
  return gx_write_frame (self, GOODIX_PKT_PLAIN, body, n);
}

/* Sends a cleartext command and drains every plain response. A TLS record
 * arriving instead is stashed for the BIO layer to pick up. */
static void
gx_send_plain_drain (FpiDeviceGoodixTls *self, const guint8 *body, gsize n)
{
/* Empty reads, 10 ms apart, that count as silence after a response. This is
 * the dominant cost of a capture: each of the nine commands in the sequence
 * pays it. Cut it too short and frame boundaries desynchronise, leaving the
 * sensor wedged in a state only a hard reset plus an spidev rebind recovers.
 *
 * Measured over 6 to 8 captures per setting:
 *   6 -> 1319 ms, no failures       (original value)
 *   4 -> 1136 ms, no failures       <- kept
 *   3 -> 1062 ms, no failures
 *   2 -> total failure, sensor wedged
 * We keep 4 rather than 3: shipping the value right next to the cliff would
 * leave no margin for load or temperature variation. */
#ifndef GX_DRAIN_SILENCE
#define GX_DRAIN_SILENCE 4
#endif

  static guint8 scratch[GOODIX_RX_MAX];
  guint8 ty;
  int r, got = 0, i, misses = 0;

  gx_send_plain_raw (self, body, n);
  g_usleep (15000);
  /* Read until a sustained silence. Some responses — the 4765-byte navigation
   * payload, the image — arrive after a delay, and giving up early leaves the
   * next read misaligned. */
  for (i = 0; i < 120; i++)
    {
      r = gx_read_frame (self, &ty, scratch, sizeof scratch);
      if (r > 0 && ty == GOODIX_PKT_PLAIN)   /* cleartext reply: keep draining */
        {
          got++;
          misses = 0;
          g_usleep (5000);
          continue;
        }
      if (r > 0 && ty == GOODIX_PKT_TLS)     /* record TLS : stocke pour le BIO */
        {
          memcpy (self->tls_rx, scratch, r);
          self->tls_rxlen = r;
          self->tls_rxpos = 0;
          fp_dbg ("drain cmd=%02x -> stashed TLS record, %d bytes", body[0], r);
          break;
        }
      misses++;
      g_usleep (10000);
      if (got && misses >= GX_DRAIN_SILENCE)  /* silence after a reply */
        break;
      if (!got && misses >= 25)              /* nothing at all after ~250 ms */
        break;
    }
  fp_dbg ("drain cmd=%02x: %d cleartext reply/replies", body[0], got);
}

/* ------------------------------------------------------------------ */
/*  Transport glue: TLS records travel inside 0xB0 SPI frames          */
/* ------------------------------------------------------------------ */

static int
gx_bio_send (gpointer ctx, const guint8 *b, gsize l)
{
  FpiDeviceGoodixTls *self = ctx;

  gsize off = 0;

  /* One TLS record per 0xB0 frame. OpenSSL batches several records into a
   * single write — the ServerHello group comes out as one 121-byte call — but
   * the sensor expects exactly one record per frame and silently ignores a
   * frame carrying two. Split on the record headers rather than forwarding the
   * buffer as it arrives. */
  while (off + 5 <= l)
    {
      gsize rec = 5 + (((gsize) b[off + 3] << 8) | b[off + 4]);

      if (off + rec > l)
        break;                        /* partial record: should not happen */
      if (!gx_write_frame (self, GOODIX_PKT_TLS, b + off, rec))
        return -1;
      off += rec;
    }
  return (int) l;
}

static int
gx_bio_recv (gpointer ctx, guint8 *b, gsize l)
{
  FpiDeviceGoodixTls *self = ctx;
  int avail, take, i;
  guint8 ty;

  if (self->tls_rxpos >= self->tls_rxlen)
    {
      /* Buffer empty: pull fresh TLS frames, skipping cleartext ones. The
       * sensor takes a while to produce the image, hence the generous poll. */
      int r = -1;
      self->tls_rxlen = self->tls_rxpos = 0;
      for (i = 0; i < 200; i++)
        {
          r = gx_read_frame (self, &ty, self->tls_rx, GOODIX_RX_MAX);
          if (r > 0 && ty == GOODIX_PKT_TLS)
            {
              fp_dbg ("bio_recv: TLS frame %d bytes (attempt %d)", r, i);
              break;
            }
          r = -1;
          g_usleep (20000);
        }
      if (r <= 0)
        {
          fp_dbg ("bio_recv: no TLS frame (timed out)");
          return -1;
        }
      self->tls_rxlen = r;
      self->tls_rxpos = 0;
    }
  avail = self->tls_rxlen - self->tls_rxpos;
  take = (int) l < avail ? (int) l : avail;
  memcpy (b, self->tls_rx + self->tls_rxpos, take);
  self->tls_rxpos += take;
  return take;
}

/* Sends a command over the encrypted channel. */
static int
gx_tls_cmd (FpiDeviceGoodixTls *self, guint8 cmd, const guint8 *data, int dl)
{
  guint8 b[64];
  int n = dl + 1;

  b[0] = cmd;
  b[1] = n & 0xFF;
  b[2] = (n >> 8) & 0xFF;
  if (dl)
    memcpy (b + 3, data, dl);
  b[3 + dl] = goodix_body_cksum (b, 3 + dl);
  return gx_tls_write (self->tls, b, 4 + dl) ? 4 + dl : -1;
}

/* ------------------------------------------------------------------ */
/*  Arbitrary memory read: this is how the pre-shared key is obtained  */
/* ------------------------------------------------------------------ */

static int
gx_mem_read (FpiDeviceGoodixTls *self, guint32 mem, guint32 len, guint8 *out)
{
  guint8 pkt[12], ty;
  guint32 a = mem - 0x08000000;
  int n, hdr;

  pkt[0] = GOODIX_CMD_MEM_READ;
  pkt[1] = 9;
  pkt[2] = 0;
  pkt[3] = a; pkt[4] = a >> 8; pkt[5] = a >> 16; pkt[6] = a >> 24;
  pkt[7] = len; pkt[8] = len >> 8; pkt[9] = len >> 16; pkt[10] = len >> 24;
  pkt[11] = goodix_body_cksum (pkt, 11);

  if (!gx_send_plain_raw (self, pkt, sizeof pkt))
    return -1;
  g_usleep (10000);
  {
    guint8 rx[512];
    gx_read_frame (self, &ty, rx, sizeof rx);   /* ACK */
    g_usleep (8000);
    n = gx_read_frame (self, &ty, rx, sizeof rx);
    if (n <= 0)
      return -1;
    hdr = (rx[0] >> 4) == 0xF ? 3 : 0;          /* memory-read reply header */
    n -= hdr;
    if (n > (int) len)
      n = len;
    if (n > 0)
      memcpy (out, rx + hdr, n);
    return n;
  }
}

/* ------------------------------------------------------------------ */
/*  Image decoding: six bytes carry four pixels, in interleaved order  */
/* ------------------------------------------------------------------ */

static void
gx_decode_12bit (const guint8 *data, gsize len, guint16 *out, gsize n)
{
  gsize i, o = 0;

  for (i = 0; i + 6 <= len && o + 4 <= n; i += 6)
    {
      const guint8 *c = data + i;
      out[o++] = ((c[0] & 0xf) << 8) | c[1];
      out[o++] = (c[3] << 4) | (c[0] >> 4);
      out[o++] = ((c[5] & 0xf) << 8) | c[2];
      out[o++] = (c[4] << 4) | (c[5] >> 4);
    }
}

/* ------------------------------------------------------------------ */
/*  Init, handshake and frame capture                                  */
/* ------------------------------------------------------------------ */

/* Hardware reset. The reset line is GPIO 58 on gpiochip0, as declared by the
 * ACPI _CRS. A short PULSE is what the sensor wants; holding the line down was
 * measured to be counter-productive, recovery failing where a pulse succeeds.
 * Without this reset, leftovers from a previous run make init and handshake
 * fail. */
static void
gx_gpio_reset (FpiDeviceGoodixTls *self)
{
  struct gpio_v2_line_request req = { 0 };
  struct gpio_v2_line_values val = { 0 };
  int chip;

  chip = open ("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
  if (chip < 0)
    return;
  req.num_lines = 1;
  req.offsets[0] = 58;
  req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
  g_strlcpy (req.consumer, "goodixtls", sizeof req.consumer);
  if (ioctl (chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0 || req.fd < 0)
    {
      close (chip);
      return;
    }
  val.mask = 1;
  val.bits = 0;                          /* assert reset */
  ioctl (req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &val);
  g_usleep (10000);
  val.bits = 1;                          /* release */
  ioctl (req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &val);
  g_usleep (120000);
  close (req.fd);
  close (chip);
}



/* Uploads the configuration blob, which opens the command gate, then enables
 * the chip and requests a TLS session. */
static gboolean
gx_upload_config_and_reqtls (FpiDeviceGoodixTls *self)
{
  gx_gpio_reset (self);

  extern const guint8 CONFIG_PCAP[];
  extern const gsize CONFIG_PCAP_LEN;
  gsize clen = CONFIG_PCAP_LEN;
  g_autofree guint8 *c = g_malloc (clen + 4);
  guint16 nn = clen + 1;
  guint8 ty, rx[512];

  c[0] = GOODIX_CMD_UPLOAD_CFG;
  c[1] = nn & 0xFF;
  c[2] = (nn >> 8) & 0xFF;
  memcpy (c + 3, CONFIG_PCAP, clen);
  c[3 + clen] = goodix_body_cksum (c, 3 + clen);

  if (!gx_write_frame (self, GOODIX_PKT_PLAIN, GX_AMORCE, sizeof GX_AMORCE))
    return FALSE;
  g_usleep (15000);
  if (!gx_write_frame (self, GOODIX_PKT_PLAIN, c, clen + 4))
    return FALSE;
  g_usleep (80000);
  gx_read_frame (self, &ty, rx, sizeof rx);          /* ACK config */

  gx_send_plain_raw (self, GX_ENABLE, sizeof GX_ENABLE);
  g_usleep (20000);
  gx_read_frame (self, &ty, rx, sizeof rx);          /* ACK enable */

  gx_send_plain_raw (self, GX_REQTLS, sizeof GX_REQTLS);
  g_usleep (20000);
  gx_read_frame (self, &ty, rx, sizeof rx);          /* ACK 0xD0 ; ClientHello reste */
  return TRUE;
}

/* Establishes the TLS-PSK channel, with us as the server. */
static gboolean
gx_tls_handshake (FpiDeviceGoodixTls *self)
{
  g_autoptr(GError) err = NULL;

  self->tls_rxlen = self->tls_rxpos = 0;
  g_clear_pointer (&self->tls, gx_tls_free);
  self->tls = gx_tls_new (self->psk, GOODIX_PSK_LEN, GOODIX_TLS_IDENTITY,
                          gx_bio_send, gx_bio_recv, self);
  if (!self->tls)
    {
      fp_warn ("cannot set up the TLS channel");
      return FALSE;
    }
  if (!gx_tls_handshake_run (self->tls, &err))
    {
      fp_warn ("%s", err->message);
      return FALSE;
    }
  fp_info ("TLS-PSK session up (%s)", gx_tls_ciphersuite (self->tls));
  self->tls_up = TRUE;
  return TRUE;
}

static void
gx_tls_teardown (FpiDeviceGoodixTls *self)
{
  if (self->tls_up)
    {
      gx_tls_close (self->tls);
      self->tls_up = FALSE;
    }
  g_clear_pointer (&self->tls, gx_tls_free);
}

/* Exact image capture sequence, as observed on the wire. */
static void
gx_send_capture_sequence (FpiDeviceGoodixTls *self)
{
/* Pause after each command in the sequence, ON TOP of the drain. It looks
 * redundant — the drain already waits for silence — but it is not: at 10 ms
 * every capture fails and the sensor wedges (measured, 19 failures out of 19).
 * Do not shorten it. */
#ifndef GX_SEQ_GAP_US
#define GX_SEQ_GAP_US 30000
#endif

  static const char *seq[] = {
    "ae0b000600ffff0008ff01001acb",
    "3623000d01a6a6b9b9bcbcb7b7a8a8babab7b7b8b8a8a8c1c1babab5b500000000000000004d",
    "362500000100004e0175017a01710152017501700172015201830177016c01000000000000000033",
    "500300010056",
    "3623000d01a7a7bababdbdb8b8a9a9babab8b8b9b9a9a9c1c1bbbbb6b6000000000000000039",
    "362500000100004d01740179017001510174016f0171015101830176016b0100000000000000003e",
    "82060000820002009e",
    "820300148091",
    "200300010086",
    NULL
  };
  guint8 body[80];
  int i, j;

  for (i = 0; seq[i]; i++)
    {
      int n = strlen (seq[i]) / 2;
      for (j = 0; j < n; j++)
        sscanf (seq[i] + 2 * j, "%2hhx", &body[j]);
      {
        gint64 t0 = g_get_monotonic_time ();
        gx_send_plain_drain (self, body, n);
        fp_dbg ("chrono cmd=%02x drain=%ld us", body[0],
                 (long) (g_get_monotonic_time () - t0));
      }
      g_usleep (GX_SEQ_GAP_US * self->timing_scale / 100);
    }
}

/* Captures one full image, assuming a TLS session is already up, and fills
 * px[GOODIX_IMG_PIXELS] with 12-bit samples. */
static gboolean
gx_capture_frame (FpiDeviceGoodixTls *self, guint16 *px)
{
  g_autofree guint8 *img = g_malloc (GOODIX_RX_MAX);
  guint8 st = 0x55;
  int total = 0, k;

  gint64 tA = g_get_monotonic_time ();
  gx_tls_cmd (self, GOODIX_CMD_MCU_STATE, &st, 1);   /* mcu_state sur TLS */
  g_usleep (80000);
  gx_send_capture_sequence (self);
  fp_dbg ("timing: capture sequence %ld us",
           (long) (g_get_monotonic_time () - tA));

  /* The image arrives as ONE record larger than TLS allows, so it is decrypted
   * here rather than handed to OpenSSL, which would reject it outright.
   *
   * The record has usually already been picked up while draining the capture
   * sequence; otherwise wait for it. */
  {
    g_autofree guint8 *rec = g_malloc (GOODIX_RX_MAX);
    int raw = 0;
    gssize got;

    if (self->tls_rxlen > self->tls_rxpos)
      {
        raw = self->tls_rxlen - self->tls_rxpos;
        memcpy (rec, self->tls_rx + self->tls_rxpos, raw);
        self->tls_rxlen = self->tls_rxpos = 0;
      }
    else
      {
        guint8 ty;

        for (k = 0; k < 200; k++)
          {
            raw = gx_read_frame (self, &ty, rec, GOODIX_RX_MAX);
            if (raw > 0 && ty == GOODIX_PKT_TLS)
              break;
            raw = 0;
            g_usleep (20000);
          }
      }

    got = raw > 0 ? gx_tls_decrypt_record (self->tls, rec, raw,
                                           img, GOODIX_RX_MAX) : -1;
    if (got < 0)
      {
        fp_warn ("cannot decrypt the image record (%d raw bytes)", raw);
        return FALSE;
      }
    total = (int) got;
  }
  fp_dbg ("timing: whole capture %ld us (image %d bytes)",
           (long) (g_get_monotonic_time () - tA), total);
  if (total < 22176)
    {
      fp_warn ("incomplete image: %d bytes", total);
      return FALSE;
    }
  /* Header is a tag, a 16-bit length and five zero bytes, then the 12-bit
   * samples: skip 8 bytes. */
  gx_decode_12bit (img + 8, total - 8, px, GOODIX_IMG_PIXELS);
  return TRUE;
}


static int
gx_cmp_dbl (const void *a, const void *b)
{
  double x = *(const double *) a - *(const double *) b;
  return (x > 0) - (x < 0);
}






/* Moyenne n trames en un tableau de pixels. */
static gboolean
gx_capture_avg (FpiDeviceGoodixTls *self, int nframes, guint16 *avg)
{
  g_autofree guint32 *acc = g_malloc0 (sizeof (guint32) * GOODIX_IMG_PIXELS);
  guint16 px[GOODIX_IMG_PIXELS];
  int got = 0, f, i;

  for (f = 0; f < nframes; f++)
    {
      if (!gx_capture_frame (self, px))
        continue;
      for (i = 0; i < GOODIX_IMG_PIXELS; i++)
        acc[i] += px[i];
      got++;
    }
  if (!got)
    return FALSE;
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    avg[i] = acc[i] / got;
  return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Device life cycle: open / close                                    */
/* ------------------------------------------------------------------ */



/* Native finger detection: twelve 16-bit values, one per sensor zone, in about
 * 33 ms — some 70 times faster than capturing an image. A finger pulls those
 * values DOWN by roughly 100 counts (around 335/371/361 idle against
 * 229/269/275 with a finger), which is what makes cheap polling possible. */
static int
gx_fdt_probe (FpiDeviceGoodixTls *self, int *out12)
{
  static const guint8 fdt[] = {
    0x36,0x23,0x00,0x0d,0x01,0xa6,0xa6,0xb9,0xb9,0xbc,0xbc,0xb7,
    0xb7,0xa8,0xa8,0xba,0xba,0xb7,0xb7,0xb8,0xb8,0xa8,0xa8,0xc1,
    0xc1,0xba,0xba,0xb5,0xb5,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x4d };
  guint8 rx[256], ty;
  int n, k;

  /* Do NOT shorten these delays. Tested in isolation they passed 20 times out
   * of 20 at a third of the value, but in real conditions — right after an
   * image capture — session setup then failed. The responsiveness gained is
   * not worth it; the polling period already bounds the latency. */
  gx_send_plain_raw (self, fdt, sizeof fdt);
  g_usleep (15000 * self->timing_scale / 100);
  gx_read_frame (self, &ty, rx, sizeof rx);          /* ACK */
  g_usleep (8000 * self->timing_scale / 100);
  n = gx_read_frame (self, &ty, rx, sizeof rx);      /* payload */
  if (n < 31)
    return -1;
  for (k = 0; k < 12; k++)
    out12[k] = rx[7 + 2 * k] | (rx[8 + 2 * k] << 8);
  return 0;
}

/* Mean of the twelve detection values: an absolute test needing no baseline. */
static int
gx_fdt_mean (const int *v)
{
  int k, s = 0;
  for (k = 0; k < 12; k++)
    s += v[k];
  return s / 12;
}

/* Total drop against the baseline; above the threshold means a finger. */
static int
gx_fdt_drop (const int *base, const int *cur)
{
  int k, d = 0;
  for (k = 0; k < 12; k++)
    d += base[k] - cur[k];
  return d / 12;
}

/* ------------------------------------------------------------------ */
/*  Enrolment and verification, using local descriptors instead of the */
/*  NBIS pipeline.                                                     */
/*                                                                     */
/*  FpImageDevice mandates NBIS (minutiae plus bozorth3), which this    */
/*  sensor cannot feed: 6x5 mm yields about 6 minutiae per capture,     */
/*  far below what reliable matching needs, and bozorth3 scored zero    */
/*  every time. Hence deriving straight from FpDevice and implementing  */
/*  enrol and verify against the descriptor matcher in goodix_sift.c.   */
/* ------------------------------------------------------------------ */

/* Decision threshold: how many points of the capture the template explains
 * (see the view fusion further down).
 *
 * BEWARE — THIS THRESHOLD DEPENDS ON HOW RICH THE TEMPLATE IS, and that is its
 * weak point. The fused score grows with the number and especially the extent
 * of the enrolled views, for a foreign finger just as much as for the right
 * one. Measured against 51 captures of NON-enrolled fingers (several fingers,
 * both hands, plus a second person):
 *
 *   - tight template, 29 views ......... impostor max 3
 *   - wide template, 30 views spanning
 *     328x248 (guided enrolment) ....... impostor max 8, four captures >= 5
 *   - same, plus adaptive views ........ impostor max 9
 *
 * In other words, improving coverage ALSO raises the impostor floor: the gain
 * in rejection rate is paid for in security margin. A threshold of 6, calibrated
 * on five captures of a single foreign finger, let three false accepts through
 * (6, 7 and 8) as soon as the set was widened — five samples from one finger
 * are not enough to set a security parameter.
 *
 * 15 sits at about twice the observed impostor maximum and well below the
 * genuine minimum measured on this template (20 over eight varied poses). Any
 * change to enrolment requires RE-MEASURING this against a varied impostor set. */
#define GX_MATCH_THRESHOLD 15

/* What determines template quality is the number of DISTINCT presses, not the
 * raw number of captures: two captures taken back to back show almost the same
 * area, since the finger does not move within a second. Measured, 5 presses of
 * 3 views each got the genuine finger REJECTED, while a dozen distinct presses
 * gave reliable recognition. Coverage is the dominant factor. */
#define GX_ENROLL_STAGES 15   /* presses asked of the user */
#define GX_VIEWS_PER_STAGE 2  /* a second view adds slight variation for free */
#define GX_ENROLL_VIEWS (GX_ENROLL_STAGES * GX_VIEWS_PER_STAGE)

/* Reads the firmware version, which also proves the SPI dialogue works. */
static gboolean
gx_read_fw_version (FpiDeviceGoodixTls *self, gchar *out, gsize cap)
{
  static const guint8 body[] = { GOODIX_CMD_FW_VERSION, 0x03, 0x00, 0x70, 0xDC, 0xB3 };
  guint8 rx[64], ty;
  int n;

  if (!gx_send_plain_raw (self, body, sizeof body))
    return FALSE;
  g_usleep (10000);
  gx_read_frame (self, &ty, rx, sizeof rx);          /* ACK */
  g_usleep (5000);
  n = gx_read_frame (self, &ty, rx, sizeof rx);
  if (n <= 3)
    return FALSE;
  g_strlcpy (out, (const gchar *) rx + 3, MIN ((gsize) (n - 3) + 1, cap));
  return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Session: TLS setup, background frame, detection baseline           */
/* ------------------------------------------------------------------ */

/* Learned protocol-timing multiplier, remembered across opens.
 *
 * timing_scale grows within a session when the sensor loses sync (see below),
 * but resets to nominal on every open — so a consistently slow unit paid one
 * failed handshake at the start of every session. Persisting the last value
 * that WORKED skips that: the next open starts at the timing this unit is known
 * to need. It lives in fprintd's state directory (the only writable path under
 * ProtectSystem=strict), is device- not user-scoped, and is a single integer.
 * Delete the file to reset the learned value. */
#define GX_TIMING_FILE "/var/lib/fprint/.goodixtls-timing"

static int
gx_timing_load (void)
{
  g_autofree gchar *txt = NULL;
  int v;

  if (!g_file_get_contents (GX_TIMING_FILE, &txt, NULL, NULL))
    return 100;
  v = atoi (txt);
  return (v >= 100 && v <= 300) ? v : 100;   /* clamp; ignore a garbled file */
}

static void
gx_timing_save (int scale)
{
  g_autofree gchar *txt = g_strdup_printf ("%d\n", scale);

  if (!g_file_set_contents (GX_TIMING_FILE, txt, -1, NULL))
    fp_warn ("could not persist timing scale to %s", GX_TIMING_FILE);
  else
    g_chmod (GX_TIMING_FILE, 0600);
}

/* Establishes the TLS channel. This is the expensive step, about half a
 * second, and it does not depend on when the finger arrives — so it can be
 * done once and kept. */
static gboolean
gx_tls_session (FpiDeviceGoodixTls *self)
{
  int att;

  if (self->tls_up)
    return TRUE;
  for (att = 1; att <= 5 && !self->tls_up; att++)
    {
      GCancellable *c = fpi_device_get_cancellable (FP_DEVICE (self));
      if (c && g_cancellable_is_cancelled (c))
        {
          fp_info ("TLS setup cancelled by the caller");
          break;
        }
      if (gx_upload_config_and_reqtls (self) && gx_tls_handshake (self))
        break;
      gx_tls_teardown (self);

      /* Self-healing on desync. The protocol delays are tuned to the author's
       * unit and sit right at the edge; a different SPI controller can be
       * slower and lose sync, which shows up here as a failed handshake. Each
       * failure loosens every scaled delay for the next attempt and for the
       * rest of this device's lifetime, so a flaky unit converges on timings
       * that hold instead of failing at the author's values. Capped so a
       * genuinely dead sensor still gives up rather than crawling. */
      if (self->timing_scale < 300)
        {
          self->timing_scale = MIN (self->timing_scale + 50, 300);
          fp_info ("handshake failed; loosening protocol timings to %d%%",
                   self->timing_scale);
        }

      /* A plain reset between attempts. Holding the reset line down for a
       * long time was tried and is actively counter-productive: recovery
       * fails where a short pulse succeeds. Deep lock-ups are not a sensor
       * state at all — they sit in the kernel's spidev driver, and only
       * rebinding it clears them, which a userspace driver cannot do. */
      gx_gpio_reset (self);
    }
  if (!self->tls_up)
    fp_warn ("no TLS session after %d attempts; if this persists the sensor "
             "needs a full recovery (long reset plus an spidev rebind)",
             att - 1);
  else if (self->timing_scale > self->timing_saved)
    {
      /* Only ever raise the persisted value, and only on success: a scale
       * reached while failing (a deep lock-up climbs to the cap and never
       * connects) must not be written, or every unit would drift to 300%. */
      gx_timing_save (self->timing_scale);
      self->timing_saved = self->timing_scale;
      fp_info ("learned timing scale %d%% persisted", self->timing_scale);
    }
  return self->tls_up;
}

/* Refreshes the background frame and the detection baseline.
 *
 * This must happen JUST BEFORE each operation, unlike the TLS setup above: the
 * sensor's baseline level drifts, and the whole chain rests on the
 * background-minus-finger difference. A background captured too early yields a
 * distorted image and collapses the score — measured at 11 to 28 matches with
 * a fresh background against 2 to 4 with one a few tens of seconds old. */

/* How long to wait for the finger to be lifted before taking the background. */
#define GX_BG_WAIT_MS 2000

static gboolean
gx_session_start (FpiDeviceGoodixTls *self)
{
  int q, k;

  if (!gx_tls_session (self))
    return FALSE;

  self->bg_dirty = FALSE;

  /* Both the background and the detection baseline are only meaningful with
   * the finger LIFTED. A finger left resting on the sensor when the operation
   * starts ends up in the background, the difference then cancels out, and the
   * capture is worthless — the observed symptom being verifications collapsing
   * from about 100 matches to 6 with no change in how the finger was placed.
   *
   * So take the background, ask the sensor whether it saw a finger, and redo it
   * if so. Background first, detection second, matching the order the rest of
   * the driver uses.
   *
   * The presence test here is the ABSOLUTE one, the only usable choice, since
   * the relative baseline is precisely what we are about to establish. */
  if (!self->bg_frame)
    self->bg_frame = g_malloc (sizeof (guint16) * GOODIX_IMG_PIXELS);

  {
    int cur[12], waited = 0;
    gboolean timeout = FALSE;
    gint64 t0 = g_get_monotonic_time ();

    for (;;)
      {
        /* Wait for the finger to be lifted BEFORE spending a capture on the
         * background, not after. Probing costs about 33 ms, a background
         * capture about 1.5 s, and taking the background first paid that
         * second on every round: four rounds of a finger still down meant
         * four discarded captures, which is how the wait before the prompt
         * reached 7 s. Probing first makes those rounds cost 200 ms each. */
        while (!timeout && gx_fdt_probe (self, cur) == 0 &&
               gx_fdt_mean (cur) < self->fdt_abs)
          {
            if (waited >= GX_BG_WAIT_MS)
              timeout = TRUE;
            else
              {
                g_usleep (200 * 1000);
                waited += 200;
              }
          }

        if (!gx_capture_avg (self, GOODIX_BG_FRAMES, self->bg_frame))
          return FALSE;

        /* The capture itself lasts about 1.5 s, so a finger can land while it
         * runs and end up baked into the background. Check once more, and
         * redo it if that happened. */
        if (gx_fdt_probe (self, cur) != 0 || gx_fdt_mean (cur) >= self->fdt_abs)
          break;                                   /* no finger: background good */
        if (timeout)
          {
            fp_warn ("finger still down after %d ms: taking the background "
                     "anyway, capture will be degraded", GX_BG_WAIT_MS);
            self->bg_dirty = TRUE;
            break;
          }
      }
    if (waited)
      fp_info ("background delayed %d ms (finger was down)", waited);
    fp_info ("session ready in %d ms",
             (int) ((g_get_monotonic_time () - t0) / 1000));
  }

  /* Detection baseline; the wait above guarantees no finger is present. */
  {
    int t[12], acc[12] = { 0 }, nb = 0;
    for (q = 0; q < 4; q++)
      if (gx_fdt_probe (self, t) == 0)
        { for (k = 0; k < 12; k++) acc[k] += t[k]; nb++; }
    if (nb)
      {
        for (k = 0; k < 12; k++) self->fdt_base[k] = acc[k] / nb;
        self->have_fdt = TRUE;
        /* Derive the absolute floor from THIS unit's idle level instead of a
         * fixed number. A finger only ever pulls the mean down, so the floor
         * sits a fixed margin below idle; where idle actually is no longer
         * matters, which is what makes detection work on a sensor other than
         * the author's. */
        self->fdt_abs = gx_fdt_mean (self->fdt_base) - GOODIX_FDT_ABS_MARGIN;
        fp_info ("FDT auto-calibrated: idle mean=%d -> floor=%d (drop>%d)",
                 gx_fdt_mean (self->fdt_base), self->fdt_abs, GOODIX_FDT_DROP);
      }
  }
  return self->have_fdt;
}



/* Pre-processing: subtract the background, then remove row and column
 * banding with a median. */

static double *
gx_preprocess (FpiDeviceGoodixTls *self, const guint16 *px)
{
  const int W = GOODIX_IMG_WIDTH, H = GOODIX_IMG_HEIGHT;
  double *d = g_malloc (sizeof (double) * GOODIX_IMG_PIXELS);
  g_autofree double *buf = g_malloc (sizeof (double) * MAX (W, H));
  int x, y, i;

  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    d[i] = (double) self->bg_frame[i] - px[i];
  for (y = 0; y < H; y++)
    {
      for (x = 0; x < W; x++) buf[x] = d[y * W + x];
      qsort (buf, W, sizeof (double), gx_cmp_dbl);
      { double m = buf[W / 2]; for (x = 0; x < W; x++) d[y * W + x] -= m; }
    }
  for (x = 0; x < W; x++)
    {
      for (y = 0; y < H; y++) buf[y] = d[y * W + x];
      qsort (buf, H, sizeof (double), gx_cmp_dbl);
      { double m = buf[H / 2]; for (y = 0; y < H; y++) d[y * W + x] -= m; }
    }
  return d;
}

/* Is a finger actually down? Costs about 33 ms. */
static gboolean
gx_finger_present (FpiDeviceGoodixTls *self)
{
  int cur[12];

  if (gx_fdt_probe (self, cur) != 0)
    return FALSE;
  return gx_fdt_drop (self->fdt_base, cur) > GOODIX_FDT_DROP ||
         gx_fdt_mean (cur) < self->fdt_abs;
}


/* Dumps captures for offline evaluation. Enabled simply by the dump directory
 * existing, rather than by an environment variable, which cannot conveniently
 * be set on an already-running systemd service.
 *
 * pN.bin holds the raw frame as 16-bit samples and pN.bin.bg the matching
 * background, so that recorded sets can be replayed through the offline
 * matcher bench. */
#define GX_DUMP_DIR "/run/goodixtls/dump"

static void
gx_dump_capture (FpiDeviceGoodixTls *self, const guint16 *px)
{
  static int seq = 0;
  g_autofree gchar *p = NULL, *b = NULL;

  if (!g_file_test (GX_DUMP_DIR, G_FILE_TEST_IS_DIR) || !self->bg_frame)
    return;

  /* Continue past captures already there, so a restart does not overwrite
   * an earlier collection run. */
  do
    {
      g_free (p);
      p = g_strdup_printf ("%s/p%03d.bin", GX_DUMP_DIR, ++seq);
    }
  while (g_file_test (p, G_FILE_TEST_EXISTS) && seq < 9999);

  b = g_strdup_printf ("%s.bg", p);
  if (g_file_set_contents (p, (const gchar *) px,
                           sizeof (guint16) * GOODIX_IMG_PIXELS, NULL) &&
      g_file_set_contents (b, (const gchar *) self->bg_frame,
                           sizeof (guint16) * GOODIX_IMG_PIXELS, NULL))
    fp_info ("capture saved: %s", p);
}

/* Captures one press and extracts its descriptors. */
static GxSiftFeatures *
gx_capture_features (FpiDeviceGoodixTls *self)
{
  guint16 px[GOODIX_IMG_PIXELS];
  g_autofree double *img = NULL;
  double m = 0, v = 0;
  int i;

  if (!gx_capture_frame (self, px))
    return NULL;
  gx_dump_capture (self, px);
  img = gx_preprocess (self, px);

  /* This quality gate is ESSENTIAL. The keypoint detector always returns
   * maxima, even on pure noise, so the number of points says nothing about
   * whether a capture is usable. What separates them is contrast: about 5
   * with no finger against about 200 with one. Without this check a failed
   * press enters the template and degrades it permanently — observed when a
   * user lifted between the two views of a press, poisoning every stage. */
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    m += img[i];
  m /= GOODIX_IMG_PIXELS;
  for (i = 0; i < GOODIX_IMG_PIXELS; i++)
    { double d = img[i] - m; v += d * d; }
  v = sqrt (v / GOODIX_IMG_PIXELS);
  if (v < GOODIX_FINGER_STD / 3.0)
    {
      fp_info ("capture rejected: contrast %.0f (no finger?)", v);
      return NULL;
    }
  return gx_sift_extract (img, GOODIX_IMG_WIDTH, GOODIX_IMG_HEIGHT);
}

/* ------------------------------------------------------------------ */
/*  Serialising a set of views into an FpPrint                         */
/* ------------------------------------------------------------------ */

static GVariant *
gx_views_to_variant (GPtrArray *views)
{
  GVariantBuilder b;
  guint i;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));
  for (i = 0; i < views->len; i++)
    {
      g_autoptr(GByteArray) ba = gx_sift_serialize (g_ptr_array_index (views, i));
      g_variant_builder_add_value (
        &b, g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, ba->data, ba->len, 1));
    }
  return g_variant_new ("aay", &b);
}

static GPtrArray *
gx_views_from_print (FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  GPtrArray *views = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
  GVariantIter it;
  GVariant *child;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("aay")))
    return views;
  g_variant_iter_init (&it, data);
  while ((child = g_variant_iter_next_value (&it)))
    {
      gsize len = 0;
      const guint8 *raw = g_variant_get_fixed_array (child, &len, 1);
      GxSiftFeatures *f = gx_sift_deserialize (raw, len);
      if (f)
        g_ptr_array_add (views, f);
      g_variant_unref (child);
    }
  return views;
}

/* ------------------------------------------------------------------ */
/*  Adaptive store: widening coverage as the sensor gets used          */
/* ------------------------------------------------------------------ */

/* libfprint offers no way to hand back an enriched template: verify completion
 * takes no FpPrint, and fprintd only writes a template from the enrol path. So
 * the driver keeps its own store alongside fprintd's — never inside it, since
 * fprintd scans its own directory and would mistake our files for templates.
 *
 * SAFETY — the rule that makes adaptation harmless: the acquisition threshold
 * sits strictly ABOVE the decision threshold. Anyone triggering an acquisition
 * is therefore already authenticating with a comfortable margin, so adaptation
 * never lowers the bar for an attacker; it only widens the coverage of someone
 * who already passes. Without that rule a doubtful accept would be written
 * permanently into the template, which is template poisoning. */
#define GX_ADAPT_MIN    30   /* twice the decision threshold, about four times
                                the highest score any foreign finger reached */
/* Novelty ceiling, expressed as a FRACTION of the capture's points rather than
 * an absolute count. The score scales with template richness — measured, a
 * 30-view template lifts ordinary poses from about 12 matches to about 100 — so
 * a fixed ceiling stops qualifying anything as soon as the template improves.
 * Above 60 % of points explained the pressed area is already well covered and
 * one more view would teach nothing. */
#define GX_ADAPT_MAX_FRAC 0.60
#define GX_ADAPT_VIEWS  20   /* cap, bounding verification time */
/* The store must live INSIDE fprintd's state directory: its systemd unit
 * declares ProtectSystem=strict with StateDirectory=fprint, leaving the rest of
 * the filesystem read-only to it. The name starts with a dot so it can never be
 * taken for a user directory — fprintd only ever opens the directory named
 * after an authenticated user, and no account is named like this. */
#define GX_ADAPT_DIR    "/var/lib/fprint/.goodixtls-adapt"

/* Store path for a given template, or NULL when the template carries no usable
 * identity. The user name comes from fprintd; reject anything that could escape
 * the directory. */
static gchar *
gx_adapt_path (FpPrint *print)
{
  const gchar *user = print ? fp_print_get_username (print) : NULL;

  if (!user || !*user || strchr (user, '/') || g_str_has_prefix (user, "."))
    return NULL;
  return g_strdup_printf ("%s/%s.%d.views", GX_ADAPT_DIR, user,
                          (int) fp_print_get_finger (print));
}

/* Views accumulated, in acquisition order: a sequence of length-prefixed
 * blocks, each one a view serialised exactly as in an fprintd template. */
static GPtrArray *
gx_adapt_load (const gchar *path)
{
  GPtrArray *views = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
  g_autofree gchar *raw = NULL;
  gsize len = 0, off = 0;

  if (!path || !g_file_get_contents (path, &raw, &len, NULL))
    return views;

  while (off + 4 <= len)
    {
      guint32 sz;
      GxSiftFeatures *f;

      memcpy (&sz, raw + off, 4);
      off += 4;
      if (sz > len - off)
        break;                          /* truncated file: keep what we have */
      f = gx_sift_deserialize ((const guint8 *) raw + off, sz);
      if (f)
        g_ptr_array_add (views, f);
      off += sz;
    }
  return views;
}

static gboolean
gx_adapt_append (const gchar *path, const GxSiftFeatures *f)
{
  g_autoptr(GByteArray) blob = NULL;
  g_autoptr(GError) err = NULL;
  guint32 sz;
  gboolean ok;
  FILE *fh;

  if (!path)
    return FALSE;
  blob = gx_sift_serialize (f);
  if (!blob)
    return FALSE;

  if (g_mkdir_with_parents (GX_ADAPT_DIR, 0700) != 0)
    {
      fp_warn ("adaptive store: cannot create %s: %s",
               GX_ADAPT_DIR, g_strerror (errno));
      return FALSE;
    }
  /* Append only: a view already written is never rewritten, so a crash mid
   * write can only damage the last one, which the reader then skips cleanly. */
  fh = fopen (path, "ab");
  if (!fh)
    {
      fp_warn ("adaptive store: %s: %s", path, g_strerror (errno));
      return FALSE;
    }
  sz = blob->len;
  ok = (fwrite (&sz, 4, 1, fh) == 1 &&
        fwrite (blob->data, 1, blob->len, fh) == blob->len);
  if (!ok)
    fp_warn ("adaptive store: short write to %s", path);
  if (fclose (fh) != 0)
    ok = FALSE;
  g_chmod (path, 0600);
  return ok;
}

/* Clears the store when its template is deleted or re-enrolled: acquired views
 * must not outlive the template that authorised them. */
static void
gx_adapt_clear (FpPrint *print)
{
  g_autofree gchar *path = gx_adapt_path (print);

  if (path && g_unlink (path) == 0)
    fp_info ("adaptive store cleared (%s)", path);
}

/* fprintd deletes the template itself and NEVER tells the driver: our prints
 * are host-stored, and libfprint's delete vfunc only covers templates kept on
 * the device. Without the sweep below, deleting a fingerprint in the desktop
 * settings would leave its descriptors on disk indefinitely — a privacy
 * problem, not merely untidiness.
 *
 * So catch up at open time: any store whose fprintd template has vanished is
 * removed. This is not instant — it takes an operation on the sensor to trigger
 * the cleanup — but it is the only hook libfprint leaves to a driver. */
static void
gx_adapt_sweep (void)
{
  GDir *d = g_dir_open (GX_ADAPT_DIR, 0, NULL);
  const gchar *name;

  if (!d)
    return;

  while ((name = g_dir_read_name (d)))
    {
      g_autofree gchar *stem = NULL, *user = NULL, *userdir = NULL;
      const gchar *finger;
      gboolean found = FALSE;
      gchar *dot;
      GDir *dd;

      if (!g_str_has_suffix (name, ".views"))
        continue;
      stem = g_strndup (name, strlen (name) - strlen (".views"));

      /* Split "<user>.<finger>" at the LAST dot: a user name may itself
       * contain one. */
      dot = strrchr (stem, '.');
      if (!dot || !dot[1])
        continue;
      *dot = '\0';
      user = g_strdup (stem);
      finger = dot + 1;

      /* The template lives under <user>/<driver id>/<device id>/<finger>,
       * and the device id may vary. */
      userdir = g_build_filename ("/var/lib/fprint", user, "goodixtls", NULL);
      dd = g_dir_open (userdir, 0, NULL);
      if (dd)
        {
          const gchar *devid;
          while (!found && (devid = g_dir_read_name (dd)))
            {
              g_autofree gchar *p = g_build_filename (userdir, devid, finger, NULL);
              found = g_file_test (p, G_FILE_TEST_EXISTS);
            }
          g_dir_close (dd);
        }

      if (!found)
        {
          g_autofree gchar *victim = g_build_filename (GX_ADAPT_DIR, name, NULL);
          if (g_unlink (victim) == 0)
            fp_info ("removed orphaned adaptive store %s "
                     "(its template is gone)", name);
        }
    }
  g_dir_close (d);
}


/* ------------------------------------------------------------------ */
/*  Enrolment and verification                                         */
/*                                                                     */
/*  A libfprint driver must not block the main loop: fprintd answers    */
/*  D-Bus calls while an operation is in flight. Waiting for the finger */
/*  is therefore done with successive detection polls driven by a       */
/*  timeout, each returning within about 33 ms.                        */
/* ------------------------------------------------------------------ */

typedef struct
{
  GPtrArray *views;        /* descriptors accumulated while enrolling */
  GxSiftFeatures *probe;   /* descriptors of the capture being verified */
  int        best;         /* best score reached across attempts */
  int        tries;        /* captures attempted for this verification */
  int        stage;        /* vue en cours */
  int        polls;        /* polls done in the current state */
  gboolean   verifying;
  GxSiftIsland *island;    /* coverage accumulated, for enrolment guidance */
} GxTask;

enum {
  GX_ST_SESSION,       /* init TLS + fond + ligne de base FDT */
  GX_ST_WAIT_ON,       /* wait for the finger, polling without blocking */
  GX_ST_CAPTURE,       /* capture, then extract descriptors */
  GX_ST_WAIT_OFF,      /* wait for the finger to be lifted */
  GX_ST_DONE,
  GX_ST_NUM,
};

#define GX_POLL_MS     100     /* detection polling period */
#define GX_POLL_MAX    300     /* about 30 s before giving up */
#define GX_POLL_OFF    100     /* about 10 s to wait for release */

/* ------------------------------------------------------------------ */
/*  Off-loading the blocking work                                      */
/* ------------------------------------------------------------------ */

/* Two steps take about a second each and cannot be split: establishing the
 * session (TLS handshake plus a background capture) and capturing an image.
 * Both go through OpenSSL's blocking API over a synchronous SPI file
 * descriptor, which cannot be pumped from callbacks without restructuring the
 * whole TLS layer.
 *
 * Run inline they froze the main loop, so fprintd stopped answering D-Bus for
 * the duration. Since libfprint runs one operation at a time and the state
 * machine waits for the completion callback, nothing else touches the device
 * while the worker runs, which makes a worker thread safe here; libfprint's own
 * secugen driver off-loads its heavy work the same way.
 *
 * Only these two steps move off the main loop. Finger detection stays on it: at
 * roughly 33 ms a poll it is short enough not to be felt, and keeping it there
 * avoids any cross-thread access to the sensor between captures. */

typedef struct
{
  FpiSsm         *ssm;
  FpDevice       *dev;
  GxSiftFeatures *feat;      /* capture result, NULL on failure */
  GPtrArray      *extra;     /* additional views of the same press */
  gboolean        ok;
} GxWork;

static void
gx_work_free (GxWork *w)
{
  g_clear_pointer (&w->feat, gx_sift_free);
  if (w->extra)
    g_ptr_array_free (w->extra, TRUE);
  g_free (w);
}

static void
gx_session_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  GxWork *w = data;

  w->ok = gx_session_start (FPI_DEVICE_GOODIXTLS (w->dev));
  g_task_return_boolean (task, TRUE);
}

static void
gx_capture_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  GxWork *w = data;
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (w->dev);
  GxTask *t = fpi_ssm_get_data (w->ssm);

  w->feat = gx_capture_features (self);
  w->ok = (w->feat != NULL);

  /* Several views per press: the finger shifts slightly between captures, which
   * enriches the template without asking the user for more gestures. They are
   * captured here but handed to the main loop for merging — the state
   * machine's data must only be mutated there. */
  if (w->ok && w->feat->n >= 8 && !t->verifying)
    {
      int extra;

      w->extra = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);
      for (extra = 1; extra < GX_VIEWS_PER_STAGE; extra++)
        {
          GxSiftFeatures *g;

          if (!gx_finger_present (self))
            break;
          g = gx_capture_features (self);
          if (g && g->n >= 8)
            g_ptr_array_add (w->extra, g);
          else
            g_clear_pointer (&g, gx_sift_free);
        }
    }
  g_task_return_boolean (task, TRUE);
}

static void
gx_session_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GxWork *w = g_task_get_task_data (G_TASK (res));
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (w->dev);

  if (!w->ok)
    {
      fpi_ssm_mark_failed (w->ssm, fpi_device_error_new_msg (
        FP_DEVICE_ERROR_PROTO, "sensor initialisation failed"));
      return;
    }
  if (self->bg_dirty)
    {
      /* Better to ask for the finger to be removed than to return a collapsed
       * score the user could not possibly explain. */
      fpi_ssm_mark_failed (w->ssm,
        fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      return;
    }
  fpi_ssm_next_state (w->ssm);
}

/* Runs @fn on a worker thread, then @done on the main loop. */
static void
gx_run_async (FpiSsm *ssm, FpDevice *dev, GTaskThreadFunc fn,
              GAsyncReadyCallback done)
{
  GTask *task = g_task_new (dev, NULL, done, NULL);
  GxWork *w = g_new0 (GxWork, 1);

  w->ssm = ssm;
  w->dev = dev;
  g_task_set_task_data (task, w, (GDestroyNotify) gx_work_free);
  g_task_run_in_thread (task, fn);
  g_object_unref (task);
}

static void
gx_task_free (GxTask *t)
{
  if (!t)
    return;
  if (t->views)
    g_ptr_array_free (t->views, TRUE);
  g_clear_pointer (&t->probe, gx_sift_free);
  g_clear_pointer (&t->island, gx_sift_island_free);
  g_free (t);
}


/* libfprint provides a GCancellable per operation; that is how fprintd asks us
 * to stop when the user dismisses the prompt or picks the password instead.
 * Without checking it the polls run to their limit, roughly 30 seconds, the
 * stop call times out and the sensor stays busy. */
static gboolean
gx_cancelled (FpDevice *dev, FpiSsm *ssm)
{
  GCancellable *c = fpi_device_get_cancellable (dev);

  if (!c || !g_cancellable_is_cancelled (c))
    return FALSE;
  fp_info ("operation cancelled by the caller");
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
  fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                         "operation cancelled"));
  return TRUE;
}

/* Non-blocking poll, re-armed by timeout until the finger is detected. */
static gboolean
gx_poll_on (gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDevice *dev = fpi_ssm_get_device (ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  GxTask *t = fpi_ssm_get_data (ssm);
  int cur[12];

  if (gx_cancelled (dev, ssm))
    { self->poll_id = 0; return G_SOURCE_REMOVE; }

  if (gx_fdt_probe (self, cur) != 0)
    {
      fp_dbg ("wait-on: detection probe failed");
      goto again;
    }

  /* Reuse the values just read; probing twice per poll perturbs the sensor. */
  if (t->polls % 5 == 0)
    fp_info ("wait-on: mean=%d drop=%d (thresholds %d / %d)",
             gx_fdt_mean (cur), gx_fdt_drop (self->fdt_base, cur),
             self->fdt_abs, GOODIX_FDT_DROP);

  if (gx_fdt_drop (self->fdt_base, cur) > GOODIX_FDT_DROP ||
      gx_fdt_mean (cur) < self->fdt_abs)
    {
      /* Keep NEEDED asserted: the capture still takes about 1.1 s and the
       * finger must stay down for it. This flag is what lets a user interface
       * say when to lift — without it people lift on detection, one second too
       * early, and spoil the capture. */
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED |
                                            FP_FINGER_STATUS_PRESENT);
      self->poll_id = 0;
      fpi_ssm_jump_to_state (ssm, GX_ST_CAPTURE);
      return G_SOURCE_REMOVE;
    }
again:
  if (++t->polls > GX_POLL_MAX)
    {
      self->poll_id = 0;
      fpi_ssm_mark_failed (ssm, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
gx_poll_off (gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDevice *dev = fpi_ssm_get_device (ssm);
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  GxTask *t = fpi_ssm_get_data (ssm);
  int cur[12];
  gboolean off = FALSE;

  if (gx_cancelled (dev, ssm))
    { self->poll_id = 0; return G_SOURCE_REMOVE; }

  if (gx_fdt_probe (self, cur) == 0 &&
      gx_fdt_drop (self->fdt_base, cur) < GOODIX_FDT_DROP / 2 &&
      gx_fdt_mean (cur) >= self->fdt_abs)
    off = TRUE;

  if (off || ++t->polls > GX_POLL_OFF)
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      /* vue suivante, ou fin */
      if (t->verifying || t->stage >= GX_ENROLL_STAGES)
        fpi_ssm_jump_to_state (ssm, GX_ST_DONE);
      else
        fpi_ssm_jump_to_state (ssm, GX_ST_WAIT_ON);
      self->poll_id = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
gx_capture_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GxWork *w = g_task_get_task_data (G_TASK (res));
  FpiSsm *ssm = w->ssm;
  FpDevice *dev = w->dev;
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  GxTask *t = fpi_ssm_get_data (ssm);
  GxSiftFeatures *f = g_steal_pointer (&w->feat);

      for (guint e = 0; w->extra && e < w->extra->len; e++)
        g_ptr_array_add (t->views, g_ptr_array_index (w->extra, e));
      if (w->extra)
        g_ptr_array_set_free_func (w->extra, NULL);   /* ownership moved */

      if (!f || f->n < 8)
        {
          /* Tell the two causes apart: they call for opposite gestures.
           * The image capture takes about 1.4 s, so a finger pressed and
           * immediately lifted does trigger detection, but the image is taken
           * once it has gone — contrast around 5, the empty level. Reporting
           * "not centred" there sends the user to fix the wrong thing, which
           * is glaring the first time someone unfamiliar tries the sensor. */
          FpDeviceRetry why = gx_finger_present (self)
                                ? FP_DEVICE_RETRY_CENTER_FINGER
                                : FP_DEVICE_RETRY_TOO_SHORT;

          g_clear_pointer (&f, gx_sift_free);
          if (why == FP_DEVICE_RETRY_TOO_SHORT)
            fp_info ("finger lifted before the capture finished");

          if (t->verifying)
            {
              fpi_ssm_mark_failed (ssm, fpi_device_retry_new (why));
              return;
            }
          /* unusable press: ask again without advancing the stage */
          fpi_device_enroll_progress (dev, t->stage, NULL,
                                      fpi_device_retry_new (why));
        }
      else if (t->verifying)
        {
          /* Score the capture, once.
           *
           * This used to re-capture while the finger was still down whenever
           * the score came out below the threshold, keeping the BEST score of
           * up to three captures. That is a security defect, not a comfort
           * feature: retrying until the score passes turns one decision into
           * the maximum of several draws, and pam_fprintd multiplies it again
           * with max-tries=3 — up to nine draws to clear a single threshold.
           * Measured: a foreign finger (middle, not enrolled) scored 3 on the
           * first capture and 20 on the second, and was accepted at a
           * threshold of 15, where the documented impostor ceiling is 8.
           *
           * A weak capture is now simply a weak result. Unusable captures are
           * still retried above, but on IMAGE QUALITY (too few points), never
           * on the score — a retry must never be conditioned on how close the
           * previous attempt came to authenticating. */
          FpPrint *tmpl = NULL;
          g_autoptr(GPtrArray) views = NULL;
          g_autoptr(GPtrArray) extra = NULL;
          g_autofree gchar *apath = NULL;
          guint i;

          fpi_device_get_verify_data (dev, &tmpl);
          views = gx_views_from_print (tmpl);
          apath = gx_adapt_path (tmpl);
          extra = gx_adapt_load (apath);

          /* Fusion des vues : on cumule les points de la capture qu'AU MOINS
           * at least one template view explains, instead of keeping only
           * the best per-view score. A press often overlaps several views
           * without covering any of them decisively, and taking the maximum
           * threw that distributed evidence away. Spurious matches from a
           * foreign finger do not accumulate the same way: they fail the
           * geometric check. Measured, borderline poses go from 5 to 18
           * matches while the impostor drops from 3 to 0. */
          {
            g_autofree guint8 *seen = g_new0 (guint8, f->n ? f->n : 1);
            int fused = 0;

            for (i = 0; i < views->len; i++)
              gx_sift_match_mask (g_ptr_array_index (views, i), f, seen);
            for (i = 0; i < extra->len; i++)
              gx_sift_match_mask (g_ptr_array_index (extra, i), f, seen);
            for (i = 0; i < f->n; i++)
              fused += seen[i];
            if (fused > t->best)
              t->best = fused;
          }

          t->tries++;
          g_clear_pointer (&t->probe, gx_sift_free);
          t->probe = f;
          fp_info ("verify: attempt %d -> %d matches (threshold %d)",
                   t->tries, t->best, GX_MATCH_THRESHOLD);

        }
      else
        {
          int dx = 0, dy = 0, sc = 0, x0, y0, x1, y1;
          guint np = 0;
          gboolean placed;

          g_ptr_array_add (t->views, f);
          t->stage++;

          /* Register this view against the coverage acquired so far. The
           * result is logged rather than returned: enrol progress carries no
           * message, and the desktop enrolment dialogue shows nothing but a
           * counter anyway. A dedicated tool reads these lines and shows the
           * user which part of the finger to present next. */
          if (!t->island)
            t->island = gx_sift_island_new ();
          placed = gx_sift_island_add (t->island, f, &dx, &dy, &sc);
          gx_sift_island_extent (t->island, &x0, &y0, &x1, &y1, &np);

          if (placed)
            fp_info ("enroll: stage=%d at=%d,%d overlap=%d "
                     "extent=%dx%d points=%u",
                     t->stage, dx, dy, sc, x1 - x0, y1 - y0, np);
          else
            fp_info ("enroll: stage=%d at=? overlap=%d "
                     "extent=%dx%d points=%u (disjoint area)",
                     t->stage, sc, x1 - x0, y1 - y0, np);

          fpi_device_enroll_progress (dev, t->stage, NULL, NULL);
        }
      /* Capture done: we no longer need the finger, only its release.
       * PRESENT without NEEDED is the "you may lift now" signal. */
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
      t->polls = 0;
      self->poll_id = g_timeout_add (GX_POLL_MS, gx_poll_off, ssm);
}

static void
gx_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  GxTask *t = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case GX_ST_SESSION:
      gx_run_async (ssm, dev, gx_session_thread, gx_session_done);
      break;

    case GX_ST_WAIT_ON:
      t->polls = 0;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      self->poll_id = g_timeout_add (GX_POLL_MS, gx_poll_on, ssm);
      break;                       /* the poll drives the next state */

    case GX_ST_CAPTURE:
      gx_run_async (ssm, dev, gx_capture_thread, gx_capture_done);
      break;

    case GX_ST_WAIT_OFF:
      /* only reached by jumping here; the poll drives what follows */
      break;

    case GX_ST_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

/* --- enrolment completion ------------------------------------------ */
static void
gx_enroll_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  GxTask *t = fpi_ssm_get_data (ssm);
  FpPrint *print = NULL;

  if (error)
    {
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }
  if (!t->views || t->views->len < 3)
    {
      fpi_device_enroll_complete (dev, NULL,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                  "too few usable views"));
      return;
    }

  fpi_device_get_enroll_data (dev, &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, "fpi-data", gx_views_to_variant (t->views), NULL);

  /* The template changes: views acquired under the authority of the old one
   * are no longer legitimate, so start from an empty store. */
  gx_adapt_clear (print);
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

/* ------------------------------------------------------------------ */
/*  Device operations                                                  */
/* ------------------------------------------------------------------ */

/* Reads the pre-shared key, with recovery. After a heavy session — a dozen or
 * so captures — the sensor refuses long exchanges such as the memory read while
 * short commands still work. It then needs a reset, and from the third attempt
 * a full reopen of the spidev node to clear the kernel driver's state too. */
static gboolean
gx_read_psk (FpiDeviceGoodixTls *self, const gchar *path)
{
  int att;

  for (att = 1; att <= 6; att++)
    {
      guint8 ty, junk[256];

      /* Drain: one pending frame shifts every subsequent read. */
      while (gx_read_frame (self, &ty, junk, sizeof junk) > 0)
        ;

      if (gx_mem_read (self, GOODIX_PSK_ADDR, GOODIX_PSK_LEN,
                       self->psk) == GOODIX_PSK_LEN)
        {
          if (att > 1)
            fp_info ("PSK read on attempt %d", att);
          return TRUE;
        }

      if (att >= 3 && path)
        {
          /* Full reopen, to clear the kernel driver's state. */
          if (self->spi_fd >= 0)
            close (self->spi_fd);
          self->spi_fd = open (path, O_RDWR);
          if (self->spi_fd < 0)
            return FALSE;
          {
            guint8 mode = SPI_MODE_0, bits = 8;
            guint32 speed = 10000000;
            ioctl (self->spi_fd, SPI_IOC_WR_MODE, &mode);
            ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
            ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
          }
          flock (self->spi_fd, LOCK_EX | LOCK_NB);
        }

      gx_gpio_reset (self);
    }
  return FALSE;
}

static void
gx_dev_open (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  GError *err = NULL;
  const gchar *path;
  gchar fw[64] = "";

  /* Drop stores whose fprintd template was deleted meanwhile. */
  gx_adapt_sweep ();

  path = fpi_device_get_udev_data (dev, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);
  self->spi_fd = open (path, O_RDWR);
  if (self->spi_fd < 0)
    {
      g_set_error (&err, G_IO_ERROR, g_io_error_from_errno (errno),
                   "cannot open spidev node %s", path);
      fpi_device_open_complete (dev, err);
      return;
    }

  /* Exclusive lock. Nothing stops two processes opening the same spidev node,
   * and two interleaved dialogues corrupt frame boundaries — the observed
   * symptoms being failed TLS handshakes and an unreadable key. Refusing the
   * open outright is better than corrupting the bus. */
  if (flock (self->spi_fd, LOCK_EX | LOCK_NB) != 0)
    {
      close (self->spi_fd);
      self->spi_fd = -1;
      fpi_device_open_complete (dev, fpi_device_error_new_msg (
        FP_DEVICE_ERROR_BUSY, "sensor already in use by another process"));
      return;
    }

  {
    guint8 mode = SPI_MODE_0, bits = 8;
    guint32 speed = 10000000;
    ioctl (self->spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
  }
  /* Fallbacks until the first calibration measures the unit: fdt_abs is the
   * hard-coded floor, timing_scale is nominal. Both then adapt per unit. */
  self->fdt_abs = GOODIX_FDT_ABS;
  self->timing_scale = gx_timing_load ();
  self->timing_saved = self->timing_scale;
  if (self->timing_scale != 100)
    fp_info ("starting from learned timing scale %d%%", self->timing_scale);

  gx_gpio_reset (self);

  if (gx_read_fw_version (self, fw, sizeof fw))
    fp_info ("GXFP5187 firmware: %s", fw);
  if (!gx_read_psk (self, path))
    fp_warn ("key read failed; the TLS channel cannot be opened");

  fpi_device_open_complete (dev, NULL);
}

static void
gx_dev_close (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);

  /* Cancel a pending finger-detection poll before tearing state down: it holds
   * the SSM as user data and would fire on freed state otherwise. */
  g_clear_handle_id (&self->poll_id, g_source_remove);
  gx_tls_teardown (self);
  if (self->spi_fd >= 0)
    {
      close (self->spi_fd);
      self->spi_fd = -1;
    }
  g_clear_pointer (&self->bg_frame, g_free);
  fpi_device_close_complete (dev, NULL);
}

/* --- verification completion --------------------------------------- */
static void
gx_verify_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  GxTask *t = fpi_ssm_get_data (ssm);
  FpPrint *template = NULL;
  g_autoptr(GPtrArray) views = NULL;
  int best = 0;

  if (error)
    {
      if (error->domain == FP_DEVICE_RETRY)
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, g_steal_pointer (&error));
      fpi_device_verify_complete (dev, error);
      return;
    }
  if (!t->probe)
    {
      fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
        fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      fpi_device_verify_complete (dev, NULL);
      return;
    }

  best = t->best;
  fpi_device_get_verify_data (dev, &template);
  views = gx_views_from_print (template);
  {
    g_autofree gchar *apath = gx_adapt_path (template);
    g_autoptr(GPtrArray) extra = gx_adapt_load (apath);

    fp_info ("verify: %d matches in %d attempt(s) "
             "(threshold %d, %u views + %u acquired)", best, t->tries,
             GX_MATCH_THRESHOLD, views->len, extra->len);

    /* Acquisition window. The lower bound means certain identity; the upper
     * bound means the area is already well covered and would teach nothing.
     * Weak poses — precisely the ones missing from the template — are excluded
     * on purpose, being too uncertain to trust. They enter the window by
     * themselves as the store fills out and lifts their score, so coverage
     * grows step by step without ever relaxing the safety constraint. */
    if (best >= GX_ADAPT_MIN &&
        best <= (int) (GX_ADAPT_MAX_FRAC * t->probe->n) &&
        extra->len < GX_ADAPT_VIEWS)
      {
        if (gx_adapt_append (apath, t->probe))
          fp_info ("adaptive store: view acquired (%u/%d)",
                   extra->len + 1, GX_ADAPT_VIEWS);
      }
  }
  fpi_device_verify_report (dev,
                            best >= GX_MATCH_THRESHOLD ? FPI_MATCH_SUCCESS
                                                       : FPI_MATCH_FAIL,
                            NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
gx_dev_enroll (FpDevice *dev)
{
  FpiSsm *ssm = fpi_ssm_new (dev, gx_run_state, GX_ST_NUM);
  GxTask *t = g_new0 (GxTask, 1);
  FpPrint *print = NULL;
  FpiPrintType ptype = FPI_PRINT_UNDEFINED;

  t->views = g_ptr_array_new_with_free_func ((GDestroyNotify) gx_sift_free);

  /* Updating an existing print: keep its views and add the new ones.
   * Note this path is unused with fprintd, whose enrol always supplies a fresh
   * FpPrint, so in practice re-enrolling REPLACES the template. */
  fpi_device_get_enroll_data (dev, &print);
  if (print)
    g_object_get (print, "fpi-type", &ptype, NULL);
  if (ptype != FPI_PRINT_UNDEFINED)
    {
      g_autoptr(GPtrArray) old = gx_views_from_print (print);
      guint i;
      for (i = 0; i < old->len; i++)
        g_ptr_array_add (t->views, g_ptr_array_index (old, i));
      g_ptr_array_set_free_func (old, NULL);   /* ownership transferred */
      if (t->views->len)
        fp_info ("enroll: kept %u existing views", t->views->len);
    }
  fpi_ssm_set_data (ssm, t, (GDestroyNotify) gx_task_free);
  fpi_ssm_start (ssm, gx_enroll_done);
}

static void
gx_dev_verify (FpDevice *dev)
{
  FpiSsm *ssm = fpi_ssm_new (dev, gx_run_state, GX_ST_NUM);
  GxTask *t = g_new0 (GxTask, 1);

  t->verifying = TRUE;
  fpi_ssm_set_data (ssm, t, (GDestroyNotify) gx_task_free);
  fpi_ssm_start (ssm, gx_verify_done);
}

/* ------------------------------------------------------------------ */

static void
fpi_device_goodixtls_init (FpiDeviceGoodixTls *self)
{
  self->spi_fd = -1;
}

static void
fpi_device_goodixtls_finalize (GObject *object)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (object);

  if (self->spi_fd >= 0)
    close (self->spi_fd);
  g_clear_pointer (&self->bg_frame, g_free);
  G_OBJECT_CLASS (fpi_device_goodixtls_parent_class)->finalize (object);
}

static void
fpi_device_goodixtls_class_init (FpiDeviceGoodixTlsClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "goodixtls";
  dev_class->full_name = "Goodix GXFP5187 SPI (TLS-PSK)";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = goodixtls_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = GX_ENROLL_STAGES;

  dev_class->temp_hot_seconds = -1;   /* slow capture: no thermal throttling */

  dev_class->open = gx_dev_open;
  dev_class->close = gx_dev_close;
  dev_class->enroll = gx_dev_enroll;
  dev_class->verify = gx_dev_verify;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_goodixtls_finalize;

  /* Derives the advertised features from the vfuncs implemented above.
   * Must be called LAST: otherwise the device is announced without
   * verification support. FpImageDevice did this for us; deriving straight
   * from FpDevice makes it the driver's job. */
  fpi_device_class_auto_initialize_features (dev_class);

  /* Advertise that an existing print can be updated rather than replaced,
   * for callers that pass one in. */
  dev_class->features |= FP_DEVICE_FEATURE_UPDATE_PRINT;
}
