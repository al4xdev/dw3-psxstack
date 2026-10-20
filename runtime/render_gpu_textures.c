/* Texture keys and the texture dump (render_gpu_textures.h; docs/PORT.md "Texture replacement"; the first game's
 * issue #70, its research comment for the measurements).
 *
 * A key names what a textured unit samples by content, not by place: games move the same image around the VRAM (the
 * first game loads one content at up to 11 places) and reuse a place for many contents. It is
 *  - the image: the SHA-1 (its first 8 bytes) of the CPU-to-VRAM transfer the unit's texels came from, taken when the
 *    transfer finished (gpu.c's listener reports it after its last pixel), with the transfer's size;
 *  - the CLUT: the SHA-1 of the 16 or 256 entries the unit reads, at the draw (CLUT rows come from transfers of many
 *    rows and from reused slots, so only their content names them); none for 15-bit textures;
 *  - the depth, 4, 8 or 15: one transfer may hold images of two depths.
 * Which transfer a unit samples comes from an owner map: each VRAM word holds the sequence number of the transfer that
 * wrote it, and every other write (a draw's bounding box within the drawing area, a fill, a copy's destination)
 * clears its words, so a unit has a key only when every word under its texel rectangle is still one transfer's. That
 * is per word on purpose: a game loads small images into part of an earlier page and keeps drawing from the rest
 * (30% of the first game's textured draws). The map's 16x16 tiles record whether they hold any transfer's words, so a
 * draw into the display buffer costs one check per tile. Render-to-texture and copies have no key: they keep the
 * VRAM's own texels.
 *
 * The dump (`--dump-textures DIR`): a key is written the first time a unit samples it (its CLUT and depth are known
 * only then), from the transfer's pixels as they were loaded (kept for the dump, since a later transfer may cover part
 * of them), as DIR/<overlay>/<image>-<clut>-<4|8>bpp-<w>x<h>.png, an indexed PNG whose palette is the CLUT, or
 * DIR/<overlay>/<image>-15bpp-<w>x<h>.png (RGBA). Colours are 5-bit channels widened as c << 3 | c >> 2; alpha is 0
 * for the texel 0x0000 (transparent), 128 for one with bit 15 set (semi-transparent where the primitive is) and 255
 * otherwise. <overlay> is the tier-1 overlay that first sampled it ("main" without one): the folders are for people.
 * Files that exist are kept (a dump continues over sessions), and DIR/index.json lists every key with where it was
 * seen and the part of it that units sampled (the texture of an atlas is mostly other sprites' colours under one
 * CLUT); it is read back at the start and rewritten at the end of every vsync that added a key, and at exit. */
#ifdef PSXSTACK_SDL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "json.h"
#include "platform.h"
#include "port_runtime.h"
#include "render_gpu.h"
#include "render_gpu_textures.h"
#include "sha1.h"

#define VRAM_W 1024
#define VRAM_H 512
#define TILE 16
#define RING 65536          /* the transfers remembered: a word of an older one has no key */
#define CLUT_CACHE 256
#define MAX_OVERLAYS 8

typedef struct {
    u32 seq;                /* 0: free */
    u64 hash;
    int x, y, w, h;         /* in VRAM words */
} TexLoad;

/* A transfer's pixels as loaded (the dump's), by content. */
typedef struct {
    u64 hash;
    int w, h;
    u16 *pixels;
} TexImage;

/* A key of the dump and its index entry. */
typedef struct {
    RenderTexKey key;
    char file[160];         /* relative to the dump's directory */
    long first_vsync;
    char overlays[MAX_OVERLAYS][32];
    int noverlays;
    int vram[4], clut_xy[2];
    int uv[4];              /* the texels units sampled, in the transfer's own texels: u0, v0, u1, v1 inclusive */
    double draws;
    int semi;
} TexEntry;

typedef struct {
    u32 x, y, n, generation, stamp;
    int valid;
    u64 hash;
} ClutCache;

static struct {
    int active;             /* keys tracked: a dump or a pack */
    int dump;
    char dir[1024];
    u32 own[VRAM_H][VRAM_W]; /* the transfer's sequence number, 0: none */
    u8 tile[VRAM_H / TILE][VRAM_W / TILE];
    u32 seq;
    TexLoad loads[RING];
    ClutCache cluts[CLUT_CACHE];
    TexImage *images;       /* open addressing by hash (the dump's) */
    size_t nimages, capimages;
    TexEntry *entries;
    size_t nentries, capentries;
    u32 *slots;             /* open addressing: entry index + 1, 0 empty */
    size_t capslots;
    int added;              /* keys added since the index was written */
    int changed;            /* counts or ranges changed since */
} t;

