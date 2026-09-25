/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * fp_eval plug-in ABI (v1)
 * ------------------------
 * Implement these four functions in a small adapter, compile it together with
 * your driver's real matcher into a shared library, and hand it to the harness:
 *
 *     python3 fp_eval.py --captures ./captures --backend-so ./mymatcher.so
 *
 * The harness then scores YOUR shipped matcher on YOUR captures under the same
 * held-out split and metrics as every other backend, and prints only aggregate
 * numbers. Nothing about the ABI touches disk or the network.
 *
 * Images are row-major, 8-bit grayscale, exactly w*h bytes, top-left origin.
 * A higher score means "more similar" (like an inlier count), consistent with
 * the other backends.
 */
#ifndef FP_EVAL_PLUGIN_H
#define FP_EVAL_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extract features/template from one capture. Return an opaque handle the
 * harness will pass back to fpeval_score(), or NULL on failure (scored as 0). */
void *fpeval_extract(const uint8_t *img, int width, int height);

/* Similarity of a probe against a gallery view. Higher = more similar. */
double fpeval_score(void *probe, void *gallery);

/* Release a handle returned by fpeval_extract(). */
void fpeval_free(void *feat);

/* Optional: a short label for the report (e.g. "gq-sigfm"). May be omitted. */
const char *fpeval_name(void);

#ifdef __cplusplus
}
#endif

#endif /* FP_EVAL_PLUGIN_H */
