#include "fbdev_present.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

static void fail(char *why, size_t why_size, const char *format, ...) {
    if (!why || !why_size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(why, why_size, format, args);
    va_end(args);
}

int Fbdev_Validate(const FbdevPresent *fb, char *why, size_t why_size) {
    if (!fb || !fb->pixels) {
        fail(why, why_size, "framebuffer is not mapped");
        return -1;
    }
    if (fb->bits_per_pixel != 32) {
        fail(why, why_size, "need 32bpp framebuffer (got %u)", fb->bits_per_pixel);
        return -1;
    }
    if (fb->xres < 640 || fb->yres < 480 || fb->line_length < fb->xres * 4 || (fb->line_length & 3)) {
        fail(why, why_size, "need at least 640x480 framebuffer with 32-bit stride");
        return -1;
    }
    if (fb->red_length == 0 || fb->red_length > 8 || fb->red_offset > 31 || (fb->red_offset + fb->red_length > 32) ||
        fb->green_length == 0 || fb->green_length > 8 || fb->green_offset > 31 || (fb->green_offset + fb->green_length > 32) ||
        fb->blue_length == 0 || fb->blue_length > 8 || fb->blue_offset > 31 || (fb->blue_offset + fb->blue_length > 32)) {
        fail(why, why_size, "unsupported framebuffer RGB bitfields");
        return -1;
    }
    uint64_t needed = (uint64_t)fb->line_length * fb->yres;
    if (needed > 0xffffffffULL) {
        fail(why, why_size, "framebuffer dimensions overflow");
        return -1;
    }
    if (fb->map_size < (size_t)needed) {
        fail(why, why_size, "framebuffer map is shorter than visible memory");
        return -1;
    }
    return 0;
}

/* The display size whose letterbox was last cleared (Fbdev_PresentVram); reset when the device is reopened. */
static int fbdev_cleared_w, fbdev_cleared_h;

int Fbdev_Open(FbdevPresent *fb, const char *device, char *why, size_t why_size) {
    if (!fb || !device) {
        fail(why, why_size, "invalid arguments");
        return -1;
    }
    fbdev_cleared_w = fbdev_cleared_h = 0;
    memset((uint8_t *)fb + 4, 0, sizeof(*fb) - 4);
    fb->fd = -1;
    fb->fd = open(device, O_RDWR | O_CLOEXEC);
    if (fb->fd < 0) {
        fail(why, why_size, "cannot open %s: %s", device, strerror(errno));
        return -1;
    }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        fail(why, why_size, "cannot query %s: %s", device, strerror(errno));
        Fbdev_Close(fb);
        return -1;
    }
    fb->xres = var.xres;
    fb->yres = var.yres;
    fb->line_length = fix.line_length;
    fb->bits_per_pixel = var.bits_per_pixel;
    fb->red_offset = var.red.offset;
    fb->red_length = var.red.length;
    fb->green_offset = var.green.offset;
    fb->green_length = var.green.length;
    fb->blue_offset = var.blue.offset;
    fb->blue_length = var.blue.length;
    uint64_t min_size = (uint64_t)fb->line_length * fb->yres;
    if (min_size > fix.smem_len) {
        fail(why, why_size, "framebuffer map is shorter than visible memory");
        Fbdev_Close(fb);
        return -1;
    }
    fb->pixels = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->pixels == MAP_FAILED) {
        fb->pixels = NULL;
        fail(why, why_size, "cannot map %s: %s", device, strerror(errno));
        Fbdev_Close(fb);
        return -1;
    }
    fb->map_size = fix.smem_len;
    if (Fbdev_Validate(fb, why, why_size) < 0) {
        Fbdev_Close(fb);
        return -1;
    }
    return 0;
}

void Fbdev_Close(FbdevPresent *fb) {
    if (!fb) return;
    if (fb->pixels) {
        munmap(fb->pixels, fb->map_size);
    }
    if (fb->fd >= 0) {
        close(fb->fd);
    }
    memset((uint8_t *)fb + 4, 0, sizeof(*fb) - 4);
    fb->fd = -1;
}

