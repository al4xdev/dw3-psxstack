/* The video output (M2; docs/PORT.md "Rendering", DECISIONS "PC port architecture"): what the PS1's video
 * DAC shows, the display area of the VRAM (psyq.h "The video output": psyq_gpu_vram, psyq_gpu_display), converted once
 * per vsync into 32-bit pixels at the display's own size. Two consumers:
 *
 * - The window (`--window`; only in a build configured with -DPSXSTACK_SDL=ON, which defines PSXSTACK_SDL): an SDL3
 *   streaming texture updated and presented every vsync, drawn at 4:3 with nearest-neighbour filtering, as large as
 *   an integer multiple of the image's lines fits (`--scale N`: a 320*N x 240*N window; `--fullscreen`; F11 toggles):
 *   with an even N a 240-line and a 480-line display come out the same size. input.c reads the window's events;
 *   pump.c paces the vsyncs to real time (50 Hz) in window mode.
 * - Screenshots (`--screenshot FRAME:PATH`, any build): the image of vsync FRAME as a binary PPM (P6), exactly the
 *   pixels the window's texture gets. They depend on nothing but the VRAM and the display area (no host state), so
 *   two runs, or a window and a headless run, give the same bytes. The debug channel's screenshot is the same image.
 *
 * The renderer (`--renderer software|gpu`, video.renderer; issue #31): `software` presents through SDL_Renderer as
 * above; `gpu` through the hardware renderer (render_gpu.c, SDL_GPU), opened before any SDL_Renderer (a window that
 * had one cannot be claimed by Vulkan on Wayland). When it cannot open (no device, no presentable surface: NVIDIA
 * on the offscreen driver, a shader) the run logs why and falls back to SDL_Renderer. Its rasteriser draws the
 * game into a VRAM of its own (render_gpu.c) and a 15-bit display is presented from there (the same picture at the
 * internal scale of 1); a 24-bit display and the disabled one are video_pixels, as above. Every vsync runs the
 * rasteriser's units, presented or not (port_video_frame). `--gpu-screenshot FRAME[@WxH]:PATH` (SDL build) writes
 * the hardware renderer's picture of vsync FRAME: the image itself, or with @WxH its present into a W x H output (the
 * window's layout); a run without a window opens a device and the rasteriser for it at the start
 * (port_video_gpu_headless), and skips the shot with a log line when there is none.
 * Widescreen (render_gpu_wide.c): a game mod calls port_video_widescreen_enable before the window opens (with the GPU
 * renderer chosen, a new window opens 16:9) and port_video_widescreen(on) every vsync; while it is on, a 15-bit display
 * that has a wide canvas is presented 16:9 inside the window (the window's size is kept: the picture switches between
 * 4:3 and 16:9 in it), and `--gpu-screenshot` and the debug channel's GPU screenshot give the wide image. The
 * software renderer, `--screenshot` and the frame hash stay 4:3 (logged once while it is asked for).
 * `<PREFIX>_PORT_PRESENT_READBACK=FRAME:PATH` reads SDL_Renderer's output of vsync FRAME back into a PPM (the comparison
 * of the two present paths: tests/port/render_gpu.py); `<PREFIX>_PORT_GPU_VRAM_CHECK=N` compares the rasteriser's whole
 * target with the software VRAM every N vsyncs (video_vram_check).
 *
 * The conversion (psx-spx "GPU Display Control", "24bit RGB"): a 15-bit display reads one VRAM pixel per screen pixel
 * (bits 0-4 red, 5-9 green, 10-14 blue; bit 15, the mask bit, is not shown); a 24-bit display (`rgb24`, the movies)
 * reads 3 bytes per screen pixel (red, green, blue) from the VRAM rows taken as byte strings, so a 320-pixel line
 * spans 480 VRAM pixels: DISPENV.disp.w counts screen pixels in both modes (gfx_init_display's 24-bit buffers are 320
 * wide at x 0 and x 480). 5-bit components become 8-bit as (c << 3) | (c >> 2). Coordinates wrap in the VRAM (1024 x
 * 512), as the GPU's reads do. An interlaced 480-line display (the title's 320x480) is shown as one frame of both
 * fields, its lines in VRAM order (the PS1 draws the whole frame into VRAM; the TV interleaves the two fields). The
 * display is black while SetDispMask(0) holds or before the first PutDispEnv (then 320x240). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "port_harness.h"
#include "port_runtime.h"
#include "psyq.h"

#ifdef PSXSTACK_SDL
#include <SDL3/SDL.h>

#include "render_gpu.h"
#include "render_gpu_textures.h"
#elif defined(PSXSTACK_SDL2)
#include <SDL2/SDL.h>
#include <unistd.h>
#include "fbdev_present.h"
#ifdef PSXSTACK_EGL
#include "render_gles.h"
#endif
#endif

#define VIDEO_MAX_W 640 /* screen pixels: the GPU's widest mode */
#define VIDEO_MAX_H 576 /* lines: PAL, interlaced */
#define VIDEO_MAX_SHOTS 64