static u64 tex_digest64(PortSha1 *c) {
    u8 d[20];
    u64 v = 0;
    int i;
    port_sha1_final(c, d);
    for (i = 0; i < 8; i++) {
        v = v << 8 | d[i];
    }
    return v;
}

/* Words of the VRAM row y from x (wrapping), little-endian, into the hash. */
static void tex_hash_row(PortSha1 *c, const u16 *vram, int x, int y, int w) {
    u8 buf[2 * VRAM_W];
    int i;
    for (i = 0; i < w; i++) {
        u16 p = vram[(y & 511) * psyq_gpu_vram_stride() + ((x + i) & 1023)];
        buf[2 * i] = (u8)p;
        buf[2 * i + 1] = (u8)(p >> 8);
    }
    port_sha1_update(c, buf, (size_t)w * 2);
}

static int tex_wrap(int d, int size) {
    return ((d + size / 2) & (size - 1)) - size / 2;
}

/* ---- The owner map ---- */

static void own_set(int x, int y, int w, int h, u32 seq) {
    int i, j;
    w = SDL_min(w, VRAM_W);
    h = SDL_min(h, VRAM_H);
    for (j = 0; j < h; j++) {
        int yy = (y + j) & 511;
        for (i = 0; i < w; i++) {
            int xx = (x + i) & 1023;
            t.own[yy][xx] = seq;
            t.tile[yy / TILE][xx / TILE] = 1;
        }
    }
}

/* Words no transfer owns any more, over a rectangle clipped to the VRAM. */
static void own_clear_clipped(int x0, int y0, int x1, int y1) {
    int tx, ty;
    for (ty = y0 / TILE; ty <= y1 / TILE; ty++) {
        for (tx = x0 / TILE; tx <= x1 / TILE; tx++) {
            int cx0, cy0, cx1, cy1, y;
            if (!t.tile[ty][tx]) {
                continue;
            }
            cx0 = SDL_max(x0, tx * TILE);
            cy0 = SDL_max(y0, ty * TILE);
            cx1 = SDL_min(x1, tx * TILE + TILE - 1);
            cy1 = SDL_min(y1, ty * TILE + TILE - 1);
            for (y = cy0; y <= cy1; y++) {
                memset(&t.own[y][cx0], 0, (size_t)(cx1 - cx0 + 1) * sizeof(u32));
            }
            if (cx1 - cx0 + 1 == TILE && cy1 - cy0 + 1 == TILE) {
                t.tile[ty][tx] = 0;
            }
        }
    }
}

/* The same over a rectangle that wraps at the VRAM's edges. */
static void own_clear(int x, int y, int w, int h) {
    int xs[2][2], ys[2][2], nx = 1, ny = 1, i, j;
    if (w <= 0 || h <= 0) {
        return;
    }
    x &= 1023;
    y &= 511;
    w = SDL_min(w, VRAM_W);
    h = SDL_min(h, VRAM_H);
    xs[0][0] = x, xs[0][1] = SDL_min(x + w, VRAM_W) - 1;
    if (x + w > VRAM_W) {
        xs[1][0] = 0, xs[1][1] = x + w - VRAM_W - 1, nx = 2;
    }
    ys[0][0] = y, ys[0][1] = SDL_min(y + h, VRAM_H) - 1;
    if (y + h > VRAM_H) {
        ys[1][0] = 0, ys[1][1] = y + h - VRAM_H - 1, ny = 2;
    }
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            own_clear_clipped(xs[i][0], ys[j][0], xs[i][1], ys[j][1]);
        }
    }
}

/* A draw's bounding box within its drawing area. */
static void own_clear_drawn(const GpuEvent *ev, int x0, int y0, int x1, int y1) {
    x0 = SDL_max(SDL_max(x0, ev->area_x0), 0);
    y0 = SDL_max(SDL_max(y0, ev->area_y0), 0);
    x1 = SDL_min(SDL_min(x1, ev->area_x1), VRAM_W - 1);
    y1 = SDL_min(SDL_min(y1, ev->area_y1), VRAM_H - 1);
    if (x0 <= x1 && y0 <= y1) {
        own_clear_clipped(x0, y0, x1, y1);
    }
}

