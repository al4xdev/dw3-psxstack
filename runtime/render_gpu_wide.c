/* The hardware renderer's widescreen (docs/PORT.md "Rendering", DECISIONS "Widescreen: a wide canvas beside the
 * VRAM"): a 4:3 display shown 16:9, with more of the scene at both sides, without a change to what the game draws.
 *
 * A game that projects its 3D through the GTE usually sends geometry past the display's edges (its culling keeps
 * whatever may reach the screen, with a margin) and the GPU clips it to the drawing area. The PS1's VRAM has no room
 * for a wider picture (textures sit beside the display buffers), so the rasteriser keeps a canvas per display buffer
 * in a strip of its target right of the VRAM (render_gpu_wide.h; the target is 1024 + 512 VRAM pixels wide while
 * widescreen is enabled) and draws every unit it draws into a display buffer a second time there:
 *  - translated by (dx, dy), so that the buffer's left edge lands P pixels into the canvas: P = ceil(w / 6), which
 *    makes the canvas w + 2P wide, 16:9 at the 4:3 display's pixel aspect (320 -> 428). dx and dy keep the 4x4
 *    dither's phase; the canvas's rows are a 256-line slot, so two buffers share the strip;
 *  - with the drawing area widened by P on each side where it reaches the buffer's edge (the layers that draw the
 *    whole screen); an area inside the buffer (a window's clip) keeps its size;
 *  - an untextured triangle whose vertices all lie on the buffer's left and right edges, or an untextured rectangle
 *    that covers the buffer's width (the background clear, fades, flashes: full-screen 2D), is stretched to the
 *    canvas's edges. Textured 2D keeps its size: the picture's 2D stays centred as drawn;
 *  - a line segment, a transfer and a VRAM copy into the buffer are copied from the VRAM part once drawn (their
 *    pixels inside the buffer; gpu.c's line walk clips to its own drawing area).
 * The VRAM part is drawn as before, so the target's VRAM equals the software VRAM at the internal scale of 1 and the
 * canvas's middle equals the 4:3 picture. A triangle at sub-pixel positions (render_gpu_subpixel.c) keeps its
 * fractions in the copy (the event's vertices move by whole pixels); its float positions near x 1100 are coarser than
 * near x 0, so a few edge pixels may land on the other side (measured: 35 of 1,228,800 in a battle frame at scale 4;
 * 0 with sub-pixel vertices off). A display buffer gets its canvas when widescreen is on and the display or a
 * drawing area of the display's size is there: the canvas is cleared to black and the buffer's picture copied into
 * it, so a picture drawn before is shown pillarboxed until it is drawn again.
 *
 * The game decides when (GAME_CONTRACT.md "4. The adapter units", the mods): port_video_widescreen_enable before the
 * window opens, port_video_widescreen(on) every vsync (video.c), on for the scenes that send the geometry. A 24-bit
 * display, a display wider than the strip allows, and the software renderer stay 4:3. */
#ifdef PSXSTACK_SDL
#include <string.h>

#include "port_harness.h"
#include "psyq.h"
#include "render_gpu.h"
#include "render_gpu_wide.h"

#define WIDE_X0 1024 /* the strip's first column */
#define WIDE_SLOTS 2
#define WIDE_SLOT_H 256
#define VRAM_W 1024
#define VRAM_H 512

typedef struct {
    int used;
    int x, y;           /* the display buffer in the VRAM (w x h: the display's size) */
    int cx, cy;         /* where its left edge lands in the canvas (target VRAM pixels) */
    unsigned long last; /* the vsync it was last used (the slot a third buffer replaces) */
} WideSlot;

static struct {
    int enabled; /* the target has the strip */
    int on;      /* the scenes that widen: port_video_widescreen */
    int w, h;    /* the display's size the slots are for */
    int pad;     /* P: the columns added on each side */
    unsigned long vsync;
    WideSlot slot[WIDE_SLOTS];
} wide;

void render_gpu_wide_enable(void) {
    wide.enabled = 1;
}

int render_gpu_wide_enabled(void) {
    return wide.enabled;
}

/* The slot of the display buffer at (x, y), or NULL. */
static WideSlot *wide_find(int x, int y) {
    int k;
    for (k = 0; k < WIDE_SLOTS; k++) {
        if (wide.slot[k].used && wide.slot[k].x == x && wide.slot[k].y == y) {
            return &wide.slot[k];
        }
    }
    return NULL;
}

/* The slot whose buffer contains the rectangle [x0, x1] x [y0, y1] (inclusive), or NULL. */
static WideSlot *wide_containing(int x0, int y0, int x1, int y1) {
    int k;
    for (k = 0; k < WIDE_SLOTS; k++) {
        const WideSlot *s = &wide.slot[k];
        if (s->used && x0 >= s->x && x1 < s->x + wide.w && y0 >= s->y && y1 < s->y + wide.h) {
            return &wide.slot[k];
        }
    }
    return NULL;
}

