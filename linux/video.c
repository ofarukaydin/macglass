// Decoding. Full frames come as ProRes or HEVC 4:4:4, whichever the client asked the Mac for:
//   - ProRes on the RX 9070 XT through FFmpeg's Vulkan decoder (~150 fps at 5120x2160 including
//     the copy back to system memory, about half a CPU core), when the card is the host's;
//   - HEVC on the Intel iGPU's media engine through VAAPI while the Windows VM has the 9070 XT
//     and the P-cores. Those frames stay on the iGPU and are drawn from there (render.c);
//   - ProRes on the CPU as the fallback when neither GPU path is available.
// Patches are always small ProRes pictures, decoded on the CPU.
#define _GNU_SOURCE // gettid
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../protocol.h"
#include "macglass.h"

struct queue packets = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};
struct queue decoded = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};
atomic_bool use_dgpu;
atomic_uint video_generation;

void item_free(struct item *it)
{
    free(it->data);
    av_frame_free(&it->frame);
    free(it);
}

// Appends; a full frame drops everything still queued that may be dropped. Returns how many items
// were dropped.
unsigned queue_push(struct queue *q, struct item *it)
{
    unsigned dropped = 0;
    it->next = NULL;
    pthread_mutex_lock(&q->lock);
    if (!it->patch) {
        struct item **link = &q->head;
        q->tail = NULL;
        while (*link) {
            struct item *cur = *link;
            if (cur->keep) {
                q->tail = cur;
                link = &cur->next;
            } else {
                *link = cur->next;
                item_free(cur);
                dropped++;
            }
        }
    }
    if (q->tail)
        q->tail->next = it;
    else
        q->head = it;
    q->tail = it;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->lock);
    return dropped;
}

// Returns NULL when empty, or when waiting, once the queue is shut down or woken (video_wake).
struct item *queue_pop(struct queue *q, bool wait)
{
    pthread_mutex_lock(&q->lock);
    while (wait && !q->head && !q->quit && !q->wake)
        pthread_cond_wait(&q->ready, &q->lock);
    q->wake = false;
    struct item *it = q->quit ? NULL : q->head;
    if (it) {
        q->head = it->next;
        if (!q->head)
            q->tail = NULL;
    }
    pthread_mutex_unlock(&q->lock);
    return it;
}

void queue_quit(struct queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->quit = true;
    pthread_cond_broadcast(&q->ready);
    pthread_mutex_unlock(&q->lock);
}

void queue_clear(struct queue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->head) {
        struct item *it = q->head;
        q->head = it->next;
        item_free(it);
    }
    q->tail = NULL;
    pthread_mutex_unlock(&q->lock);
}

void video_reset(void)
{
    video_generation++;
    queue_clear(&packets);
    video_wake();
}

void video_wake(void)
{
    pthread_mutex_lock(&packets.lock);
    packets.wake = true;
    pthread_cond_signal(&packets.ready);
    pthread_mutex_unlock(&packets.lock);
}

static const char *decoder_name = "none";
static pthread_mutex_t name_lock = PTHREAD_MUTEX_INITIALIZER;

const char *video_decoder_name(void)
{
    pthread_mutex_lock(&name_lock);
    const char *name = decoder_name;
    pthread_mutex_unlock(&name_lock);
    return name;
}

static void set_decoder_name(const char *name)
{
    pthread_mutex_lock(&name_lock);
    decoder_name = name;
    pthread_mutex_unlock(&name_lock);
}

// Uses the context's hardware device when the stream allows it, otherwise decodes in software.
static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    enum AVPixelFormat hw = AV_PIX_FMT_NONE;
    if (ctx->hw_device_ctx) {
        enum AVHWDeviceType type = ((AVHWDeviceContext *)ctx->hw_device_ctx->data)->type;
        hw = type == AV_HWDEVICE_TYPE_VULKAN ? AV_PIX_FMT_VULKAN : AV_PIX_FMT_VAAPI;
    }
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == hw)
            return *p;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    return AV_PIX_FMT_NONE;
}

