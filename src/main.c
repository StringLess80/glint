// SPDX-License-Identifier: MIT
/*
 * Glint — shows an iPad (or iPhone) screen on Linux over Wi-Fi.
 *
 * The PC acts as an AirPlay receiver (airplay.c): on the iPad, just pick it
 * under "Screen Mirroring". The H.264 video is decoded with FFmpeg and shown
 * in an SDL2 window (viewer.c).
 */

#define _GNU_SOURCE
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>

#include "mirror.h"

volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n\n"
           "Starts the receiver; then on the iPad: Control Center -> Screen Mirroring.\n\n"
           "  -n NAME     name shown on the iPad (default: this computer's hostname)\n"
           "  -p PORT     use fixed ports PORT, PORT+1, PORT+2 (TCP and UDP),\n"
           "              useful with a firewall\n"
           "  -a          disable audio\n"
           "  -f          start in fullscreen\n"
           "  -v          print debug messages\n"
           "  -h          show this help\n\n"
           "Keys: F / F11 / double-click = fullscreen, Esc = leave fullscreen, Q = quit\n",
           prog);
}

int main(int argc, char **argv)
{
    options_t o = { 0 };
    int opt;
    while ((opt = getopt(argc, argv, "n:p:afvh")) != -1) {
        switch (opt) {
        case 'n': o.name = optarg; break;
        case 'p':
            o.port = atoi(optarg);
            if (o.port < 1024 || o.port > 65533) {
                fprintf(stderr, "The port must be between 1024 and 65533.\n");
                return 2;
            }
            break;
        case 'a': o.no_audio = 1; break;
        case 'f': o.fullscreen = 1; break;
        case 'v': o.verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Avahi warns on every start that we use its Bonjour compatibility API (which
     * UxPlay's library needs); the warning is harmless, so silence it. */
    setenv("AVAHI_COMPAT_NOWARN", "1", 0);

    int rc = airplay_run(&o);
    SDL_Quit();
    return rc;
}
