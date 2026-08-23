/*
 * Offline bench for the local-descriptor matcher
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

/* Offline bench for the SIFT matcher, replaying the scenario validated in
 * Python.
 *
 *   ./test-sift <reference_dir> <genuine_dir> <impostor_dir>
 *
 * Each directory holds raw driver captures (p*.bin / c*.bin) together with
 * their background (.bg). Prints the scores and the FAR/FRR pair per threshold.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "goodix_sift.h"
#define GOODIX_IMG_WIDTH  132
#define GOODIX_IMG_HEIGHT 112

/* --- chargement d'une capture (identique au pipeline Python) --------- */

static void
decode12 (const guint8 *d, gsize len, guint16 *out, gsize n)
{
  gsize i, o = 0;

  for (i = 0; i + 6 <= len && o + 4 <= n; i += 6)
    {
      const guint8 *c = d + i;
      out[o++] = ((c[0] & 0xf) << 8) | c[1];
      out[o++] = (c[3] << 4) | (c[0] >> 4);
      out[o++] = ((c[5] & 0xf) << 8) | c[2];
      out[o++] = (c[4] << 4) | (c[5] >> 4);
    }
}

static int
cmp_dbl (const void *a, const void *b)
{
  double x = *(const double *) a - *(const double *) b;
  return (x > 0) - (x < 0);
}

/* Pre-processing: background minus finger, then median row/column debanding. */
static double *
load_capture (const char *path)
{
  gchar *raw = NULL, *bgraw = NULL;
  gsize rl = 0, bl = 0;
  g_autofree gchar *bgp = g_strdup_printf ("%s.bg", path);
  const int W = GOODIX_IMG_WIDTH, H = GOODIX_IMG_HEIGHT, N = W * H;
  double *d;
  int x, y;

  if (!g_file_get_contents (path, &raw, &rl, NULL))
    return NULL;
  if (!g_file_get_contents (bgp, &bgraw, &bl, NULL))
    { g_free (raw); return NULL; }
  if (rl < (gsize) N * 2 || bl < (gsize) N * 2)
    { g_free (raw); g_free (bgraw); return NULL; }

  d = g_malloc (sizeof (double) * N);
  {
    const guint16 *px = (const guint16 *) raw;
    const guint16 *bg = (const guint16 *) bgraw;
    for (int i = 0; i < N; i++)
      d[i] = (double) bg[i] - px[i];
  }
  g_free (raw); g_free (bgraw);

  /* debanding: subtract the median per row, then per column */
  {
    g_autofree double *buf = g_malloc (sizeof (double) * MAX (W, H));
    for (y = 0; y < H; y++)
      {
        for (x = 0; x < W; x++) buf[x] = d[y * W + x];
        qsort (buf, W, sizeof (double), cmp_dbl);
        { double m = buf[W / 2]; for (x = 0; x < W; x++) d[y * W + x] -= m; }
      }
    for (x = 0; x < W; x++)
      {
        for (y = 0; y < H; y++) buf[y] = d[y * W + x];
        qsort (buf, H, sizeof (double), cmp_dbl);
        { double m = buf[H / 2]; for (y = 0; y < H; y++) d[y * W + x] -= m; }
      }
  }
  return d;
}

