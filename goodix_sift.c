/*
 * Local descriptor extraction and matching for fingerprint images
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

#define _USE_MATH_DEFINES
#define _GNU_SOURCE

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "goodix_sift.h"

#ifndef RATIO
#define RATIO       0.90        /* Lowe's ratio test */
#endif
#ifndef GEO_TOL
#define GEO_TOL     6.0         /* geometric consistency tolerance, in pixels */
#endif
#define MIN_PAIRS   3

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Mean over a (2r+1)^2 window, via a summed-area table. */
static void
box_mean (const double *a, double *out, int w, int h, int r)
{
  int x, y;
  g_autofree double *ii = g_malloc0 (sizeof (double) * (w + 1) * (h + 1));

  for (y = 0; y < h; y++)
    {
      double row = 0;
      for (x = 0; x < w; x++)
        {
          row += a[y * w + x];
          ii[(y + 1) * (w + 1) + x + 1] = ii[y * (w + 1) + x + 1] + row;
        }
    }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int y0 = CLAMP (y - r, 0, h), y1 = CLAMP (y + r + 1, 0, h);
        int x0 = CLAMP (x - r, 0, w), x1 = CLAMP (x + r + 1, 0, w);
        double s = ii[y1 * (w + 1) + x1] - ii[y0 * (w + 1) + x1]
                   - ii[y1 * (w + 1) + x0] + ii[y0 * (w + 1) + x0];
        int n = (y1 - y0) * (x1 - x0);
        out[y * w + x] = n ? s / n : 0.0;
      }
}

/* Central differences. */
static void
gradients (const double *img, double *gx, double *gy, int w, int h)
{
  int x, y;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int xm = x > 0 ? x - 1 : x, xp = x < w - 1 ? x + 1 : x;
        int ym = y > 0 ? y - 1 : y, yp = y < h - 1 ? y + 1 : y;
        gx[y * w + x] = (img[y * w + xp] - img[y * w + xm]) * 0.5;
        gy[y * w + x] = (img[yp * w + x] - img[ym * w + x]) * 0.5;
      }
}

/* ------------------------------------------------------------------ */
/*  Extraction                                                         */
/* ------------------------------------------------------------------ */

