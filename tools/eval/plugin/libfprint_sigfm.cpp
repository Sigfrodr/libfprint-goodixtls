/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * libfprint_sigfm.cpp — fp_eval plug-in for the SIGFM matcher proposed for
 * libfprint's core (libfprint!530, libfprint/sigfm/).
 *
 * Wraps sigfm.cpp from that branch, unchanged, and reproduces how libfprint
 * calls it: fp_image rejects an image with fewer than 25 keypoints, and
 * fpi_print_sigfm_match() accepts when ANY enrolled view scores at or above
 * the driver's threshold, calling sigfm_match_score (enrolled_view, probe).
 * That is fp_eval's default best-of-gallery rule, so no gallery entry point
 * is needed. Drivers set the threshold (elan uses 24).
 *
 * Build against a checkout of the MR branch (needs OpenCV 4):
 *
 *     g++ -O2 -shared -fPIC -o libfprint_sigfm.so plugin/libfprint_sigfm.cpp \
 *         $LF/libfprint/sigfm/sigfm.cpp -I$LF/libfprint/sigfm \
 *         $(pkg-config --cflags --libs opencv4)
 */
#include "fp_eval_plugin.h"
#include "sigfm.h"

extern "C" {

const char *
fpeval_name (void)
{
  return "libfprint-sigfm";
}

void *
fpeval_extract (const uint8_t *img, int w, int h)
{
  SigfmImgInfo *info;

  if (!img || w <= 0 || h <= 0)
    return nullptr;
  info = sigfm_extract (img, w, h);
  if (info && sigfm_keypoints_count (info) < 25)
    {
      sigfm_free_info (info);         /* fp_image refuses these */
      return nullptr;
    }
  return info;
}

double
fpeval_score (void *probe, void *gallery)
{
  int s;

  if (!probe || !gallery)
    return 0.0;
  s = sigfm_match_score (static_cast<SigfmImgInfo *> (gallery),
                         static_cast<SigfmImgInfo *> (probe));
  return s < 0 ? 0.0 : s;
}

void
fpeval_free (void *feat)
{
  if (feat)
    sigfm_free_info (static_cast<SigfmImgInfo *> (feat));
}

}