int Fbdev_Present(FbdevPresent *fb, const uint32_t *pixels, int stride, int width, int height) {
    if (Fbdev_Validate(fb, NULL, 0) < 0) return -1;
    if (stride < width || !pixels || width <= 0 || height <= 0) return -1;

    int scale_x = (width <= 320) ? 2 : 1;
    int scale_y = (height <= 256) ? 2 : 1;

    int dst_w = width * scale_x;
    int dst_h = height * scale_y;

    int src_x_off = 0;
    int src_y_off = 0;
    int copy_w = width;
    int copy_h = height;

    int dst_x = ((int)fb->xres - dst_w) / 2;
    int dst_y = ((int)fb->yres - dst_h) / 2;

    if (dst_x < 0) {
        src_x_off = (-dst_x) / scale_x;
        dst_x = 0;
        copy_w = (int)fb->xres / scale_x;
    }
    if (dst_y < 0) {
        src_y_off = (-dst_y) / scale_y;
        dst_y = 0;
        copy_h = (int)fb->yres / scale_y;
    }

    int is_argb = (fb->red_offset == 16 && fb->red_length == 8 &&
                   fb->green_offset == 8 && fb->green_length == 8 &&
                   fb->blue_offset == 0 && fb->blue_length == 8);

    for (int y = 0; y < copy_h; y++) {
        const uint32_t *src_row = pixels + (src_y_off + y) * stride + src_x_off;
        uint32_t *dst_row1 = (uint32_t *)(fb->pixels + (dst_y + y * scale_y) * fb->line_length) + dst_x;
        uint32_t *dst_row2 = (scale_y == 2) ? (uint32_t *)(fb->pixels + (dst_y + y * 2 + 1) * fb->line_length) + dst_x : NULL;

        if (is_argb) {
            if (scale_x == 2) {
                int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                uint32x4_t mask = vdupq_n_u32(0x00ffffff);
                for (; x + 4 <= copy_w; x += 4) {
                    uint32x4_t in = vld1q_u32(src_row + x);
                    in = vandq_u32(in, mask);

                    uint32x4x2_t zipped = vzipq_u32(in, in);

                    vst1q_u32(dst_row1 + x * 2, zipped.val[0]);
                    vst1q_u32(dst_row1 + x * 2 + 4, zipped.val[1]);
                    if (dst_row2) {
                        vst1q_u32(dst_row2 + x * 2, zipped.val[0]);
                        vst1q_u32(dst_row2 + x * 2 + 4, zipped.val[1]);
                    }
                }
#endif
                for (; x < copy_w; x++) {
                    uint32_t p = src_row[x] & 0x00ffffff;
                    dst_row1[x * 2] = p;
                    dst_row1[x * 2 + 1] = p;
                    if (dst_row2) {
                        dst_row2[x * 2] = p;
                        dst_row2[x * 2 + 1] = p;
                    }
                }
            } else {
                int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                uint32x4_t mask = vdupq_n_u32(0x00ffffff);
                for (; x + 4 <= copy_w; x += 4) {
                    uint32x4_t in = vld1q_u32(src_row + x);
                    uint32x4_t masked = vandq_u32(in, mask);
                    vst1q_u32(dst_row1 + x, masked);
                    if (dst_row2) vst1q_u32(dst_row2 + x, masked);
                }
#endif
                for (; x < copy_w; x++) {
                    uint32_t p = src_row[x] & 0x00ffffff;
                    dst_row1[x] = p;
                    if (dst_row2) dst_row2[x] = p;
                }
            }
        } else {
            for (int x = 0; x < copy_w; x++) {
                uint32_t c = src_row[x];
                uint8_t r = (c >> 16) & 0xff;
                uint8_t g = (c >> 8) & 0xff;
                uint8_t b = c & 0xff;
                uint32_t pr = ((r * ((1u << fb->red_length) - 1) + 127) / 255) << fb->red_offset;
                uint32_t pg = ((g * ((1u << fb->green_length) - 1) + 127) / 255) << fb->green_offset;
                uint32_t pb = ((b * ((1u << fb->blue_length) - 1) + 127) / 255) << fb->blue_offset;
                uint32_t p = pr | pg | pb;
                if (scale_x == 2) {
                    dst_row1[x * 2] = p;
                    dst_row1[x * 2 + 1] = p;
                    if (dst_row2) {
                        dst_row2[x * 2] = p;
                        dst_row2[x * 2 + 1] = p;
                    }
                } else {
                    dst_row1[x] = p;
                    if (dst_row2) dst_row2[x] = p;
                }
            }
        }
    }
    return 0;
}

