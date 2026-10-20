/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * goodix_rootsift.c — fp_eval plug-in for this driver's own matcher.
 *
 * Wraps goodix_sift.c, the code the driver ships, unchanged: extraction is
 * gx_sift_extract() and the gallery score is the driver's view fusion — the
 * probe points that AT LEAST ONE enrolled view explains, accumulated with
 * gx_sift_match_mask() exactly as the verify path in goodixtls.c does it. The
 * decision threshold to compare against follows the gallery size: see
 * gx_match_threshold() (15 up to 15 views, 18 up to 20, 20 up to 30).
 *
 * What it does not reproduce: pam_fprintd allows up to three attempts, and the
 * driver also matches against views adapted after enrolment. Both only help
 * the genuine side, so the FRR measured here is a pessimistic bound.
 *
 * Feed it captures converted by goodix_dump.py, which applies the driver's
 * preprocessing; the matcher is invariant to intensity scale and offset, so
 * the 8-bit conversion costs only quantisation.
 *
 *     cc -O2 -shared -fPIC -o goodix_rootsift.so plugin/goodix_rootsift.c \
 *        ../../goodix_sift.c -I../.. $(pkg-config --cflags --libs glib-2.0) -lm
 *     python3 fp_eval.py --captures ./captures --enroll 30 \
 *        --backend-so ./goodix_rootsift.so
 */
#include "fp_eval_plugin.h"
#include "goodix_sift.h"

const char *
fpeval_name (void)
{
  return "goodixtls-rootsift";
}

void *
fpeval_extract (const uint8_t *img, int w, int h)
{
  g_autofree double *d = NULL;
  int i;

  if (!img || w <= 0 || h <= 0)
    return NULL;
  d = g_new (double, w * h);
  for (i = 0; i < w * h; i++)
    d[i] = img[i];
  return gx_sift_extract (d, w, h);
}

/* One enrolled view against the probe, as the driver counts it. */
double
fpeval_score (void *probe, void *gallery)
{
  if (!probe || !gallery)
    return 0.0;
  return gx_sift_match (gallery, probe);
}

/* The driver's real decision: fuse the evidence of every enrolled view. */
double
fpeval_score_gallery (void *probe, void **gallery, int n)
{
  const GxSiftFeatures *f = probe;
  g_autofree guint8 *seen = NULL;
  int i, fused = 0;

  if (!f)
    return 0.0;
  seen = g_new0 (guint8, f->n ? f->n : 1);
  for (i = 0; i < n; i++)
    if (gallery[i])
      gx_sift_match_mask (gallery[i], f, seen);
  for (i = 0; i < (int) f->n; i++)
    fused += seen[i];
  return fused;
}

void
fpeval_free (void *feat)
{
  if (feat)
    gx_sift_free (feat);
}
