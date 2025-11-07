#include "noise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef PSCAL_NOISE_PERMUTATION_SIZE
#define PSCAL_NOISE_PERMUTATION_SIZE 256
#endif

static float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static float grad2(uint8_t hash, float x, float y) {
    static const float gradients[8][2] = {
        {1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f},
        {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}
    };
    const float *g = gradients[hash & 7];
    return g[0] * x + g[1] * y;
}

void pscalGeneratePermutation(uint32_t seed, uint8_t *out, size_t count) {
    if (!out || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        out[i] = (uint8_t)i;
    }
    uint32_t state = seed ? seed : 0xdeadbeefu;
    for (size_t i = count - 1; i > 0; --i) {
        state = state * 1664525u + 1013904223u;
        size_t j = state % (i + 1);
        uint8_t tmp = out[i];
        out[i] = out[j];
        out[j] = tmp;
    }
}

static void buildPermutation(uint32_t seed, uint8_t *perm) {
    uint8_t base[PSCAL_NOISE_PERMUTATION_SIZE];
    pscalGeneratePermutation(seed, base, PSCAL_NOISE_PERMUTATION_SIZE);
    for (size_t i = 0; i < PSCAL_NOISE_PERMUTATION_SIZE * 2; ++i) {
        perm[i] = base[i % PSCAL_NOISE_PERMUTATION_SIZE];
    }
}

float pscalPerlin2D(float x, float y, uint32_t seed) {
    uint8_t perm[PSCAL_NOISE_PERMUTATION_SIZE * 2];
    buildPermutation(seed, perm);

    int xi0 = (int)floorf(x) & 255;
    int yi0 = (int)floorf(y) & 255;
    int xi1 = (xi0 + 1) & 255;
    int yi1 = (yi0 + 1) & 255;

    float xf0 = x - floorf(x);
    float yf0 = y - floorf(y);
    float xf1 = xf0 - 1.0f;
    float yf1 = yf0 - 1.0f;

    float u = fade(xf0);
    float v = fade(yf0);

    uint8_t aa = perm[perm[xi0] + yi0];
    uint8_t ab = perm[perm[xi0] + yi1];
    uint8_t ba = perm[perm[xi1] + yi0];
    uint8_t bb = perm[perm[xi1] + yi1];

    float x1 = lerpf(grad2(aa, xf0, yf0), grad2(ba, xf1, yf0), u);
    float x2 = lerpf(grad2(ab, xf0, yf1), grad2(bb, xf1, yf1), u);

    return lerpf(x1, x2, v);
}

float pscalSimplex2D(float x, float y, uint32_t seed) {
    static const float F2 = 0.3660254037844386f; // (sqrt(3)-1)/2
    static const float G2 = 0.2113248654051871f; // (3-sqrt(3))/6

    uint8_t perm[PSCAL_NOISE_PERMUTATION_SIZE * 2];
    buildPermutation(seed, perm);

    float s = (x + y) * F2;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);

    float t = (float)(i + j) * G2;
    float X0 = i - t;
    float Y0 = j - t;
    float x0 = x - X0;
    float y0 = y - Y0;

    int i1, j1;
    if (x0 > y0) {
        i1 = 1; j1 = 0;
    } else {
        i1 = 0; j1 = 1;
    }

    float x1 = x0 - i1 + G2;
    float y1 = y0 - j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;

    int ii = i & 255;
    int jj = j & 255;

    float n0 = 0.0f;
    float n1 = 0.0f;
    float n2 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0) {
        uint8_t gi0 = perm[ii + perm[jj]] & 7;
        t0 *= t0;
        n0 = t0 * t0 * grad2(gi0, x0, y0);
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0) {
        uint8_t gi1 = perm[ii + i1 + perm[jj + j1]] & 7;
        t1 *= t1;
        n1 = t1 * t1 * grad2(gi1, x1, y1);
    }

    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0) {
        uint8_t gi2 = perm[ii + 1 + perm[jj + 1]] & 7;
        t2 *= t2;
        n2 = t2 * t2 * grad2(gi2, x2, y2);
    }

    return 70.0f * (n0 + n1 + n2);
}