int port_window;
static u32 video_pixels[VIDEO_MAX_W * VIDEO_MAX_H]; /* 0xFFRRGGBB */
static int video_w, video_h;                         /* the converted image's size */
static struct {
    long frame;
    char *path;
} video_shots[VIDEO_MAX_SHOTS];
static int video_shot_count;
static long video_converted_frame = -1; /* the frame video_pixels was converted at (the debug channel's screenshot) */
static int video_gpu_wanted;            /* --renderer gpu / video.renderer "gpu" */
static int video_gles_wanted;           /* --renderer gles: the offscreen GLES renderer (render_gles.c) */
static int video_internal_scale = 1;    /* --internal-scale / video.internal_scale: the rasteriser's, 1..8 */
static PortFilter video_filter;         /* --filter / video.filter: the hardware renderer's present (render_gpu_present.c) */
static struct {
    long frame;
    int w, h; /* the output's size; 0: the image's own */
    char *path;
} video_gpu_shots[VIDEO_MAX_SHOTS];
static int video_gpu_shot_count;
static int video_vram_xy[2]; /* the display's corner in the VRAM, when video_vram (a 15-bit display that is on) */
#ifdef PSXSTACK_SDL
static long video_check_every = -1, video_checks, video_checks_failed; /* <PREFIX>_PORT_GPU_VRAM_CHECK (below) */
#endif
static int video_vram;
static int video_wide_wanted;  /* port_video_widescreen_enable: a game mod widens some scenes */
static int video_wide_logged;  /* the software renderer's 4:3, logged once */

