/* The GLES renderer (M1 of docs/gpu in the game repository): the VRAM as an RGBA8 texture on the Mali-G31, rendered
 * offscreen through EGL on a GBM device, with no window and no DRM master.
 *
 * Why offscreen: taking a window means SDL's KMSDRM swap, which costs 17 ms on this handheld (one 60 Hz frame of
 * waiting, docs/gpu/measurements.txt section 4), while writing the picture into /dev/fb0 costs 0.55-0.77 ms. So the
 * renderer never owns the display: it renders, hands the picture back, and the existing fb0 present shows it.
 *
 * What M1 does, and what it deliberately does not:
 *  - holds the VRAM target (RGBA8, 1024 x 512, one texel per VRAM pixel, alpha = the mask bit);
 *  - follows the shim's transfers (LoadImage, Fill, CopyVRAM) by uploading the rectangles they touched from the
 *    shim's own VRAM, which is exact and costs a dirty rect rather than a reasoning about wrap-around, masks and
 *    overlap. Those three are 0-2 per battle frame (docs/gpu/09), so a real GPU-side transfer buys nothing measurable
 *    yet - M5 turns Fill into a scissored clear and Copy into a blit when the profile asks for it;
 *  - installs itself as a listener that replaces the rasteriser, so the polygons are NOT drawn by the CPU once M2/M3
 *    exist (gpu_set_listener_replaces). Until then it counts them, so a test can see how much is left to implement;
 *  - reads the whole VRAM back (glReadPixels, 10.8 ms, only on a checkpoint) in the shim's 16-bit format.
 *
 * The present (display pass into RGB565, read back, memcpy into /dev/fb0) is M1d and lands with the video wiring. */
#ifdef PSXSTACK_EGL

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

#include "platform.h"
#include "port_harness.h"
#include "port_runtime.h"
#include "psyq_internal.h"
#include "render_gles.h"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

#define VRAM_W 1024
#define VRAM_H 512

static int gles_fd = -1;
static struct gbm_device *gles_gbm;
static EGLDisplay gles_dpy = EGL_NO_DISPLAY;
static EGLSurface gles_surf = EGL_NO_SURFACE;
static EGLContext gles_ctx = EGL_NO_CONTEXT;
static GLuint gles_vram;   /* the target: one texel per VRAM pixel, drawn into and sampled from the copy */
static GLuint gles_fbo;
static GLuint gles_read;   /* the read copy the future batch shader samples: a texture cannot be both */
static GLuint gles_read_fbo;
static int gles_open;
static int gles_rasterising;
static char gles_what[96];

/* What the listener was asked to draw but cannot yet: the M2/M3/M4 work, counted so a test can report it. */
static unsigned gles_ev_count[8];

/* The batch the drawing accumulates in (M2: untextured rectangles). It flushes when the state a draw depends on
 * changes - the drawing area (a scissor) and the mask flags - or when the vertex buffer fills, which is the same rule
 * the reference uses (gpu_hw.cpp:4280-4289). One draw call costs 11.2 us on this handheld, so a batch of one primitive
 * per call is not an option (docs/gpu/09 measures ~110 batches for ~1060 primitives per battle frame). */
typedef struct {
    float x, y;      /* VRAM pixel coordinates */
    float r, g, b;   /* 0..1, the flat colour */
    float mask;      /* 1.0 marks the pixel, 0.0 leaves it clear */
} GlesVertex;

#define GLES_BATCH_MAX 4096
static GlesVertex gles_batch[GLES_BATCH_MAX];
static int gles_batch_n;
static GLuint gles_prog_flat, gles_vbo;
static GLint gles_loc_pos = -1, gles_loc_col = -1;
static int gles_batch_area[4] = {-1, -1, -1, -1};
static int gles_batch_mask = -1;
static int gles_replace = 1; /* the listener draws instead of the software rasteriser (--renderer gles) */

/* 5-5-5-1 to 8-8-8-8, the packing the target holds: r, g, b scaled from 5 bits, alpha the mask bit. Scaled by 255/31
 * with rounding, which is what makes the round trip exact (docs/gpu/02). */
