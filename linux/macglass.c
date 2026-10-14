// MacGlass Linux client: shows the Mac's virtual display in a window.
//
// A network thread finds the Mac on the Thunderbolt link, receives pictures over TCP, and keeps
// reconnecting whenever the connection ends; the window stays open and shows why it's waiting.
// video.c decodes the pictures, render.c draws them and overlay.c the on-screen UI. Resizing the
// window asks the Mac to resize its virtual display to match, pixel for pixel. This file also
// forwards input, plays audio and syncs the clipboard and cursor.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <libavcodec/avcodec.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../protocol.h"
#include "macglass.h"

// Wait this long after the last window size change before asking the Mac to resize.
#define RESIZE_DEBOUNCE_NS (200 * 1000000ull)

static int sock = -1; // the current connection, -1 while waiting; guarded by send_lock
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
Uint32 frame_event;
atomic_bool decoded_pending;
atomic_bool running = true;
struct stats stats;

// Clipboard content received from the Mac, handed from the network thread to the main thread.
static struct {
    pthread_mutex_t lock;
    uint8_t kind;
    uint8_t *data;
    size_t len;
} clip_in = {.lock = PTHREAD_MUTEX_INITIALIZER};
static Uint32 clip_event;

// Latest cursor shape from the Mac (an MG_CURSOR payload), handed to the main thread.
static struct {
    pthread_mutex_t lock;
    uint8_t *data;
    size_t len;
} cursor_in = {.lock = PTHREAD_MUTEX_INITIALIZER};
static Uint32 cursor_event;

// Mac system audio, played as it arrives. What causes clicks is the buffer running dry, so the
// controller steers its lowest point: over each window it measures the least audio that was
// buffered when a packet arrived and nudges the playback rate (inaudibly, at most 0.5%) to bring
// that low point to AUDIO_RESERVE_MS. Latency is then only as large as the actual delivery jitter
// requires, and the integral term absorbs the clock difference between machines (measured
// +0.066%). The Mac sends 20 ms packets whose timing wanders over a ~27 ms range (a few seconds
// slightly slow, then a catch-up burst), so the window spans a whole cycle; a shorter one makes the
// controller chase that pattern and overshoot, and playback starts with enough buffered to cover it.
#define AUDIO_START_MS 60.0   // buffered before playback starts
#define AUDIO_RESERVE_MS 15.0 // what the buffer's low point is steered to
#define AUDIO_WINDOW_MS 8000
#define AUDIO_MAX_RATE_CHANGE 0.005
static SDL_AudioStream *audio;
static int audio_rate, audio_channels;
static _Atomic float audio_peak;          // loudest sample since the last stats line
static _Atomic float audio_low_ms = 1e9f; // least audio buffered since then; near 0 means clicks
static _Atomic float audio_gap_ms;        // longest wait between audio packets since then (normally ~20)
static atomic_uint audio_underruns;       // times SDL had to play silence since then (audible clicks)
static Uint64 audio_last_ns;
static bool audio_playing;      // playback starts once AUDIO_START_MS is buffered
static Uint64 audio_window_start;
static double audio_window_low; // least buffered at a packet's arrival in the current window
static double audio_drift;      // learned clock difference (integral term), as a rate change

// SDL asks for more than is queued only when the buffer ran dry and it must play silence.
static void SDLCALL audio_requested(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata;
    (void)stream;
    (void)total;
    if (additional > 0 && audio_playing)
        audio_underruns++;
}

static void play_audio(const uint8_t *p, uint32_t len)
{
    uint32_t v[2];
    memcpy(v, p, sizeof v);
    int rate = le32toh(v[0]), channels = le32toh(v[1]);
    if (rate <= 0 || channels <= 0 || channels > 8)
        return;
    if (!audio || rate != audio_rate || channels != audio_channels) {
        SDL_DestroyAudioStream(audio);
        SDL_AudioSpec spec = {SDL_AUDIO_F32LE, channels, rate};
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!audio) {
            fprintf(stderr, "cannot open audio output: %s\n", SDL_GetError());
            return;
        }
        SDL_SetAudioStreamGetCallback(audio, audio_requested, NULL);
        audio_playing = false;
        audio_rate = rate;
        audio_channels = channels;
    }
    double bytes_per_ms = rate * channels * 4 / 1000.0;
    Uint64 now = SDL_GetTicksNS();
    if (audio_last_ns && (now - audio_last_ns) / 1e6f > audio_gap_ms)
        audio_gap_ms = (now - audio_last_ns) / 1e6f;
    audio_last_ns = now;
    double before_ms = SDL_GetAudioStreamQueued(audio) / bytes_per_ms;
    if (audio_playing) {
        if (before_ms < audio_low_ms)
            audio_low_ms = (float)before_ms;
        audio_window_low = SDL_min(audio_window_low, before_ms);
    }
    SDL_PutAudioStreamData(audio, p + 8, len - 8);
    float peak = audio_peak;
    for (const uint8_t *q = p + 8; q + 4 <= p + len; q += 4) {
        float sample;
        memcpy(&sample, q, 4);
        peak = SDL_max(peak, SDL_fabsf(sample));
    }
    audio_peak = peak;

    double queued_ms = SDL_GetAudioStreamQueued(audio) / bytes_per_ms;
    if (queued_ms > 300) {
        SDL_ClearAudioStream(audio); // fell far behind (e.g. after a stall); start over
        SDL_PauseAudioStreamDevice(audio);
        audio_playing = false;
        return;
    }
    if (!audio_playing) {
        if (queued_ms < AUDIO_START_MS)
            return;
        SDL_ResumeAudioStreamDevice(audio);
        audio_playing = true;
        audio_window_start = now;
        audio_window_low = 1e9;
        return;
    }
    if (now - audio_window_start < AUDIO_WINDOW_MS * 1000000ull)
        return;
    // A low point above the reserve means more latency than needed: play slightly faster to drain
    // it over the next window. Below the reserve: slightly slower to build it back up.
    double error = (audio_window_low - AUDIO_RESERVE_MS) / AUDIO_WINDOW_MS; // fraction of the window
    audio_drift = SDL_clamp(audio_drift + error * 0.2, -AUDIO_MAX_RATE_CHANGE, AUDIO_MAX_RATE_CHANGE);
    double change = SDL_clamp(audio_drift + error * 0.5, -AUDIO_MAX_RATE_CHANGE, AUDIO_MAX_RATE_CHANGE);
    SDL_SetAudioStreamFrequencyRatio(audio, 1.0f + (float)change);
    audio_window_start = now;
    audio_window_low = 1e9;
}

