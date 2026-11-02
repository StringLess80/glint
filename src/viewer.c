// SPDX-License-Identifier: MIT
/* Window (SDL2) and H.264 decoder (FFmpeg). */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>

#include "mirror.h"
#include "viewer.h"

/* ------------------------------------------------------------------------ */
/* Decoder (any thread)                                                     */
/* ------------------------------------------------------------------------ */

int viewer_reset_decoder(viewer_t *v)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "FFmpeg has no H.264 decoder\n");
        return -1;
    }
    SDL_LockMutex(v->dec_lock);
    avcodec_free_context(&v->dec);
    v->dec = avcodec_alloc_context3(codec);
    if (!v->dec) {
        SDL_UnlockMutex(v->dec_lock);
        return -1;
    }
    /* Annex B stream (00 00 00 01 start codes) carrying its own SPS/PPS: no extradata needed */
    v->dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    v->dec->thread_type = FF_THREAD_SLICE;     /* frame threading would add latency */
    int r = avcodec_open2(v->dec, codec, NULL);
    if (r < 0) {
        char e[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(r, e, sizeof e);
        fprintf(stderr, "Cannot open the H.264 decoder: %s\n", e);
        avcodec_free_context(&v->dec);
    }
    SDL_UnlockMutex(v->dec_lock);
    return r < 0 ? -1 : 0;
}

/* Hands a decoded frame to the main thread (only the most recent one is kept) */
static void hand_over(viewer_t *v, AVFrame *f)
{
    SDL_LockMutex(v->lock);
    int was_pending = v->has_pending;
    av_frame_unref(v->pending);
    av_frame_move_ref(v->pending, f);
    v->has_pending = 1;
    SDL_UnlockMutex(v->lock);

    if (!was_pending) {                   /* wake up viewer_poll */
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = v->wake_event;
        SDL_PushEvent(&e);
    }
}

void viewer_decode(viewer_t *v, const uint8_t *data, size_t len)
{
    if (len == 0 || len > INT32_MAX)
        return;
    SDL_LockMutex(v->dec_lock);
    if (!v->dec || av_new_packet(v->pkt, (int)len) < 0) {
        SDL_UnlockMutex(v->dec_lock);
        return;
    }
    memcpy(v->pkt->data, data, len);
    int r = avcodec_send_packet(v->dec, v->pkt);
    av_packet_unref(v->pkt);
    if (r < 0 && r != AVERROR(EAGAIN) && v->verbose)
        fprintf(stderr, "Video packet rejected by the decoder (%d)\n", r);
    while (avcodec_receive_frame(v->dec, v->frame) == 0) {
        v->decoded++;
        hand_over(v, v->frame);
    }
    SDL_UnlockMutex(v->dec_lock);
}

void viewer_clear(viewer_t *v)
{
    SDL_LockMutex(v->lock);
    av_frame_unref(v->pending);
    v->has_pending = 0;
    v->want_clear = 1;
    SDL_UnlockMutex(v->lock);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = v->wake_event;
    SDL_PushEvent(&e);
}

/* ------------------------------------------------------------------------ */
/* Window (main thread only)                                                */
/* ------------------------------------------------------------------------ */

static void present(viewer_t *v)
{
    SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
    SDL_RenderClear(v->ren);
    if (v->tex) {
        int ow, oh;
        SDL_GetRendererOutputSize(v->ren, &ow, &oh);
        SDL_Rect dst;                            /* keep the aspect ratio (black bars) */
        if ((long)ow * v->tex_h > (long)oh * v->tex_w) {
            dst.h = oh;
            dst.w = (int)((long)oh * v->tex_w / v->tex_h);
        } else {
            dst.w = ow;
            dst.h = (int)((long)ow * v->tex_h / v->tex_w);
        }
        dst.x = (ow - dst.w) / 2;
        dst.y = (oh - dst.h) / 2;
        SDL_RenderCopy(v->ren, v->tex, NULL, &dst);
    }
    SDL_RenderPresent(v->ren);
}