/* ---- The dump's images and entries ---- */

static TexImage *image_find(u64 hash, int w, int h, int add) {
    size_t i, mask;
    if (t.capimages == 0 || (add && (t.nimages + 1) * 2 > t.capimages)) {
        size_t cap = t.capimages ? t.capimages * 2 : 1024, k;
        TexImage *n = calloc(cap, sizeof(*n));
        if (n == NULL) {
            port_fatal("textures: out of memory (%zu images)", cap);
        }
        for (k = 0; k < t.capimages; k++) {
            if (t.images[k].pixels != NULL) {
                size_t j = (size_t)t.images[k].hash & (cap - 1);
                while (n[j].pixels != NULL) {
                    j = (j + 1) & (cap - 1);
                }
                n[j] = t.images[k];
            }
        }
        free(t.images);
        t.images = n;
        t.capimages = cap;
    }
    mask = t.capimages - 1;
    for (i = (size_t)hash & mask; t.images[i].pixels != NULL; i = (i + 1) & mask) {
        if (t.images[i].hash == hash && t.images[i].w == w && t.images[i].h == h) {
            return &t.images[i];
        }
    }
    if (!add) {
        return NULL;
    }
    t.images[i].hash = hash;
    t.images[i].w = w;
    t.images[i].h = h;
    t.images[i].pixels = malloc((size_t)w * (size_t)h * sizeof(u16));
    if (t.images[i].pixels == NULL) {
        port_fatal("textures: out of memory (an image of %dx%d)", w, h);
    }
    t.nimages++;
    return &t.images[i];
}

static size_t key_slot_hash(const RenderTexKey *k) {
    u64 v = k->image * 0x9E3779B97F4A7C15ull ^ k->clut ^ (u64)k->depth << 56 ^ (u64)k->w << 32 ^ (u64)k->h;
    return (size_t)(v ^ v >> 29);
}

static int key_equal(const RenderTexKey *a, const RenderTexKey *b) {
    return a->image == b->image && a->clut == b->clut && a->depth == b->depth && a->w == b->w && a->h == b->h;
}

static TexEntry *entry_find(const RenderTexKey *k, int *created) {
    size_t i, mask;
    *created = 0;
    if ((t.nentries + 1) * 2 > t.capslots) {
        size_t cap = t.capslots ? t.capslots * 2 : 4096, e;
        u32 *n = calloc(cap, sizeof(*n));
        if (n == NULL) {
            port_fatal("textures: out of memory (%zu keys)", cap);
        }
        for (e = 0; e < t.nentries; e++) {
            size_t j = key_slot_hash(&t.entries[e].key) & (cap - 1);
            while (n[j] != 0) {
                j = (j + 1) & (cap - 1);
            }
            n[j] = (u32)e + 1;
        }
        free(t.slots);
        t.slots = n;
        t.capslots = cap;
    }
    mask = t.capslots - 1;
    for (i = key_slot_hash(k) & mask; t.slots[i] != 0; i = (i + 1) & mask) {
        if (key_equal(&t.entries[t.slots[i] - 1].key, k)) {
            return &t.entries[t.slots[i] - 1];
        }
    }
    if (t.nentries == t.capentries) {
        size_t cap = t.capentries ? t.capentries * 2 : 1024;
        TexEntry *n = realloc(t.entries, cap * sizeof(*n));
        if (n == NULL) {
            port_fatal("textures: out of memory (%zu keys)", cap);
        }
        t.entries = n;
        t.capentries = cap;
    }
    t.slots[i] = (u32)t.nentries + 1;
    memset(&t.entries[t.nentries], 0, sizeof(TexEntry));
    t.entries[t.nentries].key = *k;
    *created = 1;
    return &t.entries[t.nentries++];
}

static void key_name(const RenderTexKey *k, char *out, size_t size) {
    if (k->depth == 15) {
        snprintf(out, size, "%016llx-15bpp-%dx%d.png", (unsigned long long)k->image, k->w, k->h);
    } else {
        snprintf(out, size, "%016llx-%016llx-%dbpp-%dx%d.png", (unsigned long long)k->image,
                 (unsigned long long)k->clut, k->depth, k->w, k->h);
    }
}

