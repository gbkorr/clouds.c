#include "cloud.h"
#include "noise.h"

#include <math.h>

#define Y_BASE    1.6f
#define Y_TOP     4.2f
#define INV_THICK (1.0f / (Y_TOP - Y_BASE))
#define SIGMA     3.2f   /* extinction: cloud core opaque over ~1 unit */
#define FREQ      0.35f  /* base fBm frequency, cycles per world unit */
#define COV_FREQ  0.075f /* coverage field frequency */
#define WIND_SPEED 1.2f  /* field advection, world units per second at wind=1 */
#define MARCH_MAX 16.0f  /* max in-slab march distance (base-height deck) */
#define PI_F      3.14159265358979f

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline v3 mix3(v3 a, v3 b, float t)
{ return (v3){ mixf(a.x, b.x, t), mixf(a.y, b.y, t), mixf(a.z, b.z, t) }; }
#define RGB(r, g, b) ((v3){ (r) / 255.f, (g) / 255.f, (b) / 255.f })

static inline float sstep(float a, float b, float x)
{
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}

void scene_setup(scene *sc, int cols, int rows, double time)
{
    sc->cols = cols;
    sc->rows = rows;
    sc->time = time;

    sc->y_top_max = Y_TOP * sc->height;

    /* elevation 0..1 -> camera height: lower half spans ground to the
     * fly-through cruise height inside the deck, upper half continues
     * to well above the cloud tops */
    const float y_cruise = Y_BASE + 0.35f * (Y_TOP - Y_BASE);
    const float y_high = sc->y_top_max + 1.0f;
    float e = clamp01(sc->elevation);
    float ey = e <= 0.5f
             ? e * 2.f * y_cruise
             : y_cruise + (e - 0.5f) * 2.f * (y_high - y_cruise);

    /* default: aim at the middle of the slab over a fixed look-ahead
     * distance, so the view pitches up from below and down from above
     * (12 deg up at ground level, ~level inside the deck). In flight
     * this doubles as a gentle autopilot re-aiming at the deck. */
    float cam_y = sc->flight && sc->fly_init ? sc->fly_pos.y : ey;
    float pitch = isnan(sc->pitch_deg)
                ? atan2f(0.5f * (Y_BASE + Y_TOP) - cam_y, 13.6f)
                : sc->pitch_deg * PI_F / 180.f;

    /* a near-imperceptible sway keeps the view feeling alive */
    float yaw = sc->yaw_deg * PI_F / 180.f
              + 0.5f * PI_F / 180.f * sinf((float)time / 37.f);

    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw), sy = sinf(yaw);
    sc->fwd = (v3){ sy * cp, sp, cy * cp };
    /* right = normalize(cross(world_up, fwd)), up = cross(fwd, right) */
    v3 f = sc->fwd;
    v3 r = { f.z, 0.f, -f.x };
    float rl = sqrtf(r.x * r.x + r.z * r.z);
    r.x /= rl; r.z /= rl;
    sc->right = r;
    sc->up = (v3){ f.y * r.z - f.z * r.y,
                   f.z * r.x - f.x * r.z,
                   f.x * r.y - f.y * r.x };

    /* flight: the camera advances along its view direction and the field
     * holds still; otherwise the camera is static and wind advects the
     * field along -z (wz_off, applied per sample in density_at) */
    float dt = (float)(time - sc->prev_time);
    sc->prev_time = time;
    if (sc->flight) {
        if (!sc->fly_init) { sc->fly_pos = (v3){ 0.f, ey, 0.f }; sc->fly_init = 1; }
        float d = WIND_SPEED * sc->wind * dt;
        sc->fly_pos.x += f.x * d;
        sc->fly_pos.y += f.y * d;
        sc->fly_pos.z += f.z * d;
        if (sc->fly_pos.y < 0.1f) sc->fly_pos.y = 0.1f; /* stay above ground */
        sc->cam = sc->fly_pos;
    } else {
        sc->cam = (v3){ 0.f, ey, 0.f };
    }
    sc->wz_off = sc->flight ? 0.f : WIND_SPEED * sc->wind * (float)time;
    sc->near_fade = sc->cam.y > Y_BASE - 0.4f
                 && sc->cam.y < sc->y_top_max + 0.4f;

    sc->tan_h = tanf(35.f * PI_F / 180.f); /* 70 deg horizontal fov */
    /* one terminal cell is ~1x2 in pixel aspect */
    sc->tan_v = sc->tan_h * ((float)rows * 2.0f) / (float)cols;

    float el = sc->sun_elev_deg * PI_F / 180.f;
    float az = sc->sun_az_deg * PI_F / 180.f;
    sc->sun = (v3){ sinf(az) * cosf(el), sinf(el), cosf(az) * cosf(el) };
    float warm = clamp01((15.f - sc->sun_elev_deg) / 10.f);
    sc->sun_col = (v3){ 1.0f,
                        mixf(0.98f, 0.75f, warm),
                        mixf(0.92f, 0.50f, warm) };
    /* right at the horizon the gold deepens to sunset red */
    float red = clamp01((6.f - sc->sun_elev_deg) / 5.f);
    sc->sun_col.y = mixf(sc->sun_col.y, 0.45f, red);
    sc->sun_col.z = mixf(sc->sun_col.z, 0.25f, red);

    /* time of day: one dusk factor, derived from sun elevation, drives
     * the whole palette. Exactly 0 from 20 deg up, so daytime scenes are
     * untouched; the per-sample code only reads these fields. */
    float dusk = 1.f - sstep(2.f, 20.f, sc->sun_elev_deg);
    sc->dusk = dusk;
    float sl = sqrtf(sc->sun.x * sc->sun.x + sc->sun.z * sc->sun.z);
    sc->sun_xz = (v3){ sc->sun.x / sl, 0.f, sc->sun.z / sl };

    sc->sky_zenith       = mix3(RGB( 58, 110, 200), RGB( 35,  55, 110), dusk);
    sc->sky_horizon      = mix3(RGB(178, 205, 232), RGB(150, 155, 175), 0.4f * dusk);
    sc->sky_horizon_warm = RGB(255, 150, 80);
    sc->sky_haze         = mix3(RGB(150, 165, 180), RGB(120,  95, 110), dusk);

    float boost = 1.f + 1.5f * dusk;
    sc->glow_tight = mix3(RGB(140, 100, 40), RGB(255, 90, 40), dusk);
    sc->glow_wide  = mix3(RGB( 22,  16,  8), RGB( 45, 18, 10), dusk);
    sc->glow_tight = (v3){ sc->glow_tight.x * boost, sc->glow_tight.y * boost,
                           sc->glow_tight.z * boost };
    sc->glow_wide  = (v3){ sc->glow_wide.x * boost, sc->glow_wide.y * boost,
                           sc->glow_wide.z * boost };

    /* ambient dims and cools toward dusk; the lost fill light comes back
     * as the warm bounce_col lighting cloud bases from below */
    float amb_dim = 1.f - 0.35f * dusk;
    sc->amb_tint = mix3((v3){ 0.55f, 0.65f, 0.85f },
                        (v3){ 0.48f, 0.40f, 0.58f }, dusk);
    sc->amb_tint = (v3){ sc->amb_tint.x * amb_dim, sc->amb_tint.y * amb_dim,
                         sc->amb_tint.z * amb_dim };
    sc->bounce_col = (v3){ 0.50f * dusk, 0.175f * dusk, 0.09f * dusk };

    /* storm: overcast graying over the finished palette. The sun fades
     * to a weak neutral glow and ambient carries the light, so cloud
     * shading flattens along with the colors. */
    float st = clamp01(sc->storm);
    if (st > 0.f) {
        sc->sky_zenith       = mix3(sc->sky_zenith,       RGB(105, 110, 118), st);
        sc->sky_horizon      = mix3(sc->sky_horizon,      RGB(140, 143, 148), st);
        sc->sky_horizon_warm = mix3(sc->sky_horizon_warm, RGB(140, 143, 148), st);
        sc->sky_haze         = mix3(sc->sky_haze,         RGB(120, 123, 128), st);
        sc->glow_tight = mix3(sc->glow_tight, (v3){ 0.05f, 0.05f, 0.05f }, st);
        sc->glow_wide  = mix3(sc->glow_wide,  (v3){ 0.f, 0.f, 0.f }, st);
        sc->sun_col    = mix3(sc->sun_col,    (v3){ 0.45f, 0.45f, 0.47f }, st);
        sc->amb_tint   = mix3(sc->amb_tint,   (v3){ 0.52f, 0.53f, 0.55f }, st);
        sc->bounce_col = mix3(sc->bounce_col, (v3){ 0.f, 0.f, 0.f }, st);
    }
}

