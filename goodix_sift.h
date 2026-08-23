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

/*
 * Why this exists instead of NBIS: FpImageDevice mandates the NBIS pipeline
 * (minutiae plus bozorth3), which this sensor cannot feed. Its 6x5 mm surface
 * yields about 6 minutiae per capture where reliable matching needs 20 to 40,
 * and the measured bozorth3 score was consistently zero. Enhancing ridges or
 * upscaling does not help: the limit is sensor area, not image quality.
 *
 * So this is a local-descriptor matcher instead: keypoints are detected where
 * the ridge orientation field is incoherent (ridges themselves look alike
 * everywhere and discriminate nothing), described by a rotation-normalised
 * RootSIFT descriptor, matched by L2 distance with Lowe's ratio test, then
 * validated geometrically.
 *
 * These are standard published techniques (Lowe 2004; Arandjelović and
 * Zisserman 2012). No vendor code is reused.
 */

#pragma once

#include <glib.h>

#define GX_SIFT_CELLS   4
#define GX_SIFT_BINS    8
#define GX_SIFT_DIM     (GX_SIFT_CELLS * GX_SIFT_CELLS * GX_SIFT_BINS)  /* 128 */

#ifndef GX_SIFT_PATCH
#define GX_SIFT_PATCH   20      /* descriptor window side, in pixels */
#endif

#ifndef GX_SIFT_MAX_PTS
/* Keypoints kept per image. Measured on recorded capture sets: raising this
 * from 60 to 150 lifts the genuine score from 6.5 to 11.7 matches on average
 * without lifting an impostor's. Beyond 150 the detector saturates: a 132x112
 * image simply holds no more salient points. */
#define GX_SIFT_MAX_PTS 150
#endif

/* 8-bit quantised descriptor: compact, and precise enough for matching. */
typedef struct
{
  gint16 x, y;                        /* position within the image */
  guint8 desc[GX_SIFT_DIM];           /* quantised RootSIFT descriptor */
} GxSiftPoint;

typedef struct
{
  guint         n;
  GxSiftPoint  *pts;
} GxSiftFeatures;

/* Extracts keypoints and descriptors from a greyscale image. Pixel values may
 * be on any scale; only the structure matters. */
GxSiftFeatures *gx_sift_extract (const double *img, int w, int h);

void gx_sift_free (GxSiftFeatures *f);

/* Number of geometrically consistent matches between two feature sets. This is
 * the similarity score: above a threshold, both come from the same finger. */
int gx_sift_match (const GxSiftFeatures *a, const GxSiftFeatures *b);

/* Same, but also flags in @mask (sized b->n) the points of @b that matched
 * consistently. Lets the caller accumulate evidence across several template
 * views instead of keeping only the best view's score. */
int gx_sift_match_mask (const GxSiftFeatures *a, const GxSiftFeatures *b,
                        guint8 *mask);

/* Same, and also reports the (b - a) translation of the best consistent
 * cluster. This is what allows template views to be registered into a common
 * frame. */
int gx_sift_match_xform (const GxSiftFeatures *a, const GxSiftFeatures *b,
                         guint8 *mask, int *out_tx, int *out_ty);

/*
 * Incremental registration, used to guide enrolment in real time.
 *
 * Enrolment views are independent images, each in its own frame. Registering
 * each new view against the accumulated ones tells the user interface where on
 * the finger the press landed and how much of the finger is covered so far,
 * which is the dominant factor in recognition rate.
 *
 * Registration is incremental by necessity: rebuilding the whole mosaic on
 * every press would be quadratic (about 900 view matches by the 15th press)
 * and would stall the caller.
 */
typedef struct _GxSiftIsland GxSiftIsland;

GxSiftIsland *gx_sift_island_new (void);
void          gx_sift_island_free (GxSiftIsland *island);

/* Registers @v against the island and merges it in. Returns FALSE when the
 * view does not overlap the island enough to be placed safely, in which case
 * the caller should ask for a press closer to the previous ones.
 * @out_dx and @out_dy give the view's position in the island's frame. */
gboolean gx_sift_island_add (GxSiftIsland *island, const GxSiftFeatures *v,
                             int *out_dx, int *out_dy, int *out_score);

void gx_sift_island_extent (GxSiftIsland *island, int *x0, int *y0,
                            int *x1, int *y1, guint *npts);

/* Compact serialisation, for storage inside an FpPrint. */
GByteArray *gx_sift_serialize (const GxSiftFeatures *f);
GxSiftFeatures *gx_sift_deserialize (const guint8 *data, gsize len);