static void gles_expand(const u16 *src, int stride, int x, int y, int w, int h, u8 *dst) {
    int i, j;

    for (j = 0; j < h; j++) {
        const u16 *row = src + (size_t)(y + j) * (size_t)stride + x;
        u8 *out = dst + (size_t)j * (size_t)w * 4;

        for (i = 0; i < w; i++) {
            unsigned v = row[i];

            out[i * 4 + 0] = (u8)((((v >> 0) & 31) * 255 + 15) / 31);
            out[i * 4 + 1] = (u8)((((v >> 5) & 31) * 255 + 15) / 31);
            out[i * 4 + 2] = (u8)((((v >> 10) & 31) * 255 + 15) / 31);
            out[i * 4 + 3] = (u8)(((v >> 15) & 1) ? 255 : 0);
        }
    }
}

static int gles_upload_rect(int x, int y, int w, int h) {
    const u16 *vram = gpu_vram_pixels();
    int stride = gpu_vram_stride();
    u8 *rgba;

    if (w <= 0 || h <= 0) {
        return 1;
    }
    rgba = malloc((size_t)w * (size_t)h * 4);
    if (rgba == NULL) {
        return 0;
    }
    gles_expand(vram, stride, x, y, w, h, rgba);
    glBindTexture(GL_TEXTURE_2D, gles_vram);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    return 1;
}

/* The flat (untextured) rectangle shader: one colour per primitive, no blending, no dithering, the mask bit in alpha.
 * GLSL ES 1.00 on purpose - the blob gives an ES 3.2 context whatever is asked (docs/gpu/06), and a 1.00 shader is
 * what a plain ES 2.0 device would also take. */
static const char *GLES_FLAT_VS =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_col;\n" /* rgb + the mask bit in a */
    "varying vec4 v_col;\n"
    "void main() {\n"
    "  v_col = a_col;\n"
    "  /* a VRAM pixel centre in clip space: 1024 wide, 512 tall, row 0 at the bottom of the target */\n"
    "  gl_Position = vec4((a_pos.x + 0.5) / 512.0 - 1.0, (a_pos.y + 0.5) / 256.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *GLES_FLAT_FS =
    "precision highp float;\n"
    "varying vec4 v_col;\n"
    "void main() { gl_FragColor = v_col; }\n";

static GLuint gles_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};

        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        port_log("gles: shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Compiles the flat program and its buffer; 0 on failure (why says it). */
static int gles_pipelines(char *why, size_t why_size) {
    GLuint vs = gles_shader(GL_VERTEX_SHADER, GLES_FLAT_VS);
    GLuint fs = vs != 0 ? gles_shader(GL_FRAGMENT_SHADER, GLES_FLAT_FS) : 0;
    GLint ok = 0;

    if (fs == 0) {
        snprintf(why, why_size, "the flat shaders did not compile");
        return 0;
    }
    gles_prog_flat = glCreateProgram();
    glAttachShader(gles_prog_flat, vs);
    glAttachShader(gles_prog_flat, fs);
    glLinkProgram(gles_prog_flat);
    glGetProgramiv(gles_prog_flat, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        snprintf(why, why_size, "the flat program did not link");
        return 0;
    }
    gles_loc_pos = glGetAttribLocation(gles_prog_flat, "a_pos");
    gles_loc_col = glGetAttribLocation(gles_prog_flat, "a_col");
    glGenBuffers(1, &gles_vbo);
    return 1;
}

/* One draw call for everything accumulated. The scissor is the PS1 drawing area; the mask bit is the vertex alpha. */
static void gles_flush(void) {
    GLsizei n = (GLsizei)gles_batch_n;

    if (n <= 0 || gles_prog_flat == 0) {
        gles_batch_n = 0;
        return;
    }
    glUseProgram(gles_prog_flat);
    glBindBuffer(GL_ARRAY_BUFFER, gles_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * sizeof(GlesVertex)), gles_batch, GL_STREAM_DRAW);
    glEnableVertexAttribArray((GLuint)gles_loc_pos);
    glVertexAttribPointer((GLuint)gles_loc_pos, 2, GL_FLOAT, GL_FALSE, sizeof(GlesVertex), (void *)0);
    glEnableVertexAttribArray((GLuint)gles_loc_col);
    glVertexAttribPointer((GLuint)gles_loc_col, 4, GL_FLOAT, GL_FALSE, sizeof(GlesVertex),
                          (void *)(2 * sizeof(float)));
    glBindFramebuffer(GL_FRAMEBUFFER, gles_fbo);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glDrawArrays(GL_TRIANGLES, 0, n);
    gles_batch_n = 0;
}