static u32 video_rgb15(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

/* Byte `x` (0..2047, wrapping) of VRAM row `row`: the low byte of a pixel first. */
static u32 video_byte(const u16 *row, int x) {
    u16 p = row[(x >> 1) & 1023];
    return (x & 1) ? p >> 8 : p & 0xFF;
}

/* Converts the current display area into video_pixels (video_w x video_h). */
static void video_convert(void) {
    PsyqDisplay d;
    const u16 *vram = psyq_gpu_vram();
    int w, h, x, y;

    psyq_gpu_display(&d);
    w = d.w > 0 ? d.w : 320;
    h = d.h > 0 ? d.h : 240;
    w = w < VIDEO_MAX_W ? w : VIDEO_MAX_W;
    h = h < VIDEO_MAX_H ? h : VIDEO_MAX_H;
    video_w = w;
    video_h = h;
    video_converted_frame = port_frames;
    video_vram = d.enabled && d.w > 0 && d.h > 0 && !d.rgb24;
    video_vram_xy[0] = d.x;
    video_vram_xy[1] = d.y;
    if (!d.enabled || d.w <= 0 || d.h <= 0) {
        for (x = 0; x < w * h; x++) {
            video_pixels[x] = 0xFF000000u;
        }
        return;
    }
    for (y = 0; y < h; y++) {
        const u16 *row = vram + (size_t)((d.y + y) & 511) * psyq_gpu_vram_stride();
        u32 *out = video_pixels + (size_t)y * w;
        if (d.rgb24) {
            int bx = (d.x & 1023) * 2;
            for (x = 0; x < w; x++, bx += 3) {
                out[x] = 0xFF000000u | video_byte(row, bx) << 16 | video_byte(row, bx + 1) << 8 |
                         video_byte(row, bx + 2);
            }
        } else {
            for (x = 0; x < w; x++) {
                out[x] = video_rgb15(row[(d.x + x) & 1023]);
            }
        }
    }
}

int port_video_screenshot_add(const char *spec) {
    char *end;
    long frame = strtol(spec, &end, 0);
    if (end == spec || *end != ':' || end[1] == '\0' || frame <= 0 || video_shot_count == VIDEO_MAX_SHOTS) {
        return 0;
    }
    video_shots[video_shot_count].frame = frame;
    video_shots[video_shot_count].path = strdup(end + 1);
    video_shot_count++;
    return 1;
}

/* `pix` (w x h, 0xFFRRGGBB) to `path` as a binary PPM; 0 when the file cannot be written. */
static int video_ppm(const char *path, const u32 *pix, int w, int h) {
    FILE *f = fopen(path, "wb");
    int i;
    if (f == NULL) {
        return 0;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        u8 rgb[3] = { (u8)(pix[i] >> 16), (u8)(pix[i] >> 8), (u8)pix[i] };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}

/* video_pixels to `path`; 0 when the file cannot be written. */
static int video_try_ppm(const char *path) {
    if (!video_ppm(path, video_pixels, video_w, video_h)) {
        return 0;
    }
    port_log("screenshot: frame %ld, %dx%d -> %s", port_frames, video_w, video_h, path);
    return 1;
}

int port_video_set_renderer(const char *name) {
    if (strcmp(name, "software") == 0 || strcmp(name, "gpu") == 0 || strcmp(name, "gles") == 0) {
        video_gpu_wanted = name[0] == 'g';
        video_gles_wanted = strcmp(name, "gles") == 0;
        return 1;
    }
    return 0;
}

void port_video_set_internal_scale(int scale) {
    video_internal_scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
}

void port_video_set_subpixel(int mode) {
#ifdef PSXSTACK_SDL
    render_gpu_set_subpixel(mode);
#else
    (void)mode;
#endif
}

void port_video_set_filter(const PortFilter *f) {
    video_filter = *f;
}

int port_video_gpu_screenshot_add(const char *spec) {
    char *end;
    long frame = strtol(spec, &end, 0);
    long w = 0, h = 0;
    if (end == spec || frame <= 0 || video_gpu_shot_count == VIDEO_MAX_SHOTS) {
        return 0;
    }
    if (*end == '@') {
        w = strtol(end + 1, &end, 10);
        if (*end != 'x') {
            return 0;
        }
        h = strtol(end + 1, &end, 10);
        if (w < 1 || h < 1 || w > 8192 || h > 8192) {
            return 0;
        }
    }
    if (*end != ':' || end[1] == '\0') {
        return 0;
    }
    video_gpu_shots[video_gpu_shot_count].frame = frame;
    video_gpu_shots[video_gpu_shot_count].w = (int)w;
    video_gpu_shots[video_gpu_shot_count].h = (int)h;
    video_gpu_shots[video_gpu_shot_count].path = strdup(end + 1);
    video_gpu_shot_count++;
    return 1;
}

static void video_write_ppm(const char *path) {
    if (!video_try_ppm(path)) {
        port_fatal("screenshot: cannot write %s", path);
    }
}

int port_video_screenshot_now(const char *path, int *w, int *h) {
    if (video_converted_frame != port_frames) {
        video_convert(); /* nothing of this frame was presented or shot: the VRAM as it is now */
    }
    *w = video_w;
    *h = video_h;
    return video_try_ppm(path);
}

#ifdef PSXSTACK_SDL
static SDL_Window *video_window;
static SDL_Renderer *video_renderer;
static SDL_Texture *video_texture;
static int video_tex_w, video_tex_h;
static Uint64 video_start_ns;
static long video_presents;
static int video_gpu; /* the window presents through the hardware renderer */
static int video_aspect[2] = { 4, 3 }; /* the presented picture's aspect: 4:3, or 16:9 at the display's pixel aspect */

int port_video_available(void) {
    return 1;
}

void port_video_open(int scale, int fullscreen) {
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        port_fatal("SDL_Init: %s (a host without a display: SDL_VIDEO_DRIVER=offscreen)", SDL_GetError());
    }
    atexit(SDL_Quit);
    /* with widescreen and the GPU renderer, 16:9 at the same height (the picture switches between 4:3 and 16:9) */
    video_window = SDL_CreateWindow(PSXSTACK_GAME_ID, video_wide_wanted && video_gpu_wanted ? (240 * scale * 16 + 8) / 9
                                                                                           : 320 * scale,
                                    240 * scale, flags);
    if (video_window == NULL) {
        port_fatal("SDL_CreateWindow: %s", SDL_GetError());
    }
    if (video_gpu_wanted) {
        char why[256];
        video_gpu = render_gpu_open(video_window, why, sizeof(why));
        if (!video_gpu) {
            port_log("renderer: gpu unavailable (%s); software", why);
        } else if (!render_gpu_raster_start(video_internal_scale, why, sizeof(why))) {
            port_log("renderer: gpu: no rasteriser (%s); the software image through SDL_GPU", why);
        }
    }
    if (video_gpu) {
        render_gpu_set_filter(&video_filter);
    } else if (video_filter.kind != PORT_FILTER_NONE) {
        port_log("filter: %s needs the GPU renderer (--renderer gpu); the picture is unfiltered",
                 port_filter_names[video_filter.kind]);
    }
    if (!video_gpu) {
        video_renderer = SDL_CreateRenderer(video_window, NULL);
        if (video_renderer == NULL) {
            port_fatal("SDL_CreateRenderer: %s", SDL_GetError());
        }
    }
    port_window = 1;
    video_start_ns = SDL_GetTicksNS();
    port_log("window: SDL %d.%d.%d, video driver %s, renderer %s%s%s, %dx%d%s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
             SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
             SDL_GetCurrentVideoDriver(), video_gpu ? "gpu (" : SDL_GetRendererName(video_renderer),
             video_gpu ? render_gpu_describe() : "", video_gpu ? ")" : "",
             video_wide_wanted && video_gpu_wanted ? (240 * scale * 16 + 8) / 9 : 320 * scale, 240 * scale,
             fullscreen ? " (fullscreen)" : "");
}

void port_video_toggle_fullscreen(void) {
    if (video_window != NULL) {
        SDL_SetWindowFullscreen(video_window, (SDL_GetWindowFlags(video_window) & SDL_WINDOW_FULLSCREEN) == 0);
    }
}

/* The image's place in the renderer's output (ow x oh pixels): 4:3 (video_aspect; a wide picture's 16:9), as tall as
 * an integer multiple of its lines allows (every line the same height), centred; scaled to fit when the output has
 * fewer pixel rows than it has lines. The width is not an integer multiple for every mode (only 320 and 640 wide
 * displays are 4:3 pixel for pixel). */
static void video_dest(int ow, int oh, SDL_FRect *dst) {
    int f = oh / video_h, an = video_aspect[0], ad = video_aspect[1];
    float dw, dh;
    while (f > 0 && (video_h * f * an + ad - 1) / ad > ow) {
        f--;
    }
    if (f > 0) {
        dh = (float)(video_h * f);
        dw = (float)((video_h * f * an + ad - 1) / ad); /* rounded up: 4:3's (h f 4 + 2) / 3 */
    } else {
        dh = (float)oh < ow * (float)ad / (float)an ? (float)oh : ow * (float)ad / (float)an;
        dw = dh * (float)an / (float)ad;
    }
    dst->w = dw;
    dst->h = dh;
    dst->x = (float)(int)((ow - dw) / 2);
    dst->y = (float)(int)((oh - dh) / 2);
}

/* video_dest in whole pixels (the hardware renderer's rectangle: its present maps pixels with integers). */
static void video_dest_rect(int ow, int oh, int rect[4]) {
    SDL_FRect d;
    video_dest(ow, oh, &d);
    rect[0] = (int)d.x;
    rect[1] = (int)d.y;
    rect[2] = (int)(d.w + 0.5f);
    rect[3] = (int)(d.h + 0.5f);
}

/* <PREFIX>_PORT_PRESENT_READBACK=FRAME:PATH: SDL_Renderer's output of vsync FRAME (what the window shows) as a PPM. */
static void video_readback(void) {
    static long frame = -1;
    static const char *path;
    SDL_Surface *shot, *rgb;
    if (frame < 0) {
        const char *env = getenv(PSXSTACK_GAME_ENV_PREFIX "_PORT_PRESENT_READBACK");
        char *end;
        frame = 0;
        if (env != NULL && (frame = strtol(env, &end, 0)) > 0 && *end == ':') {
            path = end + 1;
        } else {
            frame = 0;
        }
    }
    if (frame == 0 || frame != port_frames) {
        return;
    }
    shot = SDL_RenderReadPixels(video_renderer, NULL);
    rgb = shot != NULL ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888) : NULL;
    if (rgb == NULL || rgb->pitch != rgb->w * 4 || !video_ppm(path, (const u32 *)rgb->pixels, rgb->w, rgb->h)) {
        port_fatal("" PSXSTACK_GAME_ENV_PREFIX "_PORT_PRESENT_READBACK: cannot read back or write %s: %s", path, SDL_GetError());
    }
    port_log("present readback: frame %ld, %dx%d -> %s", port_frames, rgb->w, rgb->h, path);
    SDL_DestroySurface(rgb);
    SDL_DestroySurface(shot);
}

