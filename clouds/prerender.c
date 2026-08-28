#include "prerender.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/statfs.h>
#endif

#include "term.h"

#define WARMUP 8            /* EMA keeps 0.4/frame; 0.4^8 residual ~0.07% */

static int dir_is_ram_backed(const char *dir)
{
#ifdef __linux__
    struct statfs st;
    if (statfs(dir, &st) != 0) return 0;
    return st.f_type == 0x01021994; /* TMPFS_MAGIC */
#else
    (void)dir;
    return 0;
#endif
}

/* Anonymous cache file: created, unlinked, reclaimed on any exit. tmpfs
 * dirs are RAM-backed and would defeat a disk cache, so they're only
 * used when nothing else works ($TMPDIR is often /tmp = tmpfs). */
static int store_open_file(void)
{
    const char *cand[3];
    int nc = 0;
    const char *td = getenv("TMPDIR");
    if (td && *td) cand[nc++] = td;
    cand[nc++] = "/var/tmp";
    cand[nc++] = ".";
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nc; i++) {
            if (pass == 0 && dir_is_ram_backed(cand[i]))
                continue;
            char path[4096];
            snprintf(path, sizeof path, "%s/clouds-XXXXXX", cand[i]);
            int fd = mkstemp(path);
            if (fd < 0) continue;
            unlink(path);
            if (pass == 1)
                fprintf(stderr, "clouds: warning: frame cache on RAM-backed %s\n",
                        cand[i]);
            return fd;
        }
    }
    fprintf(stderr, "clouds: cannot create a frame cache file\n");
    return -1;
}

static int write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len > 0) {
        ssize_t wr = write(fd, p, len);
        if (wr < 0) {
            if (errno == EINTR) continue;
            perror("clouds: frame cache write");
            return -1;
        }
        p += wr;
        len -= (size_t)wr;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t len, size_t off)
{
    unsigned char *p = data;
    while (len > 0) {
        ssize_t rd = pread(fd, p, len, (off_t)off);
        if (rd < 0 && errno == EINTR) continue;
        if (rd <= 0) return -1;
        p += rd;
        off += (size_t)rd;
        len -= (size_t)rd;
    }
    return 0;
}

static int pwrite_all(int fd, const void *data, size_t len, size_t off)
{
    const unsigned char *p = data;
    while (len > 0) {
        ssize_t wr = pwrite(fd, p, len, (off_t)off);
        if (wr < 0) {
            if (errno == EINTR) continue;
            perror("clouds: recording write");
            return -1;
        }
        p += wr;
        off += (size_t)wr;
        len -= (size_t)wr;
    }
    return 0;
}

/* ---------------- LZSS codec ----------------
 * Frames are mostly repetitive ANSI escape structure; a plain
 * byte-oriented LZSS gets a few-fold shrink with no dependencies.
 * Stream: control byte of 8 LSB-first flags; flag 0 = literal byte,
 * flag 1 = match of u16 distance (1..65535 back) + u8 length-4. */

#define LZ_MINMATCH 4
#define LZ_MAXMATCH 259
#define LZ_WINDOW 65535
#define LZ_HASHBITS 15
#define LZ_CHAIN 32

/* worst case: all literals, one control byte per 8 */
#define LZ_BOUND(n) ((n) + (n) / 8 + 16)

static inline uint32_t lz_hash(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - LZ_HASHBITS);
}

/* out must hold LZ_BOUND(n). Returns compressed size, or 0 to mean
 * "store this frame raw" (out of memory for the match tables). */