// Environment overrides for other machines: the 9070 XT is picked by Vulkan device name, the iGPU
// by its render node.
static AVBufferRef *open_device(enum AVHWDeviceType type)
{
    const char *name = type == AV_HWDEVICE_TYPE_VULKAN
                           ? (getenv("MACGLASS_DGPU_VULKAN") ? getenv("MACGLASS_DGPU_VULKAN") : "Radeon")
                           : (getenv("MACGLASS_IGPU_NODE") ? getenv("MACGLASS_IGPU_NODE")
                                                           : "/dev/dri/by-path/pci-0000:00:02.0-render");
    AVBufferRef *dev = NULL;
    int err = av_hwdevice_ctx_create(&dev, type, name, NULL, 0);
    if (err < 0)
        fprintf(stderr, "cannot open %s device \"%s\": %s\n", av_hwdevice_get_type_name(type), name, av_err2str(err));
    return dev;
}

static struct {
    AVCodecContext *prores, *hevc, *patch;
    uint32_t prores_codec; // ProRes flavour the full-frame context was opened for
    bool prores_on_dgpu;
    AVBufferRef *dgpu, *igpu;
    Uint64 last_keyframe_request;
} dec;

static AVCodecContext *open_decoder(enum AVCodecID id, uint32_t fourcc, AVBufferRef *device)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!ctx)
        return NULL;
    ctx->codec_tag = __builtin_bswap32(fourcc); // libavcodec stores tags byte-reversed
    if (device) {
        ctx->hw_device_ctx = av_buffer_ref(device);
        ctx->get_format = pick_format;
        ctx->extra_hw_frames = 8; // pictures waiting in the queue or still being drawn
    } else {
        ctx->thread_type = FF_THREAD_SLICE; // frame threading adds a frame of latency per thread
        ctx->thread_count = 0;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0)
        avcodec_free_context(&ctx);
    return ctx;
}

// Closes every handle to the 9070 XT (decoder, frame pools, Vulkan device and instance).
static void release_dgpu(void)
{
    if (dec.prores_on_dgpu)
        avcodec_free_context(&dec.prores);
    dec.prores_on_dgpu = false;
    if (dec.dgpu) {
        av_buffer_unref(&dec.dgpu);
        fprintf(stderr, "released the discrete GPU\n");
    }
}

static AVCodecContext *full_frame_decoder(uint32_t codec)
{
    if (codec == MG_CODEC_HEVC) {
        if (!dec.hevc) {
            if (!dec.igpu)
                dec.igpu = open_device(AV_HWDEVICE_TYPE_VAAPI);
            dec.hevc = open_decoder(AV_CODEC_ID_HEVC, codec, dec.igpu);
        }
        set_decoder_name(dec.igpu ? "HEVC 4:4:4 on the Intel iGPU" : "HEVC 4:4:4 on the CPU");
        return dec.hevc;
    }
    bool want_dgpu = use_dgpu;
    if (dec.prores && (dec.prores_codec != codec || dec.prores_on_dgpu != want_dgpu))
        avcodec_free_context(&dec.prores);
    if (!want_dgpu)
        release_dgpu();
    if (!dec.prores) {
        if (want_dgpu && !dec.dgpu)
            dec.dgpu = open_device(AV_HWDEVICE_TYPE_VULKAN);
        dec.prores = open_decoder(AV_CODEC_ID_PRORES, codec, want_dgpu ? dec.dgpu : NULL);
        dec.prores_codec = codec;
        dec.prores_on_dgpu = want_dgpu && dec.dgpu;
    }
    set_decoder_name(dec.prores_on_dgpu ? "ProRes on the RX 9070 XT" : "ProRes on the CPU");
    return dec.prores;
}

static void request_keyframe(void)
{
    Uint64 now = SDL_GetTicksNS();
    if (now - dec.last_keyframe_request > 500000000ull) {
        send_msg(MG_KEYFRAME, NULL, 0);
        dec.last_keyframe_request = now;
    }
}

