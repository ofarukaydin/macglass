// Drawing. Every decoded picture, a full frame or a patch, planar (CPU or 9070 XT decoding) or
// packed (Intel VAAPI frames used in place through dmabuf), is converted from YUV into one RGB
// canvas texture; the window then shows the canvas. Patches are converted straight into their
// spot on the canvas, so they work the same whichever decoder produced the frame under them.
//
// Presenting redraws only what changed. Each frame's damage (patches, overlay changes) is recorded;
// with EGL_EXT_buffer_age the back buffer still holds the picture from `age` frames ago, so only the
// damage of the frames since then needs drawing again. The compositor is told the frame's own
// damage through eglSwapBuffersWithDamage, so it recomposites only that too.
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <drm_fourcc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macglass.h"

static const char *vertex_src = "#version 330 core\n"
                                "out vec2 uv;\n"
                                "uniform bool flip;\n"
                                "void main() {\n"
                                "    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
                                "    uv = flip ? vec2(p.x, 1.0 - p.y) : p;\n"
                                "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                                "}\n";

// Planar pictures hold 10/12-bit samples in 16-bit textures; norm maps them back to 0..1 of their
// range. Packed Intel frames (Y410/XV30) are imported as an RGB10_A2 texture whose red, green and
// blue channels carry U, Y and V. Both use the BT.709 matrix the Mac encodes with.
static const char *convert_src =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D ty, tu, tv;\n"
    "uniform bool interleaved;\n"
    "uniform float norm, ymin, yscale, cmid, cscale;\n"
    "void main() {\n"
    "    vec3 yuv = interleaved ? texture(ty, uv).grb\n"
    "                      : vec3(texture(ty, uv).r, texture(tu, uv).r, texture(tv, uv).r);\n"
    "    float y = (yuv.x * norm - ymin) * yscale;\n"
    "    float u = (yuv.y * norm - cmid) * cscale;\n"
    "    float v = (yuv.z * norm - cmid) * cscale;\n"
    "    color = vec4(clamp(vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u), 0.0, 1.0), 1.0);\n"
    "}\n";

static const char *show_src = "#version 330 core\n"
                              "in vec2 uv;\n"
                              "out vec4 color;\n"
                              "uniform sampler2D canvas;\n"
                              "void main() { color = texture(canvas, uv); }\n";

typedef void (*image_target_fn)(GLenum target, void *image);

struct planes {
    GLuint tex[3];
    int w[3], h[3];
};

#define MAX_IMPORTS 32
#define MAX_RECTS 16  // per frame; more are merged into their bounding box
#define HISTORY 4     // frames of damage kept for buffer age

struct rect {
    int x, y, w, h; // window pixels, top-left origin
};

struct damage {
    struct rect rects[MAX_RECTS];
    int n;
    bool all;
};

typedef EGLBoolean (*swap_damage_fn)(EGLDisplay, EGLSurface, const EGLint *, EGLint);

static struct {
    GLuint convert, show, vao, fbo, canvas;
    int cw, ch;             // canvas (= current picture) size
    int vx, vy, vw, vh;     // where the canvas was last drawn in the window
    struct planes full, patch;
    GLuint packed_tex;      // packed frames that had to be copied (import unavailable)
    int packed_w, packed_h;
    // Intel frames used in place: one imported image per decoder surface, reused as the decoder's
    // surface pool cycles. Cleared when the pool changes (new size or decoder).
    struct {
        uintptr_t surface;
        void *pool;
        EGLImage image;
        GLuint tex;
    } imports[MAX_IMPORTS];
    int nimports;
    bool import_failed;     // dmabuf import unusable here; copy instead
    AVFrame *held[2];       // the GPU may still read the last frames' surfaces
    EGLDisplay dpy;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    image_target_fn image_target;
    // Partial redraw
    struct damage cur;            // this frame's damage, in window pixels
    struct damage history[HISTORY]; // previous frames' damage, most recent first
    struct rect canvas_damage[MAX_RECTS]; // patches since the last present, in canvas pixels
    int ncanvas;
    bool canvas_all;
    int win_w, win_h;
    bool buffer_age;
    swap_damage_fn swap_damage;
    bool debug;
    const char *snapshot; // write the next presented frame here
} r;

