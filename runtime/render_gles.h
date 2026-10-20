/* The GLES renderer (render_gles.c; docs/gpu in the game repository, M1): the VRAM as an RGBA8 texture rendered
 * offscreen through EGL on a GBM device, with no window. Built only with PSXSTACK_EGL, which needs EGL, GLES and GBM
 * headers: the handheld has them, a desktop without the EGL headers does not, so the option is off by default.
 *
 * The picture still comes out through the software path's /dev/fb0 present until M1d; these functions are the renderer's
 * own contract, and the milestones after M1 add polygons (rectangles, then textured triangles, then the transparent
 * ones) behind the same listener. */
#ifndef PORT_RENDER_GLES_H
#define PORT_RENDER_GLES_H

#ifdef PSXSTACK_EGL
#include <stddef.h>

#include "port_harness.h"

/* Opens the EGL context (no window). 0 with the reason in why: the caller falls back to the software path. */
int render_gles_open(char *why, size_t why_size);
int render_gles_active(void);
/* "EGL 1.4, Mali-G31, OpenGL ES 3.2 ..." for the log; "" when closed. */
const char *render_gles_describe(void);
/* Creates the VRAM target and the read copy, installs the listener and turns gpu_set_listener_replaces on, from the
 * shim's VRAM as it is now. Native resolution only (scale must be 1). 0 on failure (why says it). */
int render_gles_raster_start(int scale, char *why, size_t why_size);
int render_gles_rasterising(void);
/* Once per vsync: the rectangles the transfers touched are uploaded, then the read copy is refreshed. */
void render_gles_frame(void);
/* The whole target as the shim's 16-bit pixels (1024x512): the exactness test's comparison with the software VRAM.
 * 0 when not rasterising or on failure. A save-time operation: 10.8 ms on the handheld. */
int render_gles_read_vram(unsigned short *out);
/* The events the listener could not draw yet, by GpuEventKind (0 triangle, 1 rect, 2 segment): M1 reports what is left
 * instead of pretending, so a test can assert the transfers match and print the rest as coverage. */
unsigned render_gles_undrawn(int kind);
/* The listener's entry point, the same function gpu.c calls: a test can feed a transfer sequence without the shim
 * (tests/gles_selftest.c). Ignored unless rasterising. */
void render_gles_feed(const void *ev);
void render_gles_close(void);
#endif

#endif /* PORT_RENDER_GLES_H */