int Fbdev_PresentNative(FbdevPresent *fb, const uint32_t *pixels, int stride, int width, int height) {
    return Fbdev_Present(fb, pixels, stride, width, height);
}

int Fbdev_WaitVsync(FbdevPresent *fb) {
    if (!fb || fb->fd < 0) return -1;
    uint32_t arg = 0;
    return ioctl(fb->fd, FBIO_WAITFORVSYNC, &arg);
}

/* The measured present cost was the VRAM expand (video.c, -O0, one call per pixel) plus the 2x blit.
 * This writes the display rectangle straight to an XRGB8888 framebuffer at integer 2x. 5-bit channels
 * expand as (c << 3) | (c >> 2), the same bits video.c's converter uses. */
static int fb_xrgb8(const FbdevPresent *fb) {
    return fb->red_offset == 16 && fb->red_length == 8 && fb->green_offset == 8 && fb->green_length == 8 &&
           fb->blue_offset == 0 && fb->blue_length == 8;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* A row's tail (n not a multiple of eight) and the 24-bit rows' tail: a 5-bit channel expands to 8 as (c << 3) | (c >> 2). */
static uint32_t fb_pix15(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

static void fb_store_pair(uint32_t *dst, uint32_t *dst2, int i, uint32_t p) {
    dst[i * 2] = p;
    dst[i * 2 + 1] = p;
    if (dst2) {
        dst2[i * 2] = p;
        dst2[i * 2 + 1] = p;
    }
}

static void fb_store_2x8(uint8x8_t b, uint8x8_t g, uint8x8_t r, uint32_t *dst, uint32_t *dst2) {
    uint16x8_t gb = vorrq_u16(vmovl_u8(b), vshlq_n_u16(vmovl_u8(g), 8));
    uint16x8_t rr = vmovl_u8(r);
    uint32x4_t lo = vorrq_u32(vmovl_u16(vget_low_u16(gb)), vshlq_n_u32(vmovl_u16(vget_low_u16(rr)), 16));
    uint32x4_t hi = vorrq_u32(vmovl_u16(vget_high_u16(gb)), vshlq_n_u32(vmovl_u16(vget_high_u16(rr)), 16));
    uint32x4x2_t z0 = vzipq_u32(lo, lo);
    uint32x4x2_t z1 = vzipq_u32(hi, hi);

    vst1q_u32(dst, z0.val[0]);
    vst1q_u32(dst + 4, z0.val[1]);
    vst1q_u32(dst + 8, z1.val[0]);
    vst1q_u32(dst + 12, z1.val[1]);
    if (dst2) {
        vst1q_u32(dst2, z0.val[0]);
        vst1q_u32(dst2 + 4, z0.val[1]);
        vst1q_u32(dst2 + 8, z1.val[0]);
        vst1q_u32(dst2 + 12, z1.val[1]);
    }
}

static void fb_row15_neon(const uint16_t *src, int n, uint32_t *dst, uint32_t *dst2) {
    uint16x8_t mask = vdupq_n_u16(31);
    int x = 0;

    for (; x + 8 <= n; x += 8) {
        uint16x8_t c = vld1q_u16(src + x);
        uint16x8_t r5 = vandq_u16(c, mask);
        uint16x8_t g5 = vandq_u16(vshrq_n_u16(c, 5), mask);
        uint16x8_t b5 = vandq_u16(vshrq_n_u16(c, 10), mask);
        uint16x8_t r = vorrq_u16(vshlq_n_u16(r5, 3), vshrq_n_u16(r5, 2));
        uint16x8_t g = vorrq_u16(vshlq_n_u16(g5, 3), vshrq_n_u16(g5, 2));
        uint16x8_t b = vorrq_u16(vshlq_n_u16(b5, 3), vshrq_n_u16(b5, 2));

        fb_store_2x8(vmovn_u16(b), vmovn_u16(g), vmovn_u16(r), dst + x * 2, dst2 ? dst2 + x * 2 : NULL);
    }
    for (; x < n; x++) {
        fb_store_pair(dst, dst2, x, fb_pix15(src[x]));
    }
}

static void fb_row24_neon(const uint8_t *src, int n, uint32_t *dst, uint32_t *dst2) {
    int x = 0;

    for (; x + 8 <= n; x += 8) {
        uint8x8x3_t rgb = vld3_u8(src + x * 3);

        fb_store_2x8(rgb.val[2], rgb.val[1], rgb.val[0], dst + x * 2, dst2 ? dst2 + x * 2 : NULL);
    }
    for (; x < n; x++) {
        const uint8_t *s = src + x * 3;
        fb_store_pair(dst, dst2, x, ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]);
    }
}
#endif

int Fbdev_PresentVram(FbdevPresent *fb, const uint16_t *vram, int x, int y, int w, int h, int rgb24) {
    if (Fbdev_Validate(fb, NULL, 0) < 0 || !vram || w <= 0 || h <= 0) {
        return -1;
    }
    /* Same scale as Fbdev_Present: 320-wide is 2x across, a 240-line picture is 2x down, a 480-line movie is not,
     * and a taller one (the 576-line title) is cropped to the panel. */
    if (!fb_xrgb8(fb) || w > 320 || h > 576 || w * 2 > (int)fb->xres) {
        return -1;
    }
#if !defined(__ARM_NEON) && !defined(__ARM_NEON__)
    /* The expand is the NEON one: without it video.c converts and calls Fbdev_Present. */
    (void)x;
    (void)y;
    (void)rgb24;
    return -1;
#else
    int sx = x & 1023, sy = y & 511, dst_x, dst_y, row, copy_h, src_y_off;
    int scale_y = h <= 256 ? 2 : 1;

    if (sx + w > 1024) {
        return -1;
    }
    if (rgb24 && sx * 2 + w * 3 > 2048) {
        return -1;
    }
    dst_x = ((int)fb->xres - w * 2) / 2;
    dst_y = ((int)fb->yres - h * scale_y) / 2;
    src_y_off = 0;
    if (dst_y < 0) {
        src_y_off = (-dst_y) / scale_y;
        dst_y = 0;
    }
    copy_h = h - src_y_off;
    if (copy_h > (int)fb->yres / scale_y) {
        copy_h = (int)fb->yres / scale_y;
    }
    if (w != fbdev_cleared_w || h != fbdev_cleared_h) {
        memset(fb->pixels, 0, (size_t)fb->line_length * fb->yres);
        fbdev_cleared_w = w;
        fbdev_cleared_h = h;
    }
    for (row = 0; row < copy_h; row++) {
        int vy = (sy + src_y_off + row) & 511;
        const uint16_t *src = vram + (size_t)vy * 1024 + sx;
        uint32_t *dst = (uint32_t *)(fb->pixels + (dst_y + row * scale_y) * fb->line_length) + dst_x;
        uint32_t *dst2 = scale_y == 2
                             ? (uint32_t *)(fb->pixels + (dst_y + row * 2 + 1) * fb->line_length) + dst_x
                             : NULL;

        if (rgb24) {
            fb_row24_neon((const uint8_t *)(vram + (size_t)vy * 1024) + sx * 2, w, dst, dst2);
        } else {
            fb_row15_neon(src, w, dst, dst2);
        }
    }
    return 0;
#endif
}