GxSiftFeatures *
gx_sift_extract (const double *img, int w, int h)
{
  int n = w * h, i, x, y, k;
  g_autofree double *gx = g_malloc (sizeof (double) * n);
  g_autofree double *gy = g_malloc (sizeof (double) * n);
  g_autofree double *vx = g_malloc (sizeof (double) * n);
  g_autofree double *vy = g_malloc (sizeof (double) * n);
  g_autofree double *smooth_x = g_malloc (sizeof (double) * n);
  g_autofree double *smooth_y = g_malloc (sizeof (double) * n);
  g_autofree double *mag = g_malloc (sizeof (double) * n);
  g_autofree double *magm = g_malloc (sizeof (double) * n);
  g_autofree double *theta = g_malloc (sizeof (double) * n);
  g_autofree double *c2 = g_malloc (sizeof (double) * n);
  g_autofree double *s2 = g_malloc (sizeof (double) * n);
  g_autofree double *c2m = g_malloc (sizeof (double) * n);
  g_autofree double *s2m = g_malloc (sizeof (double) * n);
  g_autofree double *sal = g_malloc (sizeof (double) * n);
  GxSiftFeatures *f;

  gradients (img, gx, gy, w, h);
  for (i = 0; i < n; i++)
    {
      vx[i] = gx[i] * gx[i] - gy[i] * gy[i];
      vy[i] = 2.0 * gx[i] * gy[i];
      mag[i] = hypot (gx[i], gy[i]);
    }

  /* Smoothed orientation field. */
  box_mean (vx, smooth_x, w, h, 4);
  box_mean (vy, smooth_y, w, h, 4);
  for (i = 0; i < n; i++)
    theta[i] = 0.5 * atan2 (smooth_y[i], smooth_x[i]);

  /* Saliency: high where ridge orientation is INCOHERENT (the flow changes)
   * and contrast is sufficient. That combination is what makes a location
   * distinctive — ridges themselves look alike everywhere, so a detector keyed
   * on ridge strength alone would fire on featureless areas. */
  for (i = 0; i < n; i++)
    {
      c2[i] = cos (2 * theta[i]);
      s2[i] = sin (2 * theta[i]);
    }
  box_mean (c2, c2m, w, h, 3);
  box_mean (s2, s2m, w, h, 3);
  box_mean (mag, magm, w, h, 3);
  for (i = 0; i < n; i++)
    {
      double coh = hypot (c2m[i], s2m[i]);
      sal[i] = (1.0 - coh) * magm[i];
    }

  /* Exclude the border: the descriptor needs its full window. */
  {
    int b = GX_SIFT_PATCH / 2 + 2;
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        if (y < b || y >= h - b || x < b || x >= w - b)
          sal[y * w + x] = 0.0;
  }

  f = g_new0 (GxSiftFeatures, 1);
  f->pts = g_new0 (GxSiftPoint, GX_SIFT_MAX_PTS);

  /* Pick maxima, enforcing a minimum spacing. */
  for (k = 0; k < GX_SIFT_MAX_PTS; k++)
    {
      int best = -1;
      double bv = 0.0;

      for (i = 0; i < n; i++)
        if (sal[i] > bv)
          { bv = sal[i]; best = i; }
      if (best < 0)
        break;
      x = best % w; y = best / w;
      f->pts[f->n].x = x;
      f->pts[f->n].y = y;
      f->n++;
      for (int yy = MAX (0, y - 6); yy < MIN (h, y + 7); yy++)
        for (int xx = MAX (0, x - 6); xx < MIN (w, x + 7); xx++)
          sal[yy * w + xx] = 0.0;
    }

  /* Descriptors. */
  for (k = 0; k < (int) f->n; k++)
    {
      double hist[GX_SIFT_DIM] = { 0 };
      double a = theta[f->pts[k].y * w + f->pts[k].x];
      double ca = cos (-a), sa = sin (-a);
      double half = GX_SIFT_PATCH / 2.0;
      double step = (double) GX_SIFT_PATCH / GX_SIFT_CELLS;
      double norm = 0.0;
      int dx, dy;

      for (dy = -GX_SIFT_PATCH / 2; dy < GX_SIFT_PATCH / 2; dy++)
        for (dx = -GX_SIFT_PATCH / 2; dx < GX_SIFT_PATCH / 2; dx++)
          {
            double rx = dx * ca - dy * sa;
            double ry = dx * sa + dy * ca;
            int cx = (int) ((rx + half) / step);
            int cy = (int) ((ry + half) / step);
            int yy = f->pts[k].y + dy, xx = f->pts[k].x + dx;
            double g, ang;
            int b;

            if (cx < 0 || cx >= GX_SIFT_CELLS || cy < 0 || cy >= GX_SIFT_CELLS)
              continue;
            if (yy < 0 || yy >= h || xx < 0 || xx >= w)
              continue;
            ang = atan2 (gy[yy * w + xx], gx[yy * w + xx]) - a;
            g = fmod (ang, 2 * M_PI);
            if (g < 0) g += 2 * M_PI;
            b = (int) (g / (2 * M_PI) * GX_SIFT_BINS) % GX_SIFT_BINS;
            hist[(cy * GX_SIFT_CELLS + cx) * GX_SIFT_BINS + b] += mag[yy * w + xx];
          }

      /* L2 normalise, clip at 0.2, renormalise, square root (RootSIFT). */
      for (i = 0; i < GX_SIFT_DIM; i++) norm += hist[i] * hist[i];
      norm = sqrt (norm);
      if (norm < 1e-9)
        { memset (f->pts[k].desc, 0, GX_SIFT_DIM); continue; }
      for (i = 0; i < GX_SIFT_DIM; i++)
        hist[i] = MIN (hist[i] / norm, 0.2);
      norm = 0.0;
      for (i = 0; i < GX_SIFT_DIM; i++) norm += hist[i] * hist[i];
      norm = sqrt (norm) + 1e-9;
      for (i = 0; i < GX_SIFT_DIM; i++)
        {
          double v = sqrt (hist[i] / norm);          /* RootSIFT */
          f->pts[k].desc[i] = (guint8) CLAMP ((int) (v * 255.0), 0, 255);
        }
    }
  return f;
}

void
gx_sift_free (GxSiftFeatures *f)
{
  if (!f)
    return;
  g_free (f->pts);
  g_free (f);
}

/* ------------------------------------------------------------------ */
/*  Matching                                                           */
/* ------------------------------------------------------------------ */

static double
desc_dist2 (const guint8 *a, const guint8 *b)
{
  double s = 0;
  int i;

  for (i = 0; i < GX_SIFT_DIM; i++)
    {
      double d = (double) a[i] - b[i];
      s += d * d;
    }
  return s;
}

