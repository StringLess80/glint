// SPDX-License-Identifier: MIT
/*
 * Window and H.264 decoder.
 *
 * Video arrives on the AirPlay library's network threads, not on the window's
 * thread. Therefore:
 *   - viewer_reset_decoder / viewer_decode / viewer_clear can be called from any thread;
 *   - viewer_poll must only be called from the main thread: it handles window
 *     events and shows the latest decoded frame.
 */
#ifndef VIEWER_H
#define VIEWER_H

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <SDL.h>

typedef struct {
    /* window: main thread only */
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int tex_w, tex_h, fullscreen;
    AVFrame *display;
    struct SwsContext *sws;        /* YCbCr -> RGB conversion */
    int sws_w, sws_h, sws_fmt, sws_matrix, sws_full;
    unsigned long shown, shown_last;
    uint64_t t_title;
    char status[128];              /* window title while there is no video */

    int utf8_titles;               /* the locale is UTF-8: titles may use non-ASCII characters */

    /* decoder: protected by dec_lock */
    SDL_mutex *dec_lock;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    unsigned long decoded;

    /* frame handoff to the main thread: protected by lock */
    SDL_mutex *lock;
    AVFrame *pending;
    int has_pending, want_clear;
    Uint32 wake_event;

    int verbose;
} viewer_t;

int  viewer_init(viewer_t *v, int fullscreen, int verbose);
void viewer_free(viewer_t *v);

/* Reopens the H.264 decoder from scratch: call it at the start of every new stream */
int  viewer_reset_decoder(viewer_t *v);

/* Decodes an H.264 packet and hands the result to the window */
void viewer_decode(viewer_t *v, const uint8_t *data, size_t len);

/* Clears the window (e.g. when the iPad stops mirroring) */
void viewer_clear(viewer_t *v);

/* Text shown in the window title while there is no video (main thread) */
void viewer_set_status(viewer_t *v, const char *text);

/* Handles events and shows the most recent frame. Waits at most wait_ms. */
void viewer_poll(viewer_t *v, int wait_ms);

#endif