// The connection ended: drop what's buffered and start the controller over with the next one.
static void stop_audio(void)
{
    if (audio) {
        SDL_ClearAudioStream(audio);
        SDL_PauseAudioStreamDevice(audio);
        SDL_SetAudioStreamFrequencyRatio(audio, 1.0f);
    }
    audio_playing = false;
    audio_last_ns = 0;
    audio_drift = 0;
}

static bool read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
}

static bool send_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
}

bool send_msg(uint32_t type, const void *payload, uint32_t len)
{
    uint32_t hdr[2] = {htole32(type), htole32(len)};
    pthread_mutex_lock(&send_lock);
    bool ok = sock >= 0 && send_full(sock, hdr, sizeof hdr) && (!len || send_full(sock, payload, len));
    pthread_mutex_unlock(&send_lock);
    return ok;
}

// --- Connection ---
// Shared with the main thread, which shows it: whether a Mac is connected, its name, and while
// waiting, why.
static struct {
    pthread_mutex_t lock;
    bool connected;
    char name[128];
    char reason[192];
} conn = {.lock = PTHREAD_MUTEX_INITIALIZER};
static Uint32 conn_event;
static const char *host_arg; // explicit address, or NULL to discover the Mac
static const char *port_arg = MG_DEFAULT_PORT;
static atomic_bool reconnect_now; // skip the wait before the next attempt
static volatile sig_atomic_t snapshot_requested;
static void on_sigusr1(int sig) { (void)sig; snapshot_requested = 1; }
static bool version_mismatch;      // the last attempt found a Mac speaking another protocol

static void set_conn(bool connected, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_conn(bool connected, const char *name, const char *fmt, ...)
{
    pthread_mutex_lock(&conn.lock);
    bool changed = connected != conn.connected;
    conn.connected = connected;
    if (name)
        snprintf(conn.name, sizeof conn.name, "%s", name);
    char reason[sizeof conn.reason] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(reason, sizeof reason, fmt, ap);
        va_end(ap);
    }
    changed |= strcmp(reason, conn.reason) != 0;
    if (strcmp(reason, conn.reason) != 0 && *reason)
        fprintf(stderr, "waiting: %s\n", reason);
    memcpy(conn.reason, reason, sizeof reason);
    pthread_mutex_unlock(&conn.lock);
    if (changed)
        SDL_PushEvent(&(SDL_Event){.type = conn_event});
}

// Asks who on the Thunderbolt link (any IPv6 interface, if there is no Thunderbolt one up) is a
// MacGlass server. macOS lets an app receive multicast only with the Local Network permission, so
// the query goes out by unicast: an ICMPv6 echo to the all-nodes address, which the Mac's kernel
// answers regardless, lists the neighbours, and each gets the query (plus one multicast copy for when
// the permission is there). The Mac answers from the address to connect to.
static bool discover(struct sockaddr_in6 *mac, char *name, size_t name_size)
{
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0) {
        set_conn(false, NULL, "cannot list network interfaces: %s", strerror(errno));
        return false;
    }
    unsigned indices[32], n = 0, thunderbolt = 0;
    for (int pass = 0; pass < 2 && !n; pass++) {
        for (struct ifaddrs *i = ifs; i && n < 32; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET6 || !(i->ifa_flags & IFF_UP) ||
                (i->ifa_flags & IFF_LOOPBACK))
                continue;
            bool is_tb = strncmp(i->ifa_name, "thunderbolt", 11) == 0;
            if (pass == 0 && !is_tb)
                continue;
            unsigned index = if_nametoindex(i->ifa_name);
            bool seen = false;
            for (unsigned k = 0; k < n; k++)
                seen |= indices[k] == index;
            if (!seen) {
                indices[n++] = index;
                thunderbolt += is_tb;
            }
        }
    }

    int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int icmp = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMPV6);
    if (fd < 0) {
        freeifaddrs(ifs);
        if (icmp >= 0)
            close(icmp);
        return false;
    }
    struct sockaddr_in6 all = {.sin6_family = AF_INET6, .sin6_port = htons(atoi(port_arg))};
    inet_pton(AF_INET6, "ff02::1", &all.sin6_addr);
    const uint8_t echo[8] = {128}; // ICMPv6 echo request; the kernel fills in the rest
    for (unsigned k = 0; k < n; k++) {
        all.sin6_scope_id = indices[k];
        sendto(fd, MG_DISCOVER_QUERY, strlen(MG_DISCOVER_QUERY), 0, (struct sockaddr *)&all, sizeof all);
        if (icmp >= 0) {
            struct sockaddr_in6 to = all;
            to.sin6_port = 0;
            sendto(icmp, echo, sizeof echo, 0, (struct sockaddr *)&to, sizeof to);
        }
    }
    struct pollfd echoes = {.fd = icmp, .events = POLLIN};
    while (icmp >= 0 && poll(&echoes, 1, 200) > 0) {
        uint8_t reply[64];
        struct sockaddr_in6 from;
        socklen_t len = sizeof from;
        if (recvfrom(icmp, reply, sizeof reply, 0, (struct sockaddr *)&from, &len) <= 0)
            break;
        bool ours = false; // our own machine answers too
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET6 &&
                !memcmp(&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, &from.sin6_addr, sizeof from.sin6_addr))
                ours = true;
        if (ours)
            continue;
        from.sin6_port = htons(atoi(port_arg));
        sendto(fd, MG_DISCOVER_QUERY, strlen(MG_DISCOVER_QUERY), 0, (struct sockaddr *)&from, sizeof from);
    }
    if (icmp >= 0)
        close(icmp);
    freeifaddrs(ifs);

    bool found = false;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    while (n && !found && poll(&pfd, 1, 500) > 0) {
        char reply[256];
        socklen_t len = sizeof *mac;
        ssize_t got = recvfrom(fd, reply, sizeof reply - 1, 0, (struct sockaddr *)mac, &len);
        if (got <= (ssize_t)strlen(MG_DISCOVER_REPLY) || memcmp(reply, MG_DISCOVER_REPLY " ", strlen(MG_DISCOVER_REPLY) + 1))
            continue;
        reply[got] = 0;
        unsigned version = 0;
        int at = 0;
        sscanf(reply + strlen(MG_DISCOVER_REPLY), " %u %n", &version, &at);
        if (version != MG_PROTOCOL_VERSION) {
            version_mismatch = true;
            set_conn(false, NULL, "the Mac runs protocol %u and this client %u: rebuild MacGlass on both", version,
                     MG_PROTOCOL_VERSION);
            close(fd);
            return false;
        }
        snprintf(name, name_size, "%s", reply + strlen(MG_DISCOVER_REPLY) + at);
        found = true;
    }
    close(fd);
    if (!found)
        set_conn(false, NULL, "%s", !thunderbolt ? "the Thunderbolt link is down"
                                    : "no answer on the Thunderbolt link: is MacGlass running on the Mac?");
    return found;
}