static GLuint compile(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char msg[1024];
        glGetShaderInfoLog(s, sizeof msg, NULL, msg);
        fprintf(stderr, "shader: %s\n", msg);
    }
    return s;
}

static GLuint program(const char *fragment)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vertex_src));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fragment));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

static void texture_params(GLuint tex)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool render_init(void)
{
    r.convert = program(convert_src);
    r.show = program(show_src);
    if (!r.convert || !r.show)
        return false;
    glUseProgram(r.convert);
    glUniform1i(glGetUniformLocation(r.convert, "ty"), 0);
    glUniform1i(glGetUniformLocation(r.convert, "tu"), 1);
    glUniform1i(glGetUniformLocation(r.convert, "tv"), 2);
    glUniform1i(glGetUniformLocation(r.convert, "flip"), 1); // pictures arrive top row first
    glUseProgram(r.show);
    glUniform1i(glGetUniformLocation(r.show, "canvas"), 0);
    glUniform1i(glGetUniformLocation(r.show, "flip"), 0);

    glGenVertexArrays(1, &r.vao);
    glBindVertexArray(r.vao);
    glGenFramebuffers(1, &r.fbo);
    glGenTextures(1, &r.canvas);
    texture_params(r.canvas);
    glGenTextures(3, r.full.tex);
    glGenTextures(3, r.patch.tex);
    glGenTextures(1, &r.packed_tex);
    for (int i = 0; i < 3; i++) {
        texture_params(r.full.tex[i]);
        texture_params(r.patch.tex[i]);
    }
    texture_params(r.packed_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);

    r.dpy = SDL_EGL_GetCurrentDisplay();
    r.create_image = (PFNEGLCREATEIMAGEKHRPROC)SDL_EGL_GetProcAddress("eglCreateImageKHR");
    r.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)SDL_EGL_GetProcAddress("eglDestroyImageKHR");
    r.image_target = (image_target_fn)SDL_EGL_GetProcAddress("glEGLImageTargetTexture2DOES");
    r.import_failed = !r.dpy || !r.create_image || !r.destroy_image || !r.image_target;
    const char *exts = r.dpy ? eglQueryString(r.dpy, EGL_EXTENSIONS) : NULL;
    if (exts && !getenv("MACGLASS_FULL_REDRAW")) {
        r.buffer_age = strstr(exts, "EGL_EXT_buffer_age") != NULL;
        if (strstr(exts, "EGL_KHR_swap_buffers_with_damage"))
            r.swap_damage = (swap_damage_fn)SDL_EGL_GetProcAddress("eglSwapBuffersWithDamageKHR");
        else if (strstr(exts, "EGL_EXT_swap_buffers_with_damage"))
            r.swap_damage = (swap_damage_fn)SDL_EGL_GetProcAddress("eglSwapBuffersWithDamageEXT");
    }
    r.debug = getenv("MACGLASS_DEBUG") != NULL;
    r.cur.all = true;
    printf("drawing with %s\n", (const char *)glGetString(GL_RENDERER));
    return true;
}

int render_width(void) { return r.cw; }
int render_height(void) { return r.ch; }

void render_video_rect(int *x, int *y, int *w, int *h)
{
    *x = r.vx;
    *y = r.vy;
    *w = r.vw;
    *h = r.vh;
}