/* The width the hardware renderer shows the converted display with: its wide canvas's while widescreen is on (and
 * video_aspect 16:9 at the display's pixel aspect), else the display's (4:3). */
static int video_gpu_width(void) {
    int w = video_vram ? render_gpu_wide_width(video_vram_xy, video_w, video_h) : 0;
    video_aspect[0] = w > 0 ? 4 * w : 4;
    video_aspect[1] = w > 0 ? 3 * video_w : 3;
    return w > 0 ? w : video_w;
}

static void video_present(void) {
    SDL_FRect dst;
    int ow, oh;
    if (video_gpu) {
        if (render_gpu_present(video_pixels, video_gpu_width(), video_h, video_vram ? video_vram_xy : NULL,
                               video_dest_rect)) {
            video_presents++;
        }
        return;
    }
    video_aspect[0] = 4; /* the software renderer: always 4:3 */
    video_aspect[1] = 3;
    if (video_texture == NULL || video_tex_w != video_w || video_tex_h != video_h) {
        if (video_texture != NULL) {
            SDL_DestroyTexture(video_texture);
        }
        video_texture = SDL_CreateTexture(video_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                          video_w, video_h);
        if (video_texture == NULL) {
            port_fatal("SDL_CreateTexture %dx%d: %s", video_w, video_h, SDL_GetError());
        }
        SDL_SetTextureScaleMode(video_texture, SDL_SCALEMODE_NEAREST);
        video_tex_w = video_w;
        video_tex_h = video_h;
    }
    SDL_UpdateTexture(video_texture, NULL, video_pixels, video_w * (int)sizeof(u32));
    SDL_SetRenderDrawColor(video_renderer, 0, 0, 0, 255);
    SDL_RenderClear(video_renderer);
    if (SDL_GetCurrentRenderOutputSize(video_renderer, &ow, &oh) && ow > 0 && oh > 0) {
        video_dest(ow, oh, &dst);
        SDL_RenderTexture(video_renderer, video_texture, NULL, &dst);
    }
    video_readback();
    SDL_RenderPresent(video_renderer);
    video_presents++;
}

void port_video_refresh(void) {
    if (port_window) {
        video_present();
    }
}

static int video_paused;
static char video_status[64]; /* fast-forward's, or "" */

static void video_title(void) {
    char title[96];
    if (video_window != NULL) {
        snprintf(title, sizeof(title), PSXSTACK_GAME_ID "%s%s%s%s", video_paused ? " (paused)" : "",
                 video_status[0] ? " (" : "", video_status, video_status[0] ? ")" : "");
        SDL_SetWindowTitle(video_window, title);
    }
}

void port_video_set_paused(int paused) {
    video_paused = paused;
    video_title();
}

void port_video_set_status(const char *status) {
    snprintf(video_status, sizeof(video_status), "%s", status != NULL ? status : "");
    video_title();
}

void port_video_close(void) {
    if (port_window) {
        double s = (double)(SDL_GetTicksNS() - video_start_ns) / 1e9;
        port_log("window: %ld frames presented in %.2f s (%.2f per second)", video_presents, s,
                 s > 0 ? (double)video_presents / s : 0.0);
    }
}

/* SDL torn down before exit(), not only by the atexit(SDL_Quit) above: with NVIDIA's EGL (Wayland and offscreen
 * drivers; driver 595.104.02) an SDL_Quit inside exit() unloads libnvidia-eglcore and the process then jumps into the
 * unloaded code (SIGSEGV after the run's "exit" line, seen in play-testing). X11 (GLX) was not affected. */
void port_video_quit(void) {
    if (video_checks > 0) {
        port_log("gpu vram check: %ld checks, %ld with differences", video_checks, video_checks_failed);
    }
    render_gpu_tex_close();
    render_gpu_close(); /* the device too: destroyed before SDL_Quit, its claim before the window */
    if (video_texture != NULL) {
        SDL_DestroyTexture(video_texture);
        video_texture = NULL;
    }
    if (video_renderer != NULL) {
        SDL_DestroyRenderer(video_renderer);
        video_renderer = NULL;
    }
    if (video_window != NULL) {
        SDL_DestroyWindow(video_window);
        video_window = NULL;
    }
    SDL_Quit();
}

/* The hardware renderer's screenshots due at this vsync (video_pixels converted); without a device (none opened at
 * the start: port_video_open, port_video_gpu_headless) they are skipped with a log line. */
static void video_gpu_shots_due(void) {
    int i;
    for (i = 0; i < video_gpu_shot_count; i++) {
        int w, h;
        u32 *buf;
        if (video_gpu_shots[i].frame != port_frames) {
            continue;
        }
        /* the image: a display from the rasteriser's target at its internal scale (a wide one: its canvas) */
        int pw = video_gpu_width();
        w = video_gpu_shots[i].w > 0 ? video_gpu_shots[i].w : pw * (video_vram ? render_gpu_scale() : 1);
        h = video_gpu_shots[i].h > 0 ? video_gpu_shots[i].h : video_h * (video_vram ? render_gpu_scale() : 1);
        buf = malloc((size_t)w * (size_t)h * 4);
        if (!render_gpu_active() || buf == NULL ||
            !render_gpu_readback(video_pixels, pw, video_h, video_vram ? video_vram_xy : NULL, w, h,
                                 video_gpu_shots[i].w > 0 ? video_dest_rect : NULL, buf)) {
            port_log("gpu screenshot: frame %ld skipped (no GPU device) -> %s", port_frames, video_gpu_shots[i].path);
        } else if (!video_ppm(video_gpu_shots[i].path, buf, w, h)) {
            port_fatal("gpu screenshot: cannot write %s", video_gpu_shots[i].path);
        } else {
            port_log("gpu screenshot: frame %ld, %dx%d -> %s", port_frames, w, h, video_gpu_shots[i].path);
        }
        free(buf);
    }
}

