// Shared between the MacGlass client's modules: macglass.c (connection, input, audio, clipboard,
// cursor and the main loop), video.c (decoding) and render.c (drawing).
#pragma once

#include <SDL3/SDL.h>
#include <libavutil/frame.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// --- macglass.c ---
extern atomic_bool running;
extern Uint32 frame_event;          // pushed when decoded pictures are waiting
extern atomic_bool decoded_pending; // set with frame_event; the main thread clears it
bool send_msg(uint32_t type, const void *payload, uint32_t len);

extern struct stats {
    atomic_uint frames, patches, replaced;
    atomic_ullong bytes, decode_ns;
} stats;

// --- video.c ---
// Pictures flow net thread -> decode thread -> main thread through two ordered queues. Decoding
// runs on its own thread so audio arriving behind a frame isn't held up while it decodes. Patches
// must all be applied in order; a full frame makes everything queued before it obsolete, which is
// also how the client catches up when decoding or drawing falls behind. HEVC frames are the
// exception before decoding: each one is a reference for the next, so none may be skipped.
struct item {
    struct item *next;
    bool patch;
    bool keep;                  // must not be dropped (undecoded HEVC)
    unsigned generation;        // video_generation when it arrived
    uint32_t codec;             // fourcc of the picture's codec
    int frame_w, frame_h, x, y; // for patches: where they go, in a frame of this size
    uint8_t *data;              // picture followed by AV_INPUT_BUFFER_PADDING_SIZE zero bytes
    int size;
    AVFrame *frame; // once decoded
};

struct queue {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    struct item *head, *tail;
    bool quit, wake;
};

extern struct queue packets, decoded;
void item_free(struct item *it);
unsigned queue_push(struct queue *q, struct item *it);
struct item *queue_pop(struct queue *q, bool wait);
void queue_quit(struct queue *q);
void queue_clear(struct queue *q);

// Bumped for each new connection. Items carry the generation they arrived in; when it changes the
// decoders drop their references to earlier pictures.
extern atomic_uint video_generation;
void video_reset(void); // a new connection: drops queued packets and bumps the generation

// Whether ProRes may be decoded on the discrete GPU. Set by the main thread, which watches the
// card's driver; turning it off makes the decode thread close every handle to the card at once,
// so the Windows VM can take it.
extern atomic_bool use_dgpu;
void video_wake(void);
void *decode_thread(void *arg);
const char *video_decoder_name(void); // what decoded the last full frame

// --- render.c ---
bool render_init(void);
void render_frame(AVFrame *f);                // a full frame; replaces the picture
void render_patch(AVFrame *f, int x, int y); // part of the current picture
// Draws what changed since the last present (picture and overlay) and shows it. Returns false when
// nothing changed.
bool render_present(SDL_Window *win, int win_w, int win_h);
void render_damage(int x, int y, int w, int h); // window pixels that must be drawn again
void render_damage_all(void);
void render_snapshot(const char *path); // save the next presented frame as a PPM (SIGUSR1)
// Where frame pixels (x, y, w, h) appear in the window, in window pixels.
void render_map_rect(int x, int y, int w, int h, int *wx, int *wy, int *ww, int *wh);
int render_width(void);  // size of the current picture, 0 before the first one
int render_height(void);
void render_video_rect(int *x, int *y, int *w, int *h); // where the picture was last drawn

// --- overlay.c ---
// On-screen UI drawn over the picture: waiting screen, alerts, stats, help, the Mac cursor dot.
bool overlay_init(float scale);       // scale: the display's scale factor, for the font size
void overlay_set_scale(float scale);  // the window moved to a display with another scale
void overlay_alert(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void overlay_set_waiting(const char *reason); // NULL once connected
void overlay_set_stats(const char *text);     // NULL hides the panel
void overlay_set_help(const char *text);      // NULL hides the panel
void overlay_set_dot(bool show, int x, int y); // the Mac cursor position, in window pixels
// Expires and animates what's shown, recording damage. Returns when it next needs a call (ns,
// SDL_GetTicksNS clock), or 0.
Uint64 overlay_update(Uint64 now, int win_w, int win_h);
void overlay_draw(int win_w, int win_h); // called by render_present, possibly once per clip rect