static void ensure_canvas(int w, int h)
{
    if (w == r.cw && h == r.ch)
        return;
    glBindTexture(GL_TEXTURE_2D, r.canvas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, r.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r.canvas, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    r.cw = w;
    r.ch = h;
}

// Sample scaling for `depth`-bit video: norm undoes 16-bit storage, the rest maps limited or full
// range to 0..1 luma and +-0.5 chroma.
static void yuv_uniforms(int depth, bool full_range, bool packed, float norm)
{
    float maxv = (float)((1 << depth) - 1), step = (float)(1 << (depth - 8));
    glUseProgram(r.convert);
    glUniform1i(glGetUniformLocation(r.convert, "interleaved"), packed);
    glUniform1f(glGetUniformLocation(r.convert, "norm"), norm);
    glUniform1f(glGetUniformLocation(r.convert, "ymin"), full_range ? 0 : 16 * step / maxv);
    glUniform1f(glGetUniformLocation(r.convert, "yscale"), full_range ? 1 : maxv / (219 * step));
    glUniform1f(glGetUniformLocation(r.convert, "cmid"), 128 * step / maxv);
    glUniform1f(glGetUniformLocation(r.convert, "cscale"), full_range ? 1 : maxv / (224 * step));
}

// Converts what is bound to texture units 0-2 into the canvas rectangle (x, y, w, h), top-left
// origin, in canvas pixels.
static void convert_into_canvas(int x, int y, int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, r.fbo);
    glViewport(x, r.ch - y - h, w, h);
    glUseProgram(r.convert);
    glBindVertexArray(r.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (r.ncanvas < MAX_RECTS)
        r.canvas_damage[r.ncanvas++] = (struct rect){x, y, w, h};
    else
        r.canvas_all = true;
}

static bool upload_planar(struct planes *p, const AVFrame *f)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);
    if (!d || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) || d->comp[0].depth <= 8 || d->comp[0].depth > 16)
        return false;
    for (int i = 0; i < 3; i++) {
        int w = i ? AV_CEIL_RSHIFT(f->width, d->log2_chroma_w) : f->width;
        int h = i ? AV_CEIL_RSHIFT(f->height, d->log2_chroma_h) : f->height;
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, p->tex[i]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[i] / 2);
        if (w != p->w[i] || h != p->h[i]) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, w, h, 0, GL_RED, GL_UNSIGNED_SHORT, f->data[i]);
            p->w[i] = w;
            p->h[i] = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_SHORT, f->data[i]);
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glActiveTexture(GL_TEXTURE0);
    yuv_uniforms(d->comp[0].depth, f->color_range == AVCOL_RANGE_JPEG, false, 65535.0f / ((1 << d->comp[0].depth) - 1));
    return true;
}

static void forget_imports(void)
{
    for (int i = 0; i < r.nimports; i++) {
        glDeleteTextures(1, &r.imports[i].tex);
        r.destroy_image(r.dpy, r.imports[i].image);
    }
    r.nimports = 0;
}