int
gx_sift_match_xform (const GxSiftFeatures *A, const GxSiftFeatures *B,
                     guint8 *mask, int *out_tx, int *out_ty)
{
  int i, j, np = 0, best = 0;
  double bt_x = 0, bt_y = 0;
  g_autofree int *pi = NULL, *pj = NULL;

  if (!A || !B || A->n < MIN_PAIRS || B->n < MIN_PAIRS)
    return 0;
  pi = g_new (int, A->n);
  pj = g_new (int, A->n);

  /* Nearest-neighbour matching with Lowe's ratio test.
   *
   * Mutual-consistency filtering (keeping only pairs that are each other's
   * nearest neighbour) was tried and rejected: measured on recorded sets it
   * drops genuine scores from 16 to 12 while impostor scores stay put. */
  for (i = 0; i < (int) A->n; i++)
    {
      double d1 = 1e300, d2 = 1e300;
      int jb = -1;

      for (j = 0; j < (int) B->n; j++)
        {
          double d = desc_dist2 (A->pts[i].desc, B->pts[j].desc);
          if (d < d1) { d2 = d1; d1 = d; jb = j; }
          else if (d < d2) d2 = d;
        }
      if (jb >= 0 && d1 < RATIO * RATIO * d2)
        { pi[np] = i; pj[np] = jb; np++; }
    }
  if (np < MIN_PAIRS)
    return 0;

  /* Geometric validation: vote on the implied translation. Widening GEO_TOL
   * beyond 6 px was tried and rejected — it lifts impostor scores as much as
   * genuine ones. */
  for (i = 0; i < np; i++)
    {
      double tx = B->pts[pj[i]].x - A->pts[pi[i]].x;
      double ty = B->pts[pj[i]].y - A->pts[pi[i]].y;
      int cnt = 0;

      for (j = 0; j < np; j++)
        {
          double ex = B->pts[pj[j]].x - A->pts[pi[j]].x - tx;
          double ey = B->pts[pj[j]].y - A->pts[pi[j]].y - ty;
          if (ex * ex + ey * ey <= GEO_TOL * GEO_TOL)
            cnt++;
        }
      if (cnt > best)
        { best = cnt; bt_x = tx; bt_y = ty; }
    }

  if (out_tx) *out_tx = (int) bt_x;
  if (out_ty) *out_ty = (int) bt_y;

  /* Flag the points of B belonging to the best cluster, so the caller can
   * merge evidence across several template views. */
  if (mask && best >= MIN_PAIRS)
    for (j = 0; j < np; j++)
      {
        double ex = B->pts[pj[j]].x - A->pts[pi[j]].x - bt_x;
        double ey = B->pts[pj[j]].y - A->pts[pi[j]].y - bt_y;
        if (ex * ex + ey * ey <= GEO_TOL * GEO_TOL)
          mask[pj[j]] = 1;
      }
  return best;
}

int
gx_sift_match_mask (const GxSiftFeatures *A, const GxSiftFeatures *B,
                    guint8 *mask)
{
  return gx_sift_match_xform (A, B, mask, NULL, NULL);
}

