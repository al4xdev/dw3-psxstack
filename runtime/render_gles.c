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
    case GPU_EV_TRIANGLE:
    case GPU_EV_RECT:
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
    gles_rasterising = 1;
    gpu_set_listener(gles_event);
    gpu_set_listener_replaces(1);
    return 1;
}

int render_gles_rasterising(void) {
    return gles_rasterising;
}

/* Once per vsync. The transfers upload as they happen, so nothing is pending here yet; this is where the batch
 * flush, the read copy (blit of the dirty rectangle) and the display pass land as M2 and M3 arrive. */
void render_gles_frame(void) {
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
