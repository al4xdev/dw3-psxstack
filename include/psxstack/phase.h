/* Per-frame CPU buckets (runtime/phasetime.c). Off unless DW3_PHASE is set: begin/end are one load and a branch.
 * Nested: entering a phase pauses the one outside it, so GTE inside a model draw is not counted twice.
 * The storage is weak so a harness that links gpu.c/gte.c/mdec.c without the runtime still links; the game's
 * phasetime.c supplies the strong object. */
#ifndef PSXSTACK_PHASE_H
#define PSXSTACK_PHASE_H

#include <time.h>

enum {
    PORT_PHASE_MDEC = 0,
    PORT_PHASE_GTE,
    PORT_PHASE_MODEL,
    PORT_PHASE_GPU,
    PORT_PHASE_AUDIO,
    PORT_PHASE_PRESENT,
    PORT_PHASE_N
};

enum { PORT_PHASE_STACK = 8 };

typedef struct PortPhase {
    int on;
    int paused; /* port_pace / pause: time here is not a frame's CPU */
    int sp;
    int stack[PORT_PHASE_STACK];
    long long mark;
    long long acc[PORT_PHASE_N];
    unsigned calls[PORT_PHASE_N];
    unsigned clocks; /* port_phase_now calls this frame, so the log can subtract their cost from "rest" */
} PortPhase;

#ifndef PORT_PHASE_STRONG
__attribute__((weak)) PortPhase port_phase;
#else
extern PortPhase port_phase;
#endif

static inline long long port_phase_now(void) {
    struct timespec ts;

    port_phase.clocks++;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline void port_phase_begin(int id) __attribute__((unused));
static inline void port_phase_begin(int id) {
    long long now;

    if (!port_phase.on || port_phase.paused || (unsigned)id >= PORT_PHASE_N) {
        return;
    }
    now = port_phase_now();
    if (port_phase.sp > 0) {
        port_phase.acc[port_phase.stack[port_phase.sp - 1]] += now - port_phase.mark;
    }
    if (port_phase.sp < PORT_PHASE_STACK) {
        port_phase.stack[port_phase.sp++] = id;
    }
    port_phase.calls[id]++;
    port_phase.mark = port_phase_now();
}

static inline void port_phase_end(void) __attribute__((unused));
static inline void port_phase_end(void) {
    long long now;
    int id;

    if (!port_phase.on || port_phase.paused || port_phase.sp <= 0) {
        return;
    }
    now = port_phase_now();
    id = port_phase.stack[--port_phase.sp];
    port_phase.acc[id] += now - port_phase.mark;
    port_phase.mark = port_phase_now();
}

void port_phase_init(void);
void port_phase_frame_close(void); /* end of the frame's CPU, before port_pace sleeps */
void port_phase_resume(void);      /* after the sleep, or after a pause: the next frame's CPU starts */
void port_phase_suspend(void);     /* a wait that is not the frame (the pause loop) */
void port_phase_close(void);

#endif
