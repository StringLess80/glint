// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * airplay.c — AirPlay receiver: the iPad screen over Wi-Fi ("Screen Mirroring").
 *
 * The PC announces itself on the local network as an AirPlay receiver
 * (mDNS/Bonjour, through avahi-daemon). When you pick it under "Screen
 * Mirroring", the iPad:
 *   1. connects to our RTSP server and pairs with it (Ed25519/X25519 keys)
 *   2. runs the FairPlay handshake to exchange the stream key
 *   3. sends the screen as AES-CTR encrypted H.264, and the audio as AAC-ELD
 *
 * The FairPlay handshake is an undocumented Apple protocol, reconstructed by
 * reverse engineering; for this whole part we use UxPlay's protocol library
 * (https://github.com/FDH2/UxPlay, lib/ folder), downloaded by "make". This
 * file connects that library to our decoder and window (viewer.c) and plays
 * the audio with FFmpeg + SDL.
 *
 * The library calls the callbacks below from its own network threads, so
 * only thread-safe functions are used here (viewer_decode, viewer_clear,
 * atomic variables, SDL's audio queue).
 */

#define _GNU_SOURCE
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>

/* UxPlay's AirPlay library */
#include "dnssd.h"
#include "logger.h"
#include "raop.h"

#include "mirror.h"
#include "viewer.h"

/* FFmpeg >= 5.1 describes channels with ch_layout */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define FRAME_CHANNELS(f) ((f)->ch_layout.nb_channels)
#else
#define FRAME_CHANNELS(f) ((f)->channels)
#endif

#define FEEDBACK_TIMEOUT_MS 15000   /* the iPad sends a "heartbeat" every 2 s */

typedef struct {
    viewer_t *v;
    const options_t *o;
    raop_t *raop;
    dnssd_t *dnssd;
    char name[128];

    SDL_atomic_t open_conns;
    SDL_atomic_t mirroring;
    SDL_atomic_t restart;          /* connection lost: restart the server */
    SDL_atomic_t last_feedback;    /* SDL_GetTicks() of the last heartbeat */

    /* audio: only used by the library's audio thread */
    int no_audio;
    int act;                       /* current compression type (ct) */
    AVCodecContext *adec;
    AVPacket *apkt;
    AVFrame *aframe;
    SDL_AudioDeviceID adev;
    int adev_rate, adev_ch;
    float *abuf;
    size_t abuf_cap;
    SDL_atomic_t gain_milli;       /* volume set on the iPad × 1000 */
} receiver_t;

/* ------------------------------------------------------------------------ */
/* Audio                                                                    */
/* ------------------------------------------------------------------------ */

/* AirPlay codec configurations (from the "codec_data" used by UxPlay) */
static const uint8_t ALAC_COOKIE[36] = {   /* ct=2: ALAC 44100 Hz, 16 bit, stereo */
    0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44,
};
static const uint8_t AAC_LC_ASC[2] = { 0x12, 0x10 };              /* ct=4: AAC-LC 44100 stereo */
static const uint8_t AAC_ELD_ASC[4] = { 0xf8, 0xe8, 0x50, 0x00 }; /* ct=8: AAC-ELD 44100 stereo, 480 samples */

static void audio_open_decoder(receiver_t *rx, int ct)
{
    avcodec_free_context(&rx->adec);
    rx->act = ct;

    enum AVCodecID id;
    const uint8_t *extra;
    size_t extra_len;
    switch (ct) {
    case 2: id = AV_CODEC_ID_ALAC; extra = ALAC_COOKIE; extra_len = sizeof ALAC_COOKIE; break;
    case 4: id = AV_CODEC_ID_AAC;  extra = AAC_LC_ASC;  extra_len = sizeof AAC_LC_ASC;  break;
    case 8: id = AV_CODEC_ID_AAC;  extra = AAC_ELD_ASC; extra_len = sizeof AAC_ELD_ASC; break;
    default:
        fprintf(stderr, "Unsupported AirPlay audio format %d: audio disabled.\n", ct);
        return;
    }

    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec || !(rx->adec = avcodec_alloc_context3(codec)))
        return;
    rx->adec->sample_rate = 44100;
    rx->adec->extradata = av_mallocz(extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (rx->adec->extradata) {
        memcpy(rx->adec->extradata, extra, extra_len);
        rx->adec->extradata_size = (int)extra_len;
    }
    if (avcodec_open2(rx->adec, codec, NULL) < 0) {
        fprintf(stderr, "Cannot open the audio decoder (format %d).\n", ct);
        avcodec_free_context(&rx->adec);
    }
}

static int audio_open_device(receiver_t *rx, int rate, int ch)
{
    if (rx->adev && rx->adev_rate == rate && rx->adev_ch == ch)
        return 0;
    if (rx->adev)
        SDL_CloseAudioDevice(rx->adev);
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_F32SYS;
    want.channels = (Uint8)ch;
    want.samples = 1024;
    rx->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* SDL converts if needed */
    if (!rx->adev) {
        fprintf(stderr, "Cannot open the audio output: %s\n", SDL_GetError());
        rx->no_audio = 1;
        return -1;
    }
    rx->adev_rate = rate;
    rx->adev_ch = ch;
    SDL_PauseAudioDevice(rx->adev, 0);
    return 0;
}

/* Converts a decoded frame to interleaved float and queues it for playback */
static void audio_output(receiver_t *rx, const AVFrame *f)
{
    int ch = FRAME_CHANNELS(f), n = f->nb_samples;
    if (ch < 1 || ch > 8 || n <= 0 || audio_open_device(rx, f->sample_rate, ch) != 0)
        return;
    size_t total = (size_t)n * (size_t)ch;
    if (total > rx->abuf_cap) {
        float *nb = realloc(rx->abuf, total * sizeof(float));
        if (!nb)
            return;
        rx->abuf = nb;
        rx->abuf_cap = total;
    }
    float g = (float)SDL_AtomicGet(&rx->gain_milli) / 1000.0f;
    float *out = rx->abuf;

    for (int c = 0; c < ch; c++) {
        for (int i = 0; i < n; i++) {
            float s;
            switch (f->format) {
            case AV_SAMPLE_FMT_FLTP: s = ((const float *)f->extended_data[c])[i]; break;
            case AV_SAMPLE_FMT_FLT:  s = ((const float *)f->data[0])[i * ch + c]; break;
            case AV_SAMPLE_FMT_S16P: s = ((const int16_t *)f->extended_data[c])[i] / 32768.0f; break;
            case AV_SAMPLE_FMT_S16:  s = ((const int16_t *)f->data[0])[i * ch + c] / 32768.0f; break;
            case AV_SAMPLE_FMT_S32P: s = (float)(((const int32_t *)f->extended_data[c])[i] / 2147483648.0); break;
            case AV_SAMPLE_FMT_S32:  s = (float)(((const int32_t *)f->data[0])[i * ch + c] / 2147483648.0); break;
            default: return;
            }
            out[i * ch + c] = s * g;
        }
    }

    /* If the queue grows (e.g. the PC fell behind), drop it so latency does not build up */
    double max_s = (rx->act == 8) ? 0.35 : 1.5;  /* screen mirroring: keep latency low */
    Uint32 bytes_per_s = (Uint32)(f->sample_rate * ch * (int)sizeof(float));
    if (SDL_GetQueuedAudioSize(rx->adev) > (Uint32)(bytes_per_s * max_s))
        SDL_ClearQueuedAudio(rx->adev);
    SDL_QueueAudio(rx->adev, out, (Uint32)(total * sizeof(float)));
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *d)
{
    receiver_t *rx = cls;
    (void)ntp;
    if (rx->no_audio || !d->data || d->data_len <= 0)
        return;
    if (d->ct != rx->act)
        audio_open_decoder(rx, d->ct);
    if (!rx->adec || av_new_packet(rx->apkt, d->data_len) < 0)
        return;
    memcpy(rx->apkt->data, d->data, (size_t)d->data_len);
    avcodec_send_packet(rx->adec, rx->apkt);
    av_packet_unref(rx->apkt);
    while (avcodec_receive_frame(rx->adec, rx->aframe) == 0) {
        audio_output(rx, rx->aframe);
        av_frame_unref(rx->aframe);
    }
}

static void cb_audio_flush(void *cls)
{
    receiver_t *rx = cls;
    if (rx->adev)
        SDL_ClearQueuedAudio(rx->adev);
}

static void cb_audio_set_volume(void *cls, float volume)
{
    /* AirPlay sends the volume in dB: from -30 (minimum) to 0 (maximum), -144 = muted */
    receiver_t *rx = cls;
    float g = (volume <= -30.0f) ? 0.0f : powf(10.0f, volume / 20.0f);
    SDL_AtomicSet(&rx->gain_milli, (int)(g * 1000.0f));
}

static double cb_audio_set_client_volume(void *cls) { (void)cls; return 0.0; }

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen,
                                bool *is_media, uint64_t *format)
{
    receiver_t *rx = cls;
    (void)spf; (void)is_media; (void)format;
    if (rx->o->verbose)
        fprintf(stderr, "[airplay] audio format ct=%d (%s)\n", *ct,
                *using_screen ? "screen mirroring" : "audio only");
}

/* ------------------------------------------------------------------------ */
/* Video                                                                    */
/* ------------------------------------------------------------------------ */

static int cb_video_set_codec(void *cls, video_codec_t codec)
{
    receiver_t *rx = cls;
    if (codec != VIDEO_CODEC_H264) {
        fprintf(stderr, "The iPad offered a video codec other than H.264: not supported.\n");
        return -1;
    }
    /* new stream: fresh decoder; SPS/PPS arrive inside the stream */
    return viewer_reset_decoder(rx->v);
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *d)
{
    receiver_t *rx = cls;
    (void)ntp;
    /* the library marks packets it could not decrypt with data[0] = 1 */
    if (!d->data || d->data_len <= 4 || d->data[0] == 1 || d->is_h265)
        return;
    viewer_decode(rx->v, d->data, (size_t)d->data_len);
}

static void cb_mirror_video_running(void *cls, bool running)
{
    receiver_t *rx = cls;
    SDL_AtomicSet(&rx->mirroring, running ? 1 : 0);
    SDL_AtomicSet(&rx->last_feedback, (int)SDL_GetTicks());
    if (running) {
        printf("Screen mirroring started.\n");
    } else {
        printf("Screen mirroring stopped.\n");
        viewer_clear(rx->v);
    }
}

static void cb_video_reset(void *cls, reset_type_t type)
{
    receiver_t *rx = cls;
    if (type == RESET_TYPE_RTP_SHUTDOWN || type == RESET_TYPE_NOHOLD ||
        type == RESET_TYPE_RTP_TO_HLS_TEARDOWN)
        viewer_clear(rx->v);
}

static void cb_video_report_size(void *cls, float *ws, float *hs, float *wd, float *hd)
{
    receiver_t *rx = cls;
    if (rx->o->verbose)
        fprintf(stderr, "[airplay] iPad screen %.0fx%.0f, video %.0fx%.0f\n", *ws, *hs, *wd, *hd);
}

static void cb_video_pause(void *cls) { (void)cls; }
static void cb_video_resume(void *cls) { (void)cls; }
static void cb_video_flush(void *cls) { (void)cls; }

/* ------------------------------------------------------------------------ */
/* Connections                                                              */
/* ------------------------------------------------------------------------ */

static void cb_conn_init(void *cls)
{
    receiver_t *rx = cls;
    SDL_AtomicIncRef(&rx->open_conns);
}

static void cb_conn_destroy(void *cls)
{
    receiver_t *rx = cls;
    if (SDL_AtomicDecRef(&rx->open_conns))       /* true when it reaches zero */
        cb_audio_flush(rx);
}

static void cb_conn_feedback(void *cls)
{
    receiver_t *rx = cls;
    SDL_AtomicSet(&rx->last_feedback, (int)SDL_GetTicks());
}

static void cb_conn_reset(void *cls, int reason)
{
    receiver_t *rx = cls;
    if (reason == 1)
        printf("Lost the connection with the iPad (network problem?).\n");
    SDL_AtomicSet(&rx->restart, 1);
    viewer_clear(rx->v);
}

static void cb_report_client_request(void *cls, char *device_id, char *model, char *name, bool *admit)
{
    (void)cls; (void)device_id;
    printf("Connection from \"%s\" (%s)\n", name ? name : "?", model ? model : "?");
    *admit = true;
}

static void cb_display_pin(void *cls, char *pin) { (void)cls; printf("PIN to enter on the iPad: %s\n", pin); }
static void cb_register_client(void *cls, const char *id, const char *pk, const char *name)
{
    (void)cls; (void)id; (void)pk; (void)name;
}
static bool cb_check_register(void *cls, const char *pk) { (void)cls; (void)pk; return true; }
static const char *cb_passwd(void *cls, int *len) { (void)cls; *len = 0; return NULL; }
static void cb_export_dacp(void *cls, const char *ar, const char *id) { (void)cls; (void)ar; (void)id; }

/* Metadata and HLS video callbacks (e.g. YouTube): not used */
static void cb_audio_set_metadata(void *cls, const void *b, int n) { (void)cls; (void)b; (void)n; }
static void cb_audio_set_coverart(void *cls, const void *b, int n) { (void)cls; (void)b; (void)n; }
static void cb_audio_stop_coverart(void *cls) { (void)cls; }
static void cb_audio_remote_control_id(void *cls, const char *a, const char *b) { (void)cls; (void)a; (void)b; }
static void cb_audio_set_progress(void *cls, uint32_t *s, uint32_t *c, uint32_t *e)
{
    (void)cls; (void)s; (void)c; (void)e;
}
static void cb_on_video_play(void *cls, const char *loc, const float pos) { (void)cls; (void)loc; (void)pos; }
static void cb_on_video_scrub(void *cls, const float pos) { (void)cls; (void)pos; }
static void cb_on_video_rate(void *cls, const float rate) { (void)cls; (void)rate; }
static void cb_on_video_stop(void *cls) { (void)cls; }
static void cb_on_video_info(void *cls, playback_info_t *info) { (void)cls; (void)info; }
static float cb_on_video_playlist_remove(void *cls) { (void)cls; return 0.0f; }

static void cb_log(void *cls, int level, const char *msg)
{
    receiver_t *rx = cls;
    if (level <= LOGGER_ERR || rx->o->verbose) {
        size_t n = strlen(msg);
        while (n > 0 && msg[n - 1] == '\n')
            n--;
        fprintf(stderr, "[airplay] %.*s\n", (int)n, msg);
    }
}

/* ------------------------------------------------------------------------ */
/* Starting the receiver                                                    */
/* ------------------------------------------------------------------------ */

/* MAC address of a network interface: AirPlay uses it as the receiver's ID */
static void get_hw_addr(unsigned char mac[6])
{
    struct ifaddrs *list, *ifa;
    int found = 0;
    if (getifaddrs(&list) == 0) {
        for (ifa = list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET ||
                (ifa->ifa_flags & IFF_LOOPBACK))
                continue;
            const struct sockaddr_ll *s = (const struct sockaddr_ll *)ifa->ifa_addr;
            if (s->sll_halen != 6 || !(s->sll_addr[0] | s->sll_addr[1] | s->sll_addr[2] |
                                       s->sll_addr[3] | s->sll_addr[4] | s->sll_addr[5]))
                continue;
            memcpy(mac, s->sll_addr, 6);
            found = 1;
            if (ifa->ifa_flags & IFF_UP)
                break;                 /* prefer an interface that is up */
        }
        freeifaddrs(list);
    }
    if (!found) {                      /* no interface: random "locally administered" address */
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f || fread(mac, 1, 6, f) != 6)
            memcpy(mac, "\x02\x11\x22\x33\x44\x55", 6);
        if (f)
            fclose(f);
        mac[0] = (unsigned char)((mac[0] & ~0x01) | 0x02);
    }
}