static float density_at(const scene *sc, float px, float py, float pz, int oct)
{
    /* wind advection along -z, precomputed per frame; zero in flight */
    float wz = pz + sc->wz_off;

    float env = 0.f, thr = 1.f;
    if (py > Y_BASE && py < sc->y_top_max) {
        /* low-frequency coverage field breaks the sky into distinct clouds */
        float cov = fbm2(px * COV_FREQ, wz * COV_FREQ, 2, sc->seed ^ 0xC0FFEEu);
        cov = clamp01(0.35f + 0.65f * cov);

        /* denser coverage towers higher: local top spans Y_TOP..y_top_max */
        float ytl = Y_TOP + (sc->y_top_max - Y_TOP) * sstep(0.55f, 0.9f, cov);
        float h = (py - Y_BASE) / (ytl - Y_BASE);
        if (h < 1.f) {
            /* cumulus envelope: sharp flat base, tall soft top */
            env = sstep(0.f, 0.07f, h) * (1.f - sstep(0.42f, 1.f, h));

            thr = 1.f - sc->coverage * cov + 0.18f * h; /* altitude erosion: billowy tops */
        }
    }

    /* shaped <= env, so env <= thr guarantees zero: skip the expensive fBm */
    if (env <= thr) return 0.f;

    float fade = 1.f;
    if (sc->near_fade) {
        /* near-field fade: punch through puffs instead of whiting out.
         * Must stay well under the camera's ~0.9-unit height above the
         * slab base or steep downward rays lose their whole in-cloud
         * path and the deck below renders as an empty flat "sea".
         * Checked before the fBm so faded-out samples cost nothing. */
        float dx = px - sc->cam.x, dy = py - sc->cam.y, dz = pz - sc->cam.z;
        fade = sstep(0.15f, 0.6f, sqrtf(dx * dx + dy * dy + dz * dz));
        if (fade < 0.02f) return 0.f;
    }

    float n = fbm3(px * FREQ, py * FREQ, wz * FREQ, oct,
                   sc->seed, (float)sc->time, 1.f);
    n = clamp01(2.1f * n - 0.55f); /* fBm concentrates near 0.5: expand contrast */

    float d = sstep(thr, thr + 0.16f, n * env);
    return d * d * fade;
}

