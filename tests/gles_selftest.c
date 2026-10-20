/* tests/gles_selftest.c: the GLES renderer against the only oracle that matters, the shim's own VRAM.
 *
 * M1 draws no polygons yet, so what it must get byte-exactly right is the transfer path: LoadImage, Fill and CopyVRAM
 * land in the renderer's 1024x512 RGBA8 target, and reading it back has to reproduce the shim's 16-bit VRAM exactly -
 * including the 5-5-5-1 round trip, the mask bit in alpha, the 16-pixel rounding of a fill and the row order of an
 * overlapping copy.
 *
 * The shim is not linked here: the renderer only needs its VRAM (gpu_vram_pixels/gpu_vram_stride) and its listener
 * hook, so this test provides them and builds the same VRAM the shim would, using its documented rules. That keeps the
 * test runnable on the handheld (where EGL exists) without the rest of the runtime.
 *
 * Build (the same cross toolchain as the port):
 *   docker run --rm -v "$PWD:/src" -w /src dw3-armhf-builder:latest bash -c \
 *     'arm-linux-gnueabihf-gcc -O2 -marm -mcpu=cortex-a35 -mfpu=neon-fp-armv8 -mfloat-abi=hard \
 *      -DPSXSTACK_EGL -I psxstack/include -I psxstack/psyq -I psxstack/runtime \
 *      tests/gles_selftest.c psxstack/runtime/render_gles.c -o gles_selftest -lEGL -lGLESv2 -lgbm'
 * Then, on the console: ./gles_selftest
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psyq_internal.h"
#include "render_gles.h"

/* ---- the shim's side, as much of it as the renderer touches ---- */

#define VRAM_W 1024
#define VRAM_H 512
#define VRAM_STRIDE 1056 /* the port's padding: consecutive rows out of the same cache set (psyq/gpu.c) */

static u16 vram[VRAM_H][VRAM_STRIDE];
static void (*listener_fn)(const GpuEvent *ev);
static int listener_replaces;

const u16 *gpu_vram_pixels(void) {
    return &vram[0][0];
}

int gpu_vram_stride(void) {
    return VRAM_STRIDE;
}

void gpu_set_listener(void (*listener)(const GpuEvent *ev)) {
    listener_fn = listener;
}

void gpu_set_listener_replaces(int on) {
    listener_replaces = on != 0;
}

/* The clock the display readback's DW3_GLES_LOG uses; the test never enables it. */
long long port_clock_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

void port_log(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- the rules the shim applies, so the oracle is built the same way ---- */

/* LoadImage: the pixels as the CPU sends them, 16 bits each, wrapping at the edges; the mask bit OR-ed in. */
static void sw_load(int x, int y, int w, int h, u16 value_at(int, int), int set_mask) {
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            vram[(y + j) & 511][(x + i) & 1023] = (u16)(value_at(i, j) | set_mask);
        }
    }
}

/* Fill: x rounded down to 16, the width up to 16, no mask bit (gpu.c gpu_fill). */
static void sw_fill(int x, int y, int w, int h, u16 colour) {
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            vram[(y + j) & 511][(x + i) & 1023] = colour;
        }
    }
}

/* CopyVRAM: row by row, in order, so an overlapping copy sees its own earlier rows (gpu.c gpu_copy). */
static void sw_copy(int sx, int sy, int dx, int dy, int w, int h, int set_mask, int check_mask) {
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            u16 p = vram[(sy + j) & 511][(sx + i) & 1023];
            u16 *d = &vram[(dy + j) & 511][(dx + i) & 1023];

            if (check_mask && (*d & 0x8000)) {
                continue;
            }
            *d = (u16)(p | set_mask);
        }
    }
}

/* An untextured, opaque, undithered rectangle as gpu.c draws it: the 15-bit colour into every pixel inside the
 * drawing area, no mask bit unless set-mask is on. (gpu.c's gpu_rectangle with textured = semi = dither = 0.) */
static void sw_rect(int x, int y, int w, int h, int r, int g, int b, int area[4], int set_mask) {
    int i, j;
    u16 c = (u16)(((r >> 3) & 31) | (((g >> 3) & 31) << 5) | (((b >> 3) & 31) << 10) | (set_mask ? 0x8000 : 0));

    for (j = 0; j < h; j++) {
        int py = y + j;

        if (py < area[1] || py > area[3]) {
            continue;
        }
        for (i = 0; i < w; i++) {
            int px = x + i;

            if (px < area[0] || px > area[2]) {
                continue;
            }
            vram[py & 511][px & 1023] = c;
        }
    }
}

