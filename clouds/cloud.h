#ifndef CLOUD_H
#define CLOUD_H

#include <stdint.h>

typedef struct { float x, y, z; } v3;

typedef struct {
    /* configuration */
    uint32_t seed;
    float coverage;      /* 0..2, default 0.9 */
    float wind;          /* cloud approach/drift speed multiplier, default 1.0 */
    float elevation;     /* 0..1: ground .. high above the cloud deck */
    float height;        /* tallest cloud tops as multiple of the deck top, 1..3 */
    float yaw_deg;       /* camera yaw in degrees, default 0 */
    float pitch_deg;     /* camera pitch in degrees; NAN = auto-aim at deck */
    float sun_elev_deg;  /* set by -t preset, default 38 (afternoon) */
    float storm;         /* 0..1 overcast: gray sky/sun, flat shading */
    int   flight;        /* -i: camera flies along the view direction;
                            clouds stay fixed and -w sets the speed */
    float sun_az_deg;    /* degrees clockwise from straight ahead, default 30 */
    int   steps;         /* raymarch steps, default 40 */

    /* blocks mode: shift ray jitter coords so a cell's 2x2 subsamples
     * share one jitter value (intra-cell jitter noise breaks the
     * quadrant fg/bg split) */
    int   jitter_shift;

    /* derived per frame by scene_setup() */
    double time;
    int    cols, rows;
    int    near_fade;    /* camera is inside the cloud slab */
    float  y_top_max;    /* tallest variable deck top, Y_TOP * height */
    float  wz_off;       /* wind advection offset this frame (0 in flight) */
    v3     fly_pos;      /* flight mode: integrated camera position */
    int    fly_init;
    double prev_time;
    v3     cam, fwd, right, up;
    float  tan_h, tan_v;
    v3     sun, sun_col;

    /* time-of-day palette, derived from sun elevation once per frame */
    float  dusk;         /* 0 = high sun .. 1 = sundown */
    v3     sun_xz;       /* sun azimuth direction in the xz plane */
    v3     sky_zenith, sky_horizon, sky_horizon_warm, sky_haze;
    v3     glow_tight, glow_wide;
    v3     amb_tint, bounce_col;
} scene;

/* Per-cell raymarch result: coverage of the cell, lit cloud color
 * (unpremultiplied), and the sky color behind it. All floats 0..1. */
typedef struct {
    float alpha;
    float lr, lg, lb;
    float sr, sg, sb;
} cell;

void scene_setup(scene *sc, int cols, int rows, double time);
void cloud_render_row(const scene *sc, int row, cell *out /* [cols] */);

#endif