static size_t lzss_compress(const unsigned char *in, size_t n,
                            unsigned char *out)
{
    int32_t *head = malloc(sizeof *head << LZ_HASHBITS);
    int32_t *prev = malloc(n * sizeof *prev);
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    memset(head, -1, sizeof *head << LZ_HASHBITS);

    size_t o = 0, i = 0, ctrl = 0;
    int nbits = 8;
    while (i < n) {
        size_t best = 0, bestpos = 0;
        if (i + LZ_MINMATCH <= n) {
            uint32_t h = lz_hash(in + i);
            size_t max = n - i;
            if (max > LZ_MAXMATCH) max = LZ_MAXMATCH;
            int32_t cand = prev[i] = head[h];
            head[h] = (int32_t)i;
            for (int d = 0; cand >= 0 && d < LZ_CHAIN; d++) {
                if (i - (size_t)cand > LZ_WINDOW) break;
                const unsigned char *a = in + cand, *b = in + i;
                size_t l = 0;
                while (l < max && a[l] == b[l]) l++;
                if (l > best) {
                    best = l;
                    bestpos = (size_t)cand;
                    if (l >= max) break;
                }
                cand = prev[cand];
            }
        }
        if (nbits == 8) {
            ctrl = o++;
            out[ctrl] = 0;
            nbits = 0;
        }
        if (best >= LZ_MINMATCH) {
            out[ctrl] |= (unsigned char)(1u << nbits);
            size_t dist = i - bestpos;
            out[o++] = (unsigned char)dist;
            out[o++] = (unsigned char)(dist >> 8);
            out[o++] = (unsigned char)(best - LZ_MINMATCH);
            for (size_t k = i + 1; k < i + best && k + LZ_MINMATCH <= n; k++) {
                uint32_t h = lz_hash(in + k);
                prev[k] = head[h];
                head[h] = (int32_t)k;
            }
            i += best;
        } else {
            out[o++] = in[i++];
        }
        nbits++;
    }
    free(head);
    free(prev);
    return o;
}

/* Fully bounds-checked so a corrupt recording can't overrun anything. */
static int lzss_decompress(const unsigned char *in, size_t inlen,
                           unsigned char *out, size_t outlen)
{
    size_t i = 0, o = 0;
    unsigned ctrl = 0;
    int nbits = 0;
    while (o < outlen) {
        if (nbits == 0) {
            if (i >= inlen) return -1;
            ctrl = in[i++];
            nbits = 8;
        }
        if (ctrl & 1) {
            if (i + 3 > inlen) return -1;
            size_t dist = in[i] | ((size_t)in[i + 1] << 8);
            size_t len = (size_t)in[i + 2] + LZ_MINMATCH;
            i += 3;
            if (dist == 0 || dist > o || len > outlen - o) return -1;
            const unsigned char *src = out + o - dist;
            for (size_t k = 0; k < len; k++) /* may overlap: byte-by-byte */
                out[o + k] = src[k];
            o += len;
        } else {
            if (i >= inlen) return -1;
            out[o++] = in[i++];
        }
        ctrl >>= 1;
        nbits--;
    }
    return i == inlen ? 0 : -1;
}

/* ---------------- frame store ----------------
 * Frames always live LZSS-compressed in a file -- the anonymous cache
 * for plain -L, the recording itself for -o/-f -- and are decompressed
 * one at a time during playback, so RAM never holds the loop. */
typedef struct {
    int n;
    size_t *off;    /* absolute file offset of frame i's stream */
    size_t *clen;   /* compressed length; == rlen means stored raw */
    size_t *rlen;   /* raw (serialized ANSI) length */
    size_t base;    /* data start: 0, or past a recording's header */
    size_t total;   /* compressed bytes appended */
    size_t raw_total;
    size_t maxr, maxc;
    int fd;
    unsigned char *scratch; /* compress buffer, grown on demand */
    size_t scratch_cap;
} framestore;

static int fs_init(framestore *fs, int n)
{
    memset(fs, 0, sizeof *fs);
    fs->n = n;
    fs->fd = -1;
    fs->off = malloc((size_t)n * sizeof *fs->off);
    fs->clen = malloc((size_t)n * sizeof *fs->clen);
    fs->rlen = malloc((size_t)n * sizeof *fs->rlen);
    if (!fs->off || !fs->clen || !fs->rlen) {
        fprintf(stderr, "clouds: out of memory\n");
        return -1;
    }
    return 0;
}

static void fs_free(framestore *fs)
{
    free(fs->off);
    free(fs->clen);
    free(fs->rlen);
    free(fs->scratch);
    if (fs->fd >= 0) close(fs->fd);
}