static void tell_rect(int x, int y, int w, int h, int r, int g, int b, int area[4], int set_mask) {
    GpuEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = GPU_EV_RECT;
    ev.x = x;
    ev.y = y;
    ev.w = w;
    ev.h = h;
    ev.v[0].x = x;
    ev.v[0].y = y;
    ev.v[0].r = r;
    ev.v[0].g = g;
    ev.v[0].b = b;
    ev.area_x0 = area[0];
    ev.area_y0 = area[1];
    ev.area_x1 = area[2];
    ev.area_y1 = area[3];
    ev.set_mask = set_mask;
    render_gles_feed(&ev);
}

static void tell(GpuEventKind kind, int x, int y, int w, int h, int set_mask, int check_mask) {
    GpuEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.x = x;
    ev.y = y;
    ev.w = w;
    ev.h = h;
    ev.set_mask = set_mask;
    ev.check_mask = check_mask;
    render_gles_feed(&ev);
}

/* ---- the test ---- */

static int compare(const char *what) {
    static u16 hw[VRAM_W * VRAM_H];
    int x, y, diff = 0;

    if (!render_gles_read_vram(hw)) {
        printf("   %-28s : read_vram FAILED\n", what);
        return 0;
    }
    for (y = 0; y < VRAM_H && diff < 8; y++) {
        for (x = 0; x < VRAM_W; x++) {
            if (hw[(size_t)y * VRAM_W + x] != vram[y][x]) {
                if (diff < 8) {
                    printf("   %-28s : diff at %d,%d sw 0x%04x hw 0x%04x\n", what, x, y, vram[y][x],
                           hw[(size_t)y * VRAM_W + x]);
                }
                diff++;
            }
        }
    }
    if (diff == 0) {
        printf("   %-28s : exact (%d x %d texels)\n", what, VRAM_W, VRAM_H);
        return 1;
    }
    printf("   %-28s : %s\n", what, "DIFFERS");
    return 0;
}

static u16 pattern(int i, int j) {
    /* every 5-bit field and the mask bit, so a shifted or swapped channel cannot pass */
    return (u16)(((i * 3) & 31) | (((j * 5) & 31) << 5) | (((i ^ j) & 31) << 10) | (((i + j) & 1) << 15));
}

