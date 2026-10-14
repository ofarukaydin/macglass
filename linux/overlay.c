// On-screen UI over the Mac's picture, in the spirit of Looking Glass's: the waiting screen while no
// Mac is connected, short alerts, the stats panel (Right Ctrl + D), the help panel (hold Right Ctrl)
// and a dot marking the Mac's cursor while input isn't captured.
//
// Text is drawn with FreeType from the fontconfig "monospace" font (MACGLASS_FONT overrides), its
// glyphs rendered once into an atlas texture. Each element records its last drawn rectangle, so a
// change damages exactly the old and new area (see render.c's partial redraw).
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macglass.h"

#define FONT_PX 15.0f        // at scale 1
#define ALERT_NS (2500 * 1000000ull)
#define ATLAS_W 1024
#define MAX_QUADS 4096

static const char *vertex_src = "#version 330 core\n"
                                "layout(location = 0) in vec2 pos;\n"
                                "layout(location = 1) in vec2 tc;\n"
                                "layout(location = 2) in vec4 col;\n"
                                "uniform vec2 window;\n"
                                "out vec2 uv;\n"
                                "out vec4 color;\n"
                                "void main() {\n"
                                "    uv = tc;\n"
                                "    color = col;\n"
                                "    gl_Position = vec4(pos.x / window.x * 2.0 - 1.0, 1.0 - pos.y / window.y * 2.0, 0.0, 1.0);\n"
                                "}\n";

// Colours are premultiplied. Glyph quads have uv inside the atlas; solid quads use uv.x < -1;
// the dot uses uv.x in [-1, 1] relative to its centre, flagged by uv.y > 1 (offset by 3).
static const char *fragment_src = "#version 330 core\n"
                                  "in vec2 uv;\n"
                                  "in vec4 color;\n"
                                  "out vec4 frag;\n"
                                  "uniform sampler2D atlas;\n"
                                  "void main() {\n"
                                  "    if (uv.x < -1.5) { frag = color; return; }\n"
                                  "    if (uv.y > 1.5) {\n"
                                  "        float d = length(vec2(uv.x, uv.y - 3.0));\n"
                                  "        float a = 1.0 - smoothstep(0.85, 1.0, d);\n"
                                  "        vec3 c = mix(vec3(1.0), vec3(0.1), smoothstep(0.5, 0.62, d));\n"
                                  "        frag = vec4(c, 1.0) * a;\n"
                                  "        return;\n"
                                  "    }\n"
                                  "    frag = color * texture(atlas, uv).r;\n"
                                  "}\n";

struct glyph {
    float u0, v0, u1, v1;
    int w, h, left, top, advance;
};

struct box {
    int x, y, w, h;
};

// One thing shown on screen. `version` changes whenever its content does.
struct element {
    bool shown;
    unsigned version, drawn_version;
    struct box at, drawn; // where it goes now / where it was last drawn
};

static struct {
    bool ready;
    GLuint program, vao, vbo, atlas;
    GLint window_loc;
    FT_Library ft;
    FT_Face face;
    float scale;
    int line, ascent, char_w;
    struct glyph ascii[256];   // Latin-1
    struct glyph ellipsis, dash;
    float *verts;
    int nquads;

    struct element dim, waiting, alert, stats, help, dot;
    char waiting_text[256], waiting_reason[192];
    char alert_text[192];
    Uint64 alert_until;
    char stats_text[1024], help_text[1024];
    int dot_x, dot_y;
    Uint64 next_tick;
    unsigned dots;
} o;

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
        fprintf(stderr, "overlay shader: %s\n", msg);
    }
    return s;
}

static char *font_path(void)
{
    if (getenv("MACGLASS_FONT"))
        return strdup(getenv("MACGLASS_FONT"));
    char *path = NULL;
    if (!FcInit())
        return NULL;
    FcPattern *pat = FcNameParse((const FcChar8 *)"monospace");
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcChar8 *file;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
        path = strdup((const char *)file);
    FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return path;
}