/* Resizes the window so that the video fits in 90% of the screen */
static void fit_window(viewer_t *v, int w, int h)
{
    if (v->fullscreen || w <= 0 || h <= 0)
        return;
    int di = SDL_GetWindowDisplayIndex(v->win);
    if (di < 0)
        di = 0;
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(di, &r) != 0) {
        r.w = 1280;
        r.h = 800;
    }
    double s = 0.9 * r.w / w, s2 = 0.9 * r.h / h;
    if (s2 < s)
        s = s2;
    if (s > 1.0)
        s = 1.0;
    SDL_SetWindowSize(v->win, (int)(w * s), (int)(h * s));
    SDL_SetWindowPosition(v->win, SDL_WINDOWPOS_CENTERED_DISPLAY(di), SDL_WINDOWPOS_CENTERED_DISPLAY(di));
}

/*
 * Colors: H.264 video is YCbCr and must be converted to RGB with the right formula.
 * The stream declares which one:
 *   - matrix: BT.709 (HD video) or BT.601 (SD video)
 *   - range:  limited (black = 16, white = 235) or full (0-255)
 * Apple uses full-range BT.709. SDL2's YUV textures only support limited BT.601
 * (the default), limited BT.709 and full BT.601: with the default, black and white
 * land in the wrong place and hues shift. So we convert to RGB ourselves with
 * libswscale, using what the stream declares.
 */
