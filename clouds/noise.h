#ifndef NOISE_H
#define NOISE_H

#include <stdint.h>

uint32_t hash3u(int32_t x, int32_t y, int32_t z, uint32_t seed);
float    hash01_3(int32_t x, int32_t y, int32_t z, uint32_t seed);
float    vnoise3(float x, float y, float z, uint32_t seed);
float    vnoise2(float x, float y, uint32_t seed);
/* fBm over rotated-octave 3D value noise; t*evolve drives per-octave drift
 * so shapes churn over time. Returns [0,1]. */
float    fbm3(float x, float y, float z, int octaves, uint32_t seed,
              float t, float evolve);
float    fbm2(float x, float y, int octaves, uint32_t seed);

#endif