static SDL_Color tex_color(u16 p) {
    SDL_Color c;
    int r5 = p & 31, g5 = (p >> 5) & 31, b5 = (p >> 10) & 31;
    c.r = (Uint8)(r5 << 3 | r5 >> 2);
    c.g = (Uint8)(g5 << 3 | g5 >> 2);
    c.b = (Uint8)(b5 << 3 | b5 >> 2);
    c.a = p == 0 ? 0 : (p & 0x8000) ? 128 : 255;
    return c;
}

/* The key's PNG, unless the file exists: the transfer's pixels through the CLUT (read from the VRAM now). */
static void dump_write(const TexEntry *e, const GpuEvent *ev, const TexImage *img) {
    char path[1400];
    FILE *f;
    SDL_Surface *s;
    int x, y;
    const RenderTexKey *k = &e->key;

    snprintf(path, sizeof(path), "%s/%s", t.dir, e->file);
    if ((f = fopen(path, "rb")) != NULL) {
        fclose(f);
        return;
    }
    {
        char sub[1400];
        const char *sep;
        snprintf(sub, sizeof(sub), "%s", path);
        sep = port_path_last_sep(sub);
        if (sep != NULL) {
            sub[sep - sub] = 0;
            port_make_dirs(sub);
        }
    }
    if (k->depth == 15) {
        s = SDL_CreateSurface(k->w, k->h, SDL_PIXELFORMAT_RGBA32);
        if (s == NULL) {
            port_log("textures: %s: %s", e->file, SDL_GetError());
            return;
        }
        for (y = 0; y < k->h; y++) {
            for (x = 0; x < k->w; x++) {
                SDL_Color c = tex_color(img->pixels[y * img->w + x]);
                Uint8 *d = (Uint8 *)s->pixels + y * s->pitch + x * 4;
                d[0] = c.r, d[1] = c.g, d[2] = c.b, d[3] = c.a;
            }
        }
    } else {
        int n = k->depth == 4 ? 16 : 256, shift = k->depth == 4 ? 2 : 1, bits = k->depth;
        SDL_Color pal[256];
        SDL_Palette *p;
        const u16 *vram = gpu_vram_pixels();
        s = SDL_CreateSurface(k->w, k->h, SDL_PIXELFORMAT_INDEX8);
        p = s != NULL ? SDL_CreateSurfacePalette(s) : NULL;
        if (p == NULL) {
            port_log("textures: %s: %s", e->file, SDL_GetError());
            SDL_DestroySurface(s);
            return;
        }
        for (x = 0; x < n; x++) {
            pal[x] = tex_color(vram[ev->clut_y * psyq_gpu_vram_stride() + ((ev->clut_x + x) & 1023)]);
        }
        SDL_SetPaletteColors(p, pal, 0, n);
        for (y = 0; y < k->h; y++) {
            for (x = 0; x < k->w; x++) {
                u16 word = img->pixels[y * img->w + (x >> shift)];
                ((Uint8 *)s->pixels)[y * s->pitch + x] = (Uint8)((word >> ((x & ((1 << shift) - 1)) * bits)) & (n - 1));
            }
        }
    }
    if (!SDL_SavePNG(s, path)) {
        port_log("textures: %s: %s", path, SDL_GetError());
    }
    SDL_DestroySurface(s);
}

