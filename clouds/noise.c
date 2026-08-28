#include "noise.h"

#include <math.h>

uint32_t hash3u(int32_t x, int32_t y, int32_t z, uint32_t seed)
{
    uint32_t h = seed ^ ((uint32_t)x * 0x8da6b343u)
                      ^ ((uint32_t)y * 0xd8163841u)
                      ^ ((uint32_t)z * 0xcb1ab31fu);
    h ^= h >> 13;
    h *= 0x9e3779b1u;
    h ^= h >> 16;
    return h;
}

static inline float u2f(uint32_t h)
{
    return (float)h * (1.0f / 4294967296.0f);
}

float hash01_3(int32_t x, int32_t y, int32_t z, uint32_t seed)
{
    return u2f(hash3u(x, y, z, seed));
}

static inline float fade(float t)
{
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

static inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

float vnoise3(float x, float y, float z, uint32_t seed)
{
    float fx = floorf(x), fy = floorf(y), fz = floorf(z);
    int32_t xi = (int32_t)fx, yi = (int32_t)fy, zi = (int32_t)fz;
    float tx = fade(x - fx), ty = fade(y - fy), tz = fade(z - fz);

    float c000 = u2f(hash3u(xi,     yi,     zi,     seed));
    float c100 = u2f(hash3u(xi + 1, yi,     zi,     seed));
    float c010 = u2f(hash3u(xi,     yi + 1, zi,     seed));
    float c110 = u2f(hash3u(xi + 1, yi + 1, zi,     seed));
    float c001 = u2f(hash3u(xi,     yi,     zi + 1, seed));
    float c101 = u2f(hash3u(xi + 1, yi,     zi + 1, seed));
    float c011 = u2f(hash3u(xi,     yi + 1, zi + 1, seed));
    float c111 = u2f(hash3u(xi + 1, yi + 1, zi + 1, seed));

    float x00 = lerpf(c000, c100, tx);
    float x10 = lerpf(c010, c110, tx);
    float x01 = lerpf(c001, c101, tx);
    float x11 = lerpf(c011, c111, tx);
    float y0 = lerpf(x00, x10, ty);
    float y1 = lerpf(x01, x11, ty);
    return lerpf(y0, y1, tz);
}

float vnoise2(float x, float y, uint32_t seed)
{
    float fx = floorf(x), fy = floorf(y);
    int32_t xi = (int32_t)fx, yi = (int32_t)fy;
    float tx = fade(x - fx), ty = fade(y - fy);

    float c00 = u2f(hash3u(xi,     yi,     0x2d1u, seed));
    float c10 = u2f(hash3u(xi + 1, yi,     0x2d1u, seed));
    float c01 = u2f(hash3u(xi,     yi + 1, 0x2d1u, seed));
    float c11 = u2f(hash3u(xi + 1, yi + 1, 0x2d1u, seed));

    return lerpf(lerpf(c00, c10, tx), lerpf(c01, c11, tx), ty);
}

/* Fixed pseudo-random unit-ish drift directions, one per octave. */
static const float EVO[6][3] = {
    {  0.62f,  0.30f,  0.72f },
    { -0.55f,  0.80f,  0.23f },
    {  0.30f, -0.65f,  0.70f },
    {  0.80f,  0.10f, -0.59f },
    { -0.21f, -0.50f,  0.84f },
    {  0.45f,  0.85f, -0.27f },
};

float fbm3(float x, float y, float z, int octaves, uint32_t seed,
           float t, float evolve)
{
    if (octaves > 6) octaves = 6;
    float amp = 1.f, sum = 0.f, norm = 0.f;
    float px = x, py = y, pz = z;
    float tv = t * evolve;
    for (int i = 0; i < octaves; i++) {
        /* finer octaves churn faster than the gross shape */
        float sp = 0.05f * (float)(i + 1) / 5.0f * tv;
        sum += amp * vnoise3(px + EVO[i][0] * sp,
                             py + EVO[i][1] * sp,
                             pz + EVO[i][2] * sp,
                             seed + (uint32_t)i * 0x9E3779B9u);
        norm += amp;
        amp *= 0.5f;
        /* rotate axes (cyclic swap) per octave to kill grid-aligned streaks */
        float nx = py * 2.02f + 19.19f;
        float ny = pz * 2.02f + 7.77f;
        float nz = px * 2.02f + 33.33f;
        px = nx; py = ny; pz = nz;
    }
    return sum / norm;
}

float fbm2(float x, float y, int octaves, uint32_t seed)
{
    float amp = 1.f, sum = 0.f, norm = 0.f;
    float px = x, py = y;
    for (int i = 0; i < octaves; i++) {
        sum += amp * vnoise2(px, py, seed + (uint32_t)i * 0x85EBCA6Bu);
        norm += amp;
        amp *= 0.5f;
        float nx = py * 2.02f + 13.13f;
        float ny = -px * 2.02f + 27.27f;
        px = nx; py = ny;
    }
    return sum / norm;
}