int main(void) {
    char why[160] = {0};
    int ok = 1, i, j;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("GLES renderer self-test (M1 transfers, M2 rectangles, against the shim's VRAM)\n");
    printf("opening the EGL/GBM context...\n");
    if (!render_gles_open(why, sizeof(why))) {
        printf("FAIL: no renderer: %s\n", why);
        return 1;
    }
    printf("context: %s\n\n", render_gles_describe());
    printf("starting the VRAM target...\n");
    if (!render_gles_raster_start(1, why, sizeof(why))) {
        printf("FAIL: raster_start: %s\n", why);
        render_gles_close();
        return 1;
    }
    printf("listener installed, replaces=%d\n\n", listener_replaces);

    /* 1. LoadImage of the whole VRAM, the way a state load or a texture upload lands. */
    sw_load(0, 0, VRAM_W, VRAM_H, pattern, 0);
    tell(GPU_EV_LOAD, 0, 0, VRAM_W, VRAM_H, 0, 0);
    render_gles_frame();
    ok &= compare("LoadImage 1024x512");

    /* 2. A fill over a region, with the 16-pixel rounding the command implies, and a wrapping one. */
    sw_fill(320, 100, 64, 32, 0x7C1F);
    tell(GPU_EV_FILL, 320, 100, 64, 32, 0, 0);
    render_gles_frame();
    ok &= compare("Fill 64x32");

    sw_fill(1000, 500, 48, 20, 0x03E0);
    tell(GPU_EV_FILL, 1000, 500, 48, 20, 0, 0);
    render_gles_frame();
    ok &= compare("Fill wrapping the edge");

    /* 3. A copy that overlaps itself: the row order is what makes it exact. */
    sw_copy(10, 10, 12, 12, 64, 64, 0, 0);
    tell(GPU_EV_COPY, 12, 12, 64, 64, 0, 0);
    render_gles_frame();
    ok &= compare("Copy overlapping");

    /* 4. A checked copy: the pixels with the mask bit set must survive. The marked pixel is set through a transfer
     * (a LoadImage with the mask bit OR-ed in), never by writing the oracle's array: that would put the two sides in
     * different states, which is exactly what the first version of this test did and the comparison caught. */
    vram[300][40] = (u16)(pattern(5, 5) & ~0x8000);
    vram[300][41] = (u16)(pattern(6, 5) | 0x8000);
    tell(GPU_EV_LOAD, 40, 300, 2, 1, 0, 0); /* the values as they are, bit 15 included */
    sw_copy(40, 300, 500, 300, 2, 1, 0, 1);
    tell(GPU_EV_COPY, 500, 300, 2, 1, 0, 1);
    render_gles_frame();
    ok &= compare("Copy with check-mask");

    /* 5. A set-mask load: the bit is OR-ed into every pixel. */
    for (j = 0; j < 8; j++) {
        for (i = 0; i < 8; i++) {
            vram[400 + j][200 + i] = (u16)(pattern(i, j) | 0x8000);
        }
    }
    tell(GPU_EV_LOAD, 200, 400, 8, 8, 0x8000, 0);
    render_gles_frame();
    ok &= compare("LoadImage with set-mask");

    /* 6. M2: untextured rectangles. The risky part is the coordinate mapping (the row order and the half-pixel
     * offset), which a wrong sign or a missing 0.5 turns into a one-pixel shift the comparison cannot miss. */
    {
        int full[4] = {0, 0, 1023, 511};

        sw_rect(200, 300, 100, 50, 255, 0, 0, full, 0);
        tell_rect(200, 300, 100, 50, 255, 0, 0, full, 0);
        render_gles_frame();
        ok &= compare("Rect 100x50 red");

        /* A rectangle clipped by the drawing area: only the part inside it is drawn. */
        {
            int area[4] = {100, 100, 199, 199};

            sw_rect(150, 150, 100, 100, 0, 255, 0, area, 0);
            tell_rect(150, 150, 100, 100, 0, 255, 0, area, 0);
        }
        render_gles_frame();
        ok &= compare("Rect clipped to the area");

        /* Set-mask: every pixel drawn carries the mask bit. */
        sw_rect(500, 400, 16, 16, 0, 0, 255, full, 1);
        tell_rect(500, 400, 16, 16, 0, 0, 255, full, 0x8000);
        render_gles_frame();
        ok &= compare("Rect with set-mask");
    }

    /* 7. M3: textured rectangles. The VRAM is the texture here: a 4-bit sprite whose CLUT lives in the page's palette
     * row, and a 15-bit one read straight, both modulated by the vertex colour as the shim does ((t * c) >> 7). */
    {
        int full[4] = {0, 0, 1023, 511};
        int i2, j2;

        /* The palette at row 8, entry 3: colour 0x7C00 (blue), entry 5: 0x03E0 (green). */
        vram[8][3] = 0x7C00;
        vram[8][5] = 0x03E0;
        /* A 4-bit texture at (64, 16): nibbles 3,5,3,5... packed four per texel. */
        for (j2 = 0; j2 < 8; j2++) {
            for (i2 = 0; i2 < 8; i2++) {
                vram[16 + j2][64 + i2 / 4] = (u16)(0x5353 | ((j2 & 1) ? 0x3000 : 0));
            }
        }
        tell(GPU_EV_LOAD, 0, 0, VRAM_W, VRAM_H, 0, 0); /* the target takes the patterns above */
        render_gles_frame();
        {
            GpuEvent ev;

            memset(&ev, 0, sizeof(ev));
            ev.kind = GPU_EV_RECT;
            ev.x = 300;
            ev.y = 200;
            ev.w = 8;
            ev.h = 8;
            ev.textured = 1; /* 4-bit */
            ev.tex_x = 64;
            ev.tex_y = 16;
            ev.clut_x = 0;
            ev.clut_y = 8;
            ev.u_and = 0xFF;
            ev.u_or = 0;
            ev.v_and = 0xFF;
            ev.v_or = 0;
            ev.v[0].u = 0;
            ev.v[0].v = 0;
            ev.v[0].r = ev.v[0].g = ev.v[0].b = 128; /* the "no modulation" colour, as raw textures use */
            ev.area_x0 = full[0];
            ev.area_y0 = full[1];
            ev.area_x1 = full[2];
            ev.area_y1 = full[3];
            render_gles_feed(&ev);
        }
        render_gles_frame();
        /* What the shim would draw: for each pixel, the nibble's palette colour, (t * 128) >> 7 = t. */
        for (j2 = 0; j2 < 8; j2++) {
            for (i2 = 0; i2 < 8; i2++) {
                u16 packed = vram[16 + j2][64 + i2 / 4];
                int idx = (packed >> ((i2 & 3) * 4)) & 0xF;

                vram[200 + j2][300 + i2] = vram[8][idx];
            }
        }
        ok &= compare("Textured 4-bit sprite");
    }

    /* 8. Nothing drawn by the CPU: the polygons are counted, not pretended (M4 owns them). */
    {
        GpuEvent ev;

        memset(&ev, 0, sizeof(ev));
        ev.kind = GPU_EV_TRIANGLE;
        render_gles_feed(&ev);
    }
    printf("\nundrawn so far: %u triangle(s), %u rect(s), %u segment(s)\n", render_gles_undrawn(GPU_EV_TRIANGLE),
           render_gles_undrawn(GPU_EV_RECT), render_gles_undrawn(GPU_EV_SEGMENT));

    render_gles_close();
    printf("\n%s\n", ok ? "PASS: every transfer and rectangle reproduced the shim's VRAM exactly"
                            : "FAIL: see the diffs above");
    return ok ? 0 : 1;
}
