// SPDX-License-Identifier: MIT
/* Declarations shared by the whole program. */
#ifndef MIRROR_H
#define MIRROR_H

#include <signal.h>
#include <stdint.h>

typedef struct {
    int fullscreen;        /* -f */
    int verbose;           /* -v */
    const char *name;      /* -n: AirPlay receiver name */
    int port;              /* -p: first AirPlay port (0 = automatic) */
    int no_audio;          /* -a: ignore audio */
} options_t;

/* Set to 0 by Ctrl-C, by closing the window or by pressing Q */
extern volatile sig_atomic_t g_running;

uint64_t now_ns(void);

int airplay_run(const options_t *o);       /* airplay.c */

#endif