static void index_write(void) {
    char path[1100], tmp[1100];
    FILE *f;
    size_t i;
    snprintf(path, sizeof(path), "%s/index.json", t.dir);
    snprintf(tmp, sizeof(tmp), "%s/index.json.tmp", t.dir);
    f = fopen(tmp, "w");
    if (f == NULL) {
        port_log("textures: cannot write %s", tmp);
        return;
    }
    fprintf(f, "{\n  \"schema\": 1,\n  \"textures\": [");
    for (i = 0; i < t.nentries; i++) {
        const TexEntry *e = &t.entries[i];
        int o;
        fprintf(f, "%s\n    { \"file\": ", i ? "," : "");
        port_json_write_string(f, e->file);
        fprintf(f, ", \"image\": \"%016llx\"", (unsigned long long)e->key.image);
        if (e->key.depth != 15) {
            fprintf(f, ", \"clut\": \"%016llx\"", (unsigned long long)e->key.clut);
        }
        fprintf(f, ", \"depth\": %d, \"size\": [%d, %d], \"first_vsync\": %ld, \"overlays\": [", e->key.depth, e->key.w,
                e->key.h, e->first_vsync);
        for (o = 0; o < e->noverlays; o++) {
            fprintf(f, "%s", o ? ", " : "");
            port_json_write_string(f, e->overlays[o]);
        }
        fprintf(f, "], \"vram\": [%d, %d, %d, %d]", e->vram[0], e->vram[1], e->vram[2], e->vram[3]);
        if (e->key.depth != 15) {
            fprintf(f, ", \"clut_xy\": [%d, %d]", e->clut_xy[0], e->clut_xy[1]);
        }
        fprintf(f, ", \"uv\": [%d, %d, %d, %d], \"draws\": %.0f, \"semi\": %s }", e->uv[0], e->uv[1], e->uv[2],
                e->uv[3], e->draws, e->semi ? "true" : "false");
    }
    fprintf(f, "\n  ]\n}\n");
    if (fclose(f) != 0 || port_file_replace(tmp, path) != 0) {
        port_log("textures: cannot write %s", path);
    }
    t.added = 0;
    t.changed = 0;
}

static int json_ints(const PortJson *a, int *out, size_t n) {
    size_t i;
    if (a == NULL || a->type != PORT_JSON_ARRAY || a->count != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (a->items[i].type != PORT_JSON_NUMBER) {
            return 0;
        }
        out[i] = (int)a->items[i].number;
    }
    return 1;
}

