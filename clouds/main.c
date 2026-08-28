#include <getopt.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cloud.h"
#include "prerender.h"
#include "render.h"
#include "term.h"

#define VERSION "0.1.0"
#define DEFAULT_RAMP " .:;*oO#%@"
#define KEY_STEP 6.f /* degrees of yaw/pitch per arrow-key event */

/* Named presets: a bundle of flags applied before anything the user
 * typed, so explicit flags override a preset regardless of order.
 * Invoked as -P NAME / --preset NAME. Space-separated flags only,
 * no quoting. */
static const struct { const char *name; const char *args; } presets[] = {
    { "fixed", "-w 0.3 -e 0.6 -y 270" },
    { "flat", "-e 0.6 -H 0.6 -t sunset -w 0.5 -c 1.2 -y 75" },
    { "below", "-e 0 -w 0.1 -y 270" },
    { "thunder", "-t storm -H 6 -p 0 -y 340" },
    { "sol", "-t golden -H 2 -y 30 -p 5 -e 0.6" },
    { "nimbus", "-e 0 -H 6 -c 0.5 -p 30 -y 30" },
};
#define NPRESETS (sizeof presets / sizeof *presets)

#define MAX_ARGS 64

/* Copy argv into out with preset references replaced by their flags.
 * Preset flags collect ahead of the user's own flags, so an explicit
 * flag overrides a preset no matter where the preset appears.
 * Returns the new argc, or -1 after printing an error. */
static int expand_presets(int argc, char **argv, char **out)
{
    char *user[MAX_ARGS];
    int npre = 0, nuser = 0, literal = 0;
    out[npre++] = argv[0];
    for (int i = 1; i < argc; i++) {
        const char *name = NULL;
        if (!literal) {
            if (!strcmp(argv[i], "--")) literal = 1;
            else if (!strcmp(argv[i], "-P") || !strcmp(argv[i], "--preset")) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "clouds: %s needs a name\n", argv[i]);
                    return -1;
                }
                name = argv[++i];
            } else if (!strncmp(argv[i], "--preset=", 9))
                name = argv[i] + 9;
        }
        if (!name) {
            if (nuser >= MAX_ARGS) goto too_many;
            user[nuser++] = argv[i];
            continue;
        }
        const char *args = NULL;
        for (size_t k = 0; k < NPRESETS; k++)
            if (!strcmp(presets[k].name, name)) { args = presets[k].args; break; }
        if (!args) {
            fprintf(stderr, "clouds: unknown preset '%s' (have:", name);
            for (size_t k = 0; k < NPRESETS; k++)
                fprintf(stderr, " %s", presets[k].name);
            fputs(")\n", stderr);
            return -1;
        }
        /* tokens point into this never-freed copy; they must outlive parsing */
        char *buf = strdup(args);
        for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
            if (npre >= MAX_ARGS) goto too_many;
            out[npre++] = tok;
        }
    }
    if (npre + nuser > MAX_ARGS) goto too_many;
    memcpy(out + npre, user, (size_t)nuser * sizeof *user);
    return npre + nuser;
too_many:
    fprintf(stderr, "clouds: too many arguments\n");
    return -1;
}

