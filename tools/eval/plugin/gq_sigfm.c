/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * fp_eval plug-in: GodsQuantum's GXFP51A0 matcher (SIGFM family), unmodified.
 *
 * Author: szlukabence (contributed on issue #5). Targets GodsQuantum's
 * GXFP51A0 matcher; build it against those sources (not bundled here).
 *
 *   gcc -O2 -shared -fPIC -I. -Ifastbrief -I<dir of fp_eval_plugin.h> \
 *       gq_sigfm.c goodix_sift.c fastbrief/sigfm.c \
 *       $(pkg-config --cflags --libs glib-2.0) -lm -o gq_sigfm.so
 *
 * gx_sift_extract() takes the preprocessed frame as doubles and does its own
 * 1..99 % stretch + unsharp mask, so an already-stretched 8-bit capture goes in
 * nearly unchanged.  The score is the driver's inlier count; it accepts at
 * GX_MATCH_THRESHOLD = 7, best score over the enrolled views -- the same
 * best-of-gallery rule fp_eval applies.
 */
#include <stdlib.h>

#include "fp_eval_plugin.h"
#include "goodix_sift.h"

void *
fpeval_extract (const uint8_t *img, int width, int height)
{
  size_t n = (size_t) width * (size_t) height;
  double *buf = malloc (n * sizeof *buf);
  GxSiftFeatures *f;

  if (!buf)
    return NULL;
  for (size_t i = 0; i < n; i++)
    buf[i] = img[i];
  f = gx_sift_extract (buf, width, height);
  free (buf);
  return f;
}

double
fpeval_score (void *probe, void *gallery)
{
  return gx_sift_match (probe, gallery);
}

void
fpeval_free (void *feat)
{
  if (feat)
    gx_sift_free (feat);
}

const char *
fpeval_name (void)
{
  return "gq-sigfm";
}