// Renders the Latin-1 glyphs plus a few extras into the atlas at the current scale.
static bool build_atlas(void)
{
    FT_Set_Pixel_Sizes(o.face, 0, (FT_UInt)(FONT_PX * o.scale + 0.5f));
    o.line = (int)(o.face->size->metrics.height >> 6);
    o.ascent = (int)(o.face->size->metrics.ascender >> 6);

    uint32_t codes[256 + 2];
    int n = 0;
    for (uint32_t c = 32; c < 256; c++)
        if (c < 127 || c >= 160)
            codes[n++] = c;
    codes[n++] = 0x2026; // …
    codes[n++] = 0x2014; // —

    int pad = 1, x = pad, y = pad, row_h = 0, height = 0;
    struct {
        uint8_t *bits;
        int w, h, pitch, x, y;
    } placed[256 + 2];
    for (int i = 0; i < n; i++) {
        struct glyph *g = codes[i] == 0x2026 ? &o.ellipsis : codes[i] == 0x2014 ? &o.dash : &o.ascii[codes[i]];
        placed[i].bits = NULL;
        if (FT_Load_Char(o.face, codes[i], FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT))
            continue;
        FT_GlyphSlot s = o.face->glyph;
        int w = (int)s->bitmap.width, h = (int)s->bitmap.rows;
        if (x + w + pad > ATLAS_W) {
            x = pad;
            y += row_h + pad;
            row_h = 0;
        }
        placed[i].w = w;
        placed[i].h = h;
        placed[i].pitch = s->bitmap.pitch;
        placed[i].x = x;
        placed[i].y = y;
        placed[i].bits = malloc((size_t)(w ? w : 1) * (h ? h : 1));
        for (int r = 0; r < h; r++)
            memcpy(placed[i].bits + r * w, s->bitmap.buffer + r * s->bitmap.pitch, w);
        *g = (struct glyph){.w = w, .h = h, .left = s->bitmap_left, .top = s->bitmap_top,
                            .advance = (int)(s->advance.x >> 6)};
        x += w + pad;
        row_h = SDL_max(row_h, h);
        height = y + row_h + pad;
    }
    o.char_w = o.ascii['M'].advance;

    uint8_t *pixels = calloc((size_t)ATLAS_W * height, 1);
    for (int i = 0; i < n; i++) {
        if (!placed[i].bits)
            continue;
        for (int r = 0; r < placed[i].h; r++)
            memcpy(pixels + (size_t)(placed[i].y + r) * ATLAS_W + placed[i].x, placed[i].bits + r * placed[i].w,
                   placed[i].w);
        struct glyph *g = codes[i] == 0x2026 ? &o.ellipsis : codes[i] == 0x2014 ? &o.dash : &o.ascii[codes[i]];
        g->u0 = (float)placed[i].x / ATLAS_W;
        g->v0 = (float)placed[i].y / height;
        g->u1 = (float)(placed[i].x + placed[i].w) / ATLAS_W;
        g->v1 = (float)(placed[i].y + placed[i].h) / height;
        free(placed[i].bits);
    }
    glBindTexture(GL_TEXTURE_2D, o.atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2); // render.c's default
    free(pixels);
    return true;
}

bool overlay_init(float scale)
{
    o.scale = scale > 0 ? scale : 1;
    char *path = font_path();
    if (!path || FT_Init_FreeType(&o.ft) || FT_New_Face(o.ft, path, 0, &o.face)) {
        fprintf(stderr, "overlay: cannot load a font%s%s; no on-screen text\n", path ? " from " : "", path ? path : "");
        free(path);
        return false;
    }
    free(path);

    o.program = glCreateProgram();
    glAttachShader(o.program, compile(GL_VERTEX_SHADER, vertex_src));
    glAttachShader(o.program, compile(GL_FRAGMENT_SHADER, fragment_src));
    glLinkProgram(o.program);
    GLint ok;
    glGetProgramiv(o.program, GL_LINK_STATUS, &ok);
    if (!ok)
        return false;
    glUseProgram(o.program);
    glUniform1i(glGetUniformLocation(o.program, "atlas"), 0);
    o.window_loc = glGetUniformLocation(o.program, "window");

    glGenVertexArrays(1, &o.vao);
    glBindVertexArray(o.vao);
    glGenBuffers(1, &o.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)MAX_QUADS * 6 * 8 * sizeof(float), NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(2 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(4 * sizeof(float)));
    for (int i = 0; i < 3; i++)
        glEnableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(1, &o.atlas);
    glBindTexture(GL_TEXTURE_2D, o.atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    o.verts = malloc((size_t)MAX_QUADS * 6 * 8 * sizeof(float));
    o.ready = build_atlas();
    return o.ready;
}

void overlay_set_scale(float scale)
{
    if (!o.ready || scale <= 0 || scale == o.scale)
        return;
    o.scale = scale;
    build_atlas();
    render_damage_all();
    for (struct element *e = &o.dim; e <= &o.dot; e++)
        e->version++;
}

// --- Content ---

void overlay_alert(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o.alert_text, sizeof o.alert_text, fmt, ap);
    va_end(ap);
    printf("%s\n", o.alert_text);
    o.alert.shown = true;
    o.alert.version++;
    o.alert_until = SDL_GetTicksNS() + ALERT_NS;
}