/* The state a batch depends on. A change flushes: the drawing area becomes a scissor and the mask flags a shader
 * constant, so primitives with different ones cannot share a draw. */
static void gles_batch_state(const GpuEvent *ev) {
    int mask = ev->set_mask ? 1 : 0;
    int area[4] = {ev->area_x0, ev->area_y0, ev->area_x1, ev->area_y1};

    if (gles_batch_mask != mask || memcmp(gles_batch_area, area, sizeof(area)) != 0) {
        gles_flush();
        gles_batch_mask = mask;
        memcpy(gles_batch_area, area, sizeof(area));
        /* The drawing area is inclusive and clipped to the VRAM; empty when the corners are inverted. */
        {
            int x0 = area[0], y0 = area[1];
            int x1 = area[2] + 1, y1 = area[3] + 1;

            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > VRAM_W) x1 = VRAM_W;
            if (y1 > VRAM_H) y1 = VRAM_H;
            if (x1 <= x0 || y1 <= y0) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(0, 0, 0, 0);
            } else {
                glDisable(GL_SCISSOR_TEST);
                glEnable(GL_SCISSOR_TEST);
                glScissor(x0, y0, x1 - x0, y1 - y0);
            }
        }
    }
}

static void gles_vertex(float x, float y, float r, float g, float b, float mask) {
    GlesVertex *v;

    if (gles_batch_n >= GLES_BATCH_MAX) {
        gles_flush();
    }
    v = &gles_batch[gles_batch_n++];
    v->x = x;
    v->y = y;
    v->r = r;
    v->g = g;
    v->b = b;
    v->mask = mask;
}

/* An untextured rectangle: two triangles, clipped by the drawing area (the scissor) and the mask bit in alpha. */
static void gles_rect(const GpuEvent *ev) {
    float r = (float)ev->v[0].r / 255.0f, g = (float)ev->v[0].g / 255.0f, b = (float)ev->v[0].b / 255.0f;
    float mask = ev->set_mask ? 1.0f : 0.0f;
    float x0 = (float)ev->x, y0 = (float)ev->y;
    float x1 = x0 + (float)ev->w, y1 = y0 + (float)ev->h;

    gles_batch_state(ev);
    /* The PS1 wraps a rectangle's coordinates to the VRAM instead of clipping them; the scissor clips, so a rectangle
     * that runs past an edge is drawn up to it here and its wrapped part is not reproduced yet. Rectangles this game
     * draws inside the page are unaffected, and the VRAM check says so when one is not. */
    if (ev->w <= 0 || ev->h <= 0) {
        return;
    }
    gles_vertex(x0, y0, r, g, b, mask);
    gles_vertex(x1, y0, r, g, b, mask);
    gles_vertex(x0, y1, r, g, b, mask);
    gles_vertex(x1, y0, r, g, b, mask);
    gles_vertex(x1, y1, r, g, b, mask);
    gles_vertex(x0, y1, r, g, b, mask);
}

/* A transfer writes its rectangle with every pixel's coordinates masked to the VRAM's edges (gpu.c's gpu_fill,
 * gpu_copy and gpu_load_pixel all do `& 1023` / `& 511`), so a rectangle that runs past an edge lands wrapped. The
 * upload has to split it the same way: one glTexSubImage2D cannot write outside the texture. At most four pieces. */
static void gles_upload_wrapped(int x, int y, int w, int h) {
    int x0[2], x1[2], y0[2], y1[2];
    int nx = 0, ny = 0, i, j;

    if (w <= 0 || h <= 0) {
        return;
    }
    x &= 1023;
    y &= 511;
    x0[nx] = x;
    x1[nx] = x + w > VRAM_W ? VRAM_W : x + w;
    nx++;
    if (x + w > VRAM_W) {
        x0[nx] = 0;
        x1[nx] = (x + w) & 1023;
        if (x1[nx] == 0) {
            x1[nx] = VRAM_W; /* an exact multiple wrapped onto the start: the whole row */
        }
        nx++;
    }
    y0[ny] = y;
    y1[ny] = y + h > VRAM_H ? VRAM_H : y + h;
    ny++;
    if (y + h > VRAM_H) {
        y0[ny] = 0;
        y1[ny] = (y + h) & 511;
        if (y1[ny] == 0) {
            y1[ny] = VRAM_H;
        }
        ny++;
    }
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            /* The source coordinates come from the start of the write, which is what the shim's own loop walks. */
            gles_upload_rect(x0[i], y0[j], x1[i] - x0[i], y1[j] - y0[j]);
        }
    }
}

