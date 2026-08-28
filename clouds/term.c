#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_quit = 0;
volatile sig_atomic_t g_resize = 0;

double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void sleep_s(double s)
{
    struct timespec ts = {
        .tv_sec = (time_t)s,
        .tv_nsec = (long)((s - (double)(time_t)s) * 1e9),
    };
    nanosleep(&ts, NULL);
}

static struct termios saved_tio;
static int saved_ok = 0;
static int raw_on = 0;

void term_size(int *cols, int *rows)
{
    /* exported COLUMNS/LINES win (useful for scripts and testing) */
    const char *ec = getenv("COLUMNS"), *el = getenv("LINES");
    if (ec && el) {
        int c = atoi(ec), l = atoi(el);
        if (c > 0 && l > 0) {
            *cols = c;
            *rows = l;
            return;
        }
    }
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

void term_setup(void)
{
    if (tcgetattr(STDIN_FILENO, &saved_tio) == 0)
        saved_ok = 1;
    struct termios t = saved_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    raw_on = 1;
    fputs("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J", stdout);
    fflush(stdout);
    atexit(term_restore);
}

void term_restore(void)
{
    if (!raw_on) return;
    raw_on = 0;
    fputs("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    if (saved_ok)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
}

/* Decode one key per call from a persistent buffer topped up with a
 * non-blocking read (VMIN=0/VTIME=0). Arrows arrive as ESC [ X (CSI)
 * or ESC O X (SS3) sequences that a slow link can split across polls,
 * so a lone ESC is held for one call before it's reported as Escape. */
int term_read_key(void)
{
    static unsigned char buf[64];
    static int len, esc_held;

    ssize_t r = read(STDIN_FILENO, buf + len, sizeof buf - (size_t)len);
    if (r > 0) len += (int)r;

    while (len > 0) {
        int k = TK_NONE, used;
        if (buf[0] == 0x1b && len >= 2 && (buf[1] == '[' || buf[1] == 'O')) {
            /* skip to the sequence's final byte (0x40..0x7e); parameter
             * bytes like the "5" in ESC [ 5 ~ (PgUp) sit below that */
            int j = 2;
            while (j < len && (buf[j] < 0x40 || buf[j] > 0x7e)) j++;
            if (j >= len) {
                if (len < (int)sizeof buf) return TK_NONE; /* incomplete: wait */
                used = len; /* junk filled the buffer: drop it */
            } else {
                used = j + 1;
                switch (buf[j]) {
                case 'A': k = TK_UP;    break;
                case 'B': k = TK_DOWN;  break;
                case 'C': k = TK_RIGHT; break;
                case 'D': k = TK_LEFT;  break;
                default:  break; /* other sequence: skip it */
                }
            }
        } else if (buf[0] == 0x1b && len < 2 && !esc_held) {
            esc_held = 1; /* maybe a split sequence: decide next poll */
            return TK_NONE;
        } else {
            used = 1;
            k = buf[0];
        }
        esc_held = 0;
        len -= used;
        memmove(buf, buf + used, (size_t)len);
        if (k != TK_NONE) return k;
    }
    return TK_NONE;
}

static void on_quit(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void on_winch(int sig)
{
    (void)sig;
    g_resize = 1;
}

void term_install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = on_winch;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
    /* if we end up in a background process group (script(1), job control),
     * tty reads/writes must fail instead of stopping the process */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
}