static v3 light_at(const scene *sc, float px, float py, float pz,
                   float d, float h, int light_oct)
{
    static const float LD[3] = { 0.4f, 1.0f, 2.0f }; /* tap distances */
    static const float LW[3] = { 0.4f, 0.6f, 1.0f }; /* segment weights */
    /* dusk: the near-horizontal sun keeps taps inside the slab, so they
     * rarely hit the cheap out-of-envelope exit; two slightly heavier
     * taps approximate the three-tap sum at ~2/3 the cost */
    static const float LD2[2] = { 0.4f, 1.3f };
    static const float LW2[2] = { 0.6f, 1.4f };

    const float *ld = LD, *lw = LW;
    int ntap = 3;
    if (sc->dusk > 0.3f) { ld = LD2; lw = LW2; ntap = 2; }

    float occ = 0.f;
    for (int k = 0; k < ntap; k++)
        occ += lw[k] * density_at(sc,
                                  px + sc->sun.x * ld[k],
                                  py + sc->sun.y * ld[k],
                                  pz + sc->sun.z * ld[k], light_oct);
    float occ_t = expf(-3.f * occ);
    /* powder: thin forward edges catch a silver lining */
    float sun_t = occ_t * (1.f - 0.6f * expf(-8.f * d));

    if (h > 1.15f) h = 1.15f; /* tall tops sit far above the base deck slab */
    float amb = 0.32f + 0.33f * h; /* tops catch more sky light */

    /* dusk: low sun lights cloud bases from underneath. Strongest at the
     * base, gated by the occlusion-only transmittance so undersides on
     * the sunward side catch fire while deep cores stay dark.
     * bounce_col is zero outside golden hour/sunset. */
    float bounce = (1.f - clamp01(h)) * sqrtf(occ_t);

    return (v3){ sc->sun_col.x * sun_t + sc->amb_tint.x * amb + sc->bounce_col.x * bounce,
                 sc->sun_col.y * sun_t + sc->amb_tint.y * amb + sc->bounce_col.y * bounce,
                 sc->sun_col.z * sun_t + sc->amb_tint.z * amb + sc->bounce_col.z * bounce };
}

/* Sky color split into gradient base and sun-glow term, so callers can
 * mute the glow where blending toward it would discolor (aerial fog). */