static int fs_append(framestore *fs, int i, const void *data, size_t rawlen)
{
    if (LZ_BOUND(rawlen) > fs->scratch_cap) {
        unsigned char *p = realloc(fs->scratch, LZ_BOUND(rawlen));
        if (!p) {
            fprintf(stderr, "clouds: out of memory\n");
            return -1;
        }
        fs->scratch = p;
        fs->scratch_cap = LZ_BOUND(rawlen);
    }
    size_t c = lzss_compress(data, rawlen, fs->scratch);
    const void *src = fs->scratch;
    if (c == 0 || c >= rawlen) { /* incompressible: store raw */
        c = rawlen;
        src = data;
    }
    fs->off[i] = fs->base + fs->total;
    fs->rlen[i] = rawlen;
    fs->clen[i] = c;
    if (rawlen > fs->maxr) fs->maxr = rawlen;
    if (c > fs->maxc) fs->maxc = c;
    if (write_all(fs->fd, src, c)) return -1;
    fs->total += c;
    fs->raw_total += rawlen;
    return 0;
}

/* Loop the store's frames at fps until q/Esc/Ctrl-C, starting at frame
 * `start`, decompressing each on the fly. Returns frames played, or -1
 * on out-of-memory. */
static long fs_play(framestore *fs, int fps, int start)
{
    unsigned char *rbuf = malloc(fs->maxr ? fs->maxr : 1);
    unsigned char *cbuf = malloc(fs->maxc ? fs->maxc : 1);
    if (!rbuf || !cbuf) {
        fprintf(stderr, "clouds: out of memory\n");
        free(rbuf);
        free(cbuf);
        return -1;
    }

    long played = 0;
    term_setup();
    double period = 1.0 / (double)fps;
    double next = now_s();
    int idx = start;
    while (!g_quit) {
        if (g_resize) {
            /* frames are bound to their recorded size; just clear once so
             * stale output outside their rectangle doesn't linger */
            g_resize = 0;
            fputs("\x1b[2J", stdout);
            fflush(stdout);
        }

        if (fs->clen[idx] == fs->rlen[idx]) { /* stored raw */
            if (read_all(fs->fd, rbuf, fs->rlen[idx], fs->off[idx])) break;
        } else {
            if (read_all(fs->fd, cbuf, fs->clen[idx], fs->off[idx])
                || lzss_decompress(cbuf, fs->clen[idx], rbuf, fs->rlen[idx]))
                break;
        }
        ssize_t wr = write(STDOUT_FILENO, rbuf, fs->rlen[idx]);
        (void)wr;
        played++;

        /* no live scene to steer: only q / Esc / Ctrl-C quit */
        int key, quit = 0;
        while ((key = term_read_key()) != TK_NONE)
            if (key == 'q' || key == 0x1b || key == 3)
                quit = 1;
        if (quit)
            break;

        idx = (idx + 1) % fs->n;
        next += period;
        double sleep = next - now_s();
        if (sleep > 0)
            sleep_s(sleep);
        else
            next = now_s(); /* fell behind: don't try to catch up */
    }
    term_restore();
    free(rbuf);
    free(cbuf);
    return played;
}

/* Recording layout: "CLOUDS", u32 version/cols/rows/fps/nframes, then per
 * frame u32 raw/compressed lengths (equal = frame stored raw), then the
 * LZSS frame streams (native-endian; a local cache format, not an
 * interchange one). Streams appear in playback order, which equals
 * prerender production order, so -o writes them straight through and
 * patches the length table afterward. */
#define REC_MAGIC "CLOUDS"
#define REC_VERSION 2
#define REC_HDR (6 + 5 * 4)
#define REC_TAB(n) ((size_t)(n) * 2 * sizeof(uint32_t))