/* <PREFIX>_PORT_GPU_VRAM_CHECK=N: every N vsyncs the rasteriser's whole target against the software VRAM (the frame test:
 * tests/port/render_gpu.py); a difference is logged with its count and first pixel, the run's totals at the end. */
static void video_vram_check(void) {
    static u16 hw[1024 * 512];
    const u16 *sw = psyq_gpu_vram();
    int x, y, n = 0, first = -1;
    if (video_check_every < 0) {
        const char *env = getenv(PSXSTACK_GAME_ENV_PREFIX "_PORT_GPU_VRAM_CHECK");
        video_check_every = env != NULL ? strtol(env, NULL, 0) : 0;
    }
    if (video_check_every <= 0 || port_frames % video_check_every != 0 || !render_gpu_rasterising() ||
        !render_gpu_read_vram(hw)) {
        return;
    }
    video_checks++;
    /* The software VRAM's rows are padded (psyq_gpu_vram_stride); the rasteriser's are 1024 apart. */
    for (y = 0; y < 512; y++) {
        const u16 *row = sw + (size_t)y * psyq_gpu_vram_stride();

        for (x = 0; x < 1024; x++) {
            if (hw[y * 1024 + x] != row[x]) {
                if (first < 0) {
                    first = y * 1024 + x;
                }
                n++;
            }
        }
    }
    if (n > 0) {
        video_checks_failed++;
        port_log("gpu vram check: frame %ld: %d pixels differ, the first at %d,%d (software %04x, gpu %04x)",
                 port_frames, n, first % 1024, first / 1024, sw[(first / 1024) * psyq_gpu_vram_stride() + first % 1024],
                 hw[first]);
    }
}

void port_video_gpu_headless(void) {
    char why[256];
    if ((video_gpu_shot_count == 0 && !video_gpu_wanted) || render_gpu_active()) {
        return;
    }
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        port_log("renderer: gpu unavailable (SDL_InitSubSystem: %s)", SDL_GetError());
        return;
    }
    if (!render_gpu_open(NULL, why, sizeof(why))) {
        port_log("renderer: gpu unavailable (%s)", why);
        return;
    }
    if (!render_gpu_raster_start(video_internal_scale, why, sizeof(why))) {
        port_log("renderer: gpu: no rasteriser (%s); the software image through SDL_GPU", why);
    }
    port_log("renderer: gpu for the screenshots (%s)", render_gpu_describe());
    render_gpu_set_filter(&video_filter);
}

int port_video_dump_textures(const char *dir) {
    return render_gpu_tex_dump_open(dir);
}

int port_video_texture_pack(const char *dir) {
    if (!video_gpu_wanted && video_gpu_shot_count == 0) {
        port_log("texture packs: drawn by the hardware renderer only (--renderer gpu)");
    }
    return render_gpu_tex_pack_add(dir);
}

int port_video_gpu_screenshot_now(const char *path, int *w, int *h) {
    u32 *buf;
    int ok, pw, s = render_gpu_scale();
    if (!render_gpu_active()) {
        return -1;
    }
    if (video_converted_frame != port_frames) {
        video_convert();
    }
    pw = video_gpu_width();
    *w = pw * (video_vram ? s : 1);
    *h = video_h * (video_vram ? s : 1);
    buf = malloc((size_t)*w * (size_t)*h * 4);
    ok = buf != NULL && render_gpu_readback(video_pixels, pw, video_h, video_vram ? video_vram_xy : NULL, *w, *h,
                                            NULL, buf) && video_ppm(path, buf, *w, *h);
    free(buf);
    return ok;
}
#elif defined(PSXSTACK_SDL2)
static SDL_Window *video_window;
static SDL_Renderer *video_renderer;
static SDL_Texture *video_texture;
static int video_tex_w, video_tex_h;
static Uint32 video_start_ms;
static long video_presents;
static FbdevPresent fbdev;
static int fbdev_active;
#ifdef PSXSTACK_EGL
static int video_gles; /* the offscreen GLES renderer draws; the present still comes out through fbdev */
static long video_gles_check_every; /* DW3_GLES_CHECK: frames between VRAM comparisons, 0 off */
/* The display area as the renderer reads it back: 320x240 of 16-bit pixels, presented by Fbdev_PresentVram. */
static u16 video_gles_display[VIDEO_MAX_W * VIDEO_MAX_H];
#endif

int port_video_available(void) {
    return 1;
}

int port_fbdev_active(void) {
    return fbdev_active;
}