static int start_receiver(receiver_t *rx)
{
    unsigned char mac[6];
    char mac_str[18];
    int err = 0;
    get_hw_addr(mac);
    snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 1) data for the mDNS announcement */
    rx->dnssd = dnssd_init(rx->name, (int)strlen(rx->name), (const char *)mac, 6, &err, 0);
    if (!rx->dnssd || err) {
        fprintf(stderr, "Cannot initialize DNS-SD (error %d).\n", err);
        return -1;
    }
    dnssd_set_airplay_features(rx->dnssd, 0, 0);    /* no HLS video (YouTube & co.)      */
    dnssd_set_airplay_features(rx->dnssd, 4, 0);
    dnssd_set_airplay_features(rx->dnssd, 42, 0);   /* H.264 only, no H.265              */
    dnssd_set_airplay_features(rx->dnssd, 27, 0);   /* no "legacy pairing": the iPad skips the
                                                       pairing step and connects faster (UxPlay's default) */

    /* 2) AirPlay server */
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.cls = rx;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.conn_reset = cb_conn_reset;
    cbs.video_reset = cb_video_reset;
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_set_metadata = cb_audio_set_metadata;
    cbs.audio_set_coverart = cb_audio_set_coverart;
    cbs.audio_stop_coverart_rendering = cb_audio_stop_coverart;
    cbs.audio_remote_control_id = cb_audio_remote_control_id;
    cbs.audio_set_progress = cb_audio_set_progress;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.video_report_size = cb_video_report_size;
    cbs.mirror_video_running = cb_mirror_video_running;
    cbs.report_client_request = cb_report_client_request;
    cbs.display_pin = cb_display_pin;
    cbs.register_client = cb_register_client;
    cbs.check_register = cb_check_register;
    cbs.passwd = cb_passwd;
    cbs.export_dacp = cb_export_dacp;
    cbs.video_set_codec = cb_video_set_codec;
    cbs.on_video_play = cb_on_video_play;
    cbs.on_video_scrub = cb_on_video_scrub;
    cbs.on_video_rate = cb_on_video_rate;
    cbs.on_video_stop = cb_on_video_stop;
    cbs.on_video_acquire_playback_info = cb_on_video_info;
    cbs.on_video_playlist_remove = cb_on_video_playlist_remove;

    rx->raop = raop_init(&cbs);
    if (!rx->raop) {
        fprintf(stderr, "Cannot create the AirPlay server.\n");
        return -1;
    }
    raop_set_log_callback(rx->raop, cb_log, rx);
    raop_set_log_level(rx->raop, rx->o->verbose ? LOGGER_INFO : LOGGER_ERR);
    /* nohold = 1: another iPad/iPhone can take over from the connected one */
    if (raop_init2(rx->raop, 1, mac_str, "") != 0) {
        fprintf(stderr, "Cannot initialize the AirPlay server.\n");
        return -1;
    }
    raop_set_plist(rx->raop, "width", 1920);
    raop_set_plist(rx->raop, "height", 1080);
    raop_set_plist(rx->raop, "refreshRate", 60);
    raop_set_plist(rx->raop, "maxFPS", 60);

    if (rx->o->port) {                /* fixed ports, to open in the firewall */
        unsigned short p = (unsigned short)rx->o->port;
        unsigned short tcp[3] = { p, (unsigned short)(p + 1), (unsigned short)(p + 2) };
        unsigned short udp[3] = { p, (unsigned short)(p + 1), (unsigned short)(p + 2) };
        raop_set_tcp_ports(rx->raop, tcp);
        raop_set_udp_ports(rx->raop, udp);
    }
    unsigned short port = raop_get_port(rx->raop);
    if (raop_start_httpd(rx->raop, &port) < 0) {
        fprintf(stderr, "Cannot open network port %u (already in use?).\n", port);
        return -1;
    }
    raop_set_port(rx->raop, port);
    raop_set_dnssd(rx->raop, rx->dnssd);

    /* 3) announce on the network: from here on the PC appears under "Screen Mirroring" */
    err = dnssd_register_raop(rx->dnssd, port);
    if (!err)
        err = dnssd_register_airplay(rx->dnssd, port);
    if (err) {
        fprintf(stderr, "Cannot announce the receiver on the network (DNS-SD error %d).\n", err);
        if (err == -65537)
            fprintf(stderr, "avahi-daemon is not responding: start it with  sudo systemctl enable --now avahi-daemon\n");
        else if (err == -65548)
            fprintf(stderr, "Another AirPlay receiver with the same name is already running on this computer.\n");
        return -1;
    }
    if (rx->o->verbose)
        fprintf(stderr, "[airplay] listening on TCP port %u, ID %s\n", port, mac_str);
    return 0;
}

