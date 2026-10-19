/* One line a second when DW3_PHASE is set (a path, or "1" for stderr only). Averages are CPU time per
 * vsync, excluding the pace sleep: that is the slowdown, not the time the port spent waiting to be on time.
 * Buckets are exclusive (phase.h). "rest" is the frame minus those buckets minus the timer calls themselves. */
#define PORT_PHASE_STRONG 1
#include "phase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "platform.h"
#include "port_harness.h"
#include "port_runtime.h"

PortPhase port_phase;

static long long phase_frame_t0;
static long long phase_clock_cost;
static FILE *phase_file;

static long long sec_t0;
static int sec_frames;
static long long sec_cpu;
static long long sec_acc[PORT_PHASE_N];
static unsigned sec_calls[PORT_PHASE_N];
static long long sec_clocks;

static void phase_calibrate(void) {
    enum { N = 2000 };
    long long t0;
    int i;

    t0 = port_phase_now();
    for (i = 0; i < N; i++) {
        (void)port_phase_now();
    }
    phase_clock_cost = (port_phase_now() - t0) / (N + 1);
    if (phase_clock_cost < 0) {
        phase_clock_cost = 0;
    }
}

void port_phase_init(void) {
    const char *env = getenv("DW3_PHASE");
    long long unused;

    if (env == NULL || env[0] == '\0' || strcmp(env, "0") == 0) {
        return;
    }
    /* Calibrate before `on`, so these clock calls are not a frame. */
    unused = port_phase.clocks;
    phase_calibrate();
    port_phase.clocks = (unsigned)unused;
    port_phase.on = 1;
    if (strcmp(env, "1") != 0) {
        phase_file = fopen(env, "a");
        if (phase_file == NULL) {
            port_log("phase: cannot open %s", env);
        } else {
            port_file_line_buffered(phase_file);
            fprintf(phase_file, "# cpu ms/frame excluding pace sleep; budget is 1000/rate; rest excludes timer calls\n");
        }
    }
    port_log("phase: on (clock ~%lld ns); mdec gte model gpu audio present rest", phase_clock_cost);
}

static void phase_freeze(void) {
    long long now;

    if (!port_phase.on || port_phase.paused) {
        return;
    }
    now = port_phase_now();
    if (port_phase.sp > 0) {
        port_phase.acc[port_phase.stack[port_phase.sp - 1]] += now - port_phase.mark;
    }
    port_phase.mark = now;
    port_phase.paused = 1;
}

static void phase_emit(long long wall) {
    double n = sec_frames > 0 ? (double)sec_frames : 1.0;
    double sec = sec_t0 > 0 ? (double)(wall - sec_t0) / 1e9 : 1.0;
    double budget = port_rate > 0 ? 1000.0 / (double)port_rate : 0.0;
    double ms[PORT_PHASE_N];
    double cpu, rest, ovh, setup;
    long long sum;
    int i;
    char line[512];

    if (sec <= 0.0) {
        sec = 1.0;
    }
    sum = 0;
    for (i = 0; i < PORT_PHASE_N; i++) {
        sum += sec_acc[i];
        ms[i] = (double)sec_acc[i] / n / 1e6;
    }
    cpu = (double)sec_cpu / n / 1e6;
    ovh = (double)sec_clocks * (double)phase_clock_cost / n / 1e6;
    rest = (double)(sec_cpu - sum) / n / 1e6 - ovh;
    setup = ms[PORT_PHASE_GTE] + ms[PORT_PHASE_MODEL];
    snprintf(line, sizeof line,
             "phase: fps %.0f cpu %.1f ms budget %.1f | mdec %.1f gte %.1f model %.1f setup %.1f gpu %.1f "
             "audio %.1f present %.1f rest %.1f ovh %.1f | map 0x%X stage %d | calls m %u g %u mdl %u gpu %u",
             (double)sec_frames / sec, cpu, budget, ms[PORT_PHASE_MDEC], ms[PORT_PHASE_GTE], ms[PORT_PHASE_MODEL],
             setup, ms[PORT_PHASE_GPU], ms[PORT_PHASE_AUDIO], ms[PORT_PHASE_PRESENT], rest, ovh,
             (unsigned)game_state_map(), (int)game_state_stage(), sec_calls[PORT_PHASE_MDEC], sec_calls[PORT_PHASE_GTE],
             sec_calls[PORT_PHASE_MODEL], sec_calls[PORT_PHASE_GPU]);
    port_log("%s", line);
    if (phase_file != NULL) {
        fprintf(phase_file, "%s\n", line);
    }
    sec_frames = 0;
    sec_cpu = 0;
    sec_clocks = 0;
    memset(sec_acc, 0, sizeof sec_acc);
    memset(sec_calls, 0, sizeof sec_calls);
    sec_t0 = wall;
}

void port_phase_frame_close(void) {
    long long now, cpu;
    int i;

    if (!port_phase.on) {
        return;
    }
    phase_freeze();
    if (phase_frame_t0 == 0) {
        return;
    }
    now = port_phase.mark;
    cpu = now - phase_frame_t0;
    if (cpu < 0) {
        cpu = 0;
    }
    sec_cpu += cpu;
    sec_clocks += port_phase.clocks;
    for (i = 0; i < PORT_PHASE_N; i++) {
        sec_acc[i] += port_phase.acc[i];
        sec_calls[i] += port_phase.calls[i];
        port_phase.acc[i] = 0;
        port_phase.calls[i] = 0;
    }
    port_phase.clocks = 0;
    sec_frames++;
    if (sec_t0 == 0) {
        sec_t0 = now;
    }
    if (now - sec_t0 >= 1000000000LL) {
        phase_emit(now);
    }
}

void port_phase_resume(void) {
    long long now;

    if (!port_phase.on) {
        return;
    }
    now = port_phase_now();
    phase_frame_t0 = now;
    port_phase.mark = now;
    port_phase.paused = 0;
    port_phase.clocks = 0;
    memset(port_phase.acc, 0, sizeof port_phase.acc);
    memset(port_phase.calls, 0, sizeof port_phase.calls);
}

void port_phase_suspend(void) {
    phase_freeze();
}

void port_phase_close(void) {
    if (phase_file != NULL) {
        fflush(phase_file);
        fclose(phase_file);
        phase_file = NULL;
    }
    port_phase.on = 0;
}
