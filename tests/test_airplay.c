/*
 * Receiver test without an iPad: calls the same callbacks UxPlay's AirPlay
 * library would call, video included from a separate network thread.
 * Run it with "make test", which prepares the input files land.h264 and sine.aac.
 */
#include <time.h>
#include "airplay.c"   /* included directly so the static callbacks can be called */

volatile sig_atomic_t g_running = 1;
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int failures = 0;
static void check(const char *name, int ok)
{
    printf("  %s %s\n", ok ? "OK  " : "FAIL", name);
    if (!ok)
        failures++;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)size);
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    *n = (size_t)size;
    return d;
}

/* Splits an Annex B file into "AirPlay packets": one frame per packet, with SPS+PPS
 * in front of the first one and 4-byte start codes, as UxPlay's library delivers them */
typedef struct { uint8_t *d; int len, nals; } packet_t;
static int make_packets(const uint8_t *d, size_t n, packet_t *out, int max)
{
    size_t starts[1024];
    int k = 0;
    for (size_t i = 0; i + 3 < n && k < 1024; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { starts[k++] = i + 3; i += 2; }

    int np = 0, cur_len = 0, cur_nals = 0;
    uint8_t *cur = NULL;
    for (int j = 0; j < k; j++) {
        size_t s = starts[j], e = (j + 1 < k) ? starts[j + 1] - 3 : n;
        while (e > s && d[e - 1] == 0) e--;
        cur = realloc(cur, (size_t)cur_len + 4 + (e - s));
        memcpy(cur + cur_len, "\0\0\0\1", 4);
        memcpy(cur + cur_len + 4, d + s, e - s);
        cur_len += 4 + (int)(e - s);
        cur_nals++;
        int type = d[s] & 0x1f;
        if ((type == 1 || type == 5) && np < max) {         /* a slice ends the frame */
            out[np++] = (packet_t){ cur, cur_len, cur_nals };
            cur = NULL;
            cur_len = cur_nals = 0;
        }
    }
    free(cur);
    return np;
}

typedef struct { receiver_t *rx; packet_t *p; int n; volatile int done; } net_arg_t;

/* Simulates the library's "mirror" network thread */
static int net_thread(void *arg)
{
    net_arg_t *a = arg;
    cb_mirror_video_running(a->rx, true);
    cb_video_set_codec(a->rx, VIDEO_CODEC_H264);
    for (int i = 0; i < a->n; i++) {
        video_decode_struct vd = { .is_h265 = false, .nal_count = a->p[i].nals,
                                   .data = a->p[i].d, .data_len = a->p[i].len };
        cb_video_process(a->rx, NULL, &vd);
        if (i == 5) {                      /* a packet the library could not decrypt */
            uint8_t bad[64];
            memset(bad, 0xAB, sizeof bad);
            bad[0] = 1;
            video_decode_struct b = { .data = bad, .data_len = sizeof bad };
            cb_video_process(a->rx, NULL, &b);
        }
        SDL_Delay(4);
    }
    a->done = 1;
    return 0;
}

/* Peak amplitude of the last audio frame sent to the sound card */
static float peak(const receiver_t *rx, size_t n)
{
    float m = 0;
    for (size_t i = 0; i < n; i++) {
        float x = fabsf(rx->abuf[i]);
        if (x > m) m = x;
    }
    return m;
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);   /* no real window */
    setenv("SDL_AUDIODRIVER", "dummy", 1);   /* no real sound card */
    options_t o = { 0 };
    receiver_t rx;
    viewer_t v;
    memset(&rx, 0, sizeof rx);
    memset(&v, 0, sizeof v);
    rx.v = &v;
    rx.o = &o;
    rx.act = -1;
    SDL_AtomicSet(&rx.gain_milli, 1000);
    rx.apkt = av_packet_alloc();
    rx.aframe = av_frame_alloc();
    if (viewer_init(&v, 0, 0) != 0 || SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("setup failed: %s\n", SDL_GetError());
        return 1;
    }

    printf("1) Video from a network thread\n");
    size_t n;
    uint8_t *raw = read_file("land.h264", &n);
    packet_t packets[64];
    int np = make_packets(raw, n, packets, 64);
    printf("  (an \"H.265 not supported\" message is expected here)\n");
    check("rejects H.265", cb_video_set_codec(&rx, VIDEO_CODEC_H265) < 0);
    net_arg_t a = { &rx, packets, np, 0 };
    SDL_Thread *t = SDL_CreateThread(net_thread, "net", &a);
    uint64_t end = now_ns() + 5000000000ull;
    while (!a.done && now_ns() < end)
        viewer_poll(&v, 10);
    for (int i = 0; i < 10; i++)
        viewer_poll(&v, 5);
    SDL_WaitThread(t, NULL);
    printf("  (%d packets, %lu decoded, %lu shown)\n", np, v.decoded, v.shown);
    check("every frame decoded (undecryptable packet dropped)", v.decoded == (unsigned long)np);
    check("frames shown in the window", v.shown > 0 && v.shown <= v.decoded);
    check("640x480 texture", v.tex_w == 640 && v.tex_h == 480);
    check("mirroring marked as active", SDL_AtomicGet(&rx.mirroring) == 1);
    cb_mirror_video_running(&rx, false);
    viewer_poll(&v, 5);
    check("mirroring stopped: window cleared", v.tex == NULL && SDL_AtomicGet(&rx.mirroring) == 0);

    printf("2) AAC-LC audio (ct=4) and volume\n");
    uint8_t *aac = read_file("sine.aac", &n);
    int frames = 0;
    size_t off = 0;
    float peak_0db = 0;
    while (off + 7 <= n && frames < 200) {
        /* strip the ADTS header: AirPlay sends raw AAC frames */
        int protection_absent = aac[off + 1] & 1;
        size_t frame_len = ((size_t)(aac[off + 3] & 3) << 11) | ((size_t)aac[off + 4] << 3) | (aac[off + 5] >> 5);
        size_t header_len = protection_absent ? 7 : 9;
        audio_decode_struct ad;
        memset(&ad, 0, sizeof ad);
        ad.ct = 4;
        ad.data = aac + off + header_len;
        ad.data_len = (int)(frame_len - header_len);

        if (frames == 20)
            cb_audio_set_volume(&rx, -144.0f);           /* mute */
        if (frames == 21) {
            check("muted volume -> silence", peak(&rx, 2048) == 0.0f);
            cb_audio_set_volume(&rx, -6.0f);
        }
        cb_audio_process(&rx, NULL, &ad);
        if (frames == 10)
            peak_0db = peak(&rx, 2048);
        if (frames == 30) {
            float peak_6db = peak(&rx, 2048);
            printf("  (peak at 0 dB %.3f, at -6 dB %.3f)\n", peak_0db, peak_6db);
            check("-6 dB roughly halves the amplitude", peak_6db > peak_0db * 0.4f && peak_6db < peak_0db * 0.6f);
        }
        off += frame_len;
        frames++;
    }
    check("AAC-LC decoder opened", rx.adec != NULL && rx.act == 4);
    check("audio output opened at 44100 Hz stereo", rx.adev != 0 && rx.adev_rate == 44100 && rx.adev_ch == 2);
    check("signal present (sine wave, amplitude 1/8)", peak_0db > 0.05f);
    check("audio queued on the sound card", SDL_GetQueuedAudioSize(rx.adev) > 0);

    printf("3) AirPlay audio decoders\n");
    audio_open_decoder(&rx, 8);
    check("AAC-ELD (screen mirroring) opens", rx.adec != NULL && rx.adec->sample_rate == 44100);
    audio_open_decoder(&rx, 2);
    check("ALAC (audio only) opens", rx.adec != NULL);

    printf("4) Connections\n");
    cb_conn_init(&rx);
    cb_conn_init(&rx);
    cb_conn_destroy(&rx);
    check("connection counter", SDL_AtomicGet(&rx.open_conns) == 1);
    cb_conn_destroy(&rx);
    check("all connections closed", SDL_AtomicGet(&rx.open_conns) == 0);
    cb_conn_reset(&rx, 1);
    check("lost connection -> restart requested", SDL_AtomicGet(&rx.restart) == 1);

    if (rx.adev)
        SDL_CloseAudioDevice(rx.adev);
    avcodec_free_context(&rx.adec);
    viewer_free(&v);
    SDL_Quit();
    printf("\n%s (%d failed)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures != 0;
}
