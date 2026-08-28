#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include "cloud.h"

typedef enum { CM_TRUECOLOR, CM_256, CM_MONO } colormode;

typedef struct renderer renderer;

/* blocks=1 paints backgrounds: blue-sky gradient plus bg-filled cloud
 * bodies. blocks=0 is transparent ascii (fg-only glyphs). */
renderer *renderer_create(int cols, int rows, colormode cm,
                          const char *ramp, int blocks, int nthreads);
void renderer_destroy(renderer *r);
void renderer_resize(renderer *r, int cols, int rows);

/* Raymarch the scene (threaded), map cells to glyph+colors, serialize to an
 * internal ANSI byte buffer and return it. animate=0 bypasses EMA/hysteresis
 * (first frame, --once). home=1 addresses each row with the cursor (loop
 * mode); home=0 emits plain newline-separated rows (--once). */
const char *renderer_frame(renderer *r, const scene *sc, int animate,
                           int home, size_t *len);

/* renderer_frame plus a cross-dissolve: after the raymarch, every raw cell
 * is lerped toward base (same layout as renderer_cells) with weight base_w
 * before filtering/mapping. base=NULL is a plain frame. */
const char *renderer_frame_blend(renderer *r, const scene *sc,
                                 const cell *base, float base_w,
                                 int animate, int home, size_t *len);

/* Raw raymarch grid of the last frame (pre-filter, pre-EMA). count is
 * cols*rows, or cols*rows*4 in blocks mode. Valid until the next
 * renderer_frame/renderer_frame_blend/renderer_resize. */
const cell *renderer_cells(const renderer *r, size_t *count);

#endif