// Connects with a deadline, so a vanished address doesn't stall reconnecting.
static int connect_addr(const struct sockaddr *addr, socklen_t len)
{
    int fd = socket(addr->sa_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, addr, len) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    int err = 0;
    socklen_t elen = sizeof err;
    if (poll(&pfd, 1, 2000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
        close(fd);
        errno = err ? err : ETIMEDOUT;
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    int one = 1, rcvbuf = 32 << 20, idle = 2, interval = 1, count = 3;
    unsigned timeout_ms = 3000; // unacknowledged data for this long ends the connection
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout_ms, sizeof timeout_ms);
    return fd;
}

// Finds and connects to the Mac. Returns the socket, or -1 with the reason recorded.
static int open_connection(char *name, size_t name_size)
{
    if (!host_arg) {
        struct sockaddr_in6 mac;
        if (!discover(&mac, name, name_size))
            return -1;
        mac.sin6_port = htons(atoi(port_arg));
        int fd = connect_addr((struct sockaddr *)&mac, sizeof mac);
        if (fd < 0)
            set_conn(false, NULL, "cannot connect to %s: %s", name, strerror(errno));
        return fd;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
    int err = getaddrinfo(host_arg, port_arg, &hints, &res);
    if (err) {
        set_conn(false, NULL, "cannot resolve %s: %s", host_arg, gai_strerror(err));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next)
        fd = connect_addr(ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(res);
    if (fd < 0)
        set_conn(false, NULL, "cannot connect to [%s]:%s: %s", host_arg, port_arg, strerror(errno));
    snprintf(name, name_size, "%s", host_arg);
    return fd;
}

static struct {
    atomic_int x, y;
    atomic_bool visible;
    atomic_bool pending;
} cursor_pos;
static Uint32 cursor_pos_event;

// Reads messages until the connection ends. Returns why it ended, or NULL when asked to quit.
static const char *receive(int fd, bool *mismatch)
{
    static uint8_t *buf;
    static size_t cap;
    while (running) {
        uint32_t hdr[2];
        if (!read_full(fd, hdr, sizeof hdr))
            break;
        uint32_t type = le32toh(hdr[0]), len = le32toh(hdr[1]);
        if (len > 256u << 20)
            return "bad message from the Mac";
        if (len + AV_INPUT_BUFFER_PADDING_SIZE > cap) {
            cap = len + AV_INPUT_BUFFER_PADDING_SIZE;
            buf = realloc(buf, cap);
        }
        if (!read_full(fd, buf, len))
            break;
        memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        if (type == MG_HELLO && len >= 8) {
            uint32_t version, fourcc;
            memcpy(&version, buf, 4);
            memcpy(&fourcc, buf + 4, 4);
            fourcc = le32toh(fourcc);
            printf("connected: protocol %u, codec %c%c%c%c\n", le32toh(version), fourcc >> 24, fourcc >> 16 & 0xff,
                   fourcc >> 8 & 0xff, fourcc & 0xff);
            if (le32toh(version) != MG_PROTOCOL_VERSION) {
                *mismatch = true;
                return "the Mac speaks another protocol version: rebuild MacGlass on both";
            }
        } else if (type == MG_AUDIO && len >= 8) {
            play_audio(buf, len);
        } else if (type == MG_CURSOR_POS && len >= 12) {
            uint32_t v[3];
            memcpy(v, buf, sizeof v);
            cursor_pos.x = (int)le32toh(v[0]);
            cursor_pos.y = (int)le32toh(v[1]);
            cursor_pos.visible = le32toh(v[2]) != 0;
            if (getenv("MACGLASS_DEBUG"))
                printf("Mac cursor at %d,%d%s\n", cursor_pos.x, cursor_pos.y, cursor_pos.visible ? "" : " (off screen)");
            if (!atomic_exchange(&cursor_pos.pending, true))
                SDL_PushEvent(&(SDL_Event){.type = cursor_pos_event});
        } else if (type == MG_CURSOR && len >= 8) {
            pthread_mutex_lock(&cursor_in.lock);
            free(cursor_in.data);
            cursor_in.data = malloc(len);
            memcpy(cursor_in.data, buf, len);
            cursor_in.len = len;
            pthread_mutex_unlock(&cursor_in.lock);
            SDL_PushEvent(&(SDL_Event){.type = cursor_event});
        } else if (type == MG_CLIPBOARD && len >= 1) {
            pthread_mutex_lock(&clip_in.lock);
            free(clip_in.data);
            clip_in.kind = buf[0];
            clip_in.len = len - 1;
            clip_in.data = malloc(clip_in.len + 1);
            memcpy(clip_in.data, buf + 1, clip_in.len);
            clip_in.data[clip_in.len] = 0; // lets text be used as a C string
            pthread_mutex_unlock(&clip_in.lock);
            SDL_PushEvent(&(SDL_Event){.type = clip_event});
        } else if ((type == MG_FRAME && len > MG_FRAME_HEADER_SIZE) ||
                   (type == MG_PATCH && len > MG_PATCH_HEADER_SIZE)) {
            uint32_t v[6];
            memcpy(v, buf, sizeof v);
            int header = type == MG_PATCH ? MG_PATCH_HEADER_SIZE : MG_FRAME_HEADER_SIZE;
            struct item *it = calloc(1, sizeof *it);
            it->patch = type == MG_PATCH;
            it->generation = video_generation;
            it->frame_w = le32toh(v[0]);
            it->frame_h = le32toh(v[1]);
            if (it->patch) {
                it->x = le32toh(v[2]);
                it->y = le32toh(v[3]);
                it->codec = MG_CODEC_PRORES;
            } else {
                it->codec = le32toh(v[4]); // v[2..3] are the capture time
                it->keep = it->codec == MG_CODEC_HEVC;
            }
            it->size = len - header;
            it->data = malloc(it->size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(it->data, buf + header, it->size + AV_INPUT_BUFFER_PADDING_SIZE);
            static int dumped;
            if (getenv("MACGLASS_DUMP") && !it->patch && dumped < 30) {
                char path[256];
                snprintf(path, sizeof path, "%s/frame%03d.bin", getenv("MACGLASS_DUMP"), dumped++);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(it->data, 1, it->size, f);
                    fclose(f);
                }
            }
            stats.bytes += len;
            stats.replaced += queue_push(&packets, it);
        }
    }
    return running ? "the connection to the Mac ended" : NULL;
}

// Waits up to `ms`, returning early on quit or a reconnect request.
static void pause_ms(int ms)
{
    for (int t = 0; t < ms && running && !reconnect_now; t += 50)
        SDL_Delay(50);
    reconnect_now = false;
}

static void *net_thread(void *arg)
{
    (void)arg;
    while (running) {
        char name[128] = "";
        version_mismatch = false;
        int fd = open_connection(name, sizeof name);
        if (fd < 0) {
            pause_ms(version_mismatch ? 5000 : 1000);
            continue;
        }
        video_reset();
        pthread_mutex_lock(&send_lock);
        sock = fd;
        pthread_mutex_unlock(&send_lock);
        set_conn(true, name, NULL);

        bool mismatch = false;
        const char *why = receive(fd, &mismatch);

        pthread_mutex_lock(&send_lock);
        sock = -1;
        pthread_mutex_unlock(&send_lock);
        close(fd);
        stop_audio();
        queue_clear(&packets);
        if (!why)
            break;
        set_conn(false, NULL, "%s", why);
        pause_ms(mismatch ? 5000 : 1000);
    }
    queue_quit(&packets);
    return NULL;
}

// Clipboard sync. Wayland only offers the clipboard to the focused window, so a Linux copy reaches
// the Mac when MacGlass gains focus; that is also when it can be pasted there.
static uint64_t clip_last; // hash of the last content exchanged, so copies aren't echoed back

static uint64_t clip_hash(uint8_t kind, const void *data, size_t len)
{
    uint64_t h = 1469598103934665603ull ^ kind; // FNV-1a
    for (const uint8_t *p = data; len--; p++)
        h = (h ^ *p) * 1099511628211ull;
    return h;
}

static void clip_send(uint8_t kind, const void *data, size_t len)
{
    uint64_t h = clip_hash(kind, data, len);
    if (h == clip_last || len > (64u << 20) - 1)
        return;
    clip_last = h;
    uint8_t *msg = malloc(len + 1);
    msg[0] = kind;
    memcpy(msg + 1, data, len);
    if (send_msg(MG_CLIPBOARD, msg, len + 1))
        overlay_alert(kind == MG_CLIP_PNG ? "Image copied to the Mac" : "Clipboard copied to the Mac");
    free(msg);
}

// Text wins over images, matching what a plain paste would insert.
static void clip_local_changed(void)
{
    size_t len;
    if (SDL_HasClipboardText()) {
        char *text = SDL_GetClipboardText();
        if (text && *text)
            clip_send(MG_CLIP_TEXT, text, strlen(text));
        SDL_free(text);
    } else if (SDL_HasClipboardData("image/png")) {
        void *png = SDL_GetClipboardData("image/png", &len);
        if (png && len)
            clip_send(MG_CLIP_PNG, png, len);
        SDL_free(png);
    }
}

static struct {
    uint8_t *data;
    size_t len;
} clip_png; // image we currently offer to Linux apps

static const void *clip_png_provide(void *userdata, const char *mime, size_t *size)
{
    (void)userdata;
    if (!clip_png.data || SDL_strcmp(mime, "image/png") != 0) {
        *size = 0;
        return NULL;
    }
    *size = clip_png.len;
    return clip_png.data;
}

static void clip_remote_changed(void)
{
    pthread_mutex_lock(&clip_in.lock);
    uint8_t kind = clip_in.kind, *data = clip_in.data;
    size_t len = clip_in.len;
    clip_in.data = NULL;
    pthread_mutex_unlock(&clip_in.lock);
    if (!data)
        return;
    clip_last = clip_hash(kind, data, len);
    overlay_alert(kind == MG_CLIP_PNG ? "Image copied from the Mac" : "Clipboard copied from the Mac");
    if (kind == MG_CLIP_TEXT) {
        SDL_SetClipboardText((const char *)data);
        free(data);
    } else if (kind == MG_CLIP_PNG) {
        free(clip_png.data);
        clip_png.data = data;
        clip_png.len = len;
        const char *mimes[] = {"image/png"};
        SDL_SetClipboardData(clip_png_provide, NULL, NULL, mimes, 1);
    } else {
        free(data);
    }
}

// Keyboard and mouse forwarding. As in Looking Glass, input is captured while the window has focus
// (keyboard grabbed, so niri's shortcuts reach the Mac) and released when it loses it; tapping the
// escape key toggles capture in between, holding it shows the help panel, and escape + key runs a
// client command.
#define ESCAPE_KEY SDL_SCANCODE_RCTRL
#define HELP_DELAY_NS (200 * 1000000ull)

static struct {
    SDL_Window *win;
    bool capture, focused, connected;
    bool escape_down, escape_used;
    Uint64 escape_since;
    bool show_stats;
    bool held[SDL_SCANCODE_COUNT]; // keys the Mac has seen go down
} in;

static bool forwarding(void) { return in.capture && in.focused; }

// The Mac's cursor, shown as this window's own cursor so it moves without streaming latency.
static SDL_Cursor *mac_cursor;

static void cursor_remote_changed(void)
{
    pthread_mutex_lock(&cursor_in.lock);
    uint8_t *p = cursor_in.data;
    size_t len = cursor_in.len;
    cursor_in.data = NULL;
    pthread_mutex_unlock(&cursor_in.lock);
    if (!p)
        return;
    uint16_t v[4];
    memcpy(v, p, sizeof v);
    int w = le16toh(v[0]), h = le16toh(v[1]), hot_x = le16toh(v[2]), hot_y = le16toh(v[3]);
    if (w && h && len >= 8 + (size_t)w * h * 4) {
        // The image is in Mac pixels, which the window shows 1:1. SDL wants the cursor in logical
        // size, so give it a scaled-down base plus the exact image for the display's scale.
        float scale = SDL_GetWindowDisplayScale(in.win);
        if (scale <= 0)
            scale = 1;
        SDL_Surface *wrapped = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, p + 8, w * 4);
        SDL_Surface *full = wrapped ? SDL_DuplicateSurface(wrapped) : NULL;
        SDL_Surface *base = full ? SDL_ScaleSurface(full, SDL_max(1, (int)SDL_lroundf(w / scale)),
                                                    SDL_max(1, (int)SDL_lroundf(h / scale)), SDL_SCALEMODE_LINEAR)
                                 : NULL;
        SDL_Cursor *c = NULL;
        if (base) {
            SDL_AddSurfaceAlternateImage(base, full);
            c = SDL_CreateColorCursor(base, (int)(hot_x / scale), (int)(hot_y / scale));
        }
        SDL_DestroySurface(base);
        SDL_DestroySurface(full);
        SDL_DestroySurface(wrapped);
        if (c) {
            if (in.capture)
                SDL_SetCursor(c);
            if (mac_cursor)
                SDL_DestroyCursor(mac_cursor);
            mac_cursor = c;
        }
    }
    free(p);
}

static void release_all(void)
{
    memset(in.held, 0, sizeof in.held);
    send_msg(MG_RELEASE_ALL, NULL, 0);
}

static void update_title(void)
{
    char title[300];
    pthread_mutex_lock(&conn.lock);
    if (in.connected)
        snprintf(title, sizeof title, "MacGlass: %s, %s%s", conn.name, video_decoder_name(),
                 in.capture ? "" : " (input released, tap Right Ctrl)");
    else
        snprintf(title, sizeof title, "MacGlass: waiting for the Mac");
    pthread_mutex_unlock(&conn.lock);
    SDL_SetWindowTitle(in.win, title);
}

static void update_dot(void);

static void set_capture(bool on)
{
    in.capture = on;
    if (!on)
        release_all();
    // Keeps niri's own shortcuts away while captured; while waiting for a Mac there's nothing to send them to.
    SDL_SetWindowKeyboardGrab(in.win, on && in.connected);
    SDL_SetCursor(on && mac_cursor ? mac_cursor : SDL_GetDefaultCursor());
    update_title();
    update_dot();
}

// While input isn't going to the Mac, a dot shows where the Mac's cursor is (it may move on its
// own, e.g. under automation).
static void update_dot(void)
{
    bool show = in.connected && !forwarding() && cursor_pos.visible && render_width();
    int x = 0, y = 0, w, h;
    if (show)
        render_map_rect(cursor_pos.x, cursor_pos.y, 1, 1, &x, &y, &w, &h);
    overlay_set_dot(show, x, y);
}

static const char *help_text(void);

// Which codec full frames use. While the RX 9070 XT belongs to the host it decodes ProRes; when
// the Windows VM takes it (its libvirt hook rebinds it to vfio-pci, and kills anything still
// holding it) the card is released at once and the Mac is asked for HEVC 4:4:4, which the Intel
// iGPU decodes. The card is taken again only after it has been the host's for a few seconds.
// Right Ctrl + C cycles auto -> HEVC -> ProRes.
enum codec_choice { CHOOSE_AUTO, CHOOSE_HEVC, CHOOSE_PRORES };
static const char *choice_names[] = {"automatic", "HEVC", "ProRes"};

static struct {
    enum codec_choice choice;
    uint32_t sent;      // codec last requested from the Mac
    bool dgpu_hosts;    // the card's driver is amdgpu
    Uint64 dgpu_since;  // when that last changed; 0 if it hasn't since startup
    Uint64 next_check;
} cc;

static bool dgpu_is_hosts(void)
{
    const char *pci = getenv("MACGLASS_DGPU_PCI") ? getenv("MACGLASS_DGPU_PCI") : "0000:04:00.0";
    char path[128], target[256];
    snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/driver", pci);
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n <= 0)
        return false;
    target[n] = 0;
    const char *base = strrchr(target, '/');
    return base && strcmp(base + 1, "amdgpu") == 0;
}

static void update_codec(bool force)
{
    Uint64 now = SDL_GetTicksNS();
    if (now >= cc.next_check || force) {
        bool hosts = dgpu_is_hosts();
        if (hosts != cc.dgpu_hosts) {
            cc.dgpu_hosts = hosts;
            cc.dgpu_since = now;
        }
        cc.next_check = now + 500000000ull;
    }
    bool dgpu_ok = cc.dgpu_hosts && (!cc.dgpu_since || now - cc.dgpu_since >= 3000000000ull);
    bool dgpu = dgpu_ok && cc.choice != CHOOSE_HEVC;
    uint32_t codec = cc.choice == CHOOSE_HEVC || (cc.choice == CHOOSE_AUTO && !dgpu_ok) ? MG_CODEC_HEVC : MG_CODEC_PRORES;
    if (dgpu != use_dgpu) {
        use_dgpu = dgpu;
        video_wake(); // releases the card right away when it's no longer ours
    }
    if (codec != cc.sent || force) {
        uint32_t msg = htole32(codec);
        send_msg(MG_SET_CODEC, &msg, sizeof msg);
        const char *where = dgpu ? "decoding on the RX 9070 XT"
                            : codec == MG_CODEC_HEVC ? "decoding on the Intel iGPU" : "decoding on the CPU";
        if (codec != cc.sent && cc.sent)
            overlay_alert("%s (%s): %s", codec == MG_CODEC_HEVC ? "HEVC 4:4:4" : "ProRes", choice_names[cc.choice], where);
        else
            printf("codec %s (%s): %s\n", codec == MG_CODEC_HEVC ? "HEVC 4:4:4" : "ProRes", choice_names[cc.choice], where);
        cc.sent = codec;
    }
}

// Floats go over the wire as-is: both ends are little-endian.
static void send_pointer(float x, float y)
{
    int vx, vy, vw, vh;
    render_video_rect(&vx, &vy, &vw, &vh);
    if (!vw || !vh)
        return;
    float density = SDL_GetWindowPixelDensity(in.win);
    float pos[2] = {SDL_clamp((x * density - vx) / vw, 0.0f, 1.0f), SDL_clamp((y * density - vy) / vh, 0.0f, 1.0f)};
    send_msg(MG_POINTER, pos, sizeof pos);
}

static int map_button(Uint8 b)
{
    switch (b) {
    case SDL_BUTTON_LEFT: return MG_BUTTON_LEFT;
    case SDL_BUTTON_RIGHT: return MG_BUTTON_RIGHT;
    case SDL_BUTTON_MIDDLE: return MG_BUTTON_MIDDLE;
    case SDL_BUTTON_X1: return MG_BUTTON_BACK;
    case SDL_BUTTON_X2: return MG_BUTTON_FORWARD;
    default: return -1;
    }
}

static void input_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        SDL_Scancode sc = e->key.scancode;
        if (sc == ESCAPE_KEY) {
            if (down && !e->key.repeat) {
                in.escape_down = true;
                in.escape_used = false;
                in.escape_since = SDL_GetTicksNS();
            } else if (!down) {
                in.escape_down = false;
                overlay_set_help(NULL);
                if (!in.escape_used && SDL_GetTicksNS() - in.escape_since < HELP_DELAY_NS) {
                    set_capture(!in.capture);
                    overlay_alert(in.capture ? "Input captured" : "Input released: tap Right Ctrl to capture");
                }
            }
            return;
        }
        if (in.escape_down) {
            if (down && !e->key.repeat) {
                in.escape_used = true;
                overlay_set_help(NULL);
                if (sc == SDL_SCANCODE_F) {
                    bool full = !(SDL_GetWindowFlags(in.win) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(in.win, full);
                    overlay_alert(full ? "Fullscreen" : "Windowed");
                } else if (sc == SDL_SCANCODE_Q) {
                    running = false;
                } else if (sc == SDL_SCANCODE_C) {
                    cc.choice = (cc.choice + 1) % 3;
                    overlay_alert("Codec: %s", choice_names[cc.choice]);
                    update_codec(true);
                } else if (sc == SDL_SCANCODE_D) {
                    in.show_stats = !in.show_stats;
                    if (!in.show_stats)
                        overlay_set_stats(NULL);
                } else if (sc == SDL_SCANCODE_R) {
                    overlay_alert("Reconnecting");
                    reconnect_now = true;
                    pthread_mutex_lock(&send_lock);
                    if (sock >= 0)
                        shutdown(sock, SHUT_RDWR);
                    pthread_mutex_unlock(&send_lock);
                }
            }
            return;
        }
        if (!forwarding() || sc >= SDL_SCANCODE_COUNT || (!down && !in.held[sc]))
            return;
        in.held[sc] = down;
        uint8_t msg[4] = {sc & 0xff, sc >> 8, down, e->key.repeat};
        send_msg(MG_KEY, msg, sizeof msg);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (forwarding())
            send_pointer(e->motion.x, e->motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = map_button(e->button.button);
        if (!forwarding() || b < 0)
            break;
        send_pointer(e->button.x, e->button.y);
        uint8_t msg[4] = {b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN, 0, 0};
        send_msg(MG_BUTTON, msg, sizeof msg);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (forwarding()) {
            float sign = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            float delta[2] = {e->wheel.x * sign, e->wheel.y * sign};
            send_msg(MG_SCROLL, delta, sizeof delta);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        in.focused = true;
        set_capture(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        in.focused = false;
        in.escape_down = false;
        overlay_set_help(NULL);
        set_capture(false); // sends key-ups for everything held
        break;
    }
}

static const char *help_text(void)
{
    static char text[512];
    snprintf(text, sizeof text,
             "Right Ctrl        tap: capture input on/off\n"
             "Right Ctrl + F    fullscreen\n"
             "Right Ctrl + D    stats\n"
             "Right Ctrl + C    codec (now %s)\n"
             "Right Ctrl + R    reconnect\n"
             "Right Ctrl + Q    quit",
             choice_names[cc.choice]);
    return text;
}

static struct {
    int want_w, want_h, sent_w, sent_h;
    Uint64 resize_at;
} win_size;

static void connection_changed(void)
{
    pthread_mutex_lock(&conn.lock);
    bool connected = conn.connected;
    char name[sizeof conn.name], reason[sizeof conn.reason];
    memcpy(name, conn.name, sizeof name);
    memcpy(reason, conn.reason, sizeof reason);
    pthread_mutex_unlock(&conn.lock);

    if (connected && !in.connected) {
        in.connected = true;
        overlay_set_waiting(NULL);
        overlay_alert("Connected to %s", name);
        // The Mac starts every connection afresh: tell it the size and codec again.
        win_size.sent_w = win_size.sent_h = 0;
        win_size.resize_at = SDL_GetTicksNS();
        update_codec(true);
        release_all();
        set_capture(in.focused);
        SDL_DisableScreenSaver(); // idle inhibit while the window is visible
    } else if (!connected) {
        if (in.connected) {
            in.connected = false;
            set_capture(false);
            SDL_SetCursor(SDL_GetDefaultCursor());
            if (mac_cursor)
                SDL_DestroyCursor(mac_cursor);
            mac_cursor = NULL;
            SDL_EnableScreenSaver();
        }
        overlay_set_waiting(*reason ? reason : "connecting");
    }
    update_title();
    update_dot();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    host_arg = argc > 1 ? argv[1] : getenv("MACGLASS_HOST");
    if (host_arg && (!*host_arg || !strcmp(host_arg, "auto")))
        host_arg = NULL; // find the Mac on the Thunderbolt link
    if (argc > 2)
        port_arg = argv[2];

    // One viewer per machine: the Mac serves one client and a new one replaces the current one,
    // so two would keep taking the Mac from each other.
    char lock_path[512];
    snprintf(lock_path, sizeof lock_path, "%s/macglass.lock", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "MacGlass is already running\n");
        return 3;
    }

    // Fetch audio in ~5 ms chunks: PipeWire runs small quanta, and big chunks make the buffer saw.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    SDL_SetHint(SDL_HINT_APP_ID, "dev.faruk.macglass");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1"); // inhibited only while a Mac is connected
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win = SDL_CreateWindow("MacGlass", 1280, 540,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_GLContext gl = win ? SDL_GL_CreateContext(win) : NULL;
    if (!gl) {
        fprintf(stderr, "cannot create window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(0); // present as soon as a frame is ready; the compositor won't tear
    if (!render_init()) {
        fprintf(stderr, "cannot set up drawing\n");
        return 1;
    }
    overlay_init(SDL_GetWindowDisplayScale(win));
    signal(SIGUSR1, on_sigusr1); // kill -USR1: save the window to /tmp/macglass-window.ppm

    frame_event = SDL_RegisterEvents(1);
    clip_event = SDL_RegisterEvents(1);
    cursor_event = SDL_RegisterEvents(1);
    cursor_pos_event = SDL_RegisterEvents(1);
    conn_event = SDL_RegisterEvents(1);

    in.win = win;
    in.show_stats = getenv("MACGLASS_SHOW_STATS") != NULL; // like Right Ctrl + D at startup
    in.focused = SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS;
    set_capture(in.focused);
    const char *choice = getenv("MACGLASS_CODEC");
    cc.choice = choice && !strcmp(choice, "hevc") ? CHOOSE_HEVC : choice && !strcmp(choice, "prores") ? CHOOSE_PRORES : CHOOSE_AUTO;
    cc.dgpu_hosts = dgpu_is_hosts();
    set_conn(false, NULL, "looking for the Mac");
    connection_changed();

    pthread_t net, decoder;
    pthread_create(&decoder, NULL, decode_thread, NULL);
    pthread_create(&net, NULL, net_thread, NULL);
    const char *shown_decoder = NULL;

    SDL_GetWindowSizeInPixels(win, &win_size.want_w, &win_size.want_h);
    Uint64 last_report = SDL_GetTicksNS();
    unsigned presented = 0;
    Uint64 overlay_next = 0;

    while (running) {
        // Sleep until an event, or until something on a timer is due.
        Uint64 now = SDL_GetTicksNS(), due = now + 250000000ull;
        if (win_size.resize_at)
            due = SDL_min(due, win_size.resize_at);
        if (overlay_next)
            due = SDL_min(due, overlay_next);
        if (in.escape_down && !in.escape_used)
            due = SDL_min(due, in.escape_since + HELP_DELAY_NS);
        SDL_Event e;
        Sint32 wait_ms = due > now ? (Sint32)((due - now + 999999) / 1000000) : 0;
        if (SDL_WaitEventTimeout(&e, wait_ms)) {
            do {
                input_event(&e);
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    win_size.want_w = e.window.data1;
                    win_size.want_h = e.window.data2;
                    win_size.resize_at = SDL_GetTicksNS() + RESIZE_DEBOUNCE_NS;
                    render_damage_all();
                    break;
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    overlay_set_scale(SDL_GetWindowDisplayScale(win));
                    break;
                case SDL_EVENT_WINDOW_EXPOSED:
                    render_damage_all();
                    break;
                case SDL_EVENT_CLIPBOARD_UPDATE:
                    if (!e.clipboard.owner && in.connected)
                        clip_local_changed();
                    break;
                default:
                    if (e.type == conn_event)
                        connection_changed();
                    if (e.type == clip_event)
                        clip_remote_changed();
                    if (e.type == cursor_event)
                        cursor_remote_changed();
                    if (e.type == cursor_pos_event) {
                        cursor_pos.pending = false;
                        update_dot();
                    }
                    if (e.type == frame_event) {
                        decoded_pending = false;
                        struct item *it;
                        bool resized = false;
                        while ((it = queue_pop(&decoded, false))) {
                            if (it->generation != video_generation) {
                                // left over from an earlier connection
                            } else if (!it->patch) {
                                resized |= it->frame->width != render_width() || it->frame->height != render_height();
                                render_frame(it->frame);
                            } else if (it->frame_w == render_width() && it->frame_h == render_height()) {
                                render_patch(it->frame, it->x, it->y);
                            }
                            item_free(it);
                        }
                        if (resized)
                            update_dot();
                    }
                }
            } while (SDL_PollEvent(&e));
        }

        update_codec(false);
        if (shown_decoder != video_decoder_name()) {
            shown_decoder = video_decoder_name();
            update_title();
        }

        now = SDL_GetTicksNS();
        if (win_size.resize_at && now >= win_size.resize_at) {
            // Even sizes keep the Mac in 2x (Retina) mode; odd ones would switch it to 1x.
            int w = win_size.want_w & ~1, h = win_size.want_h & ~1;
            if (in.connected && (w != win_size.sent_w || h != win_size.sent_h)) {
                uint32_t msg[2] = {htole32(w), htole32(h)};
                if (send_msg(MG_RESIZE, msg, sizeof msg)) {
                    printf("asked the Mac for %dx%d\n", w, h);
                    win_size.sent_w = w;
                    win_size.sent_h = h;
                }
            }
            win_size.resize_at = 0;
        }
        if (in.escape_down && !in.escape_used && now - in.escape_since >= HELP_DELAY_NS)
            overlay_set_help(help_text());

        if (now - last_report >= 1000000000ull) {
            unsigned frames = atomic_exchange(&stats.frames, 0), patches = atomic_exchange(&stats.patches, 0);
            double secs = (now - last_report) / 1e9;
            float peak = atomic_exchange(&audio_peak, 0.0f), low = atomic_exchange(&audio_low_ms, 1e9f);
            double mbit = atomic_exchange(&stats.bytes, 0) * 8 / secs / 1e6;
            double decode_ms = frames + patches ? atomic_exchange(&stats.decode_ns, 0) / 1e6 / (frames + patches) : 0.0;
            unsigned skipped = atomic_exchange(&stats.replaced, 0), underruns = atomic_exchange(&audio_underruns, 0);
            float gap = atomic_exchange(&audio_gap_ms, 0.0f);
            if (in.connected) {
                printf("%dx%d %s  recv %.0f fps + %.0f patches/s  %.0f Mbit/s  decode %.2f ms  shown %.0f fps  skipped %u  audio %s%.0f dB, buffer low %.0f ms, gap %.0f ms, underruns %u\n",
                       render_width(), render_height(), video_decoder_name(), frames / secs, patches / secs, mbit,
                       decode_ms, presented / secs, skipped, peak > 0 ? "" : "<",
                       peak > 0 ? 20 * SDL_log10f(peak) : -90.0f, low < 1e8f ? low : 0.0f, gap, underruns);
                fflush(stdout);
            }
            if (in.show_stats) {
                char text[512];
                snprintf(text, sizeof text,
                         "%d x %d   %s\n"
                         "received   %.0f fps + %.0f patches/s, %.0f Mbit/s\n"
                         "decode     %.2f ms per picture\n"
                         "shown      %.0f fps, %u skipped\n"
                         "audio      buffer low %.0f ms, gap %.0f ms, %u underruns",
                         render_width(), render_height(), video_decoder_name(), frames / secs, patches / secs, mbit,
                         decode_ms, presented / secs, skipped, low < 1e8f ? low : 0.0f, gap, underruns);
                overlay_set_stats(in.connected ? text : "not connected");
            }
            presented = 0;
            last_report = now;
        }

        if (snapshot_requested) {
            snapshot_requested = 0;
            render_snapshot("/tmp/macglass-window.ppm");
        }
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        overlay_next = overlay_update(now, w, h);
        if (render_present(win, w, h))
            presented++;
    }

    pthread_mutex_lock(&send_lock);
    if (sock >= 0)
        shutdown(sock, SHUT_RDWR);
    pthread_mutex_unlock(&send_lock);
    pthread_join(net, NULL);
    pthread_join(decoder, NULL);
    SDL_DestroyAudioStream(audio);
    queue_quit(&decoded);
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
