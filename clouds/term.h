#ifndef TERM_H
#define TERM_H

#include <signal.h>

extern volatile sig_atomic_t g_quit;
extern volatile sig_atomic_t g_resize;

/* above raw bytes so arrow codes can never collide with one */
enum { TK_NONE = 0, TK_UP = 256, TK_DOWN, TK_RIGHT, TK_LEFT };

double now_s(void);         /* CLOCK_MONOTONIC seconds */
void sleep_s(double s);     /* nanosleep wrapper */

void term_size(int *cols, int *rows);
void term_setup(void);      /* raw mode + alt screen; atexit-restores */
void term_restore(void);    /* idempotent */
int  term_read_key(void);   /* TK_NONE, a TK_* arrow, or the raw byte */
void term_install_signals(void);

#endif