int
gx_sift_match (const GxSiftFeatures *A, const GxSiftFeatures *B)
{
  return gx_sift_match_xform (A, B, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Serialisation                                                      */
/* ------------------------------------------------------------------ */

GByteArray *
gx_sift_serialize (const GxSiftFeatures *f)
{
  GByteArray *a = g_byte_array_new ();
  guint32 n = f ? f->n : 0;
  guint i;

  g_byte_array_append (a, (guint8 *) &n, sizeof n);
  for (i = 0; i < n; i++)
    {
      g_byte_array_append (a, (guint8 *) &f->pts[i].x, sizeof (gint16));
      g_byte_array_append (a, (guint8 *) &f->pts[i].y, sizeof (gint16));
      g_byte_array_append (a, f->pts[i].desc, GX_SIFT_DIM);
    }
  return a;
}

GxSiftFeatures *
gx_sift_deserialize (const guint8 *data, gsize len)
{
  GxSiftFeatures *f;
  guint32 n;
  gsize need;
  guint i;
  const guint8 *p = data;

  if (len < sizeof (guint32))
    return NULL;
  memcpy (&n, p, sizeof n); p += sizeof n;
  need = sizeof (guint32) + (gsize) n * (2 * sizeof (gint16) + GX_SIFT_DIM);
  if (n > 4096 || len < need)
    return NULL;

  f = g_new0 (GxSiftFeatures, 1);
  f->n = n;
  f->pts = g_new0 (GxSiftPoint, n ? n : 1);
  for (i = 0; i < n; i++)
    {
      memcpy (&f->pts[i].x, p, sizeof (gint16)); p += sizeof (gint16);
      memcpy (&f->pts[i].y, p, sizeof (gint16)); p += sizeof (gint16);
      memcpy (f->pts[i].desc, p, GX_SIFT_DIM);   p += GX_SIFT_DIM;
    }
  return f;
}

/* ------------------------------------------------------------------ */
/*  Incremental registration (enrolment guidance)                      */
/* ------------------------------------------------------------------ */

/* Registration reuses matching itself: the translation of the best
 * geometrically consistent cluster between two views IS their relative
 * offset. Views that cannot be attached stay outside the island — that is
 * intended, an enrolment may legitimately cover disjoint areas. */

#define MERGE_MIN_PAIRS  6   /* demanding: a misplacement would corrupt the
                                whole island */
#define MERGE_DEDUP_PX   3   /* closer than this in the common frame means the
                                same physical point */

struct _GxSiftIsland
{
  GArray  *pts;                   /* GxSiftPoint, in the island's frame */
  int      x0, y0, x1, y1;        /* current extent */
  gboolean empty;
};

/* Merges @src, offset by (tx, ty), into @dst, dropping points that duplicate
 * one already present.
 *
 * Deduplication is not a refinement, it is a requirement: two near-identical
 * descriptors from the same physical point seen by two views are each other's
 * nearest neighbour, which makes Lowe's ratio test reject the match outright.
 * Without it, pooling views DEGRADES matching instead of improving it. */
static void
merge_points (GArray *dst, const GxSiftFeatures *src, int tx, int ty)
{
  guint i, j;

  for (i = 0; i < src->n; i++)
    {
      GxSiftPoint p = src->pts[i];
      gboolean dup = FALSE;

      p.x = (gint16) (p.x + tx);
      p.y = (gint16) (p.y + ty);

      for (j = 0; j < dst->len && !dup; j++)
        {
          const GxSiftPoint *q = &g_array_index (dst, GxSiftPoint, j);
          int dx = q->x - p.x, dy = q->y - p.y;
          dup = (dx * dx + dy * dy) <= MERGE_DEDUP_PX * MERGE_DEDUP_PX;
        }
      if (!dup)
        g_array_append_val (dst, p);
    }
}

GxSiftIsland *
gx_sift_island_new (void)
{
  GxSiftIsland *i = g_new0 (GxSiftIsland, 1);

  i->pts = g_array_new (FALSE, FALSE, sizeof (GxSiftPoint));
  i->empty = TRUE;
  return i;
}

void
gx_sift_island_free (GxSiftIsland *i)
{
  if (!i)
    return;
  g_array_free (i->pts, TRUE);
  g_free (i);
}

static void
island_bounds (GxSiftIsland *i)
{
  guint k;

  if (!i->pts->len)
    return;
  i->x0 = i->x1 = g_array_index (i->pts, GxSiftPoint, 0).x;
  i->y0 = i->y1 = g_array_index (i->pts, GxSiftPoint, 0).y;
  for (k = 1; k < i->pts->len; k++)
    {
      const GxSiftPoint *p = &g_array_index (i->pts, GxSiftPoint, k);
      i->x0 = MIN (i->x0, p->x); i->x1 = MAX (i->x1, p->x);
      i->y0 = MIN (i->y0, p->y); i->y1 = MAX (i->y1, p->y);
    }
}

gboolean
gx_sift_island_add (GxSiftIsland *isl, const GxSiftFeatures *v,
                    int *out_dx, int *out_dy, int *out_score)
{
  GxSiftFeatures cur;
  int tx = 0, ty = 0, sc;

  if (!isl || !v || !v->n)
    return FALSE;

  if (isl->empty)
    {
      merge_points (isl->pts, v, 0, 0);
      isl->empty = FALSE;
      island_bounds (isl);
      if (out_dx) *out_dx = 0;
      if (out_dy) *out_dy = 0;
      if (out_score) *out_score = (int) v->n;
      return TRUE;
    }

  cur.n = isl->pts->len;
  cur.pts = (GxSiftPoint *) isl->pts->data;
  sc = gx_sift_match_xform (&cur, v, NULL, &tx, &ty);
  if (out_score) *out_score = sc;
  if (sc < MERGE_MIN_PAIRS)
    return FALSE;                 /* no usable overlap */

  merge_points (isl->pts, v, -tx, -ty);
  island_bounds (isl);
  if (out_dx) *out_dx = -tx;
  if (out_dy) *out_dy = -ty;
  return TRUE;
}

void
gx_sift_island_extent (GxSiftIsland *i, int *x0, int *y0, int *x1, int *y1,
                       guint *npts)
{
  if (!i)
    return;
  if (x0) *x0 = i->x0;
  if (y0) *y0 = i->y0;
  if (x1) *x1 = i->x1;
  if (y1) *y1 = i->y1;
  if (npts) *npts = i->pts->len;
}