/* After a lost connection: close the remaining connections and reopen the server */
static void restart_receiver(receiver_t *rx)
{
    unsigned short port = raop_get_port(rx->raop);
    raop_stop_httpd(rx->raop);
    raop_remove_known_connections(rx->raop);
    if (raop_start_httpd(rx->raop, &port) < 0)
        fprintf(stderr, "Cannot reopen the AirPlay server on port %u.\n", port);
    else
        raop_set_port(rx->raop, port);
    SDL_AtomicSet(&rx->mirroring, 0);
    viewer_clear(rx->v);
}

int airplay_run(const options_t *o)
{
    receiver_t rx;
    viewer_t v;
    memset(&rx, 0, sizeof rx);
    memset(&v, 0, sizeof v);
    rx.v = &v;
    rx.o = o;
    rx.no_audio = o->no_audio;
    rx.act = -1;
    SDL_AtomicSet(&rx.gain_milli, 1000);

    if (o->name && *o->name) {
        snprintf(rx.name, sizeof rx.name, "%s", o->name);
    } else {
        char host[64] = "Linux";
        gethostname(host, sizeof host - 1);
        host[sizeof host - 1] = '\0';
        snprintf(rx.name, sizeof rx.name, "%s", host);
    }

    signal(SIGPIPE, SIG_IGN);              /* an iPad disconnecting must not kill the program */
    rx.apkt = av_packet_alloc();
    rx.aframe = av_frame_alloc();
    int rc = 1;
    if (!rx.apkt || !rx.aframe || viewer_init(&v, o->fullscreen, o->verbose) != 0)
        goto out;
    if (!rx.no_audio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Audio not available (%s): continuing without it.\n", SDL_GetError());
        rx.no_audio = 1;
    }
    if (start_receiver(&rx) != 0)
        goto out;

    printf("Ready. On the iPad open Control Center → Screen Mirroring → \"%s\".\n"
           "(F = fullscreen, Q = quit)\n", rx.name);

    while (g_running) {
        viewer_poll(&v, 20);
        if (SDL_AtomicGet(&rx.restart)) {
            SDL_AtomicSet(&rx.restart, 0);
            restart_receiver(&rx);
        }
        /* the iPad disappeared without closing the connection (e.g. Wi-Fi turned off) */
        Uint32 since = SDL_GetTicks() - (Uint32)SDL_AtomicGet(&rx.last_feedback);
        if (SDL_AtomicGet(&rx.mirroring) && SDL_AtomicGet(&rx.open_conns) > 0 && since > FEEDBACK_TIMEOUT_MS) {
            printf("No signal from the iPad for %d seconds: restarting the receiver.\n",
                   FEEDBACK_TIMEOUT_MS / 1000);
            restart_receiver(&rx);
        }
    }
    rc = 0;

out:
    if (rx.dnssd) {
        dnssd_unregister_raop(rx.dnssd);
        dnssd_unregister_airplay(rx.dnssd);
    }
    if (rx.raop)
        raop_destroy(rx.raop);              /* stops all network threads */
    if (rx.dnssd)
        dnssd_destroy(rx.dnssd);
    if (rx.adev)
        SDL_CloseAudioDevice(rx.adev);
    avcodec_free_context(&rx.adec);
    av_packet_free(&rx.apkt);
    av_frame_free(&rx.aframe);
    free(rx.abuf);
    viewer_free(&v);
    return rc;
}