/* The listener: gpu.c calls this instead of rasterising (gpu_set_listener_replaces). The transfers are uploaded at
 * once, in order, so they interleave with the drawing that follows; everything else is counted until its milestone
 * exists. */
static void gles_event(const GpuEvent *ev) {
    if (!gles_rasterising) {
        return;
    }
    switch (ev->kind) {
    case GPU_EV_LOAD:
    case GPU_EV_FILL:
    case GPU_EV_COPY:
        /* One upload per transfer: the shim's VRAM already holds the result, including the wrap-around, the mask rules
         * and the row order of an overlapping copy, so the target cannot disagree with it. */
        gles_upload_wrapped(ev->x, ev->y, ev->w, ev->h);
        break;
    case GPU_EV_POWER_ON:
        gles_upload_rect(0, 0, VRAM_W, VRAM_H);
        break;
    case GPU_EV_RECT:
        if (ev->textured) {
            gles_ev_count[ev->kind]++; /* a sprite waits for M3's CLUT sampling */
        } else if (ev->semi) {
            gles_ev_count[ev->kind]++; /* semi-transparent: M4's fragment fetch */
        } else {
            gles_rect(ev);
        }
        break;
    case GPU_EV_TRIANGLE:
    case GPU_EV_SEGMENT:
        gles_ev_count[ev->kind]++;
        break;
    }
}

const char *render_gles_describe(void) {
    return gles_open ? gles_what : "";
}

int render_gles_active(void) {
    return gles_open;
}

/* Offscreen context: an EGL display on a GBM device and a pbuffer, which needs no window and no DRM master (spike #8,
 * docs/gpu/measurements.txt). 0 with the reason in why. */