int loop_run(scene *sc, int cols, int rows, colormode cm, const char *ramp,
             int blocks, int nthreads, int fps, double loop_sec,
             const char *out_path, int stats)
{
    int N = (int)lround((double)fps * loop_sec);
    if (N < 1) N = 1;
    /* crossfade tail: ~1s, at most a quarter of the loop */
    int M = fps < N / 4 ? fps : N / 4;

    term_install_signals();

    framestore fs;
    if (fs_init(&fs, N)) {
        fs_free(&fs);
        return 1;
    }
    if (out_path) {
        fs.fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fs.fd < 0) {
            fprintf(stderr, "clouds: %s: %s\n", out_path, strerror(errno));
            fs_free(&fs);
            return 1;
        }
        /* the length table is known only after compressing, so write a
         * placeholder now and patch it in place at the end */
        unsigned char hdr[REC_HDR];
        uint32_t v[5] = { REC_VERSION, (uint32_t)cols, (uint32_t)rows,
                          (uint32_t)fps, (uint32_t)N };
        memcpy(hdr, REC_MAGIC, 6);
        memcpy(hdr + 6, v, sizeof v);
        uint32_t *tab = calloc((size_t)N * 2, sizeof *tab);
        if (!tab) fprintf(stderr, "clouds: out of memory\n");
        int bad = !tab || write_all(fs.fd, hdr, sizeof hdr)
               || write_all(fs.fd, tab, REC_TAB(N));
        free(tab);
        if (bad) {
            fs_free(&fs);
            unlink(out_path);
            return 1;
        }
        fs.base = REC_HDR + REC_TAB(N);
    } else {
        fs.fd = store_open_file();
        if (fs.fd < 0) {
            fs_free(&fs);
            return 1;
        }
    }

    renderer *R = renderer_create(cols, rows, cm, ramp, blocks, nthreads);
    cell **saved = NULL;
    if (!R || (M > 0 && !(saved = calloc((size_t)M, sizeof *saved)))) {
        fprintf(stderr, "clouds: out of memory\n");
        if (R) renderer_destroy(R);
        fs_free(&fs);
        if (out_path) unlink(out_path);
        return 1;
    }

    /* One pass over WARMUP + N + M frames at t = k/fps: warm the EMA,
     * capture the loop body, then re-render the first M frames' scene
     * times shifted past the end and blend the saved raw cells back in
     * with rising weight, so frame 0 continues frame N-1 seamlessly. */
    int total = WARMUP + N + M;
    int step = total >= 200 ? 10 : total >= 50 ? 5 : 1;
    double render_total = 0.0;
    int rc = 0;

    for (int k = 0; k < total && !g_quit && !rc; k++) {
        if (g_resize) g_resize = 0; /* cache is bound to the launch size */
        scene_setup(sc, cols, rows, (double)k / (double)fps);
        int i = k - WARMUP;
        int j = i - N;
        double a = now_s();
        size_t len;
        const char *out;
        if (j >= 0)
            out = renderer_frame_blend(R, sc, saved[j], (float)j / (float)M,
                                       1, 1, &len);
        else
            out = renderer_frame(R, sc, k > 0, 1, &len);
        double dt = now_s() - a;
        render_total += dt;

        if (i < 0) {
            /* warm-up frame, discarded */
        } else if (j >= 0) {
            if (fs_append(&fs, j, out, len)) rc = 1;
        } else if (i < M) {
            size_t cnt;
            const cell *c = renderer_cells(R, &cnt);
            saved[i] = malloc(cnt * sizeof *c);
            if (!saved[i]) {
                fprintf(stderr, "clouds: out of memory\n");
                rc = 1;
            } else {
                memcpy(saved[i], c, cnt * sizeof *c);
            }
        } else {
            if (fs_append(&fs, i, out, len)) rc = 1;
        }

        if ((k + 1) % step == 0 || k + 1 == total) {
            printf("\rframe %d/%d (%d%%)", k + 1, total, (k + 1) * 100 / total);
            fflush(stdout);
        }

        /* duty-cycle cap: ~2/3 CPU so the prerender can't freeze the box */
        double zz = dt * 0.5;
        if (zz > 0.25) zz = 0.25;
        if (zz > 0) sleep_s(zz);
    }
    putchar('\n');

    for (int i = 0; i < M && saved; i++) free(saved[i]);
    free(saved);
    renderer_destroy(R);

    int cancelled = g_quit && !rc;
    if (rc || cancelled) {
        if (cancelled) fputs("clouds: cancelled\n", stderr);
        fs_free(&fs);
        if (out_path) unlink(out_path);
        return rc;
    }

    if (out_path) {
        /* patch the table: file stream order is production order, i.e.
         * playback order rotated to start at logical frame M */
        uint32_t *tab = malloc(REC_TAB(fs.n));
        int bad = !tab;
        if (bad) fprintf(stderr, "clouds: out of memory\n");
        for (int k = 0; !bad && k < fs.n; k++) {
            int i = (M + k) % fs.n;
            tab[2 * k] = (uint32_t)fs.rlen[i];
            tab[2 * k + 1] = (uint32_t)fs.clen[i];
        }
        if (!bad)
            bad = pwrite_all(fs.fd, tab, REC_TAB(fs.n), REC_HDR) != 0;
        free(tab);
        if (!bad) {
            printf("clouds: wrote %d frames (%.1f MB) to %s\n",
                   fs.n, (double)(fs.base + fs.total) / 1e6, out_path);
            if (stats)
                fprintf(stderr,
                        "clouds: prerendered %d frames, avg %.1f ms/frame\n",
                        total, render_total / (double)total * 1000.0);
        }
        fs_free(&fs);
        if (bad) unlink(out_path);
        return bad ? 1 : 0;
    }

    /* the compress buffer is prerender-only; drop it before playback */
    free(fs.scratch);
    fs.scratch = NULL;
    fs.scratch_cap = 0;

    /* frames 0..M-1 hold the crossfaded seam; start past them so the
     * first fade-in appears a full loop from now, not at t=0 */
    long played = fs_play(&fs, fps, M % fs.n);
    if (played < 0) {
        fs_free(&fs);
        return 1;
    }

    if (stats)
        fprintf(stderr,
                "clouds: prerendered %d frames, avg %.1f ms/frame, "
                "%.1f MB cached (%.1f MB raw); played %ld frames\n",
                total, render_total / (double)total * 1000.0,
                (double)fs.total / 1e6, (double)fs.raw_total / 1e6, played);

    fs_free(&fs);
    return 0;
}

