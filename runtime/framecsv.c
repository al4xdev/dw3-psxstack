/* One CSV line per frame when DW3_FRAME_CSV is set. Counts come from gpu.c.
 * Off unless that path is set: the note call returns immediately. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "port_harness.h"
#include "port_runtime.h"

enum { CSV_POLY, CSV_QUAD, CSV_TEX, CSV_LINE, CSV_RECT, CSV_COPY, CSV_N };

static int csv_on;
static unsigned csv_n[CSV_N];
static FILE *csv_file;
static long long csv_t0, csv_prev;
static int csv_lines;

void port_framecsv_note(int kind) {
    if (csv_on && (unsigned)kind < CSV_N) {
        csv_n[kind]++;
    }
}

void port_framecsv_open(const char *path) {
    if (path == NULL || path[0] == '\0' || csv_file != NULL) {
        return;
    }
    csv_file = fopen(path, "w");
    if (csv_file == NULL) {
        port_log("framecsv: cannot open %s", path);
        return;
    }
    port_file_line_buffered(csv_file);
    fputs("frame,t_ms,dt_ms,map,stage,file,polys,quads,textured,lines,rects,copies\n", csv_file);
    csv_t0 = csv_prev = port_clock_ns();
    csv_on = 1;
    port_log("framecsv: %s", path);
}

void port_framecsv_frame(void) {
    long long now, t, dt;
    if (!csv_on) {
        return;
    }
    now = port_clock_ns();
    t = (now - csv_t0) / 1000000;
    dt = (now - csv_prev) / 1000000;
    csv_prev = now;
    fprintf(csv_file, "%ld,%lld,%lld,0x%X,%d,%d,%u,%u,%u,%u,%u,%u\n", port_frames, t, dt,
            (unsigned)game_state_map(), (int)game_state_stage(), (int)game_state_file(), csv_n[CSV_POLY],
            csv_n[CSV_QUAD], csv_n[CSV_TEX], csv_n[CSV_LINE], csv_n[CSV_RECT], csv_n[CSV_COPY]);
    memset(csv_n, 0, sizeof csv_n);
    if (++csv_lines == 30) {
        csv_lines = 0;
        fflush(csv_file);
    }
}

void port_framecsv_close(void) {
    if (csv_file != NULL) {
        fflush(csv_file);
        fclose(csv_file);
        csv_file = NULL;
    }
    csv_on = 0;
}