int render_gles_open(char *why, size_t why_size) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    static const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                      EGL_NONE};
    static const EGLint pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint ncfg = 0, major = 0, minor = 0;
    const char *version, *renderer;

    if (gles_open) {
        return 1;
    }
    gles_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (gles_fd < 0) {
        gles_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (gles_fd < 0) {
            snprintf(why, why_size, "no /dev/dri node (errno %d)", errno);
            return 0;
        }
    }
    gles_gbm = gbm_create_device(gles_fd);
    if (gles_gbm == NULL) {
        snprintf(why, why_size, "gbm_create_device failed");
        goto fail;
    }
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display == NULL) {
        snprintf(why, why_size, "eglGetPlatformDisplayEXT is not in this libEGL");
        goto fail;
    }
    gles_dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gles_gbm, NULL);
    if (gles_dpy == EGL_NO_DISPLAY) {
        snprintf(why, why_size, "eglGetPlatformDisplayEXT(GBM) failed: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    if (!eglInitialize(gles_dpy, &major, &minor)) {
        snprintf(why, why_size, "eglInitialize failed: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        snprintf(why, why_size, "eglBindAPI(ES) failed: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    if (!eglChooseConfig(gles_dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        snprintf(why, why_size, "no pbuffer config: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    gles_surf = eglCreatePbufferSurface(gles_dpy, cfg, pb_attr);
    if (gles_surf == EGL_NO_SURFACE) {
        snprintf(why, why_size, "eglCreatePbufferSurface failed: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    gles_ctx = eglCreateContext(gles_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (gles_ctx == EGL_NO_CONTEXT) {
        /* The blob reports ES 3.2 whatever is asked for (docs/gpu/06), but keep a 2.0 fallback for other drivers. */
        static const EGLint ctx2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

        gles_ctx = eglCreateContext(gles_dpy, cfg, EGL_NO_CONTEXT, ctx2);
    }
    if (gles_ctx == EGL_NO_CONTEXT) {
        snprintf(why, why_size, "eglCreateContext failed: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    if (!eglMakeCurrent(gles_dpy, gles_surf, gles_surf, gles_ctx)) {
        snprintf(why, why_size, "eglMakeCurrent failed: 0x%x", (unsigned)eglGetError());
        goto fail;
    }
    version = (const char *)glGetString(GL_VERSION);
    renderer = (const char *)glGetString(GL_RENDERER);
    snprintf(gles_what, sizeof(gles_what), "EGL %d.%d, %s, %s", major, minor, renderer != NULL ? renderer : "?",
             version != NULL ? version : "?");
    gles_open = 1;
    return 1;

fail:
    render_gles_close();
    return 0;
}

int render_gles_raster_start(int scale, char *why, size_t why_size) {
    GLenum st;

    if (!gles_open) {
        snprintf(why, why_size, "no EGL context");
        return 0;
    }
    if (scale != 1) {
        /* M1 and the milestones after it draw at native resolution: the 2x belongs to the present, where it costs
         * nothing and keeps the picture pixel-exact (docs/gpu/08). */
        snprintf(why, why_size, "internal scale %d is not implemented (native only for now)", scale);
        return 0;
    }
    if (gles_rasterising) {
        return 1;
    }
    glGenTextures(1, &gles_vram);
    glBindTexture(GL_TEXTURE_2D, gles_vram);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_W, VRAM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &gles_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gles_vram, 0);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(why, why_size, "VRAM target incomplete: 0x%x", (unsigned)st);
        render_gles_close();
        return 0;
    }
    glGenTextures(1, &gles_read);
    glBindTexture(GL_TEXTURE_2D, gles_read);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_W, VRAM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &gles_read_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_read_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gles_read, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_fbo);
    /* The target starts as the VRAM is now (the shim's power-on state or a loaded state). */
    gles_upload_rect(0, 0, VRAM_W, VRAM_H);
    if (!gles_pipelines(why, why_size)) {
        render_gles_close();
        return 0;
    }
    gles_batch_n = 0;
    gles_batch_mask = -1;
    gles_batch_area[0] = gles_batch_area[1] = gles_batch_area[2] = gles_batch_area[3] = -1;
    gles_rasterising = 1;
    gpu_set_listener(gles_event);
    gpu_set_listener_replaces(gles_replace);
    return 1;
}

void render_gles_set_replace(int on) {
    gles_replace = on != 0;
}

int render_gles_rasterising(void) {
    return gles_rasterising;
}

/* Once per vsync. The transfers upload as they happen, so nothing is pending here yet; this is where the batch
 * flush, the read copy (blit of the dirty rectangle) and the display pass land as M2 and M3 arrive. */
void render_gles_frame(void) {
    if (gles_rasterising) {
        gles_flush();
    }
}

/* The display area back as 16-bit pixels, for the present that already exists (runtime/fbdev_present.c does the 2x
 * nearest scaling and the 15-to-16-bit expansion with NEON, 0.77 ms measured). Reading 320x240 costs 1.7-1.95 ms
 * (docs/gpu/measurements.txt): cheaper than the 17 ms a window swap would cost, and it leaves the present path, the
 * screenshots and the 24-bit modes exactly as they are. 0 on failure. */
int render_gles_read_display(u16 *out, int x, int y, int w, int h) {
    u8 *rgba;
    int i, j;
    /* DW3_GLES_LOG=1: what the display readback costs, split in its two halves (the driver's glReadPixels and the
     * 8-to-5-bit conversion below), once a second. The present is where a measurement surprised: 9.8 ms on the
     * handheld against the 1.7 the spike predicted. */
    static long long log_sec;
    static unsigned long long log_read, log_conv;
    static unsigned log_n;
    static int log_on = -1;
    long long t_read = 0;

    if (!gles_rasterising || out == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    if (log_on < 0) {
        const char *env = getenv("DW3_GLES_LOG");

        log_on = env != NULL && env[0] == '1';
    }
    rgba = malloc((size_t)w * (size_t)h * 4);
    if (rgba == NULL) {
        return 0;
    }
    if (log_on) {
        t_read = (long long)port_clock_ns();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gles_fbo);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (log_on) {
        long long t1 = (long long)port_clock_ns();

        log_read += (unsigned long long)(t1 - t_read);
        t_read = t1;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            const u8 *p = rgba + ((size_t)j * w + i) * 4;
            unsigned r = ((unsigned)p[0] * 31 + 127) / 255;
            unsigned g = ((unsigned)p[1] * 31 + 127) / 255;
            unsigned b = ((unsigned)p[2] * 31 + 127) / 255;

            out[(size_t)j * w + i] = (u16)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10));
        }
    }
    free(rgba);
    if (log_on) {
        long long now = (long long)port_clock_ns();

        log_conv += (unsigned long long)(now - t_read);
        log_n++;
        if (log_sec == 0) {
            log_sec = now;
        } else if (now - log_sec >= 1000000000LL) {
            port_log("gles: present readback %.2f ms (glReadPixels %.2f, 5-bit conversion %.2f), %u presents",
                     (double)(log_read + log_conv) / log_n / 1e6, (double)log_read / log_n / 1e6,
                     (double)log_conv / log_n / 1e6, log_n);
            log_read = log_conv = 0;
            log_n = 0;
            log_sec = now;
        }
    }
    return 1;
}

/* The whole target back as the shim's 16-bit pixels: the exactness test's comparison (and, later, the checkpoint).
 * glReadPixels of 1024x512 costs 10.8 ms on the handheld, so this is a save-time operation, never per frame. */
int render_gles_read_vram(u16 *out) {
    u8 *rgba;
    int x, y;

    if (!gles_rasterising || out == NULL) {
        return 0;
    }
    rgba = malloc((size_t)VRAM_W * VRAM_H * 4);
    if (rgba == NULL) {
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gles_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (y = 0; y < VRAM_H; y++) {
        for (x = 0; x < VRAM_W; x++) {
            const u8 *p = rgba + ((size_t)y * VRAM_W + x) * 4;
            unsigned r = ((unsigned)p[0] * 31 + 127) / 255;
            unsigned g = ((unsigned)p[1] * 31 + 127) / 255;
            unsigned b = ((unsigned)p[2] * 31 + 127) / 255;

            out[(size_t)y * VRAM_W + x] = (u16)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10) | (p[3] >= 128 ? 0x8000 : 0));
        }
    }
    free(rgba);
    return 1;
}

/* The events the listener could not draw yet, by GpuEventKind: M1 reports them instead of pretending. */
unsigned render_gles_undrawn(int kind) {
    return (kind >= 0 && kind < 8) ? gles_ev_count[kind] : 0;
}

void render_gles_feed(const void *ev) {
    gles_event((const GpuEvent *)ev);
}

void render_gles_close(void) {
    if (gles_rasterising) {
        gpu_set_listener_replaces(0);
        gpu_set_listener(NULL);
        gles_rasterising = 0;
    }
    if (gles_vbo != 0) {
        glDeleteBuffers(1, &gles_vbo);
        gles_vbo = 0;
    }
    if (gles_prog_flat != 0) {
        glDeleteProgram(gles_prog_flat);
        gles_prog_flat = 0;
    }
    if (gles_vram != 0) {
        glDeleteFramebuffers(1, &gles_fbo);
        glDeleteTextures(1, &gles_vram);
        gles_fbo = gles_vram = 0;
    }
    if (gles_read != 0) {
        glDeleteFramebuffers(1, &gles_read_fbo);
        glDeleteTextures(1, &gles_read);
        gles_read_fbo = gles_read = 0;
    }
    if (gles_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(gles_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gles_ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(gles_dpy, gles_ctx);
        }
        if (gles_surf != EGL_NO_SURFACE) {
            eglDestroySurface(gles_dpy, gles_surf);
        }
        eglTerminate(gles_dpy);
    }
    gles_ctx = EGL_NO_CONTEXT;
    gles_surf = EGL_NO_SURFACE;
    gles_dpy = EGL_NO_DISPLAY;
    if (gles_gbm != NULL) {
        gbm_device_destroy(gles_gbm);
        gles_gbm = NULL;
    }
    if (gles_fd >= 0) {
        close(gles_fd);
        gles_fd = -1;
    }
    gles_open = 0;
    gles_what[0] = '\0';
}

#endif /* PSXSTACK_EGL */