static void decode_failed(AVCodecContext *ctx, uint32_t codec, int err)
{
    if (ctx == dec.prores && dec.prores_on_dgpu) {
        fprintf(stderr, "ProRes decode on the discrete GPU failed (%s); using the CPU\n", av_err2str(err));
        use_dgpu = false;
        release_dgpu();
        return;
    }
    if (codec == MG_CODEC_HEVC)
        request_keyframe(); // a lost or bad frame breaks the chain until the next keyframe
    fprintf(stderr, "decode failed: %s\n", av_err2str(err));
}

void *decode_thread(void *arg)
{
    (void)arg;
    // CPU decoding of a 5K frame can keep a dozen cores busy. Run it (and the slice workers
    // libavcodec creates from this thread, which inherit this) below everything else so audio
    // never starves.
    setpriority(PRIO_PROCESS, gettid(), 10);
    if (getenv("MACGLASS_DEBUG"))
        av_log_set_level(AV_LOG_DEBUG);
    AVPacket *pkt = av_packet_alloc();

    while (running) {
        struct item *it = queue_pop(&packets, true);
        if (!use_dgpu)
            release_dgpu(); // promptly, even while no ProRes frames arrive
        if (!it)
            continue; // woken up or shutting down

        // A new connection: nothing from the old one may serve as a reference any more.
        static unsigned generation;
        if (it->generation != generation) {
            generation = it->generation;
            AVCodecContext *contexts[] = {dec.hevc, dec.prores, dec.patch};
            for (int i = 0; i < 3; i++)
                if (contexts[i])
                    avcodec_flush_buffers(contexts[i]);
        }

        AVCodecContext *ctx;
        if (it->patch) {
            if (!dec.patch)
                dec.patch = open_decoder(AV_CODEC_ID_PRORES, it->codec, NULL);
            ctx = dec.patch;
        } else {
            ctx = full_frame_decoder(it->codec);
        }
        if (!ctx) {
            fprintf(stderr, "cannot open a decoder for this picture\n");
            item_free(it);
            continue;
        }

        Uint64 t0 = SDL_GetTicksNS();
        pkt->data = it->data;
        pkt->size = it->size;
        int err = avcodec_send_packet(ctx, pkt);
        free(it->data);
        it->data = NULL;
        if (err < 0) {
            decode_failed(ctx, it->codec, err);
            item_free(it);
            continue;
        }
        // Hardware decoders may hand a frame back a call later, so take whatever is ready. Each
        // frame becomes its own item; only the first can reuse this packet's.
        for (;;) {
            AVFrame *frame = av_frame_alloc();
            err = avcodec_receive_frame(ctx, frame);
            if (err < 0) {
                av_frame_free(&frame);
                if (err != AVERROR(EAGAIN))
                    decode_failed(ctx, it ? it->codec : 0, err);
                break;
            }
            // 9070 XT frames have to cross to the iGPU anyway, so they come back to system
            // memory here. Intel VAAPI frames stay on the iGPU; render.c draws them in place.
            if (frame->format == AV_PIX_FMT_VULKAN) {
                AVFrame *sw = av_frame_alloc();
                if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
                    av_frame_free(&sw);
                    av_frame_free(&frame);
                    continue;
                }
                av_frame_copy_props(sw, frame);
                av_frame_free(&frame);
                frame = sw;
            }
            struct item *out = it;
            if (!out) {
                out = calloc(1, sizeof *out);
                out->patch = ctx == dec.patch;
            }
            it = NULL;
            out->frame = frame;
            out->keep = false; // decoded pictures can be superseded
            stats.decode_ns += SDL_GetTicksNS() - t0;
            if (out->patch)
                stats.patches++;
            else
                stats.frames++;
            stats.replaced += queue_push(&decoded, out);
            if (!atomic_exchange(&decoded_pending, true))
                SDL_PushEvent(&(SDL_Event){.type = frame_event});
        }
        if (it)
            item_free(it); // its frame comes out with a later packet
    }
    av_packet_free(&pkt);
    avcodec_free_context(&dec.patch);
    avcodec_free_context(&dec.hevc);
    release_dgpu();
    avcodec_free_context(&dec.prores);
    av_buffer_unref(&dec.igpu);
    return NULL;
}
