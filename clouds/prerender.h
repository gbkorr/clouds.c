#ifndef PRERENDER_H
#define PRERENDER_H

#include "cloud.h"
#include "render.h"

/* -L mode: prerender fps*loop_sec seconds of frames into a RAM or disk
 * cache (with a crossfaded loop seam), then replay them at fps with
 * near-zero CPU — or, with out_path set, save them to a recording file
 * and exit instead. Returns the process exit code. */
int loop_run(scene *sc, int cols, int rows, colormode cm, const char *ramp,
             int blocks, int nthreads, int fps, double loop_sec,
             const char *out_path, int stats);

/* -f mode: replay a recording saved by loop_run's out_path, at its
 * recorded fps unless fps_override > 0. Returns the process exit code. */
int replay_run(const char *path, int fps_override, int stats);

#endif