int replay_run(const char *path, int fps_override, int stats)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "clouds: %s: %s\n", path, strerror(errno));
        return 1;
    }
    unsigned char hdr[REC_HDR];
    uint32_t v[5];
    if (read_all(fd, hdr, sizeof hdr, 0) || memcmp(hdr, REC_MAGIC, 6) != 0) {
        fprintf(stderr, "clouds: %s is not a clouds recording\n", path);
        close(fd);
        return 1;
    }
    memcpy(v, hdr + 6, sizeof v);
    uint32_t version = v[0], ffps = v[3], n = v[4];
    if (version != REC_VERSION) {
        fprintf(stderr, "clouds: %s: unsupported recording version %u\n",
                path, version);
        close(fd);
        return 1;
    }
    if (n < 1 || n > 10u * 1000 * 1000 || ffps < 1 || ffps > 60) {
        fprintf(stderr, "clouds: %s: corrupt recording\n", path);
        close(fd);
        return 1;
    }

    framestore fs;
    if (fs_init(&fs, (int)n)) {
        close(fd);
        return 1;
    }
    fs.fd = fd;
    uint32_t *tab = malloc(REC_TAB(n));
    if (!tab) {
        fprintf(stderr, "clouds: out of memory\n");
        fs_free(&fs);
        return 1;
    }

    /* validate the table and index the streams; frames stay compressed
     * in the file and are decompressed per tick by fs_play */
    int bad = read_all(fd, tab, REC_TAB(n), REC_HDR);
    fs.base = REC_HDR + REC_TAB(n);
    size_t off = fs.base;
    for (uint32_t i = 0; !bad && i < n; i++) {
        size_t raw = tab[2 * i], comp = tab[2 * i + 1];
        if (raw == 0 || raw > ((size_t)1 << 30) || comp == 0
            || comp > LZ_BOUND(raw)) {
            bad = 1;
            break;
        }
        fs.off[i] = off;
        fs.rlen[i] = raw;
        fs.clen[i] = comp;
        if (raw > fs.maxr) fs.maxr = raw;
        if (comp > fs.maxc) fs.maxc = comp;
        off += comp;
        fs.raw_total += raw;
        fs.total += comp;
    }
    free(tab);
    struct stat st;
    if (!bad && (fstat(fd, &st) != 0 || (size_t)st.st_size != off))
        bad = 1;
    if (bad) {
        fprintf(stderr, "clouds: %s: corrupt recording\n", path);
        fs_free(&fs);
        return 1;
    }

    term_install_signals();
    int fps = fps_override ? fps_override : (int)ffps;
    long played = fs_play(&fs, fps, 0);
    if (stats && played >= 0)
        fprintf(stderr, "clouds: %ux%u recording, %u frames, %.1f MB "
                "(%.1f MB file), %d fps; played %ld frames\n",
                v[1], v[2], n, (double)fs.raw_total / 1e6,
                (double)(fs.base + fs.total) / 1e6, fps, played);
    fs_free(&fs);
    return played < 0 ? 1 : 0;
}
