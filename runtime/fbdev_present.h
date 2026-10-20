#ifndef PORT_FBDEV_PRESENT_H
#define PORT_FBDEV_PRESENT_H

#include <stddef.h>
#include <stdint.h>

/* Linux fbdev presenter for R36S / embedded ARM handhelds.
 * Blits 32-bit ARGB pixels directly to the mapped /dev/fb0 framebuffer
 * with 2x integer scaling or native resolution centering, accelerated by ARM NEON. */
typedef struct FbdevPresent {
    int fd;
    uint8_t *pixels;
    size_t map_size;
    unsigned xres, yres, line_length;
    unsigned bits_per_pixel;
    unsigned red_offset, red_length;
    unsigned green_offset, green_length;
    unsigned blue_offset, blue_length;
} FbdevPresent;

/* Open and map device (normally /dev/fb0). Returns 0 on success, -1 on failure. */
int Fbdev_Open(FbdevPresent *fb, const char *device, char *why, size_t why_size);
void Fbdev_Close(FbdevPresent *fb);

/* Validate a populated descriptor. */
int Fbdev_Validate(const FbdevPresent *fb, char *why, size_t why_size);

/* Copy one composed canvas at 2x nearest-neighbour, centred in the display. */
int Fbdev_Present(FbdevPresent *fb, const uint32_t *pixels, int stride, int width, int height);

/* Wait for VBlank using FBIO_WAITFORVSYNC on the framebuffer device. */
int Fbdev_WaitVsync(FbdevPresent *fb);

/* Copy a picture at native resolution, centred in the framebuffer (for 480-line displays). */
int Fbdev_PresentNative(FbdevPresent *fb, const uint32_t *pixels, int stride, int width, int height);

/* Expand a PS1 display rectangle straight into the framebuffer at integer 2x.
 * 15-bit: one VRAM pixel per screen pixel. 24-bit: 3 bytes (R,G,B) per screen pixel.
 * VRAM is 1024x512 of u16, row-major, `stride` pixels per row (psyq_gpu_vram_stride: 1024 visible plus padding).
 * Returns 0 when it drew, -1 when the caller should use Fbdev_Present (not XRGB8888, not a 2x fit, or the
 * rectangle wraps). */
int Fbdev_PresentVram(FbdevPresent *fb, const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24);

#endif