static int frame_matrix(const AVFrame *f)
{
    switch (f->colorspace) {
    case AVCOL_SPC_BT709:      return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:  return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M:  return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:  return SWS_CS_BT2020;
    default:                   /* not declared: usual convention based on height */
        return f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

static enum AVPixelFormat plain_format(enum AVPixelFormat fmt, int *full)
{
    /* FFmpeg's "J" formats are the old way of saying "full range" */
    switch (fmt) {
    case AV_PIX_FMT_YUVJ420P: *full = 1; return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: *full = 1; return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: *full = 1; return AV_PIX_FMT_YUV444P;
    default:                  return fmt;
    }
}

static int setup_color_conversion(viewer_t *v, const AVFrame *f)
{
    int full = (f->color_range == AVCOL_RANGE_JPEG);
    enum AVPixelFormat fmt = plain_format((enum AVPixelFormat)f->format, &full);
    int matrix = frame_matrix(f);

    if (v->sws && v->sws_w == f->width && v->sws_h == f->height && v->sws_fmt == (int)fmt &&
        v->sws_matrix == matrix && v->sws_full == full)
        return 0;                                 /* nothing changed */

    sws_freeContext(v->sws);
    v->sws = sws_getContext(f->width, f->height, fmt, f->width, f->height, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!v->sws)
        return -1;
    /* source: the stream's matrix and range; destination: RGB 0-255 */
    sws_setColorspaceDetails(v->sws, sws_getCoefficients(matrix), full,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    v->sws_w = f->width;
    v->sws_h = f->height;
    v->sws_fmt = (int)fmt;
    v->sws_matrix = matrix;
    v->sws_full = full;
    if (v->verbose)
        fprintf(stderr, "Video colors: matrix %s, %s range, primaries %s, transfer %s\n",
                av_color_space_name(f->colorspace) ? av_color_space_name(f->colorspace) : "?",
                full ? "full (0-255)" : "limited (16-235)",
                av_color_primaries_name(f->color_primaries) ? av_color_primaries_name(f->color_primaries) : "?",
                av_color_transfer_name(f->color_trc) ? av_color_transfer_name(f->color_trc) : "?");
    return 0;
}

static void show_frame(viewer_t *v, const AVFrame *f)
{
    if (f->width <= 0 || f->height <= 0)
        return;

    if (!v->tex || v->tex_w != f->width || v->tex_h != f->height) {
        /* first frame or iPad rotated: new texture and new window size */
        if (v->tex)
            SDL_DestroyTexture(v->tex);
        v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STREAMING,
                                   f->width, f->height);
        if (!v->tex) {
            fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
            v->tex_w = v->tex_h = 0;
            return;
        }
        SDL_SetTextureBlendMode(v->tex, SDL_BLENDMODE_NONE);   /* alpha is not used */
        v->tex_w = f->width;
        v->tex_h = f->height;
        fit_window(v, f->width, f->height);
    }
    if (setup_color_conversion(v, f) != 0)
        return;

    /* convert straight into the texture, without intermediate copies */
    void *pixels;
    int pitch;
    if (SDL_LockTexture(v->tex, NULL, &pixels, &pitch) != 0)
        return;
    uint8_t *dst[4] = { pixels, NULL, NULL, NULL };
    int dst_stride[4] = { pitch, 0, 0, 0 };
    sws_scale(v->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
    SDL_UnlockTexture(v->tex);
    v->shown++;
    present(v);
}

static void toggle_fullscreen(viewer_t *v)
{
    v->fullscreen = !v->fullscreen;
    SDL_SetWindowFullscreen(v->win, v->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    present(v);
}

static void handle_event(viewer_t *v, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        g_running = 0;
        break;
    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_q:
            g_running = 0;
            break;
        case SDLK_ESCAPE:
            if (v->fullscreen)
                toggle_fullscreen(v);
            else
                g_running = 0;
            break;
        case SDLK_f:
        case SDLK_F11:
            toggle_fullscreen(v);
            break;
        default:
            break;
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT && e->button.clicks == 2)
            toggle_fullscreen(v);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_EXPOSED ||
            e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            present(v);
        break;
    default:
        break;
    }
}

void viewer_poll(viewer_t *v, int wait_ms)
{
    SDL_Event e;
    int got = wait_ms > 0 ? SDL_WaitEventTimeout(&e, wait_ms) : SDL_PollEvent(&e);
    while (got) {
        handle_event(v, &e);
        got = SDL_PollEvent(&e);
    }

    /* take the most recent frame from the decoding thread */
    int clear, have_frame;
    SDL_LockMutex(v->lock);
    clear = v->want_clear;
    v->want_clear = 0;
    have_frame = v->has_pending;
    if (have_frame) {
        av_frame_unref(v->display);
        av_frame_move_ref(v->display, v->pending);
        v->has_pending = 0;
    }
    SDL_UnlockMutex(v->lock);

    if (clear && !have_frame && v->tex) {
        SDL_DestroyTexture(v->tex);
        v->tex = NULL;
        v->tex_w = v->tex_h = 0;
        present(v);
    }
    if (have_frame) {
        show_frame(v, v->display);
        av_frame_unref(v->display);
    }
}

/* ------------------------------------------------------------------------ */
/* Setup and teardown                                                       */
/* ------------------------------------------------------------------------ */

int viewer_init(viewer_t *v, int fullscreen, int verbose)
{
    memset(v, 0, sizeof *v);
    v->verbose = verbose;

    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    v->pending = av_frame_alloc();
    v->display = av_frame_alloc();
    v->lock = SDL_CreateMutex();
    v->dec_lock = SDL_CreateMutex();
    if (!v->pkt || !v->frame || !v->pending || !v->display || !v->lock || !v->dec_lock)
        return -1;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    v->wake_event = SDL_RegisterEvents(1);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    v->win = SDL_CreateWindow("Glint", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 640,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!v->win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_ACCELERATED);
    if (!v->ren)
        v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_SOFTWARE);
    if (!v->ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    if (fullscreen)
        toggle_fullscreen(v);
    present(v);
    return 0;
}

void viewer_free(viewer_t *v)
{
    if (v->tex) SDL_DestroyTexture(v->tex);
    if (v->ren) SDL_DestroyRenderer(v->ren);
    if (v->win) SDL_DestroyWindow(v->win);
    avcodec_free_context(&v->dec);
    av_packet_free(&v->pkt);
    av_frame_free(&v->frame);
    av_frame_free(&v->pending);
    av_frame_free(&v->display);
    sws_freeContext(v->sws);
    if (v->lock) SDL_DestroyMutex(v->lock);
    if (v->dec_lock) SDL_DestroyMutex(v->dec_lock);
    memset(v, 0, sizeof *v);
}