static void sky_at(const scene *sc, v3 d, v3 *base, v3 *glow)
{
    v3 zen = sc->sky_zenith, hor = sc->sky_horizon, haze = sc->sky_haze;

    /* at dusk the horizon warms around the sun's azimuth while the
     * anti-sun side stays cool */
    if (sc->dusk > 0.f) {
        float dl = sqrtf(d.x * d.x + d.z * d.z);
        if (dl > 1e-4f) {
            float ca = (d.x * sc->sun_xz.x + d.z * sc->sun_xz.z) / dl;
            float w = sc->dusk * (0.35f + 0.65f * (ca > 0.f ? ca : 0.f));
            hor = mix3(hor, sc->sky_horizon_warm, w);
            haze = mix3(haze, sc->sky_horizon_warm, 0.5f * w);
        }
    }

    float R, G, B;
    if (d.y >= 0.f) {
        float t = powf(clamp01(d.y), 0.6f);
        R = mixf(hor.x, zen.x, t); G = mixf(hor.y, zen.y, t); B = mixf(hor.z, zen.z, t);
    } else {
        /* wide blend below the horizon: a tight one draws a hard, fake
         * sea-horizon line in fly mode */
        float t = clamp01(-d.y / 0.3f);
        R = mixf(hor.x, haze.x, t); G = mixf(hor.y, haze.y, t); B = mixf(hor.z, haze.z, t);
    }

    *base = (v3){ R, G, B };

    *glow = (v3){ 0.f, 0.f, 0.f };
    float ds = d.x * sc->sun.x + d.y * sc->sun.y + d.z * sc->sun.z;
    if (ds > 0.f) {
        float tight = powf(ds, 24.f); /* sun disc halo */
        float wide = powf(ds, 4.f);   /* faint broad brightening */
        *glow = (v3){ tight * sc->glow_tight.x + wide * sc->glow_wide.x,
                      tight * sc->glow_tight.y + wide * sc->glow_wide.y,
                      tight * sc->glow_tight.z + wide * sc->glow_wide.z };
    }
}