/* An index from an earlier session: its entries are kept (their files are). */
static void index_read(void) {
    char path[1100], err[256];
    FILE *f;
    long size;
    char *text;
    PortJson *root;
    const PortJson *list;
    size_t i;

    snprintf(path, sizeof(path), "%s/index.json", t.dir);
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = size > 0 ? malloc((size_t)size) : NULL;
    if (text == NULL || fread(text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(text);
        port_log("textures: %s: unreadable; started over", path);
        return;
    }
    fclose(f);
    root = port_json_parse(text, (size_t)size, err, sizeof(err));
    free(text);
    list = port_json_get(root, "textures");
    if (list == NULL || list->type != PORT_JSON_ARRAY) {
        port_log("textures: %s: %s; started over", path, root == NULL ? err : "no \"textures\" list");
        port_json_free(root);
        return;
    }
    for (i = 0; i < list->count; i++) {
        const PortJson *it = &list->items[i];
        const PortJson *file = port_json_get(it, "file"), *image = port_json_get(it, "image");
        const PortJson *clut = port_json_get(it, "clut"), *depth = port_json_get(it, "depth");
        const PortJson *first = port_json_get(it, "first_vsync"), *draws = port_json_get(it, "draws");
        const PortJson *ovl = port_json_get(it, "overlays"), *semi = port_json_get(it, "semi");
        RenderTexKey k;
        int size2[2], created;
        TexEntry *e;
        memset(&k, 0, sizeof(k));
        if (file == NULL || file->type != PORT_JSON_STRING || image == NULL || image->type != PORT_JSON_STRING ||
            depth == NULL || depth->type != PORT_JSON_NUMBER || !json_ints(port_json_get(it, "size"), size2, 2)) {
            continue;
        }
        k.image = strtoull(image->string, NULL, 16);
        k.clut = clut != NULL && clut->type == PORT_JSON_STRING ? strtoull(clut->string, NULL, 16) : 0;
        k.depth = (int)depth->number;
        k.w = size2[0];
        k.h = size2[1];
        e = entry_find(&k, &created);
        if (!created) {
            continue;
        }
        snprintf(e->file, sizeof(e->file), "%s", file->string);
        e->first_vsync = first != NULL && first->type == PORT_JSON_NUMBER ? (long)first->number : 0;
        e->draws = draws != NULL && draws->type == PORT_JSON_NUMBER ? draws->number : 0;
        e->semi = semi != NULL && semi->type == PORT_JSON_BOOL && semi->boolean;
        json_ints(port_json_get(it, "vram"), e->vram, 4);
        json_ints(port_json_get(it, "clut_xy"), e->clut_xy, 2);
        if (!json_ints(port_json_get(it, "uv"), e->uv, 4)) {
            e->uv[0] = e->uv[1] = 0;
            e->uv[2] = e->uv[3] = -1;
        }
        if (ovl != NULL && ovl->type == PORT_JSON_ARRAY) {
            size_t o;
            for (o = 0; o < ovl->count && e->noverlays < MAX_OVERLAYS; o++) {
                if (ovl->items[o].type == PORT_JSON_STRING) {
                    snprintf(e->overlays[e->noverlays++], sizeof(e->overlays[0]), "%s", ovl->items[o].string);
                }
            }
        }
    }
    port_log("textures: %s: %zu keys from earlier dumps", path, t.nentries);
    port_json_free(root);
}

static const char *overlay_name(void) {
    const PortOverlay *o = port_overlay_current(1);
    return o != NULL ? o->name : "main";
}

/* A unit with a key, in the dump: its entry (made, and its PNG written, the first time), the counts and the range. */
static void dump_unit(const GpuEvent *ev, const RenderTexKey *k, int u_lo, int u_hi, int v_lo, int v_hi) {
    int created, o;
    const char *ovl = overlay_name();
    TexEntry *e = entry_find(k, &created);
    int lu0 = u_lo - k->u0, lu1 = u_hi - k->u0, lv0 = v_lo - k->v0, lv1 = v_hi - k->v0;

    if (created) {
        char name[96];
        const TexImage *img;
        key_name(k, name, sizeof(name));
        snprintf(e->file, sizeof(e->file), "%s/%s", ovl, name);
        e->first_vsync = port_frames;
        e->vram[0] = k->vx, e->vram[1] = k->vy;
        e->vram[2] = k->depth == 4 ? k->w / 4 : k->depth == 8 ? k->w / 2 : k->w;
        e->vram[3] = k->h;
        e->clut_xy[0] = ev->clut_x, e->clut_xy[1] = ev->clut_y;
        e->uv[0] = lu0, e->uv[1] = lv0, e->uv[2] = lu1, e->uv[3] = lv1;
        img = image_find(k->image, e->vram[2], k->h, 0);
        if (img != NULL) {
            dump_write(e, ev, img);
        }
        t.added = 1;
    } else if (e->uv[2] < e->uv[0]) {
        e->uv[0] = lu0, e->uv[1] = lv0, e->uv[2] = lu1, e->uv[3] = lv1;
    } else {
        e->uv[0] = SDL_min(e->uv[0], lu0);
        e->uv[1] = SDL_min(e->uv[1], lv0);
        e->uv[2] = SDL_max(e->uv[2], lu1);
        e->uv[3] = SDL_max(e->uv[3], lv1);
    }
    e->draws += 1;
    e->semi |= ev->semi;
    for (o = 0; o < e->noverlays && strcmp(e->overlays[o], ovl) != 0; o++) {
    }
    if (o == e->noverlays && o < MAX_OVERLAYS) {
        snprintf(e->overlays[e->noverlays++], sizeof(e->overlays[0]), "%s", ovl);
    }
    t.changed = 1;
}

/* ---- Keys ---- */

static u64 clut_hash(int x, int y, int n) {
    const u32 *stamps = gpu_block_stamps();
    ClutCache *c = &t.cluts[((unsigned)x * 7 + (unsigned)y * 13 + (unsigned)n) & (CLUT_CACHE - 1)];
    const u16 *vram = gpu_vram_pixels();
    PortSha1 sha;
    int b, i, fresh;

    fresh = c->valid && c->x == (u32)x && c->y == (u32)y && c->n == (u32)n && c->generation == gpu_stamp_generation();
    for (b = x >> 6; fresh && b <= (x + n - 1) >> 6; b++) {
        fresh = stamps[y * 16 + (b & 15)] <= c->stamp;
    }
    if (fresh) {
        return c->hash;
    }
    c->stamp = gpu_stamp_bump(); /* later writes carry newer stamps than every block has now */
    c->generation = gpu_stamp_generation();
    port_sha1_init(&sha);
    {
        u8 buf[512];
        for (i = 0; i < n; i++) {
            u16 p = vram[y * psyq_gpu_vram_stride() + ((x + i) & 1023)];
            buf[2 * i] = (u8)p;
            buf[2 * i + 1] = (u8)(p >> 8);
        }
        port_sha1_update(&sha, buf, (size_t)n * 2);
    }
    c->hash = tex_digest64(&sha);
    c->x = (u32)x, c->y = (u32)y, c->n = (u32)n;
    c->valid = 1;
    return c->hash;
}

int render_gpu_tex_key(const GpuEvent *ev, int u_lo, int u_hi, int v_lo, int v_hi, RenderTexKey *key) {
    static const int shifts[4] = { 0, 2, 1, 0 };
    int shift, x0, x1, i, j;
    u32 seq;
    const TexLoad *l;

    if (!t.active || !ev->textured) {
        return 0;
    }
    shift = shifts[ev->textured];
    x0 = u_lo >> shift;
    x1 = u_hi >> shift;
    seq = t.own[(ev->tex_y + v_lo) & 511][(ev->tex_x + x0) & 1023];
    if (seq == 0 || t.loads[seq & (RING - 1)].seq != seq) {
        return 0;
    }
    for (j = v_lo; j <= v_hi; j++) {
        const u32 *row = t.own[(ev->tex_y + j) & 511];
        for (i = x0; i <= x1; i++) {
            if (row[(ev->tex_x + i) & 1023] != seq) {
                return 0;
            }
        }
    }
    l = &t.loads[seq & (RING - 1)];
    key->image = l->hash;
    key->depth = ev->textured == 1 ? 4 : ev->textured == 2 ? 8 : 15;
    key->w = l->w << shift;
    key->h = l->h;
    key->u0 = tex_wrap(l->x - ev->tex_x, VRAM_W) * (1 << shift);
    key->v0 = tex_wrap(l->y - ev->tex_y, VRAM_H);
    key->vx = l->x;
    key->vy = l->y;
    key->clut = ev->textured < 3 ? clut_hash(ev->clut_x, ev->clut_y, ev->textured == 1 ? 16 : 256) : 0;
    return 1;
}

/* A textured unit's texel range (gpu.c's: the vertices' range, a triangle's far edges not sampled; a rectangle's
 * texels exactly; the texture window's when one is set) into range (u_lo, u_hi, v_lo, v_hi), and its key. */
static int unit_range_key(const GpuEvent *ev, int umin, int umax, int vmin, int vmax, int range[4], RenderTexKey *k) {
    if (ev->u_and != 0xFF || ev->u_or != 0) {
        umin = ev->u_or;
        umax = ev->u_or | (~ev->u_and & 0xFF);
    }
    if (ev->v_and != 0xFF || ev->v_or != 0) {
        vmin = ev->v_or;
        vmax = ev->v_or | (~ev->v_and & 0xFF);
    }
    if (umin < 0 || umax > 255 || umin > umax) {
        umin = 0, umax = 255;
    }
    if (vmin < 0 || vmax > 255 || vmin > vmax) {
        vmin = 0, vmax = 255;
    }
    range[0] = umin, range[1] = umax, range[2] = vmin, range[3] = vmax;
    return render_gpu_tex_key(ev, umin, umax, vmin, vmax, k);
}

int render_gpu_tex_unit_key(const GpuEvent *ev, RenderTexKey *key, int range[4]) {
    if (!t.active || !ev->textured) {
        return 0;
    }
    if (ev->kind == GPU_EV_TRIANGLE) {
        const GpuVertex *a = &ev->v[0], *b = &ev->v[1], *c = &ev->v[2];
        int umin = SDL_min(a->u, SDL_min(b->u, c->u)), umax = SDL_max(a->u, SDL_max(b->u, c->u));
        int vmin = SDL_min(a->v, SDL_min(b->v, c->v)), vmax = SDL_max(a->v, SDL_max(b->v, c->v));
        return unit_range_key(ev, umin, umax > umin ? umax - 1 : umax, vmin, vmax > vmin ? vmax - 1 : vmax, range,
                              key);
    }
    if (ev->kind == GPU_EV_RECT && ev->w > 0 && ev->h > 0) {
        return unit_range_key(ev, ev->v[0].u, ev->v[0].u + ev->w - 1, ev->v[0].v, ev->v[0].v + ev->h - 1, range, key);
    }
    return 0;
}

void render_gpu_tex_event(const GpuEvent *ev) {
    RenderTexKey k;
    int range[4];
    if (!t.active) {
        return;
    }
    switch (ev->kind) {
    case GPU_EV_TRIANGLE: {
        const GpuVertex *a = &ev->v[0], *b = &ev->v[1], *c = &ev->v[2];
        if (t.dump && render_gpu_tex_unit_key(ev, &k, range)) {
            dump_unit(ev, &k, range[0], range[1], range[2], range[3]);
        }
        own_clear_drawn(ev, SDL_min(a->x, SDL_min(b->x, c->x)), SDL_min(a->y, SDL_min(b->y, c->y)),
                        SDL_max(a->x, SDL_max(b->x, c->x)), SDL_max(a->y, SDL_max(b->y, c->y)));
        break;
    }
    case GPU_EV_RECT:
        if (t.dump && render_gpu_tex_unit_key(ev, &k, range)) {
            dump_unit(ev, &k, range[0], range[1], range[2], range[3]);
        }
        own_clear_drawn(ev, ev->x, ev->y, ev->x + ev->w - 1, ev->y + ev->h - 1);
        break;
    case GPU_EV_SEGMENT:
        own_clear_drawn(ev, SDL_min(ev->v[0].x, ev->v[1].x), SDL_min(ev->v[0].y, ev->v[1].y),
                        SDL_max(ev->v[0].x, ev->v[1].x), SDL_max(ev->v[0].y, ev->v[1].y));
        break;
    case GPU_EV_FILL:
    case GPU_EV_COPY:
        own_clear(ev->x, ev->y, ev->w, ev->h);
        break;
    case GPU_EV_LOAD: {
        const u16 *vram = gpu_vram_pixels();
        int w = SDL_min(ev->w, VRAM_W), h = SDL_min(ev->h, VRAM_H), j;
        PortSha1 sha;
        TexLoad *l;
        if (w <= 0 || h <= 0) {
            break;
        }
        port_sha1_init(&sha);
        for (j = 0; j < h; j++) {
            tex_hash_row(&sha, vram, ev->x, ev->y + j, w);
        }
        if (++t.seq == 0) {
            t.seq = 1;
        }
        l = &t.loads[t.seq & (RING - 1)];
        l->seq = t.seq;
        l->hash = tex_digest64(&sha);
        l->x = ev->x & 1023;
        l->y = ev->y & 511;
        l->w = w;
        l->h = h;
        own_set(l->x, l->y, w, h, t.seq);
        if (t.dump && image_find(l->hash, w, h, 0) == NULL) {
            TexImage *img = image_find(l->hash, w, h, 1);
            for (j = 0; j < h; j++) {
                int i;
                for (i = 0; i < w; i++) {
                    img->pixels[j * w + i] = vram[((l->y + j) & 511) * psyq_gpu_vram_stride() + ((l->x + i) & 1023)];
                }
            }
        }
        break;
    }
    case GPU_EV_POWER_ON:
        memset(t.own, 0, sizeof(t.own));
        memset(t.tile, 0, sizeof(t.tile));
        break;
    }
}

int render_gpu_tex_active(void) {
    return t.active;
}

int render_gpu_tex_dump_open(const char *dir) {
    if (port_make_dirs(dir) != 0) {
        port_log("textures: cannot make %s", dir);
        return 0;
    }
    snprintf(t.dir, sizeof(t.dir), "%s", dir);
    index_read();
    t.dump = 1;
    render_gpu_tex_track();
    port_log("textures: dumping to %s", dir);
    return 1;
}

void render_gpu_tex_track(void) {
    if (t.active) {
        return;
    }
    t.active = 1;
    if (!render_gpu_rasterising()) {
        gpu_set_listener(render_gpu_tex_event); /* the rasteriser's listener forwards to it once it runs */
    }
}

void render_gpu_tex_frame(void) {
    if (t.dump && t.added) {
        index_write();
    }
}

void render_gpu_tex_close(void) {
    size_t i;
    if (!t.active) {
        return;
    }
    if (t.dump) {
        if (t.added || t.changed) {
            index_write();
        }
        port_log("textures: %zu keys in %s/index.json", t.nentries, t.dir);
    }
    for (i = 0; i < t.capimages; i++) {
        free(t.images[i].pixels);
    }
    free(t.images);
    free(t.entries);
    free(t.slots);
    memset(&t, 0, sizeof(t));
    if (!render_gpu_rasterising()) {
        gpu_set_listener(NULL);
    }
}
#endif