// Wraps an Intel VAAPI frame's surface in a GL texture without copying it. Returns 0 when that
// isn't possible, so the caller copies the frame instead.
static GLuint import_vaapi(const AVFrame *f)
{
    uintptr_t surface = (uintptr_t)f->data[3];
    void *pool = f->hw_frames_ctx ? f->hw_frames_ctx->data : NULL;
    if (r.nimports && r.imports[0].pool != pool)
        forget_imports(); // a new decoder or size: surface IDs mean something else now
    for (int i = 0; i < r.nimports; i++)
        if (r.imports[i].surface == surface)
            return r.imports[i].tex;
    if (r.import_failed || r.nimports == MAX_IMPORTS)
        return 0;

    AVFrame *drm = av_frame_alloc();
    drm->format = AV_PIX_FMT_DRM_PRIME;
    GLuint tex = 0;
    if (av_hwframe_map(drm, f, AV_HWFRAME_MAP_READ) == 0) {
        const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)drm->data[0];
        const AVDRMLayerDescriptor *layer = &d->layers[0];
        // Y410/XV30 pack U, Y, V into bits 0-9, 10-19, 20-29: the layout of XBGR2101010.
        bool packed444 = d->nb_layers == 1 && layer->nb_planes == 1 &&
                         (layer->format == DRM_FORMAT_XVYU2101010 || layer->format == fourcc_code('Y', '4', '1', '0'));
        if (packed444) {
            const AVDRMObjectDescriptor *obj = &d->objects[layer->planes[0].object_index];
            EGLint attrs[] = {EGL_WIDTH, f->width, EGL_HEIGHT, f->height, EGL_LINUX_DRM_FOURCC_EXT,
                              DRM_FORMAT_XBGR2101010, EGL_DMA_BUF_PLANE0_FD_EXT, obj->fd,
                              EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)layer->planes[0].offset,
                              EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)layer->planes[0].pitch,
                              EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(obj->format_modifier & 0xffffffff),
                              EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(obj->format_modifier >> 32), EGL_NONE};
            EGLImage image = r.create_image(r.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
            if (image != EGL_NO_IMAGE) {
                glGenTextures(1, &tex);
                texture_params(tex);
                r.image_target(GL_TEXTURE_2D, image);
                r.imports[r.nimports++] = (typeof(r.imports[0])){surface, pool, image, tex};
            } else {
                fprintf(stderr, "cannot import Intel frames (EGL error 0x%x, modifier 0x%llx); copying them instead\n",
                        eglGetError(), (unsigned long long)obj->format_modifier);
                r.import_failed = true;
            }
        } else {
            fprintf(stderr, "unexpected Intel frame layout (format 0x%x); copying frames instead\n", layer->format);
            r.import_failed = true;
        }
    } else {
        r.import_failed = true;
    }
    av_frame_free(&drm); // the EGL image keeps its own reference to the buffer
    return tex;
}

// Fallback for Intel frames that can't be imported: copy to system memory (packed XV30) and
// upload. Much slower at 5K (~50 fps), but correct.
static bool upload_packed_copy(const AVFrame *f)
{
    AVFrame *sw = av_frame_alloc();
    bool ok = av_hwframe_transfer_data(sw, f, 0) == 0 && sw->format == AV_PIX_FMT_XV30LE;
    if (ok) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r.packed_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, sw->linesize[0] / 4);
        if (sw->width != r.packed_w || sw->height != r.packed_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, sw->width, sw->height, 0, GL_RGBA,
                         GL_UNSIGNED_INT_2_10_10_10_REV, sw->data[0]);
            r.packed_w = sw->width;
            r.packed_h = sw->height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw->width, sw->height, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                            sw->data[0]);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    }
    av_frame_free(&sw);
    return ok;
}

void render_frame(AVFrame *f)
{
    if (f->format == AV_PIX_FMT_VAAPI) {
        GLuint tex = import_vaapi(f);
        glActiveTexture(GL_TEXTURE0);
        if (tex)
            glBindTexture(GL_TEXTURE_2D, tex);
        else if (!upload_packed_copy(f))
            return;
        yuv_uniforms(10, f->color_range == AVCOL_RANGE_JPEG, true, 1.0f);
        ensure_canvas(f->width, f->height);
        convert_into_canvas(0, 0, f->width, f->height);
        r.canvas_all = true;
        if (tex) {
            // Keep the frame's surface out of the decoder's pool until the GPU is surely done.
            av_frame_free(&r.held[0]);
            r.held[0] = r.held[1];
            r.held[1] = av_frame_clone(f);
        }
        return;
    }
    if (!upload_planar(&r.full, f))
        return;
    ensure_canvas(f->width, f->height);
    convert_into_canvas(0, 0, f->width, f->height);
    r.canvas_all = true;
}

void render_patch(AVFrame *f, int x, int y)
{
    if (x + f->width > r.cw || y + f->height > r.ch || !upload_planar(&r.patch, f))
        return;
    convert_into_canvas(x, y, f->width, f->height);
}

