/* Save states (runtime/savestate.c; docs/PORT.md "Save states", docs/RUNTIME.md "The state file"): the whole machine
 * at the end of a vsync, written to a file and resumed from it later, by this same binary.
 *
 * Each module that holds state the game can observe has one sync function, `<module>_state(PortState *s)`, at the
 * end of its file, which names its variables in a fixed order with port_state_bytes: saving appends them, loading
 * reads them back in the same order (each block's tag and size must match, else the state is not this build's). The
 * same function does both, so the two directions cannot drift apart. Host-only state (SDL, files, caches, options)
 * is not named: the loading run keeps its own. */
#ifndef PSXSTACK_SAVESTATE_H
#define PSXSTACK_SAVESTATE_H

#include <stddef.h>

typedef struct PortState PortState;

/* One block, both directions: saving appends `size` bytes from `data` under `tag`; loading checks that the next block
 * has this tag and size (fatal otherwise) and copies it into `data`. The copies are byte-wise and uninstrumented: a
 * block may span AddressSanitizer's redzones (a game section, the game's stack). */
/* Both are declared weak: a harness that compiles a module without the runtime (the host replays of the goldens, the
 * sound replay) links without savestate.c, and never calls a sync function. */
void port_state_bytes(PortState *s, const char *tag, void *data, size_t size) __attribute__((weak));
#define PORT_STATE_VAR(s, var) port_state_bytes((s), #var, &(var), sizeof(var))
/* 1 while loading (the sync function then fixes up what depends on the loaded bytes: caches, listeners). */
int port_state_loading(const PortState *s) __attribute__((weak));

/* ---- The sync functions, called in this order by savestate.c (each at the end of its module's file) */
void port_audio_state(PortState *s);     /* audio.c: the vsyncs rendered (each vsync's sample count) */
void port_overlay_state(PortState *s);   /* overlay.c: every game section, the current overlay and word per tier */
void port_arena_state(PortState *s);     /* arena.c: the PS1 RAM */
void psyq_state(PortState *s);           /* psyq/psyq.c: every library of the shim (psyq_internal.h) */
void spu_state(PortState *s);            /* spu.c: the SPU's registers, voices and RAM */
void port_framelog_state(PortState *s);  /* framelog.c: the record so far (sequences, checkpoints, input trace) */
void port_script_state(PortState *s);    /* script.c: the script's progress (resumed by a run with the same script) */
void port_fiber_state(PortState *s);     /* fiber.c: the fiber table and every suspended fiber's stack */

/* ---- The runtime's side (savestate.c) */
/* Options (main.c): --save-state WHEN:FILE (WHEN a frame number, or a script checkpoint's name: the end of the frame
 * in which it ran; repeatable; 0 when malformed), --save-state-exit (exit 0 once every save is written),
 * --load-state FILE. port_savestate_wanted: any of them. */
int port_savestate_add(const char *spec);
void port_savestate_set_exit(void);
void port_savestate_set_load(const char *path);
int port_savestate_wanted(void);
/* The game on its own stack from now on (a state option, `required`: fatal when it cannot be; or the debug channel:
 * logged); before port_savestate_run. */
void port_savestate_arm(int required);
/* main's last call: game_main (on the game stack when armed), or the state to load resumed (never returns then).
 * `jumped`: main's setjmp returned from a reset or a load. */
void port_savestate_run(int jumped);
/* script.c: the checkpoint `name` ran in this frame (a --save-state NAME:FILE is due at the frame's end). */
void port_savestate_checkpoint(const char *name);
/* reset.c: the game stack is left for main's (the reset's longjmp). */
void port_savestate_leave_stack(void);
/* The debug channel (debug.c): a save as of the last vsync's end (the pause that follows it, or the capture point
 * itself: port_debug_state_point); a load: read and checked first (its frame in *frame), then applied through main
 * (port_savestate_load_request longjmps there). 0 with `err` set on failure. */
int port_savestate_save_now(const char *path, char *err, size_t err_size);
int port_savestate_load_prepare(const char *path, long *frame, char *err, size_t err_size);
void port_savestate_load_request(void) __attribute__((noreturn));
void port_debug_state_point(void);

/* The capture point (pump.c, at the end of every vsync, armed only): __builtin_setjmp keeps the context in this
 * function's frame (it must stay live: hence a macro, not a call); a load's __builtin_longjmp returns here with 1.
 * Clang's AArch64 backend lacks __builtin_setjmp/longjmp, so fall back to <setjmp.h> there (unmangled on Bionic). */
#if defined(__clang__) && defined(__aarch64__)
#include <setjmp.h>
extern jmp_buf port_savestate_ctx;
#define PORT_SAVESTATE_SETJMP() setjmp(port_savestate_ctx)
#define PORT_SAVESTATE_LONGJMP() longjmp(port_savestate_ctx, 1)
#else
extern void *port_savestate_ctx[5];
#define PORT_SAVESTATE_SETJMP() __builtin_setjmp(port_savestate_ctx)
#define PORT_SAVESTATE_LONGJMP() __builtin_longjmp(port_savestate_ctx, 1)
#endif
extern int port_savestate_armed;
void port_savestate_captured(void);
void port_savestate_resumed(void);
#define PORT_SAVESTATE_POINT()                                                                                         \
    do {                                                                                                               \
        if (port_savestate_armed) {                                                                                    \
            if (PORT_SAVESTATE_SETJMP() == 0) {                                                                        \
                port_savestate_captured();                                                                             \
            } else {                                                                                                   \
                port_savestate_resumed();                                                                              \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#endif
