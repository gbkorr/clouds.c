#include "render.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RAMP 63
#define CHIDX_NONE 255

/* Mutex+cond thread barrier, used on every platform: POSIX barriers are
 * an optional feature macOS never implemented, and one shared
 * implementation beats a per-OS split. Its cost (a broadcast per phase,
 * three per frame) is noise next to the raymarch. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned count, waiting, phase;
} barrier_t;

static void barrier_init(barrier_t *b, unsigned count)
{
    b->count = count;
    b->waiting = 0;
    b->phase = 0;
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
}

static void barrier_wait(barrier_t *b)
{
    pthread_mutex_lock(&b->mu);
    unsigned phase = b->phase;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->phase++;
        pthread_cond_broadcast(&b->cv);
    } else {
        while (b->phase == phase)
            pthread_cond_wait(&b->cv, &b->mu);
    }
    pthread_mutex_unlock(&b->mu);
}

static void barrier_destroy(barrier_t *b)
{
    pthread_mutex_destroy(&b->mu);
    pthread_cond_destroy(&b->cv);
}

/* 256-color quantization state, per cell, for fg and bg: the RGB that
 * produced the current index, kept for hysteresis. */
typedef struct {
    unsigned char rgb[3];
    unsigned char idx;
    unsigned char valid;
} cq;

/* Quadrant block elements indexed by bitmap: bit 8 = upper-left,
 * 4 = upper-right, 2 = lower-left, 1 = lower-right. */
static const char *QUAD[16] = {
    " ",      "▗", "▖", "▄",
    "▝", "▐", "▞", "▟",
    "▘", "▚", "▌", "▙",
    "▀", "▜", "▛", "█",
};

struct renderer {
    int cols, rows;
    colormode cm;
    char ramp[MAX_RAMP + 1];
    int ramplen;

    cell *cells;
    cell *cellsf;          /* spatially filtered copy, mappers read this */
    float *ema;            /* 4 per cell: alpha + premultiplied lit rgb */
    unsigned char *chidx;  /* current glyph index, CHIDX_NONE = invalid */
    unsigned char *glyph;
    unsigned char *fg;     /* 3 per cell */
    unsigned char *bg;     /* 3 per cell, blocks style only */
    cq *fgq, *bgq;
    int blocks;

    char *buf;
    size_t bufcap;

    int nthreads;
    pthread_t *tids;
    barrier_t start, mid, done;
    _Atomic int next_row, next_map_row;
    const scene *cur;
    scene subscene; /* blocks mode: scene re-derived at 2x resolution */
    int animate;
    const cell *blend_src; /* non-NULL: mix into cells after march */
    float blend_w;         /* weight of blend_src, 0..1 */
    int shutdown;
};