void cloud_render_row(const scene *sc, int row, cell *out)
{
    for (int col = 0; col < sc->cols; col++) {
        cell *c = &out[col];
        c->alpha = 0.f;
        c->lr = c->lg = c->lb = 0.f;

        float u = 2.f * ((float)col + 0.5f) / (float)sc->cols - 1.f;
        float v = 1.f - 2.f * ((float)row + 0.5f) / (float)sc->rows;
        v3 d = { sc->fwd.x + sc->right.x * u * sc->tan_h + sc->up.x * v * sc->tan_v,
                 sc->fwd.y + sc->right.y * u * sc->tan_h + sc->up.y * v * sc->tan_v,
                 sc->fwd.z + sc->right.z * u * sc->tan_h + sc->up.z * v * sc->tan_v };
        float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        d.x /= dl; d.y /= dl; d.z /= dl;

        v3 skyb, skyg;
        sky_at(sc, d, &skyb, &skyg);
        c->sr = clamp01(skyb.x + skyg.x);
        c->sg = clamp01(skyb.y + skyg.y);
        c->sb = clamp01(skyb.z + skyg.z);

        /* analytic slab entry/exit; taller tops extend the march cap so
         * distant towers aren't truncated */
        float ytop = sc->y_top_max;
        float mmax = MARCH_MAX + (sc->y_top_max - Y_TOP);
        float t0, t1;
        if (fabsf(d.y) < 1e-4f) {
            if (sc->cam.y <= Y_BASE || sc->cam.y >= ytop) continue;
            t0 = 0.f; t1 = mmax;
        } else {
            float ta = (Y_BASE - sc->cam.y) / d.y;
            float tb = (ytop - sc->cam.y) / d.y;
            t0 = ta < tb ? ta : tb;
            t1 = ta < tb ? tb : ta;
            if (t1 <= 0.f) continue;
            if (t0 < 0.f) t0 = 0.f;
        }
        if (t1 > t0 + mmax) t1 = t0 + mmax;
        if (t1 <= t0) continue;

        int n = sc->steps;
        float dt = (t1 - t0) / (float)n;
        /* a taller slab must not coarsen sampling: cap dt at the
         * base-height worst case and let the loop's iteration headroom
         * (3n) plus empty-space skipping cover the longer path */
        if (dt > MARCH_MAX / (float)n) dt = MARCH_MAX / (float)n;
        /* static per-cell jitter kills banding without temporal shimmer */
        float t = t0 + hash01_3(col >> sc->jitter_shift,
                                row >> sc->jitter_shift, 71, 0xBEEFu) * dt;

        float T = 1.f, rr = 0.f, rg = 0.f, rb = 0.f;
        float tw = 0.f, wsum = 0.f;
        int empty_run = 0;
        float t_prev = t0;              /* last confirmed-empty position */
        int prev_hit = 0, refines = 0;  /* refines: per-ray cost cap */
        for (int i = 0; i < 3 * n && t < t1; i++) {
            float step = dt;
            float px = sc->cam.x + d.x * t;
            float py = sc->cam.y + d.y * t;
            float pz = sc->cam.z + d.z * t;
            /* in-slab rays march cloud from t=0, so pull the LOD
             * boundaries in: full octaves only really near the camera */
            int oct = sc->near_fade
                    ? (t < 5.f ? 5 : (t < 9.f ? 4 : 3))
                    : (t < 8.f ? 5 : (t < 12.f ? 4 : 3));
            float dens = density_at(sc, px, py, pz, oct);
            /* soft-fade toward the march cap so distant clouds melt into
             * haze instead of truncating into fragments */
            dens *= 1.f - sstep(mmax * 0.7f, mmax, t - t0);
            if (dens > 0.005f) {
                empty_run = 0;
                /* first-hit refinement: the jittered sample lands anywhere
                 * within a stride of the entry surface, and with a low sun
                 * the occlusion taps swing lit-to-dark over that overshoot
                 * -- per-cell jitter then reads as harsh luminance mottle.
                 * Bisect the last stride to pin lighting (and the recovered
                 * partial segment) to the surface instead of the lottery. */
                float t_lit = t, seg = step, d_surf = dens;
                if (!prev_hit && refines < 4 && T > 0.3f
                    && t - t_prev > 0.25f * dt) {
                    refines++;
                    /* probes capped at 4 octaves: cheaper, and the small
                     * isosurface shift is consistent across neighbors */
                    int roct = oct > 4 ? 4 : oct;
                    float lo = t_prev, hi = t;
                    for (int b = 0; b < 2; b++) {
                        float mid = 0.5f * (lo + hi);
                        float dm = density_at(sc, sc->cam.x + d.x * mid,
                                              sc->cam.y + d.y * mid,
                                              sc->cam.z + d.z * mid, roct);
                        dm *= 1.f - sstep(mmax * 0.7f, mmax, mid - t0);
                        if (dm > 0.005f) { hi = mid; d_surf = dm; }
                        else lo = mid;
                    }
                    t_lit = hi;
                    /* re-base the march at the surface: the next sample then
                     * sits exactly one stride below it for every cell. Left
                     * at the jittered t, the interior sample's depth (and so
                     * its occlusion) stays a per-cell lottery, which at low
                     * step counts reads as static dark speckle on lit faces. */
                    t = hi;
                    seg = step;
                }
                prev_hit = 1;
                float lx = sc->cam.x + d.x * t_lit;
                float ly = sc->cam.y + d.y * t_lit;
                float lz = sc->cam.z + d.z * t_lit;
                float h = (ly - Y_BASE) * INV_THICK;
                float a = 1.f - expf(-SIGMA * 0.5f * (d_surf + dens) * seg);
                /* near-camera fog needs no lighting detail: cheaper taps */
                int loct = (sc->near_fade && t < 2.5f) ? 2 : 3;
                /* powder term sees the segment's mean density, not the
                 * density at the jittered sample */
                v3 L = light_at(sc, lx, ly, lz, 0.5f * (d_surf + dens), h, loct);
                float w = T * a;
                rr += w * L.x; rg += w * L.y; rb += w * L.z;
                tw += w * t_lit; wsum += w;
                T *= 1.f - a;
                if (T < 0.02f) break;
            } else {
                /* empty-space skipping: stretch the stride through clear
                 * air, snapping back to normal on the next hit */
                empty_run++;
                prev_hit = 0;
                t_prev = t;
            }
            if (empty_run >= 2) step *= 2.f;
            t += step;
        }

        float alpha = 1.f - T;
        if (alpha > 0.001f) {
            float inv = 1.f / alpha;
            float lr = rr * inv, lg = rg * inv, lb = rb * inv;
            /* aerial perspective: distant clouds fade toward sky */
            float tmid = wsum > 0.f ? tw / wsum : t0;
            float fog = 1.f - expf(-0.06f * tmid);
            /* fog toward the glow-muted sky: hazing shaded clouds toward
             * the full yellow sun glow turns them a muddy olive */
            c->alpha = alpha;
            c->lr = clamp01(mixf(lr, clamp01(skyb.x + 0.35f * skyg.x), fog));
            c->lg = clamp01(mixf(lg, clamp01(skyb.y + 0.35f * skyg.y), fog));
            c->lb = clamp01(mixf(lb, clamp01(skyb.z + 0.35f * skyg.z), fog));
        }
    }
}
