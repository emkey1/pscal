#ifndef PSCAL_NOISE_H
#define PSCAL_NOISE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

float pscalPerlin2D(float x, float y, uint32_t seed);
float pscalSimplex2D(float x, float y, uint32_t seed);

void pscalGeneratePermutation(uint32_t seed, uint8_t *out, size_t count);

#ifdef __cplusplus
}
#endif

#endif // PSCAL_NOISE_H