static const int BAYER[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static inline float clamp01f(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

static inline unsigned char to_u8(float x) { return (unsigned char)(clamp01f(x) * 255.f + 0.5f); }

static void map_cell(renderer *R, int x, int y)
{
    int i = y * R->cols + x;
    const cell *c = &R->cellsf[i];
    float *e = &R->ema[4 * i];

    /* temporal EMA on premultiplied values: smooths sample jitter and
     * doubles as subtle motion blur */
    float alpha = c->alpha;
    float pr = c->lr * c->alpha, pg = c->lg * c->alpha, pb = c->lb * c->alpha;
    if (R->animate) {
        alpha = 0.6f * alpha + 0.4f * e[0];
        pr = 0.6f * pr + 0.4f * e[1];
        pg = 0.6f * pg + 0.4f * e[2];
        pb = 0.6f * pb + 0.4f * e[3];
    }
    e[0] = alpha; e[1] = pr; e[2] = pg; e[3] = pb;

    float lr = 0.f, lg = 0.f, lb = 0.f;
    if (alpha > 1e-3f) {
        lr = pr / alpha; lg = pg / alpha; lb = pb / alpha;
    }

    if (R->cm == CM_MONO) {
        float luma = 0.299f * lr + 0.587f * lg + 0.114f * lb;
        float comb = alpha * (0.35f + 0.65f * luma);
        if (comb < 0.02f) {
            R->glyph[i] = ' ';
            R->chidx[i] = CHIDX_NONE;
            return;
        }
        float level = powf(comb, 0.8f) * (float)(R->ramplen - 1)
                    + ((float)BAYER[y & 3][x & 3] / 16.f - 0.5f) * 0.75f;
        int idx;
        if (R->animate && R->chidx[i] != CHIDX_NONE
            && fabsf(level - (float)R->chidx[i]) <= 0.6f)
            idx = R->chidx[i];
        else
            idx = (int)lroundf(level);
        if (idx < 0) idx = 0;
        if (idx > R->ramplen - 1) idx = R->ramplen - 1;
        R->chidx[i] = (unsigned char)idx;
        R->glyph[i] = (unsigned char)R->ramp[idx];
        return;
    }

    if (alpha < 0.004f) {
        /* ascii style: sky is transparent, the terminal's own background
         * shows through */
        R->glyph[i] = ' ';
        R->chidx[i] = CHIDX_NONE;
        return;
    }

    /* glyph encodes density; static Bayer dither turns edge banding
     * into stipple. Exponent >1 spreads the ramp over the mid-alphas so
     * edges grade through the light glyphs instead of jumping to '@'. */
    float level = powf(alpha, 1.15f) * (float)(R->ramplen - 1)
                + ((float)BAYER[y & 3][x & 3] / 16.f - 0.5f) * 0.75f;
    int idx;
    if (R->animate && R->chidx[i] != CHIDX_NONE
        && fabsf(level - (float)R->chidx[i]) <= 0.6f)
        idx = R->chidx[i];
    else
        idx = (int)lroundf(level);
    if (idx < 0) idx = 0;
    if (idx > R->ramplen - 1) idx = R->ramplen - 1;
    R->chidx[i] = (unsigned char)idx;
    R->glyph[i] = (unsigned char)R->ramp[idx];

    /* color encodes light; density lives entirely in the glyph choice */
    float k = 0.9f + 0.25f * alpha;
    float fr = lr * k, fgc = lg * k, fb = lb * k;
    /* transparent bg may be black: keep a luminance floor so shadowed
     * undersides never vanish (hue preserved) */
    float luma = 0.299f * fr + 0.587f * fgc + 0.114f * fb;
    if (luma < 0.30f && luma > 1e-4f) {
        float s = 0.30f / luma;
        fr *= s; fgc *= s; fb *= s;
    }
    R->fg[3*i]   = to_u8(fr);
    R->fg[3*i+1] = to_u8(fgc);
    R->fg[3*i+2] = to_u8(fb);
}

/* blocks style: one output cell covers a 2x2 patch of subsamples. Pick the
 * quadrant glyph plus fg/bg pair that best represents the four composited
 * sub-pixel colors (split by luminance about the mean). */
static void map_cell_quad(renderer *R, int x, int y)
{
    int scols = R->cols * 2;
    float col[4][3], lum[4];
    for (int k = 0; k < 4; k++) {
        int sx = 2 * x + (k & 1), sy = 2 * y + (k >> 1);
        int j = sy * scols + sx;
        const cell *c = &R->cellsf[j];
        float *e = &R->ema[4 * j];

        float alpha = c->alpha;
        float pr = c->lr * alpha, pg = c->lg * alpha, pb = c->lb * alpha;
        if (R->animate) {
            alpha = 0.6f * alpha + 0.4f * e[0];
            pr = 0.6f * pr + 0.4f * e[1];
            pg = 0.6f * pg + 0.4f * e[2];
            pb = 0.6f * pb + 0.4f * e[3];
        }
        e[0] = alpha; e[1] = pr; e[2] = pg; e[3] = pb;

        /* composite cloud over painted sky (lit is premultiplied) */
        col[k][0] = pr + c->sr * (1.f - alpha);
        col[k][1] = pg + c->sg * (1.f - alpha);
        col[k][2] = pb + c->sb * (1.f - alpha);
        lum[k] = 0.299f * col[k][0] + 0.587f * col[k][1] + 0.114f * col[k][2];
    }

    float mean = 0.25f * (lum[0] + lum[1] + lum[2] + lum[3]);
    static const int BIT[4] = { 8, 4, 2, 1 }; /* UL UR LL LR */
    int bits = 0, nf = 0, nb = 0;
    float fsum[3] = {0}, bsum[3] = {0};
    for (int k = 0; k < 4; k++) {
        /* margin: near-uniform cells collapse to a solid bg space. Must
         * exceed the luma noise the raymarch start-jitter leaves between
         * sub-columns, or uniform fog renders as half-block speckle. */
        if (lum[k] > mean + 0.012f) {
            bits |= BIT[k];
            for (int q = 0; q < 3; q++) fsum[q] += col[k][q];
            nf++;
        } else {
            for (int q = 0; q < 3; q++) bsum[q] += col[k][q];
            nb++;
        }
    }
    int i = y * R->cols + x;
    R->glyph[i] = (unsigned char)bits;
    if (nf > 0) {
        R->fg[3*i]   = to_u8(fsum[0] / (float)nf);
        R->fg[3*i+1] = to_u8(fsum[1] / (float)nf);
        R->fg[3*i+2] = to_u8(fsum[2] / (float)nf);
    }
    R->bg[3*i]   = to_u8(bsum[0] / (float)nb);
    R->bg[3*i+1] = to_u8(bsum[1] / (float)nb);
    R->bg[3*i+2] = to_u8(bsum[2] / (float)nb);
}

/* ---------------- 256-color quantization ---------------- */

static const int CUBE[6] = { 0, 95, 135, 175, 215, 255 };

/* cube levels bracketing v: floor index (ceil is fi+1 unless at the end) */
static inline int cube_floor(int v)
{
    for (int i = 5; i > 0; i--)
        if (v >= CUBE[i]) return i;
    return 0;
}

/* Nearest 256-palette color by distance over the 8 bracketing cube corners
 * plus the best gray. Independent per-channel rounding shifts hue badly on
 * the subtle sky blues, so a real distance search matters here. */
static int quant256(const unsigned char *rgb)
{
    int r = rgb[0], g = rgb[1], b = rgb[2];
    int rf = cube_floor(r), gf = cube_floor(g), bf = cube_floor(b);
    long best = -1;
    int best_idx = 16;
    for (int k = 0; k < 8; k++) {
        int ri = rf + (k & 1),  gi = gf + ((k >> 1) & 1), bi = bf + (k >> 2);
        if (ri > 5 || gi > 5 || bi > 5) continue;
        long dr = r - CUBE[ri], dg = g - CUBE[gi], db = b - CUBE[bi];
        /* weight green highest: hue errors there are most visible */
        long d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (best < 0 || d < best) {
            best = d;
            best_idx = 16 + 36 * ri + 6 * gi + bi;
        }
    }
    int gray = (r * 299 + g * 587 + b * 114) / 1000;
    int gi = (gray - 8) / 10;
    if (gi < 0) gi = 0;
    if (gi > 23) gi = 23;
    int gv = 8 + gi * 10;
    long dr = r - gv, dg = g - gv, db = b - gv;
    long dgray = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
    if (dgray < best)
        return 232 + gi;
    return best_idx;
}

static int quant256_hyst(renderer *R, cq *q, const unsigned char *rgb)
{
    if (R->animate && q->valid
        && abs((int)rgb[0] - (int)q->rgb[0]) < 12
        && abs((int)rgb[1] - (int)q->rgb[1]) < 12
        && abs((int)rgb[2] - (int)q->rgb[2]) < 12)
        return q->idx;
    q->rgb[0] = rgb[0]; q->rgb[1] = rgb[1]; q->rgb[2] = rgb[2];
    q->idx = (unsigned char)quant256(rgb);
    q->valid = 1;
    return q->idx;
}

/* ---------------- spatial anti-aliasing ---------------- */

/* 5-tap cross filter over premultiplied color+alpha, cells -> cellsf.
 * Smooths the residual per-cell luminance noise the raymarch jitter
 * leaves on cloud surfaces; runs on the raymarch grid (the subsample
 * grid in blocks mode), so silhouettes soften by one subsample. Sky
 * channels pass through: they're analytic and already smooth, and
 * filtering them would blur the sun disc. */
#define AA_CENTER 0.5f
#define AA_SIDE   0.125f

static void filter_row(renderer *R, int sy, int scols, int srows)
{
    const cell *src = R->cells;
    cell *dst = R->cellsf;
    for (int sx = 0; sx < scols; sx++) {
        int i = sy * scols + sx;
        const cell *c = &src[i];
        const cell *nb[4];
        int nn = 0;
        if (sx > 0)         nb[nn++] = c - 1;
        if (sx < scols - 1) nb[nn++] = c + 1;
        if (sy > 0)         nb[nn++] = c - scols;
        if (sy < srows - 1) nb[nn++] = c + scols;

        int clear = c->alpha < 1e-3f;
        for (int k = 0; k < nn && clear; k++)
            clear = nb[k]->alpha < 1e-3f;
        if (clear) { dst[i] = *c; continue; }

        float wsum = AA_CENTER;
        float a  = AA_CENTER * c->alpha;
        float pr = AA_CENTER * c->alpha * c->lr;
        float pg = AA_CENTER * c->alpha * c->lg;
        float pb = AA_CENTER * c->alpha * c->lb;
        for (int k = 0; k < nn; k++) {
            const cell *b = nb[k];
            wsum += AA_SIDE;
            a  += AA_SIDE * b->alpha;
            pr += AA_SIDE * b->alpha * b->lr;
            pg += AA_SIDE * b->alpha * b->lg;
            pb += AA_SIDE * b->alpha * b->lb;
        }
        a /= wsum;
        dst[i].alpha = a;
        if (a > 1e-3f) {
            float inv = 1.f / (a * wsum);
            dst[i].lr = pr * inv; dst[i].lg = pg * inv; dst[i].lb = pb * inv;
        } else {
            dst[i].lr = dst[i].lg = dst[i].lb = 0.f;
        }
        dst[i].sr = c->sr; dst[i].sg = c->sg; dst[i].sb = c->sb;
    }
}

/* ---------------- worker pool ---------------- */

/* Cross-dissolve blend_src into a freshly marched row. Premultiplied lerp
 * of the lit color (cells store it unpremultiplied); runs before the
 * spatial filter and EMA so both smooth the blended stream. */
static void blend_row(renderer *R, int sy, int scols)
{
    float w = R->blend_w;
    cell *c = &R->cells[sy * scols];
    const cell *s = &R->blend_src[sy * scols];
    for (int x = 0; x < scols; x++, c++, s++) {
        float a = c->alpha + (s->alpha - c->alpha) * w;
        float pr = c->lr * c->alpha + (s->lr * s->alpha - c->lr * c->alpha) * w;
        float pg = c->lg * c->alpha + (s->lg * s->alpha - c->lg * c->alpha) * w;
        float pb = c->lb * c->alpha + (s->lb * s->alpha - c->lb * c->alpha) * w;
        c->alpha = a;
        if (a > 1e-6f) {
            c->lr = pr / a; c->lg = pg / a; c->lb = pb / a;
        } else {
            c->lr = c->lg = c->lb = 0.f;
        }
        c->sr += (s->sr - c->sr) * w;
        c->sg += (s->sg - c->sg) * w;
        c->sb += (s->sb - c->sb) * w;
    }
}

/* Two parallel phases per frame: raymarch every row into cells, then --
 * after the mid barrier, so neighbor rows exist -- filter and map. */
static void march_rows(renderer *R)
{
    int r;
    while ((r = atomic_fetch_add(&R->next_row, 4)) < R->rows) {
        int end = r + 4 < R->rows ? r + 4 : R->rows;
        for (int y = r; y < end; y++) {
            if (R->blocks) {
                int scols = R->cols * 2;
                cloud_render_row(R->cur, 2*y,   &R->cells[(2*y)   * scols]);
                cloud_render_row(R->cur, 2*y+1, &R->cells[(2*y+1) * scols]);
                if (R->blend_src) {
                    blend_row(R, 2*y,   scols);
                    blend_row(R, 2*y+1, scols);
                }
            } else {
                cloud_render_row(R->cur, y, &R->cells[y * R->cols]);
                if (R->blend_src)
                    blend_row(R, y, R->cols);
            }
        }
    }
}

static void map_rows(renderer *R)
{
    int scols = R->blocks ? R->cols * 2 : R->cols;
    int srows = R->blocks ? R->rows * 2 : R->rows;
    int r;
    while ((r = atomic_fetch_add(&R->next_map_row, 4)) < R->rows) {
        int end = r + 4 < R->rows ? r + 4 : R->rows;
        for (int y = r; y < end; y++) {
            if (R->blocks) {
                filter_row(R, 2*y,   scols, srows);
                filter_row(R, 2*y+1, scols, srows);
                for (int x = 0; x < R->cols; x++)
                    map_cell_quad(R, x, y);
            } else {
                filter_row(R, y, scols, srows);
                for (int x = 0; x < R->cols; x++)
                    map_cell(R, x, y);
            }
        }
    }
}

static void *worker(void *arg)
{
    renderer *R = arg;
    for (;;) {
        barrier_wait(&R->start);
        if (R->shutdown) return NULL;
        march_rows(R);
        barrier_wait(&R->mid);
        map_rows(R);
        barrier_wait(&R->done);
    }
}

/* ---------------- serialization ---------------- */

static size_t put_num(char *b, int v)
{
    /* v in 0..255 */
    if (v >= 100) {
        b[0] = (char)('0' + v / 100);
        b[1] = (char)('0' + (v / 10) % 10);
        b[2] = (char)('0' + v % 10);
        return 3;
    }
    if (v >= 10) {
        b[0] = (char)('0' + v / 10);
        b[1] = (char)('0' + v % 10);
        return 2;
    }
    b[0] = (char)('0' + v);
    return 1;
}

static size_t put_sgr_true(char *b, int fgbg, const unsigned char *rgb)
{
    size_t n = 0;
    b[n++] = '\x1b'; b[n++] = '[';
    b[n++] = fgbg ? '4' : '3'; b[n++] = '8';
    b[n++] = ';'; b[n++] = '2'; b[n++] = ';';
    n += put_num(b + n, rgb[0]); b[n++] = ';';
    n += put_num(b + n, rgb[1]); b[n++] = ';';
    n += put_num(b + n, rgb[2]); b[n++] = 'm';
    return n;
}

static size_t put_sgr_256(char *b, int fgbg, int idx)
{
    size_t n = 0;
    b[n++] = '\x1b'; b[n++] = '[';
    b[n++] = fgbg ? '4' : '3'; b[n++] = '8';
    b[n++] = ';'; b[n++] = '5'; b[n++] = ';';
    n += put_num(b + n, idx); b[n++] = 'm';
    return n;
}

static const char *serialize(renderer *R, int home, size_t *outlen)
{
    char *b = R->buf;
    size_t n = 0;
    int lastf = -1, lastb = -1;               /* 256 mode */
    unsigned char lf[3] = {0}, lbg[3] = {0};  /* truecolor mode */
    int have_fg = 0, have_bg = 0;

    for (int y = 0; y < R->rows; y++) {
        if (home) {
            b[n++] = '\x1b'; b[n++] = '[';
            n += put_num(b + n, y + 1);
            /* rows can exceed 255; put_num caps at 3 digits which covers
             * any real terminal height */
            b[n++] = ';'; b[n++] = '1'; b[n++] = 'H';
        } else if (y > 0) {
            b[n++] = '\n';
        }
        for (int x = 0; x < R->cols; x++) {
            int i = y * R->cols + x;
            if (R->cm == CM_MONO) {
                b[n++] = (char)R->glyph[i];
                continue;
            }
            /* fg is invisible on spaces (quadrant bitmap 0 = space) */
            int blank = R->blocks ? R->glyph[i] == 0 : R->glyph[i] == ' ';
            if (blank && !R->blocks) {
                /* ascii style never sets bg: sky stays transparent */
                b[n++] = ' ';
                continue;
            }
            if (R->cm == CM_TRUECOLOR) {
                const unsigned char *f = &R->fg[3*i];
                if (!blank && (!have_fg || memcmp(lf, f, 3) != 0)) {
                    n += put_sgr_true(b + n, 0, f);
                    memcpy(lf, f, 3);
                    have_fg = 1;
                }
                if (R->blocks) {
                    const unsigned char *g = &R->bg[3*i];
                    if (!have_bg || memcmp(lbg, g, 3) != 0) {
                        n += put_sgr_true(b + n, 1, g);
                        memcpy(lbg, g, 3);
                        have_bg = 1;
                    }
                }
            } else {
                if (!blank) {
                    int fi = quant256_hyst(R, &R->fgq[i], &R->fg[3*i]);
                    if (fi != lastf) { n += put_sgr_256(b + n, 0, fi); lastf = fi; }
                }
                if (R->blocks) {
                    int bi = quant256_hyst(R, &R->bgq[i], &R->bg[3*i]);
                    if (bi != lastb) { n += put_sgr_256(b + n, 1, bi); lastb = bi; }
                }
            }
            if (R->blocks) {
                const char *q = QUAD[R->glyph[i] & 15];
                size_t ql = strlen(q);
                memcpy(b + n, q, ql);
                n += ql;
            } else {
                b[n++] = (char)R->glyph[i];
            }
        }
        if (!home && R->cm != CM_MONO) {
            /* reset per line so scrollback/pipes stay clean */
            memcpy(b + n, "\x1b[0m", 4); n += 4;
            have_fg = 0; have_bg = 0; lastf = -1; lastb = -1;
        }
    }
    if (home && R->cm != CM_MONO) {
        memcpy(b + n, "\x1b[0m", 4); n += 4;
    }
    *outlen = n;
    return R->buf;
}

/* ---------------- lifecycle ---------------- */

static void alloc_grids(renderer *R)
{
    size_t nc = (size_t)R->cols * (size_t)R->rows;
    size_t ns = R->blocks ? nc * 4 : nc; /* subsample grid is 2x each axis */
    R->cells  = calloc(ns, sizeof *R->cells);
    R->cellsf = calloc(ns, sizeof *R->cellsf);
    R->ema   = calloc(ns * 4, sizeof *R->ema);
    R->chidx = malloc(nc);
    R->glyph = malloc(nc);
    R->fg    = calloc(nc * 3, 1);
    R->bg    = calloc(nc * 3, 1);
    R->fgq   = calloc(nc, sizeof *R->fgq);
    R->bgq   = calloc(nc, sizeof *R->bgq);
    if (!R->cells || !R->cellsf || !R->ema || !R->chidx || !R->glyph
        || !R->fg || !R->bg || !R->fgq || !R->bgq) {
        fprintf(stderr, "clouds: out of memory\n");
        exit(1);
    }
    memset(R->chidx, CHIDX_NONE, nc);
    memset(R->glyph, ' ', nc);
    /* worst case ~44 bytes/cell (two truecolor SGRs + glyph) + row headers */
    R->bufcap = nc * 44 + (size_t)R->rows * 16 + 64;
    R->buf = malloc(R->bufcap);
    if (!R->buf) { fprintf(stderr, "clouds: out of memory\n"); exit(1); }
}

static void free_grids(renderer *R)
{
    free(R->cells); free(R->cellsf); free(R->ema); free(R->chidx); free(R->glyph);
    free(R->fg); free(R->bg); free(R->fgq); free(R->bgq); free(R->buf);
}

renderer *renderer_create(int cols, int rows, colormode cm,
                          const char *ramp, int blocks, int nthreads)
{
    renderer *R = calloc(1, sizeof *R);
    if (!R) return NULL;
    R->cols = cols; R->rows = rows; R->cm = cm;
    R->blocks = blocks && cm != CM_MONO; /* mono has no colors to paint */
    snprintf(R->ramp, sizeof R->ramp, "%s", ramp);
    R->ramplen = (int)strlen(R->ramp);
    alloc_grids(R);

    if (nthreads < 1) nthreads = 1;
    R->nthreads = nthreads;
    barrier_init(&R->start, (unsigned)nthreads);
    barrier_init(&R->mid, (unsigned)nthreads);
    barrier_init(&R->done, (unsigned)nthreads);
    R->tids = calloc((size_t)nthreads - 1, sizeof *R->tids);
    for (int i = 0; i < nthreads - 1; i++)
        pthread_create(&R->tids[i], NULL, worker, R);
    return R;
}

void renderer_destroy(renderer *R)
{
    if (!R) return;
    R->shutdown = 1;
    barrier_wait(&R->start);
    for (int i = 0; i < R->nthreads - 1; i++)
        pthread_join(R->tids[i], NULL);
    barrier_destroy(&R->start);
    barrier_destroy(&R->mid);
    barrier_destroy(&R->done);
    free(R->tids);
    free_grids(R);
    free(R);
}

void renderer_resize(renderer *R, int cols, int rows)
{
    free_grids(R);
    R->cols = cols; R->rows = rows;
    alloc_grids(R);
}

const cell *renderer_cells(const renderer *R, size_t *count)
{
    size_t nc = (size_t)R->cols * (size_t)R->rows;
    *count = R->blocks ? nc * 4 : nc;
    return R->cells;
}

const char *renderer_frame_blend(renderer *R, const scene *sc,
                                 const cell *base, float base_w,
                                 int animate, int home, size_t *len)
{
    if (R->blocks) {
        /* raymarch at 2x resolution; the fov math is cell-count invariant */
        R->subscene = *sc;
        scene_setup(&R->subscene, R->cols * 2, R->rows * 2, sc->time);
        R->subscene.jitter_shift = 1;
        R->cur = &R->subscene;
    } else {
        R->cur = sc;
    }
    R->animate = animate;
    R->blend_src = base;
    R->blend_w = base_w;
    atomic_store(&R->next_row, 0);
    atomic_store(&R->next_map_row, 0);
    barrier_wait(&R->start);
    march_rows(R); /* main thread is worker 0 */
    barrier_wait(&R->mid);
    map_rows(R);
    barrier_wait(&R->done);
    return serialize(R, home, len);
}

const char *renderer_frame(renderer *R, const scene *sc, int animate,
                           int home, size_t *len)
{
    return renderer_frame_blend(R, sc, NULL, 0.f, animate, home, len);
}