static void add_rect(struct damage *d, struct rect a)
{
    // Clip to the window; drop what's left empty.
    int x0 = SDL_max(a.x, 0), y0 = SDL_max(a.y, 0);
    int x1 = SDL_min(a.x + a.w, r.win_w), y1 = SDL_min(a.y + a.h, r.win_h);
    if (d->all || x1 <= x0 || y1 <= y0)
        return;
    if (x0 == 0 && y0 == 0 && x1 == r.win_w && y1 == r.win_h) {
        d->all = true;
        return;
    }
    // Drop rectangles already covered (e.g. the same patch in this and the previous frame), and
    // those the new one covers, so nothing is drawn twice.
    for (int i = 0; i < d->n; i++) {
        struct rect *e = &d->rects[i];
        if (e->x <= x0 && e->y <= y0 && e->x + e->w >= x1 && e->y + e->h >= y1)
            return;
        if (x0 <= e->x && y0 <= e->y && x1 >= e->x + e->w && y1 >= e->y + e->h)
            d->rects[i--] = d->rects[--d->n];
    }
    if (d->n == MAX_RECTS) {
        struct rect *b = &d->rects[0];
        for (int i = 1; i < d->n; i++) {
            int bx1 = SDL_max(b->x + b->w, d->rects[i].x + d->rects[i].w);
            int by1 = SDL_max(b->y + b->h, d->rects[i].y + d->rects[i].h);
            b->x = SDL_min(b->x, d->rects[i].x);
            b->y = SDL_min(b->y, d->rects[i].y);
            b->w = bx1 - b->x;
            b->h = by1 - b->y;
        }
        d->n = 1;
    }
    d->rects[d->n++] = (struct rect){x0, y0, x1 - x0, y1 - y0};
}

void render_damage(int x, int y, int w, int h) { add_rect(&r.cur, (struct rect){x, y, w, h}); }

void render_damage_all(void) { r.cur.all = true; }

void render_map_rect(int x, int y, int w, int h, int *wx, int *wy, int *ww, int *wh)
{
    double sx = r.cw ? (double)r.vw / r.cw : 1, sy = r.ch ? (double)r.vh / r.ch : 1;
    *wx = r.vx + (int)(x * sx);
    *wy = r.vy + (int)(y * sy);
    *ww = (int)((x + w) * sx + 0.999) - (int)(x * sx);
    *wh = (int)((y + h) * sy + 0.999) - (int)(y * sy);
}

// Letterboxed to keep the aspect ratio; 1:1 once the Mac has matched the window size.
static void layout(int win_w, int win_h)
{
    int vx = 0, vy = 0, vw = 0, vh = 0;
    if (r.cw) {
        double scale = SDL_min((double)win_w / r.cw, (double)win_h / r.ch);
        vw = (int)(r.cw * scale + 0.5);
        vh = (int)(r.ch * scale + 0.5);
        vx = (win_w - vw) / 2;
        vy = (win_h - vh) / 2;
    }
    if (win_w != r.win_w || win_h != r.win_h || vx != r.vx || vy != r.vy || vw != r.vw || vh != r.vh)
        r.cur.all = true;
    r.win_w = win_w;
    r.win_h = win_h;
    r.vx = vx;
    r.vy = vy;
    r.vw = vw;
    r.vh = vh;
}