static GPtrArray *
load_dir (const char *dir)
{
  GPtrArray *out = g_ptr_array_new ();
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);

  if (!d)
    return out;
  while ((name = g_dir_read_name (d)))
    if (g_str_has_suffix (name, ".bin") || g_str_has_suffix (name, ".view"))
      g_ptr_array_add (names, g_strdup (name));
  g_dir_close (d);
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);

  for (guint i = 0; i < names->len; i++)
    {
      g_autofree gchar *p = g_build_filename (dir, g_ptr_array_index (names, i), NULL);
      double *img;

      /* Views already stored as descriptors (a template extracted from
         fprintd): load them as they are, so what gets evaluated is the
         template actually IN SERVICE. */
      if (g_str_has_suffix (p, ".view"))
        {
          gchar *b = NULL; gsize n = 0;
          if (g_file_get_contents (p, &b, &n, NULL))
            {
              GxSiftFeatures *f = gx_sift_deserialize ((const guint8 *) b, n);
              g_free (b);
              if (f) g_ptr_array_add (out, f);
            }
          continue;
        }
      img = load_capture (p);
      if (img)
        {
          GxSiftFeatures *f = gx_sift_extract (img, GOODIX_IMG_WIDTH, GOODIX_IMG_HEIGHT);
          const char *tr = g_getenv ("TRUNC");
          g_free (img);
          if (tr && f && f->n > (guint) atoi (tr)) f->n = atoi (tr);  /* simulates an older template */
          g_ptr_array_add (out, f);
        }
    }
  g_ptr_array_free (names, TRUE);
  return out;
}

/* skip_idx: index to leave out (cross-validation when reference == probes) */
static int
best_against (GPtrArray *refs, GxSiftFeatures *probe, int skip_idx)
{
  int best = 0, uni = 0;
  g_autofree guint8 *mask = g_new0 (guint8, probe->n ? probe->n : 1);

  for (guint i = 0; i < refs->len; i++)
    {
      GxSiftFeatures *r = g_ptr_array_index (refs, i);
      if ((int) i == skip_idx)
        continue;
      int s = g_getenv ("SWAP") ? gx_sift_match (probe, r)
                                : gx_sift_match_mask (r, probe, mask);
      if (s > best) best = s;
    }
  for (guint k = 0; k < probe->n; k++) uni += mask[k];
  return g_getenv ("FUSION") ? uni : best;
}

int
main (int argc, char **argv)
{
  if (argc < 4)
    { g_print ("usage: %s <reference> <genuine> <impostor>\n", argv[0]); return 1; }

  gboolean same_dir = (g_strcmp0 (argv[1], argv[2]) == 0);
  GPtrArray *R = load_dir (argv[1]);
  GPtrArray *G = load_dir (argv[2]);
  GPtrArray *I = load_dir (argv[3]);
  g_print ("reference %u, genuine %u, impostor %u\n", R->len, G->len, I->len);
  if (!R->len || !G->len || !I->len)
    return 1;

  g_autofree int *gs = g_new0 (int, G->len);
  g_autofree int *is = g_new0 (int, I->len);
  double gm = 0, im = 0;
  int gmin = 1 << 30, imax = 0;

  g_print ("genuine  :");
  for (guint k = 0; k < G->len; k++)
    {
      /* when the probes come from the reference directory, leave the probe
         itself out — otherwise the cross-validation flatters the result */
      gs[k] = best_against (R, g_ptr_array_index (G, k),
                            same_dir ? (int) k : -1);
      g_print (" %d", gs[k]);
      gm += gs[k];
      gmin = MIN (gmin, gs[k]);
    }
  g_print ("\nimpostor:");
  for (guint k = 0; k < I->len; k++)
    {
      is[k] = best_against (R, g_ptr_array_index (I, k), -1);
      g_print (" %d", is[k]);
      im += is[k];
      imax = MAX (imax, is[k]);
    }
  g_print ("\n\ngenuine avg=%.1f min=%d | impostor avg=%.1f max=%d\n",
           gm / G->len, gmin, im / I->len, imax);
  g_print ("SEPARATION = %+d\n", gmin - imax);

  for (int t = 3; t <= 12; t++)
    {
      int fa = 0, fr = 0;
      for (guint k = 0; k < I->len; k++) if (is[k] >= t) fa++;
      for (guint k = 0; k < G->len; k++) if (gs[k] < t) fr++;
      g_print ("  threshold %2d -> FAR=%3.0f%%  FRR=%3.0f%%\n", t,
               100.0 * fa / I->len, 100.0 * fr / G->len);
    }
  return 0;
}
