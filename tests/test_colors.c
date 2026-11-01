/*
 * Color test: decodes H.264 streams with known colors and measures the pixels
 * drawn in the window.
 *
 *   test_colors --pattern FILE.ppm    writes the reference image (6 colored blocks)
 *   test_colors STREAM.h264...        decodes each stream and compares it with the original colors
 *
 * "make test" encodes the streams with ffmpeg in three variants: full-range
 * BT.709 (the one Apple uses), limited-range BT.709 and limited-range BT.601.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mirror.h"
#include "viewer.h"

volatile sig_atomic_t g_running = 1;
uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define W 384
#define H 256
#define TOLERANCE 4                 /* largest accepted error, out of 255 */

/* Typical colors of an iOS interface */
static const int COLORS[6][3] = { {0,122,255}, {255,59,48}, {52,199,89}, {255,204,0}, {242,242,247}, {28,28,30} };
static const char *NAMES[6] = { "blue", "red", "green", "yellow", "white", "dark gray" };

static int write_pattern(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int *c = COLORS[(y / 128) * 3 + x / 128];
            fputc(c[0], f);
            fputc(c[1], f);
            fputc(c[2], f);
        }
    fclose(f);
    return 0;
}

/* Feeds the stream to the decoder one frame at a time, like AirPlay does: it splits
 * before each SPS/AUD/slice once the current chunk already holds a slice.
 * Start codes can be 4 bytes (00 00 00 01) or 3 bytes (00 00 01) long. */
static void feed(viewer_t *v, const uint8_t *buf, size_t n)
{
    size_t start = 0;
    int has_slice = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (!(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1))
            continue;
        size_t sc = (i > 0 && buf[i - 1] == 0) ? i - 1 : i;   /* where the start code begins */
        int type = buf[i + 3] & 0x1f;
        if ((type == 7 || type == 9 || type == 1 || type == 5) && has_slice) {
            viewer_decode(v, buf + start, sc - start);
            start = sc;
            has_slice = 0;
        }
        if (type == 1 || type == 5)
            has_slice = 1;
        i += 2;
    }
    viewer_decode(v, buf + start, n - start);
}

static int check_stream(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return -1; }
    static uint8_t buf[4 << 20];
    size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);

    viewer_t v;
    if (viewer_init(&v, 0, 0) != 0 || viewer_reset_decoder(&v) != 0)
        return -1;
    feed(&v, buf, n);
    for (int i = 0; i < 5; i++)
        viewer_poll(&v, 5);
    if (!v.tex) {
        printf("  no frame decoded\n");
        viewer_free(&v);
        return 255;
    }

    /* draw the frame over the whole window and read the pixels back */
    int ow, oh;
    SDL_GetRendererOutputSize(v.ren, &ow, &oh);
    SDL_SetRenderDrawColor(v.ren, 0, 0, 0, 255);
    SDL_RenderClear(v.ren);
    SDL_Rect dst = { 0, 0, ow, oh };
    SDL_RenderCopy(v.ren, v.tex, NULL, &dst);
    uint8_t *px = malloc((size_t)ow * oh * 4);
    SDL_RenderReadPixels(v.ren, NULL, SDL_PIXELFORMAT_RGBA32, px, ow * 4);

    int worst = 0;
    for (int k = 0; k < 6; k++) {
        int cx = (int)((k % 3 * 128 + 64) * (double)ow / W);
        int cy = (int)((k / 3 * 128 + 64) * (double)oh / H);
        const uint8_t *p = px + ((size_t)cy * ow + cx) * 4;
        int e = 0;
        for (int c = 0; c < 3; c++) {
            int d = abs(p[c] - COLORS[k][c]);
            if (d > e) e = d;
        }
        if (e > worst) worst = e;
        printf("  %-10s expected (%3d,%3d,%3d)  drawn (%3d,%3d,%3d)  error %d\n", NAMES[k],
               COLORS[k][0], COLORS[k][1], COLORS[k][2], p[0], p[1], p[2], e);
    }
    free(px);
    viewer_free(&v);
    return worst;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--pattern") == 0)
        return write_pattern(argv[2]);
    if (argc < 2) {
        fprintf(stderr, "usage: %s --pattern FILE.ppm | STREAM.h264...\n", argv[0]);
        return 2;
    }

    setenv("SDL_VIDEODRIVER", "dummy", 1);    /* no real window */
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        printf("%s\n", argv[i]);
        int worst = check_stream(argv[i]);
        if (worst < 0)
            return 1;
        printf("  => %s (largest error %d out of 255)\n", worst <= TOLERANCE ? "OK" : "WRONG COLORS", worst);
        if (worst > TOLERANCE)
            failures++;
    }
    SDL_Quit();
    printf("\n%s\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED");
    return failures != 0;
}