static void usage(FILE *f)
{
    fputs(
"Usage: clouds [OPTIONS]\n"
"Use the arrow keys to look around.\n"
"\n"
"volumetric clouds~\n"
"\n"
"Presets: flat below thunder sol nimbus\n"
"Times: noon afternoon golden sunset storm\n"
"\n"
"  -P, --preset NAME    use a preset (listed above)\n"
"  -c, --coverage F     cloud amount (default 0.9) [0,2]\n"
"  -H, --height F       cloud size (default 4.0) [0.5,6]\n"
"  -t, --time NAME      time/weather for backdrop (listed above)\n"
"  -z, --azimuth DEG    sun azimuth in degrees (default 30) [0,360]\n"
"  -w, --wind F         wind/cloud speed (default 1.0)\n"
"  -e, --elevation F    camera elevation; [0,1] below/above clouds (default 0.6)\n"
"  -y, --yaw DEG        camera yaw (default 0) [0,360]\n"
"  -p, --pitch DEG      camera pitch [-89,89]\n"
"  -q, --quality Q      raymarch steps (default 40), beware CPU cost! [10,512]\n"
"  -F, --fps N          target fps (default 24) \n"
"  -C, --color MODE     auto (default) | truecolor | 256 | mono\n"
"  -a, --ascii          draw with ASCII glyphs instead of block elements\n"
"  -r, --ramp STR       ASCII transparency ramp (default \"" DEFAULT_RAMP "\")\n"
"  -i, --interactive    fly with the arrow keys; -w sets speed\n"
"  -O, --once           render a single frame to stdout\n"
"  -L, --loop SEC       prerender n seconds of frames to loop\n"
"  -o, --output FILE    with -L: save prerender to file instead of playing\n"
"  -f, --file FILE      play a preredered file\n", f);
}

static colormode detect_color(void)
{
    const char *ct = getenv("COLORTERM");
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))
        return CM_TRUECOLOR;
    return CM_256;
}

/* block elements need a UTF-8 terminal; anything else falls back to ASCII */
static int detect_utf8(void)
{
    setlocale(LC_CTYPE, "");
    const char *cs = nl_langinfo(CODESET);
    return cs && (!strcmp(cs, "UTF-8") || !strcmp(cs, "utf8"));
}