void overlay_set_waiting(const char *reason)
{
    bool shown = reason != NULL;
    if (shown != o.waiting.shown || (reason && strcmp(reason, o.waiting_reason) != 0)) {
        o.waiting.version++;
        o.dim.version++;
    }
    o.waiting.shown = o.dim.shown = shown;
    snprintf(o.waiting_reason, sizeof o.waiting_reason, "%s", reason ? reason : "");
}

static void set_text(struct element *e, char *dst, size_t size, const char *text)
{
    if ((text != NULL) == e->shown && (!text || strcmp(dst, text) == 0))
        return;
    e->shown = text != NULL;
    snprintf(dst, size, "%s", text ? text : "");
    e->version++;
}

void overlay_set_stats(const char *text) { set_text(&o.stats, o.stats_text, sizeof o.stats_text, text); }
void overlay_set_help(const char *text) { set_text(&o.help, o.help_text, sizeof o.help_text, text); }

void overlay_set_dot(bool show, int x, int y)
{
    if (show == o.dot.shown && (!show || (x == o.dot_x && y == o.dot_y)))
        return;
    o.dot.shown = show;
    o.dot_x = x;
    o.dot_y = y;
    o.dot.version++;
}

// --- Layout ---

static uint32_t next_char(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80)
        c = (c & 0x1f) << 6 | (*p++ & 0x3f);
    else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
        c = (c & 0x0f) << 12 | (p[0] & 0x3f) << 6 | (p[1] & 0x3f);
        p += 2;
    }
    *s = (const char *)p;
    return c;
}

static const struct glyph *glyph_for(uint32_t c)
{
    if (c == 0x2026)
        return &o.ellipsis;
    if (c == 0x2014)
        return &o.dash;
    return &o.ascii[c < 256 ? c : '?'];
}

// Size of a multi-line text block.
static void measure(const char *text, int *w, int *h)
{
    int lines = 1, x = 0, widest = 0;
    for (const char *s = text; *s;) {
        uint32_t c = next_char(&s);
        if (c == '\n') {
            lines++;
            x = 0;
            continue;
        }
        x += glyph_for(c)->advance;
        widest = SDL_max(widest, x);
    }
    *w = widest;
    *h = lines * o.line;
}

static int pad(void) { return (int)(o.line * 0.6f); }

// A panel's box for a text block, positioned by anchor: 0 = top left, 1 = top centre, 2 = centre.
static struct box panel_box(const char *text, int anchor, int win_w, int win_h)
{
    int tw, th;
    measure(text, &tw, &th);
    int p = pad(), w = tw + 2 * p, h = th + 2 * p, margin = (int)(o.line * 1.2f);
    switch (anchor) {
    case 0: return (struct box){margin, margin, w, h};
    case 1: return (struct box){(win_w - w) / 2, margin, w, h};
    default: return (struct box){(win_w - w) / 2, (win_h - h) / 2, w, h};
    }
}

static void update(struct element *e, struct box at)
{
    if (!e->shown)
        at = (struct box){0};
    if (e->version == e->drawn_version && memcmp(&at, &e->drawn, sizeof at) == 0)
        return;
    if (e->drawn.w)
        render_damage(e->drawn.x, e->drawn.y, e->drawn.w, e->drawn.h);
    if (at.w)
        render_damage(at.x, at.y, at.w, at.h);
    e->at = e->drawn = at;
    e->drawn_version = e->version;
}