/* The display buffer at (x, y) gets a canvas: the first free slot (the one of its own 256 lines first), else the one
 * used longest ago; the canvas is cleared and the buffer's pixels copied in. NULL when the buffer does not fit. */
static WideSlot *wide_add(int x, int y) {
    WideSlot *s = wide_find(x, y);
    int k, pick = -1;
    if (s != NULL) {
        return s;
    }
    if (x < 0 || y < 0 || x + wide.w > VRAM_W || y + wide.h > VRAM_H) {
        return NULL;
    }
    k = (y / WIDE_SLOT_H) % WIDE_SLOTS;
    if (!wide.slot[k].used) {
        pick = k;
    }
    for (k = 0; pick < 0 && k < WIDE_SLOTS; k++) {
        if (!wide.slot[k].used) {
            pick = k;
        }
    }
    if (pick < 0) {
        pick = wide.slot[0].last <= wide.slot[1].last ? 0 : 1;
    }
    s = &wide.slot[pick];
    /* dx and dy are multiples of 4: the dither's phase (raster.frag.hlsl, on the VRAM pixel) stays the buffer's */
    s->cx = WIDE_X0 + wide.pad + ((x - WIDE_X0 - wide.pad) & 3);
    s->cy = pick * WIDE_SLOT_H + ((y - pick * WIDE_SLOT_H) & 3);
    if (s->cy + wide.h > (pick + 1) * WIDE_SLOT_H) {
        return NULL;
    }
    s->used = 1;
    s->x = x;
    s->y = y;
    s->last = wide.vsync;
    render_gpu_unit_fill(s->cx - wide.pad, s->cy, wide.w + 2 * wide.pad, wide.h, 0);
    render_gpu_unit_mirror(x, y, s->cx, s->cy, wide.w, wide.h);
    return s;
}

/* The display's size: the slots start over when it changes; 0 when it cannot be shown wide. */
static int wide_display(PsyqDisplay *d) {
    int pad;
    psyq_gpu_display(d);
    if (!d->enabled || d->rgb24 || d->w <= 0 || d->h <= 0) {
        return 0;
    }
    pad = (d->w + 5) / 6;
    if (d->w + 2 * pad + 3 > RENDER_WIDE_COLS || d->h > WIDE_SLOT_H) {
        return 0;
    }
    if (d->w != wide.w || d->h != wide.h) {
        memset(wide.slot, 0, sizeof(wide.slot));
        wide.w = d->w;
        wide.h = d->h;
        wide.pad = pad;
    }
    return 1;
}

void render_gpu_wide_set(int on) {
    PsyqDisplay d;
    wide.vsync++;
    on = on && wide.enabled && render_gpu_rasterising();
    if (!on) {
        if (wide.on) {
            memset(wide.slot, 0, sizeof(wide.slot)); /* drawn again from the next time it is on */
        }
        wide.on = 0;
        return;
    }
    wide.on = 1;
    if (wide_display(&d)) {
        wide_add(d.x & 1023, d.y & 511);
    }
}

int render_gpu_wide_pad(void) {
    PsyqDisplay d;
    return wide.on && wide_display(&d) ? wide.pad : 0;
}

int render_gpu_wide_width(const int vram_xy[2], int w, int h) {
    PsyqDisplay d;
    WideSlot *s;
    if (!wide.on || !wide_display(&d) || w != wide.w || h != wide.h) {
        return 0;
    }
    s = wide_add(vram_xy[0] & 1023, vram_xy[1] & 511);
    if (s == NULL) {
        return 0;
    }
    s->last = wide.vsync;
    return w + 2 * wide.pad;
}

int render_gpu_wide_cut(const int vram_xy[2], int w, int h, int cut[2]) {
    const WideSlot *s;
    if (!wide.on || h != wide.h || w != wide.w + 2 * wide.pad) {
        return 0;
    }
    s = wide_find(vram_xy[0] & 1023, vram_xy[1] & 511);
    if (s == NULL) {
        return 0;
    }
    cut[0] = s->cx - wide.pad;
    cut[1] = s->cy;
    return 1;
}

/* A drawing event's buffer: the one its drawing area lies in. */
static WideSlot *wide_area_slot(const GpuEvent *ev) {
    return wide_containing(ev->area_x0, ev->area_y0, ev->area_x1, ev->area_y1);
}

void render_gpu_wide_before(const GpuEvent *ev) {
    if (!wide.on || wide.w == 0) {
        return;
    }
    if ((ev->kind == GPU_EV_TRIANGLE || ev->kind == GPU_EV_RECT || ev->kind == GPU_EV_SEGMENT) &&
        ev->area_x1 - ev->area_x0 + 1 == wide.w && ev->area_y1 - ev->area_y0 + 1 == wide.h) {
        wide_add(ev->area_x0, ev->area_y0); /* a layer that draws the whole screen: a display buffer */
    }
}