int main(int argc, char **argv)
{
    scene sc;
    memset(&sc, 0, sizeof sc);
    sc.seed = (uint32_t)time(NULL);
    sc.coverage = 0.9f;
    sc.wind = 1.0f;
    sc.elevation = 0.6f;
    sc.height = 4.0f;
    sc.yaw_deg = 0.f;
    sc.pitch_deg = NAN; /* auto: aim at the cloud deck */
    sc.sun_elev_deg = 38.f;
    sc.sun_az_deg = 30.f;
    sc.steps = 40;

    int fps = 24, fps_set = 0;
    int once = 0, stats = 0, ascii = 0;
    double loop_sec = 0.0;
    const char *out_path = NULL, *play_path = NULL;
    const char *ramp = DEFAULT_RAMP;
    const char *color_arg = "auto";

    /* -P/--preset never reaches getopt: expand_presets() consumes it
     * (standalone flag only, not clustered) */
    static const struct option longopts[] = {
        { "interactive",   no_argument,       NULL, 'i' },
        { "ascii",    no_argument,       NULL, 'a' },
        { "seed",     required_argument, NULL, 's' },
        { "coverage", required_argument, NULL, 'c' },
        { "wind",     required_argument, NULL, 'w' },
        { "elevation", required_argument, NULL, 'e' },
        { "height",   required_argument, NULL, 'H' },
        { "yaw",      required_argument, NULL, 'y' },
        { "pitch",    required_argument, NULL, 'p' },
        { "time",     required_argument, NULL, 't' },
        { "azimuth",  required_argument, NULL, 'z' },
        { "fps",      required_argument, NULL, 'F' },
        { "loop",     required_argument, NULL, 'L' },
        { "color",    required_argument, NULL, 'C' },
        { "ramp",     required_argument, NULL, 'r' },
        { "quality",  required_argument, NULL, 'q' },
        { "once",     no_argument,       NULL, 'O' },
        { "output",   required_argument, NULL, 'o' },
        { "file",     required_argument, NULL, 'f' },
        { "stats",    no_argument,       NULL, 1000 },
        { "help",     no_argument,       NULL, 'h' },
        { "version",  no_argument,       NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };

    char *xargv[MAX_ARGS];
    int xargc = expand_presets(argc, argv, xargv);
    if (xargc < 0) return 1;

    int opt;
    while ((opt = getopt_long(xargc, xargv, "iL:as:c:w:e:H:y:p:t:z:C:r:q:Oo:f:hv",
                              longopts, NULL)) != -1) {
        switch (opt) {
        case 'i': sc.flight = 1; break;
        case 'a': ascii = 1; break;
        case 's': sc.seed = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'c': sc.coverage = strtof(optarg, NULL);
                  if (sc.coverage < 0.f) sc.coverage = 0.f;
                  if (sc.coverage > 2.f) sc.coverage = 2.f;
                  break;
        case 'w': sc.wind = strtof(optarg, NULL); break;
        case 'e': sc.elevation = strtof(optarg, NULL);
                  if (sc.elevation < 0.f) sc.elevation = 0.f;
                  if (sc.elevation > 5.f) sc.elevation = 1.f;
                  break;
        case 'H': sc.height = strtof(optarg, NULL);
                  if (sc.height < 0.5f) sc.height = 0.5f;
                  if (sc.height > 6.f) sc.height = 3.f;
                  break;
        case 'y': sc.yaw_deg = fmodf(strtof(optarg, NULL), 360.f);
                  if (sc.yaw_deg < 0.f) sc.yaw_deg += 360.f;
                  break;
        case 'p': sc.pitch_deg = strtof(optarg, NULL);
                  if (sc.pitch_deg < -89.f) sc.pitch_deg = -89.f;
                  if (sc.pitch_deg > 89.f) sc.pitch_deg = 89.f;
                  break;
        case 't':
            if (!strcmp(optarg, "noon")) sc.sun_elev_deg = 68.f;
            else if (!strcmp(optarg, "afternoon")) sc.sun_elev_deg = 20.f;
            else if (!strcmp(optarg, "golden")) sc.sun_elev_deg = 10.f;
            else if (!strcmp(optarg, "sunset")) sc.sun_elev_deg = 2.5f;
            else if (!strcmp(optarg, "storm")) sc.storm = 1.f;
            else { fprintf(stderr, "clouds: bad time '%s'\n", optarg); usage(stderr); return 1; }
            break;
        case 1001: fps = atoi(optarg);
                  if (fps < 1) fps = 1;
                  if (fps > 60) fps = 60;
                  fps_set = 1;
                  break;
        case 'L': loop_sec = strtod(optarg, NULL);
                  if (loop_sec <= 0) {
                      fprintf(stderr, "clouds: bad loop seconds '%s'\n", optarg);
                      usage(stderr);
                      return 1;
                  }
                  if (loop_sec > 3600) loop_sec = 3600;
                  break;
        case 'z': sc.sun_az_deg = fmodf(strtof(optarg, NULL), 360.f);
                  if (sc.sun_az_deg < 0.f) sc.sun_az_deg += 360.f;
                  break;
        case 'C': color_arg = optarg; break;
        case 'r': if (strlen(optarg) < 2) {
                      fprintf(stderr, "clouds: ramp needs at least 2 chars\n");
                      return 1;
                  }
                  ramp = optarg;
                  break;
        case 'q': sc.steps = strtof(optarg, NULL);
                  if (sc.steps < 10) sc.steps = 10;
                  if (sc.steps > 512) sc.steps = 512;
                  break;
        case 'O': once = 1; break;
        case 'o': out_path = optarg; break;
        case 'f': play_path = optarg; break;
        case 1000: stats = 1; break;
        case 'h': usage(stdout); return 0;
        case 'v': puts("clouds " VERSION); return 0;
        default: usage(stderr); return 1;
        }
    }
    if (optind < xargc) {
        fprintf(stderr, "clouds: unexpected argument '%s'\n", xargv[optind]);
        usage(stderr);
        return 1;
    }
    if (loop_sec > 0 && sc.flight) {
        fprintf(stderr, "clouds: -L prerenders a fixed loop; it cannot be combined with -i\n");
        return 1;
    }
    if (loop_sec > 0 && once) {
        fprintf(stderr, "clouds: -L and --once cannot be combined\n");
        return 1;
    }
    if (out_path && loop_sec <= 0) {
        fprintf(stderr, "clouds: -o/--output needs -L to prerender a loop\n");
        return 1;
    }
    if (play_path && (loop_sec > 0 || out_path || once || sc.flight)) {
        fprintf(stderr, "clouds: -f replays a finished recording; it cannot be "
                        "combined with -L, -o, -O, or -i\n");
        return 1;
    }
    if (play_path)
        return replay_run(play_path, fps_set ? fps : 0, stats);

    colormode cm;
    if (!strcmp(color_arg, "auto")) cm = detect_color();
    else if (!strcmp(color_arg, "truecolor")) cm = CM_TRUECOLOR;
    else if (!strcmp(color_arg, "256")) cm = CM_256;
    else if (!strcmp(color_arg, "mono")) cm = CM_MONO;
    else { fprintf(stderr, "clouds: bad color mode '%s'\n", color_arg); usage(stderr); return 1; }

    int blocks = !ascii && detect_utf8();

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (int)(ncpu < 1 ? 1 : (ncpu > 12 ? 12 : ncpu));

    int cols, rows;
    term_size(&cols, &rows);

    if (once) {
        if (rows > 1) rows--; /* leave room for the shell prompt */
        /* undocumented: CLOUDS_T renders the frame at scene time T
         * (seconds), for debugging moments mid-animation */
        const char *et = getenv("CLOUDS_T");
        scene_setup(&sc, cols, rows, et ? atof(et) : 0.0);
        renderer *R = renderer_create(cols, rows, cm, ramp, blocks, nthreads);
        size_t len;
        const char *out = renderer_frame(R, &sc, 0, 0, &len);
        fwrite(out, 1, len, stdout);
        fputc('\n', stdout);
        renderer_destroy(R);
        return 0;
    }

    if (loop_sec > 0)
        return loop_run(&sc, cols, rows, cm, ramp, blocks, nthreads,
                        fps, loop_sec, out_path, stats);

    term_install_signals();
    term_setup();
    renderer *R = renderer_create(cols, rows, cm, ramp, blocks, nthreads);

    double t_start = now_s();
    double period = 1.0 / (double)fps;
    double next = t_start;
    double render_total = 0.0;
    long frames = 0;
    int first = 1;

    while (!g_quit) {
        if (g_resize) {
            g_resize = 0;
            term_size(&cols, &rows);
            renderer_resize(R, cols, rows);
            fputs("\x1b[2J", stdout);
            fflush(stdout);
            first = 1;
        }

        scene_setup(&sc, cols, rows, now_s() - t_start);
        double a = now_s();
        size_t len;
        const char *out = renderer_frame(R, &sc, !first, 1, &len);
        first = 0;
        ssize_t wr = write(STDOUT_FILENO, out, len);
        (void)wr;
        render_total += now_s() - a;
        frames++;

        int key, quit = 0;
        while ((key = term_read_key()) != TK_NONE) {
            if (key == TK_LEFT || key == TK_RIGHT) {
                sc.yaw_deg = fmodf(sc.yaw_deg + 360.f
                                   + (key == TK_RIGHT ? KEY_STEP : -KEY_STEP),
                                   360.f);
            } else if (key == TK_UP || key == TK_DOWN) {
                /* leaving auto-aim: seed from the current view direction */
                if (isnan(sc.pitch_deg))
                    sc.pitch_deg = asinf(sc.fwd.y) * (180.f / 3.14159265f);
                sc.pitch_deg += key == TK_UP ? KEY_STEP : -KEY_STEP;
                if (sc.pitch_deg > 89.f) sc.pitch_deg = 89.f;
                if (sc.pitch_deg < -89.f) sc.pitch_deg = -89.f;
            } else if (key == 'q' || key == 0x1b || key == 3) {
                quit = 1;
            }
        }
        if (quit)
            break;

        next += period;
        double sleep = next - now_s();
        if (sleep > 0)
            sleep_s(sleep);
        else
            next = now_s(); /* fell behind: don't try to catch up */
    }

    term_restore();
    renderer_destroy(R);
    if (stats && frames > 0)
        fprintf(stderr, "clouds: %ld frames, avg %.1f ms/frame (render+emit)\n",
                frames, render_total / (double)frames * 1000.0);
    return 0;
}