void port_video_open(int scale, int fullscreen) {
    (void)scale;
    const char *video_env = getenv("DW3_VIDEO");
    int want_fbdev = (video_env && strcmp(video_env, "fbdev") == 0);
    if (!video_env && !getenv("SDL_VIDEODRIVER")) {
        if (access("/dev/fb0", R_OK | W_OK) == 0) {
            want_fbdev = 1;
        }
    }
    if (want_fbdev) {
        char why[160];
        if (Fbdev_Open(&fbdev, "/dev/fb0", why, sizeof(why)) == 0) {
            fbdev_active = 1;
            port_window = 1;
            video_start_ms = SDL_GetTicks();
            port_log("video: direct fbdev opened (/dev/fb0, %ux%u %ubpp, NEON 2x scaling)",
                     fbdev.xres, fbdev.yres, fbdev.bits_per_pixel);
#ifdef PSXSTACK_EGL
            if (video_gles_wanted) {
                /* The offscreen renderer draws; the picture still leaves through fbdev (a window swap costs 17 ms on
                 * this handheld, the fb0 present 0.77 ms: docs/gpu/measurements.txt). */
                char gles_why[160] = {0};

                const char *replace_env = getenv("DW3_GLES_REPLACE");
                const char *check_env = getenv("DW3_GLES_CHECK");

                /* DW3_GLES_REPLACE=0 keeps the CPU drawing as well, so the VRAM check can compare the two.
                 * DW3_GLES_CHECK=N compares the whole target against the shim's VRAM every N frames and logs the
                 * first differing pixel. Both are measurement switches, not settings. */
                video_gles_check_every = check_env != NULL ? atoi(check_env) : 0;
                if (replace_env != NULL && replace_env[0] == '0') {
                    render_gles_set_replace(0);
                }
                if (render_gles_open(gles_why, sizeof(gles_why)) &&
                    render_gles_raster_start(1, gles_why, sizeof(gles_why))) {
                    video_gles = 1;
                    port_log("renderer: gles (%s)%s%s", render_gles_describe(),
                             video_gles_check_every > 0 ? ", vram check every " : "",
                             video_gles_check_every > 0 ? check_env : "");
                } else {
                    port_log("renderer: gles unavailable (%s); the software renderer draws", gles_why);
                }
            }
#elif defined(PSXSTACK_SDL2)
            if (video_gles_wanted) {
                port_log("renderer: gles needs -DPSXSTACK_EGL=ON; the software renderer draws");
            }
#endif
            return;
        } else {
            port_log("video: fbdev open failed (%s); falling back to SDL2", why);
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        port_fatal("SDL_Init: %s", SDL_GetError());
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    Uint32 flags = fullscreen ? SDL_WINDOW_FULLSCREEN : 0;
    video_window = SDL_CreateWindow(PSXSTACK_GAME_ID, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    640, 480, flags);
    if (video_window == NULL) {
        port_fatal("SDL_CreateWindow: %s", SDL_GetError());
    }
    video_renderer = SDL_CreateRenderer(video_window, -1, SDL_RENDERER_ACCELERATED);
    if (video_renderer == NULL) {
        video_renderer = SDL_CreateRenderer(video_window, -1, 0);
    }
    if (video_renderer == NULL) {
        port_fatal("SDL_CreateRenderer: %s", SDL_GetError());
    }
    SDL_RenderSetLogicalSize(video_renderer, 640, 480);
    SDL_RendererInfo rinfo;
    memset(&rinfo, 0, sizeof(rinfo));
    SDL_GetRendererInfo(video_renderer, &rinfo);
    port_window = 1;
    video_start_ms = SDL_GetTicks();
    port_log("video: SDL2 %d.%d.%d, driver %s, renderer %s, 640x480%s",
             SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL,
             SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown",
             rinfo.name ? rinfo.name : "unknown", fullscreen ? " (fullscreen)" : "");
}

void port_video_toggle_fullscreen(void) {
    if (video_window != NULL) {
        Uint32 f = SDL_GetWindowFlags(video_window);
        SDL_SetWindowFullscreen(video_window, (f & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN);
    }
}

static void video_present(void) {
    if (fbdev_active) {
        Fbdev_Present(&fbdev, video_pixels, video_w, video_w, video_h);
        video_presents++;
        return;
    }
    if (video_renderer != NULL) {
        if (video_texture == NULL || video_tex_w != video_w || video_tex_h != video_h) {
            if (video_texture != NULL) {
                SDL_DestroyTexture(video_texture);
            }
            video_texture = SDL_CreateTexture(video_renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, video_w, video_h);
            if (video_texture == NULL) {
                port_fatal("SDL_CreateTexture %dx%d: %s", video_w, video_h, SDL_GetError());
            }
            video_tex_w = video_w;
            video_tex_h = video_h;
        }
        SDL_UpdateTexture(video_texture, NULL, video_pixels, video_w * (int)sizeof(u32));
        SDL_SetRenderDrawColor(video_renderer, 0, 0, 0, 255);
        SDL_RenderClear(video_renderer);
        SDL_RenderCopy(video_renderer, video_texture, NULL, NULL);
        SDL_RenderPresent(video_renderer);
        video_presents++;
    }
}

void port_video_refresh(void) {
    if (port_window) {
        video_present();
    }
}

void port_video_set_paused(int paused) {
    (void)paused;
}

void port_video_set_status(const char *status) {
    if (video_window != NULL && status != NULL) {
        char title[128];
        snprintf(title, sizeof(title), "%s%s%s", PSXSTACK_GAME_ID, *status ? " - " : "", status);
        SDL_SetWindowTitle(video_window, title);
    }
}

void port_video_close(void) {
    if (port_window) {
        double s = (double)(SDL_GetTicks() - video_start_ms) / 1000.0;
        port_log("window: %ld frames presented in %.2f s (%.2f per second)", video_presents, s,
                 s > 0 ? (double)video_presents / s : 0.0);
    }
}

void port_video_quit(void) {
#ifdef PSXSTACK_EGL
    if (video_gles) {
        render_gles_close();
        video_gles = 0;
    }
#endif
    if (fbdev_active) {
        Fbdev_Close(&fbdev);
        fbdev_active = 0;
    }
    if (video_texture != NULL) {
        SDL_DestroyTexture(video_texture);
        video_texture = NULL;
    }
    if (video_renderer != NULL) {
        SDL_DestroyRenderer(video_renderer);
        video_renderer = NULL;
    }
    if (video_window != NULL) {
        SDL_DestroyWindow(video_window);
        video_window = NULL;
    }
    SDL_Quit();
}

static void video_gpu_shots_due(void) {
}

void port_video_gpu_headless(void) {}
int port_video_dump_textures(const char *dir) { (void)dir; return 0; }
int port_video_texture_pack(const char *dir) { (void)dir; return 0; }
int port_video_gpu_screenshot_now(const char *path, int *w, int *h) { (void)path; (void)w; (void)h; return -1; }
#else
int port_video_available(void) {
    return 0;
}

void port_video_gpu_headless(void) {
}

int port_video_dump_textures(const char *dir) {
    (void)dir;
    return 0;
}

int port_video_texture_pack(const char *dir) {
    (void)dir;
    return 0;
}

int port_video_gpu_screenshot_now(const char *path, int *w, int *h) {
    (void)path;
    (void)w;
    (void)h;
    return -1;
}

static void video_gpu_shots_due(void) {
}

void port_video_open(int scale, int fullscreen) {
    (void)scale;
    (void)fullscreen;
    port_fatal("this build has no window: configure with -DPSXSTACK_SDL=ON (port/README.md \"The window\")");
}

void port_video_toggle_fullscreen(void) {
}

static void video_present(void) {
}

void port_video_refresh(void) {
}

void port_video_set_paused(int paused) {
    (void)paused;
}

void port_video_set_status(const char *status) {
    (void)status;
}

void port_video_close(void) {
}

void port_video_quit(void) {
}
#endif

void port_video_widescreen_enable(void) {
    video_wide_wanted = 1;
#ifdef PSXSTACK_SDL
    render_gpu_wide_enable();
#endif
}

void port_video_widescreen(int on) {
#ifdef PSXSTACK_SDL
    int gpu = render_gpu_rasterising();
    render_gpu_wide_set(on);
#else
    int gpu = 0;
#endif
    if (on && !gpu && !video_wide_logged) {
        video_wide_logged = 1;
        port_log("widescreen: needs the GPU renderer (video.renderer gpu); the picture stays 4:3");
    }
}

/* The present cap (fast-forward): at most `hz` presents a second (0: every vsync). Every vsync is still drawn into
 * the VRAM by the software GPU; only the conversion and the present are skipped. */
static int video_cap_hz;

void port_video_set_present_cap(int hz) {
    video_cap_hz = hz;
}

/* Whether this vsync is presented under the cap. */
static int video_due(void) {
#ifdef PSXSTACK_SDL
    static Uint64 last;
    Uint64 now;
    if (video_cap_hz <= 0) {
        return 1;
    }
    now = SDL_GetTicksNS();
    if (now - last < 1000000000ull / (Uint64)video_cap_hz) {
        return 0;
    }
    last = now;
#elif defined(PSXSTACK_SDL2)
    static Uint32 last;
    Uint32 now;
    if (video_cap_hz <= 0) {
        return 1;
    }
    now = SDL_GetTicks();
    if (now - last < (Uint32)(1000 / video_cap_hz)) {
        return 0;
    }
    last = now;
#endif
    return 1;
}

/* One line a second. The old present bucket was this whole stretch: VRAM expand plus the blit. */
static void video_present_stat(long long t0, int direct, int bits, int w, int h) {
    static long long acc, sec0;
    static int n;
    long long now = port_clock_ns();
    const char *path;
    double ms;
    PsyqDisplay dd;

    /* Why the direct path was refused: its rectangle has to be linear in the VRAM, and the display area's x can be
     * anywhere in the 1024-pixel row (the converter wraps, the framebuffer expand cannot). */
    if (!direct) {
        static int told;

        psyq_gpu_display(&dd);
        if (!told) {
            told = 1;
            port_log("present: direct refused for %dx%d at %d,%d (24-bit %d)", dd.w, dd.h, dd.x, dd.y, dd.rgb24);
        }
    }

    if (sec0 == 0) {
        sec0 = now;
    }
    acc += now - t0;
    n++;
    if (now - sec0 < 1000000000LL) {
        return;
    }
    ms = n > 0 ? (double)acc / n / 1e6 : 0.0;
    port_log("present: %.1f ms (%s %d-bit %dx%d)", ms, direct ? "direct" : "convert", bits, w, h);
    path = getenv("DW3_PRESENT_LOG");
    if (path != NULL && path[0] != '\0') {
        FILE *f = fopen(path, "a");
        if (f != NULL) {
            fprintf(f, "present: %.1f ms (%s %d-bit %dx%d)\n", ms, direct ? "direct" : "convert", bits, w, h);
            fclose(f);
        }
    }
    acc = 0;
    n = 0;
    sec0 = now;
}

#if defined(PSXSTACK_SDL2)
/* Skips the scalar expand when the framebuffer can take the VRAM rectangle directly. 1 if it drew. */
static int video_present_direct(long long *t0) {
    PsyqDisplay d;

    if (!fbdev_active) {
        return 0;
    }
    psyq_gpu_display(&d);
    *t0 = port_clock_ns();
    if (!d.enabled || d.w <= 0 || d.h <= 0) {
        return 0;
    }
#ifdef PSXSTACK_EGL
    if (video_gles && d.w <= VIDEO_MAX_W && d.h <= VIDEO_MAX_H) {
        /* The display's pixels come from the renderer's VRAM target, not from the shim's array: with the listener
         * replacing the rasteriser, the array has only what the transfers wrote. The 24-bit modes stay on the software
         * path for now (the GLES target holds 5-5-5-1, and nothing in this game's battles uses them). */
        if (render_gles_read_display(video_gles_display, d.x, d.y, d.w, d.h) &&
            Fbdev_PresentVram(&fbdev, video_gles_display, d.w, 0, 0, d.w, d.h, d.rgb24) == 0) {
            video_w = d.w;
            video_h = d.h;
            video_presents++;
            video_present_stat(*t0, 1, d.rgb24 ? 24 : 15, d.w, d.h);
            return 1;
        }
    }
#endif
    if (Fbdev_PresentVram(&fbdev, psyq_gpu_vram(), psyq_gpu_vram_stride(), d.x, d.y, d.w, d.h, d.rgb24) != 0) {
        return 0;
    }
    video_w = d.w < VIDEO_MAX_W ? d.w : VIDEO_MAX_W;
    video_h = d.h < VIDEO_MAX_H ? d.h : VIDEO_MAX_H;
    video_presents++;
    video_present_stat(*t0, 1, d.rgb24 ? 24 : 15, d.w, d.h);
    return 1;
}
#endif

#if defined(PSXSTACK_SDL2) && defined(PSXSTACK_EGL)
/* DW3_GLES_CHECK: the whole GLES target against the shim's VRAM, the plan's exactness criterion made a live measure.
 * With DW3_GLES_REPLACE=0 the CPU draws the same primitives, so every difference is a bug in the renderer rather than
 * a milestone that has not landed yet. */
static void video_gles_vram_check(void) {
    static u16 hw[1024 * 512];
    static long checks, failed;

    if (!video_gles || video_gles_check_every <= 0 || port_frames % video_gles_check_every != 0) {
        return;
    }
    if (!render_gles_read_vram(hw)) {
        return;
    }
    {
        const u16 *sw = psyq_gpu_vram();
        int stride = psyq_gpu_vram_stride();
        int x, y, diffs = 0, fx = -1, fy = -1;
        int bx0 = 1024, by0 = 512, bx1 = -1, by1 = -1;
        u16 fsw = 0, fhw = 0;

        for (y = 0; y < 512; y++) {
            for (x = 0; x < 1024; x++) {
                u16 a = sw[(size_t)y * stride + x];
                u16 b = hw[(size_t)y * 1024 + x];

                if (a != b) {
                    if (diffs == 0) {
                        fx = x;
                        fy = y;
                        fsw = a;
                        fhw = b;
                    }
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (y < by0) by0 = y;
                    if (y > by1) by1 = y;
                    diffs++;
                }
            }
        }
        checks++;
        if (diffs == 0) {
            port_log("gles: vram check %ld (frame %ld): exact", checks, port_frames);
        } else {
            failed++;
            if (getenv("DW3_GLES_DUMP") != NULL &&
                (failed == 1 || (getenv("DW3_GLES_DUMP_FRAME") != NULL &&
                                 atol(getenv("DW3_GLES_DUMP_FRAME")) == port_frames))) {
                /* One dump of both sides, on the first failure: the quickest way to see what differs is to look. */
                FILE *f = fopen("/tmp/gles_sw.bin", "wb");
                int r;

                if (f != NULL) {
                    for (r = 0; r < 512; r++) {
                        fwrite(&sw[(size_t)r * stride], 2, 1024, f);
                    }
                    fclose(f);
                }
                f = fopen("/tmp/gles_hw.bin", "wb");
                if (f != NULL) {
                    fwrite(hw, 2, 1024 * 512, f);
                    fclose(f);
                }
                port_log("gles: vram dumped to /tmp/gles_sw.bin and /tmp/gles_hw.bin");
            }
            port_log("gles: vram check %ld (frame %ld): %.1f%% of the VRAM differs (%d pixels), first at %d,%d"
                     " sw 0x%04x hw 0x%04x, inside %d,%d..%d,%d; the undrawn counters say what is missing (triangles %u,"
                     " rects %u, segments %u, uploaded %lu px)",
                     checks, port_frames, 100.0 * (double)diffs / (1024.0 * 512.0), diffs, fx, fy, fsw, fhw,
                     bx0, by0, bx1, by1,
                     render_gles_undrawn(0), render_gles_undrawn(1), render_gles_undrawn(2),
                     render_gles_uploaded());
            {
                unsigned g = 0, s2 = 0, d = 0;

                render_gles_undrawn_reasons(&g, &s2, &d);
                port_log("gles: undrawn reasons: gouraud %u, semi %u, dither %u", g, s2, d);
            }
        }
    }
}
#endif

void port_video_frame(void) {
    int i, shot = 0, present;
    long long present_t0 = 0;
#ifdef PSXSTACK_SDL
    render_gpu_frame(); /* the rasteriser's units of this vsync, presented or not: its target is state */
    render_gpu_tex_frame();
    video_vram_check();
#endif
#if defined(PSXSTACK_SDL2) && defined(PSXSTACK_EGL)
    render_gles_frame();
    video_gles_vram_check();
#endif
    for (i = 0; i < video_shot_count; i++) {
        shot |= video_shots[i].frame == port_frames;
    }
    for (i = 0; i < video_gpu_shot_count; i++) {
        shot |= video_gpu_shots[i].frame == port_frames;
    }
    present = port_window && video_due();
    if (!present && !shot) {
        return;
    }
#if defined(PSXSTACK_SDL2)
    if (present && !shot && video_present_direct(&present_t0)) {
        return;
    }
#endif
    present_t0 = port_clock_ns();
    video_convert();
    for (i = 0; i < video_shot_count; i++) {
        if (video_shots[i].frame == port_frames) {
            video_write_ppm(video_shots[i].path);
        }
    }
    video_gpu_shots_due();
    if (present) {
        PsyqDisplay shown;

        video_present();
        psyq_gpu_display(&shown);
        video_present_stat(present_t0, 0, shown.rgb24 ? 24 : 15, video_w, video_h);
    }
}
