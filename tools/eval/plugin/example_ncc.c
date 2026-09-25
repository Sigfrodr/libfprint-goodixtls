/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * example_ncc.c — a minimal, working fp_eval plug-in.
 *
 * It is NOT a serious matcher: it normalises each capture to a fixed 32x32
 * zero-mean unit-norm vector and scores two captures by normalised
 * cross-correlation. Its only job is to show the ABI end to end and give you
 * something to copy. Replace the three bodies with calls into your driver's
 * real matcher (feature extraction, comparison, free).
 *
 *     cc -O2 -shared -fPIC -o example_ncc.so example_ncc.c -lm
 *     python3 ../fp_eval.py --captures ./captures --backend-so ./example_ncc.so
 */
#include "fp_eval_plugin.h"
#include <math.h>
#include <stdlib.h>

#define N 32   /* fixed descriptor grid side */

typedef struct { float v[N * N]; } feat_t;

const char *fpeval_name(void) { return "example-ncc"; }

void *fpeval_extract(const uint8_t *img, int w, int h)
{
    if (!img || w <= 0 || h <= 0)
        return NULL;
    feat_t *f = (feat_t *)malloc(sizeof(feat_t));
    if (!f)
        return NULL;

    /* box-average the image down to N x N */
    for (int gy = 0; gy < N; gy++) {
        for (int gx = 0; gx < N; gx++) {
            int x0 = (int)((long)gx * w / N), x1 = (int)((long)(gx + 1) * w / N);
            int y0 = (int)((long)gy * h / N), y1 = (int)((long)(gy + 1) * h / N);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            double s = 0; long n = 0;
            for (int y = y0; y < y1 && y < h; y++)
                for (int x = x0; x < x1 && x < w; x++) { s += img[y * w + x]; n++; }
            f->v[gy * N + gx] = (float)(n ? s / n : 0.0);
        }
    }

    /* zero-mean, unit-norm */
    double mean = 0;
    for (int i = 0; i < N * N; i++) mean += f->v[i];
    mean /= N * N;
    double norm = 0;
    for (int i = 0; i < N * N; i++) { f->v[i] -= (float)mean; norm += (double)f->v[i] * f->v[i]; }
    norm = sqrt(norm);
    if (norm < 1e-6) { free(f); return NULL; }
    for (int i = 0; i < N * N; i++) f->v[i] /= (float)norm;
    return f;
}

double fpeval_score(void *probe, void *gallery)
{
    const feat_t *a = (const feat_t *)probe, *b = (const feat_t *)gallery;
    if (!a || !b)
        return 0.0;
    double dot = 0;
    for (int i = 0; i < N * N; i++) dot += (double)a->v[i] * b->v[i];
    if (dot < 0) dot = 0;          /* clamp; keep it monotone and non-negative */
    return dot * 100.0;            /* NCC in [0,1] -> a 0..100 "score" */
}

void fpeval_free(void *feat) { free(feat); }