static void draw_area(int win_h)
{
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (r.cw) {
        glViewport(r.vx, win_h - r.vy - r.vh, r.vw, r.vh);
        glUseProgram(r.show);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r.canvas);
        glBindVertexArray(r.vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    overlay_draw(r.win_w, r.win_h);
}

void render_snapshot(const char *path)
{
    r.snapshot = path;
    r.cur.all = true;
}

// Saves what was just drawn (the whole window) as a binary PPM.
static void write_snapshot(int win_w, int win_h)
{
    uint8_t *px = malloc((size_t)win_w * win_h * 4);
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(r.snapshot, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
        for (int y = win_h - 1; y >= 0; y--)
            for (int x = 0; x < win_w; x++)
                fwrite(px + ((size_t)y * win_w + x) * 4, 1, 3, f);
        fclose(f);
        printf("saved the window to %s\n", r.snapshot);
    }
    free(px);
    r.snapshot = NULL;
}

bool render_present(SDL_Window *win, int win_w, int win_h)
{
    layout(win_w, win_h);
    // The picture's changes, mapped into the window; a pixel of margin covers linear filtering.
    if (r.canvas_all) {
        int x, y, w, h;
        render_map_rect(0, 0, r.cw, r.ch, &x, &y, &w, &h);
        add_rect(&r.cur, (struct rect){x, y, w, h});
    } else {
        for (int i = 0; i < r.ncanvas; i++) {
            struct rect c = r.canvas_damage[i];
            int x, y, w, h;
            render_map_rect(c.x, c.y, c.w, c.h, &x, &y, &w, &h);
            add_rect(&r.cur, (struct rect){x - 1, y - 1, w + 2, h + 2});
        }
    }
    r.canvas_all = false;
    r.ncanvas = 0;
    if (!r.cur.all && !r.cur.n)
        return false;

    // What must be drawn again: this frame's damage plus that of the frames the back buffer missed.
    EGLSurface surface = SDL_EGL_GetWindowSurface(win);
    EGLint age = 0;
    if (r.buffer_age && surface && !eglQuerySurface(r.dpy, surface, EGL_BUFFER_AGE_EXT, &age))
        age = 0;
    struct damage repaint = r.cur;
    if (age <= 0 || age > HISTORY + 1)
        repaint.all = true;
    for (int i = 0; i < age - 1 && !repaint.all; i++) {
        if (r.history[i].all)
            repaint.all = true;
        for (int j = 0; j < r.history[i].n; j++)
            add_rect(&repaint, r.history[i].rects[j]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    if (repaint.all) {
        draw_area(win_h);
    } else {
        glEnable(GL_SCISSOR_TEST);
        for (int i = 0; i < repaint.n; i++) {
            struct rect a = repaint.rects[i];
            glScissor(a.x, win_h - a.y - a.h, a.w, a.h);
            glViewport(0, 0, win_w, win_h);
            draw_area(win_h);
        }
        glDisable(GL_SCISSOR_TEST);
    }

    if (r.snapshot && repaint.all)
        write_snapshot(win_w, win_h);

    static unsigned presents, partial;
    static double area;
    presents++;
    if (!repaint.all) {
        partial++;
        for (int i = 0; i < repaint.n; i++)
            area += (double)repaint.rects[i].w * repaint.rects[i].h / ((double)win_w * win_h);
    } else {
        area += 1;
    }
    if (r.debug && presents == 120) {
        printf("redraw: %u of 120 presents partial, %.1f%% of the window drawn on average (buffer age %d)\n",
               partial, 100 * area / presents, age);
        presents = partial = 0;
        area = 0;
    }

    if (r.swap_damage && surface && !r.cur.all) {
        EGLint rects[MAX_RECTS * 4];
        for (int i = 0; i < r.cur.n; i++) { // bottom-left origin
            rects[i * 4] = r.cur.rects[i].x;
            rects[i * 4 + 1] = win_h - r.cur.rects[i].y - r.cur.rects[i].h;
            rects[i * 4 + 2] = r.cur.rects[i].w;
            rects[i * 4 + 3] = r.cur.rects[i].h;
        }
        r.swap_damage(r.dpy, surface, rects, r.cur.n);
    } else {
        SDL_GL_SwapWindow(win);
    }

    memmove(&r.history[1], &r.history[0], sizeof r.history[0] * (HISTORY - 1));
    r.history[0] = r.cur;
    r.cur = (struct damage){0};
    return true;
}