/* The pixels [x, x + w) x [y, y + h) of the VRAM part copied into the canvas of the buffer they lie in (clipped). */
static void wide_mirror(int x, int y, int w, int h) {
    int k;
    for (k = 0; k < WIDE_SLOTS; k++) {
        const WideSlot *s = &wide.slot[k];
        int x0 = x > s->x ? x : s->x, y0 = y > s->y ? y : s->y;
        int x1 = x + w < s->x + wide.w ? x + w : s->x + wide.w, y1 = y + h < s->y + wide.h ? y + h : s->y + wide.h;
        if (s->used && x0 < x1 && y0 < y1) {
            render_gpu_unit_mirror(x0, y0, x0 - s->x + s->cx, y0 - s->y + s->cy, x1 - x0, y1 - y0);
        }
    }
}

/* Full-screen 2D: an untextured triangle whose vertices all lie on the buffer's left or right edge (at most 8 pixels
 * past it), or an untextured rectangle across its width, is stretched to the canvas's edges. */
static int wide_full_width(const GpuEvent *ev, const WideSlot *s) {
    int i, left = s->x, right = s->x + wide.w;
    if (ev->textured) {
        return 0;
    }
    if (ev->kind == GPU_EV_RECT) {
        return ev->x <= left && ev->x + ev->w >= right;
    }
    for (i = 0; i < 3; i++) {
        int x = ev->v[i].x;
        if (!((x <= left && x >= left - 8) || (x >= right && x <= right + 8))) {
            return 0;
        }
    }
    return 1;
}

void render_gpu_wide_event(const GpuEvent *ev) {
    WideSlot *s;
    GpuEvent t;
    int dx, dy, i;

    if (!wide.on || wide.w == 0) {
        return;
    }
    switch (ev->kind) {
    case GPU_EV_TRIANGLE:
    case GPU_EV_RECT:
        s = wide_area_slot(ev);
        if (s == NULL) {
            return;
        }
        dx = s->cx - s->x;
        dy = s->cy - s->y;
        t = *ev;
        t.area_x0 += ev->area_x0 == s->x ? dx - wide.pad : dx;
        t.area_x1 += ev->area_x1 == s->x + wide.w - 1 ? dx + wide.pad : dx;
        t.area_y0 += dy;
        t.area_y1 += dy;
        if (wide_full_width(ev, s)) {
            if (ev->kind == GPU_EV_RECT) {
                t.x -= wide.pad;
                t.w += 2 * wide.pad;
                t.v[0].x -= wide.pad;
            } else {
                for (i = 0; i < 3; i++) {
                    t.v[i].x += t.v[i].x <= s->x ? -wide.pad : wide.pad;
                }
            }
        }
        for (i = 0; i < 3; i++) {
            t.v[i].x += dx;
            t.v[i].y += dy;
        }
        t.x += dx;
        t.y += dy;
        render_gpu_unit_event(&t, ev->area_x0 + dx);
        break;
    case GPU_EV_SEGMENT:
        if (wide_area_slot(ev) != NULL) {
            int x0 = ev->v[0].x < ev->v[1].x ? ev->v[0].x : ev->v[1].x;
            int y0 = ev->v[0].y < ev->v[1].y ? ev->v[0].y : ev->v[1].y;
            int x1 = ev->v[0].x > ev->v[1].x ? ev->v[0].x : ev->v[1].x;
            int y1 = ev->v[0].y > ev->v[1].y ? ev->v[0].y : ev->v[1].y;
            x0 = x0 > ev->area_x0 ? x0 : ev->area_x0;
            y0 = y0 > ev->area_y0 ? y0 : ev->area_y0;
            x1 = x1 < ev->area_x1 ? x1 : ev->area_x1;
            y1 = y1 < ev->area_y1 ? y1 : ev->area_y1;
            if (x0 <= x1 && y0 <= y1) {
                wide_mirror(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            }
        }
        break;
    case GPU_EV_FILL:
        /* a fill ignores the drawing area: one inside a buffer is drawn into its canvas too (across it: widened) */
        s = wide_containing(ev->x, ev->y, ev->x + ev->w - 1, ev->y + ev->h - 1);
        if (s == NULL) {
            wide_mirror(ev->x, ev->y, ev->w, ev->h); /* after the fill's own unit: what it left there */
        } else if (ev->x == s->x && ev->w >= wide.w) {
            render_gpu_unit_fill(s->cx - wide.pad, ev->y - s->y + s->cy, wide.w + 2 * wide.pad, ev->h, ev->color);
        } else {
            render_gpu_unit_fill(ev->x - s->x + s->cx, ev->y - s->y + s->cy, ev->w, ev->h, ev->color);
        }
        break;
    case GPU_EV_COPY:
    case GPU_EV_LOAD:
        wide_mirror(ev->x & 1023, ev->y & (gpu_vram_rows() - 1), ev->w, ev->h);
        break;
    case GPU_EV_POWER_ON:
        memset(wide.slot, 0, sizeof(wide.slot));
        break;
    }
}
#endif