Uint64 overlay_update(Uint64 now, int win_w, int win_h)
{
    if (!o.ready)
        return 0;
    Uint64 next = 0;
    if (o.alert.shown) {
        if (now >= o.alert_until) {
            o.alert.shown = false;
            o.alert.version++;
        } else {
            next = o.alert_until;
        }
    }
    if (o.waiting.shown) {
        unsigned dots = (unsigned)(now / 500000000ull) % 4;
        if (dots != o.dots) {
            o.dots = dots;
            o.waiting.version++;
        }
        Uint64 tick = (now / 500000000ull + 1) * 500000000ull;
        next = next ? SDL_min(next, tick) : tick;
        snprintf(o.waiting_text, sizeof o.waiting_text, "Waiting for the Mac%.*s%*s\n%s", (int)dots, "...",
                 3 - (int)dots, "", o.waiting_reason);
    }

    update(&o.dim, (struct box){0, 0, win_w, win_h});
    update(&o.waiting, panel_box(o.waiting_text, 2, win_w, win_h));
    struct box stats = panel_box(o.stats_text, 0, win_w, win_h), alert = panel_box(o.alert_text, 1, win_w, win_h);
    if (o.stats.shown && alert.x < stats.x + stats.w && stats.x < alert.x + alert.w)
        alert.y = stats.y + stats.h + pad(); // a narrow window: below the stats instead of on them
    update(&o.alert, alert);
    update(&o.stats, stats);
    update(&o.help, panel_box(o.help_text, 2, win_w, win_h));
    int r = (int)(5 * o.scale + 0.5f);
    update(&o.dot, (struct box){o.dot_x - r, o.dot_y - r, 2 * r, 2 * r});
    return next;
}

// --- Drawing ---

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float col[4])
{
    if (o.nquads == MAX_QUADS)
        return;
    float corners[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                           {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    float *v = o.verts + (size_t)o.nquads * 6 * 8;
    for (int i = 0; i < 6; i++, v += 8) {
        memcpy(v, corners[i], 4 * sizeof(float));
        memcpy(v + 4, col, 4 * sizeof(float));
    }
    o.nquads++;
}

static void solid(struct box b, const float col[4])
{
    quad((float)b.x, (float)b.y, (float)(b.x + b.w), (float)(b.y + b.h), -2, -2, -2, -2, col);
}

static void text(const char *s, int x, int y, const float col[4])
{
    int pen = x, baseline = y + o.ascent;
    while (*s) {
        uint32_t c = next_char(&s);
        if (c == '\n') {
            pen = x;
            baseline += o.line;
            continue;
        }
        const struct glyph *g = glyph_for(c);
        if (g->w)
            quad((float)(pen + g->left), (float)(baseline - g->top), (float)(pen + g->left + g->w),
                 (float)(baseline - g->top + g->h), g->u0, g->v0, g->u1, g->v1, col);
        pen += g->advance;
    }
}

static const float panel_bg[4] = {0.06f * 0.85f, 0.06f * 0.85f, 0.07f * 0.85f, 0.85f};
static const float panel_edge[4] = {0.3f * 0.6f, 0.3f * 0.6f, 0.32f * 0.6f, 0.6f};
static const float white[4] = {0.93f, 0.93f, 0.93f, 1};

static void panel(const struct element *e, const char *s)
{
    if (!e->shown)
        return;
    struct box b = e->at;
    solid(b, panel_edge);
    solid((struct box){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, panel_bg);
    text(s, b.x + pad(), b.y + pad(), white);
}

void overlay_draw(int win_w, int win_h)
{
    if (!o.ready)
        return;
    o.nquads = 0;
    if (o.dim.shown) {
        static const float dim[4] = {0, 0, 0, 0.6f};
        solid(o.dim.at, dim);
    }
    panel(&o.waiting, o.waiting_text);
    panel(&o.stats, o.stats_text);
    panel(&o.help, o.help_text);
    panel(&o.alert, o.alert_text);
    if (o.dot.shown) {
        struct box b = o.dot.at;
        quad((float)b.x, (float)b.y, (float)(b.x + b.w), (float)(b.y + b.h), -1, 2, 1, 4, white);
    }
    if (!o.nquads)
        return;

    glViewport(0, 0, win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(o.program);
    glUniform2f(o.window_loc, (float)win_w, (float)win_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, o.atlas);
    glBindVertexArray(o.vao);
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)o.nquads * 6 * 8 * sizeof(float), o.verts);
    glDrawArrays(GL_TRIANGLES, 0, o.nquads * 6);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}
