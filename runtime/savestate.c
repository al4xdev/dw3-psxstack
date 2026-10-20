/* Save states (include/psxstack/savestate.h; docs/PORT.md "Save states", docs/RUNTIME.md "The state file"): the
 * whole machine at the end of a vsync in a file, and a later run of the same binary resumed from it.
 *
 * What a state holds, in this order (each module's sync function names its own part):
 *  - the header: the format, the binary's SHA-1, the pointer size, the addresses of the image, the arena and the game
 *    stack, the game's id, the rate, the frame, the stack protector's canary;
 *  - the runtime: the frame count, the audio's vsync count (its samples per vsync), every game section (the EXE's and
 *    the overlays' .data and .bss) and the current overlay per tier, the arena;
 *  - the Psy-Q shim (psyq.c psyq_state: every library's state the game can observe), the SPU;
 *  - the run's record so far (framelog.c) and the script's progress (script.c), so that a script resumed from a state
 *    ends with the record of the straight run;
 *  - the fibers (fiber.c port_fiber_state: the table, every suspended fiber's live stack, the pending preemption);
 *  - the game's execution context: the stack the vsync ended on (the game stack, or a fiber's) and the registers of
 *    its vsync.
 * Not in it: what the host owns (the window, the audio device, files, the debug channel, the options, caches that
 * are rebuilt: gpu.c's decoded textures, the hardware renderer's VRAM, reloaded from the software VRAM as at power-on),
 * the memory cards' contents (media, like the disc: the loading run's --memcard1/2) and the game's mods' own state.
 *
 * The game's execution context. The game is native code: at a vsync its C stack holds its frames, from game_main
 * down to the pump (VSync or a PLATFORM_WAIT, psyq_vsync_tick, pump.c port_frame), and those frames hold host
 * pointers: return addresses, frame pointers, pointers to the game's globals, the arena and the stack itself. Game
 * memory holds host pointers too (function pointers in the game's objects, the callbacks the shim keeps). A state can
 * hold them as they are only if every address is the same in the loading run, so:
 *  - the game runs on a stack of its own, a static array (state_stack below), whenever states may be used (an option
 *    here or the debug channel): its address is fixed with the image;
 *  - the image is at a fixed address: ELF builds are linked non-PIE (cmake/psxstack.cmake); a PE image may be moved
 *    by ASLR (the header's addresses then differ and the load is refused);
 *  - everything a state holds points into the image (code, the game's sections, the arena, the shim's statics, the
 *    game stack), never into the heap (the memory cards' images are the one heap buffer the shim reaches: kept out).
 * The context is the end of port_frame, the last thing of every vsync before the pause (pump.c PORT_SAVESTATE_POINT):
 * there __builtin_setjmp keeps the frame and stack pointers and the resume address (not glibc's setjmp, whose jmp_buf
 * is mangled with a per-process key), and the function saves every callee-saved register in its frame, so the stack
 * above that point and that buffer are the whole context. A save copies them; a load writes the stack back, then
 * __builtin_longjmp's into it from main's stack: port_frame returns into the vsync that was saved.
 *
 * AddressSanitizer: the game stack is announced to it as a fiber; its fake stacks (detect_stack_use_after_return, on
 * by default) put frames on the heap, which a state cannot hold: a run with states needs
 * ASAN_OPTIONS=detect_stack_use_after_return=0 (checked: fatal otherwise). A build with a stack protector has the
 * canary in every frame: the loading run takes the saved one (glibc on x86, its thread pointer's word; elsewhere a
 * state from such a build is refused). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "port_harness.h"
#include "port_runtime.h"
#include "psyq.h"
#include "savestate.h"
#include "sha1.h"
#include "spu.h"

#if defined(__SANITIZE_ADDRESS__)
#define STATE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STATE_ASAN 1
#endif
#endif
#ifdef STATE_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

#if defined(__SSP__) || defined(__SSP_STRONG__) || defined(__SSP_ALL__) || defined(__SSP_EXPLICIT__)
#if defined(__GLIBC__) && (defined(__x86_64__) || defined(__i386__))
#define STATE_CANARY 1 /* the guard is the word at %fs:0x28 (x86_64) or %gs:0x14 (i386): glibc's tcbhead_t */
#else
#define STATE_CANARY_UNKNOWN 1
#endif
#endif

#define STATE_MAGIC "PSXSTATE"
#define STATE_VERSION 1
#define STATE_STACK_SIZE (8u << 20)
#define STATE_STACK_TOP_PAD 64 /* above the first frame: zeros, where an unwinder looking past it finds nothing */
#define STATE_MAX_SAVES 16

struct PortState {
    int loading;
    const char *path;
    u8 *data;
    size_t n, cap, pos;
};

/* ---- The game stack and the context */

static u8 state_stack[STATE_STACK_SIZE] __attribute__((aligned(64)));
int port_savestate_armed;          /* the game runs on state_stack; port_frame keeps its context */
void *port_savestate_ctx[5];       /* __builtin_setjmp's buffer at the last vsync's end (pump.c) */
/* The stack the context is on: the game stack, or the fiber's the vsync ended on (fiber.c): a state resumes that one. */
static u8 *state_cur_base;         /* that stack's lowest byte and its size, at the last capture */
static size_t state_cur_size;
static u8 *state_stack_lo;         /* the lowest byte of it the context needs, at the last capture */
static u8 *state_stack_copy;       /* [state_stack_lo, top) at the last capture (a save while paused uses it) */
static size_t state_stack_copy_n, state_stack_copy_cap;
#ifdef STATE_ASAN
static const void *state_main_bottom; /* main's stack (AddressSanitizer's fiber switch back, at a reset) */
static size_t state_main_size;
#endif

/* ---- The requests */

typedef struct StateSave {
    long frame;    /* > 0: at this frame's end */
    char *name;    /* else: at the end of the frame in which the script's checkpoint `name` ran */
    char *path;
    int done;
} StateSave;
static StateSave state_saves[STATE_MAX_SAVES];
static int state_save_count, state_save_exit;
static const char *state_load_path;  /* --load-state, or the debug channel's load: resumed by port_savestate_run */
static PortState state_loaded;        /* a validated state waiting to be applied (its data) */
static int state_load_ready;
static const char *state_checkpoint;  /* the checkpoint that ran in this frame (script.c) */

/* ---- Raw copies: under AddressSanitizer a block may span its redzones (the game's sections, the game stack's
 * frames), which memcpy would report: byte by byte, uninstrumented, there. */

#ifdef STATE_ASAN
__attribute__((no_sanitize_address)) static void state_copy_raw(void *dst, const void *src, size_t n) {
    volatile u8 *d = dst;
    const volatile u8 *s = src;
    while (n--) {
        *d++ = *s++;
    }
}
#else
static void state_copy_raw(void *dst, const void *src, size_t n) {
    if (n != 0) {
        memcpy(dst, src, n);
    }
}
#endif

/* ---- Blocks */

static void state_grow(PortState *s, size_t more) {
    if (s->n + more > s->cap) {
        size_t cap = s->cap ? s->cap : (16u << 20);
        while (cap < s->n + more) {
            cap *= 2;
        }
        s->data = realloc(s->data, cap);
        if (s->data == NULL) {
            port_fatal("state: out of memory");
        }
        s->cap = cap;
    }
}

static void state_fail(const PortState *s, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void state_fail(const PortState *s, const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    port_fatal("state %s: %s", s->path, msg);
}

/* Block layout: the tag's length (1 byte) and characters, the size (8 bytes, the host's order), the bytes. */
void port_state_bytes(PortState *s, const char *tag, void *data, size_t size) {
    size_t tn = strlen(tag);
    uint64_t sz = size;
    if (tn > 255) {
        port_fatal("state: tag %.32s...: too long", tag);
    }
    if (!s->loading) {
        state_grow(s, 1 + tn + sizeof(sz) + size);
        s->data[s->n++] = (u8)tn;
        memcpy(s->data + s->n, tag, tn);
        s->n += tn;
        memcpy(s->data + s->n, &sz, sizeof(sz));
        s->n += sizeof(sz);
        state_copy_raw(s->data + s->n, data, size);
        s->n += size;
        return;
    }
    if (s->pos + 1 > s->n || s->pos + 1 + s->data[s->pos] + sizeof(sz) > s->n) {
        state_fail(s, "ends before block %s (truncated)", tag);
    }
    if (s->data[s->pos] != tn || memcmp(s->data + s->pos + 1, tag, tn) != 0) {
        state_fail(s, "block %.*s where %s was expected: not a state of this build", (int)s->data[s->pos],
                   (const char *)s->data + s->pos + 1, tag);
    }
    s->pos += 1 + tn;
    memcpy(&sz, s->data + s->pos, sizeof(sz));
    s->pos += sizeof(sz);
    if (sz != size) {
        state_fail(s, "block %s: %llu bytes where this build has %zu: not a state of this build", tag,
                   (unsigned long long)sz, size);
    }
    if (s->pos + size > s->n) {
        state_fail(s, "ends inside block %s (truncated)", tag);
    }
    state_copy_raw(data, s->data + s->pos, size);
    s->pos += size;
}

int port_state_loading(const PortState *s) {
    return s->loading;
}

/* ---- The header */

/* The running binary's SHA-1 (40 hex digits): a state belongs to one binary. */
static void state_exe_sha1(char hex[41]) {
    static char cached[41];
    char path[4096];
    u8 buf[65536];
    PortSha1 c;
    uint8_t digest[20];
    long long off = 0, n;
    int fd;
    if (cached[0] != '\0') {
        memcpy(hex, cached, 41);
        return;
    }
    if (!port_exe_path(path, sizeof(path)) || (fd = port_file_open_read(path)) < 0) {
        port_fatal("state: cannot read the running binary (to name it in the state)");
    }
    port_sha1_init(&c);
    while ((n = port_file_pread(fd, buf, sizeof(buf), off)) > 0) {
        port_sha1_update(&c, buf, (size_t)n);
        off += n;
    }
    port_file_close(fd);
    if (n < 0) {
        port_fatal("state: cannot read the running binary %s", path);
    }
    port_sha1_final(&c, digest);
    port_sha1_hex(digest, cached);
    memcpy(hex, cached, 41);
}

static uint64_t state_canary(void) {
    uint64_t v = 0;
#if defined(STATE_CANARY) && defined(__x86_64__)
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
#elif defined(STATE_CANARY)
    uint32_t w;
    __asm__ volatile("movl %%gs:0x14, %0" : "=r"(w));
    v = w;
#endif
    return v;
}

/* Called right before the jump into the restored stack: the frames below main's are left for good. */
static void state_set_canary(uint64_t v) {
#if defined(STATE_CANARY) && defined(__x86_64__)
    __asm__ volatile("movq %0, %%fs:0x28" : : "r"(v) : "memory");
#elif defined(STATE_CANARY)
    uint32_t w = (uint32_t)v;
    __asm__ volatile("movl %0, %%gs:0x14" : : "r"(w) : "memory");
#else
    (void)v;
#endif
}

/* The same layout in every build (fixed widths, 8-byte fields at multiples of 8), so that any build reads any state's
 * header and says what it is. */
typedef struct StateHeader {
    char magic[8];
    uint32_t version;
    uint32_t ptr_size;
    char exe_sha1[48];
    char game_id[32];
    uint64_t image, arena, stack; /* where this run's code, arena and game stack are */
    int64_t rate;
    int64_t frame;
    uint64_t canary;
} StateHeader;

static void state_header_now(StateHeader *h) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, STATE_MAGIC, 8);
    h->version = STATE_VERSION;
    h->ptr_size = (u32)sizeof(void *);
    state_exe_sha1(h->exe_sha1);
    snprintf(h->game_id, sizeof(h->game_id), "%s", PSXSTACK_GAME_ID);
    h->image = (uint64_t)(uintptr_t)&port_savestate_run;
    h->arena = (uint64_t)(uintptr_t)port_arena_base();
    h->stack = (uint64_t)(uintptr_t)state_stack;
    h->rate = port_rate;
    h->frame = port_frames;
    h->canary = state_canary();
}

/* The loaded header against this run's: whatever differs makes the state unusable here. */
static int state_header_check(const StateHeader *h, const char *path, char *err, size_t err_size) {
    StateHeader now;
    if (memcmp(h->magic, STATE_MAGIC, 8) != 0) {
        snprintf(err, err_size, "%s: not a state file", path);
        return 0;
    }
    if (h->version != STATE_VERSION) {
        snprintf(err, err_size, "%s: state format %u, this build reads %u", path, h->version, STATE_VERSION);
        return 0;
    }
    state_header_now(&now);
    if (strcmp(h->game_id, now.game_id) != 0 || h->ptr_size != now.ptr_size) {
        snprintf(err, err_size, "%s: saved by another binary (%.32s, %u-bit, sha1 %.12s; this is %s, %u-bit, %.12s): a "
                 "state holds the binary's own addresses, only the same build can load it", path, h->game_id,
                 h->ptr_size * 8, h->exe_sha1, now.game_id, now.ptr_size * 8, now.exe_sha1);
        return 0;
    }
    if (strncmp(h->exe_sha1, now.exe_sha1, 40) != 0 || h->image != now.image) {
        /* DW3_STATE_ANY=1: take a state from another build of the same game, for measuring a renderer change without
         * replaying the game to the same scene first (a battle state costs ~7 minutes to make). Sound only when the
         * change cannot move the game's own writable memory - a renderer, a timer - and never for play: the state holds
         * the binary's addresses, so a change that moved the game's data would read another build's layouts. The arena
         * and the game stack are checked below, and that is where the game's own state lives. */
        const char *any = getenv("DW3_STATE_ANY");

        if (any == NULL || any[0] != '1') {
            snprintf(err, err_size, "%s: saved by another binary (%.32s, %u-bit, sha1 %.12s, image 0x%llx; this is %s, "
                     "%u-bit, sha1 %.12s, image 0x%llx): a state holds the binary's own addresses, only the same build "
                     "can load it", path, h->game_id, h->ptr_size * 8, h->exe_sha1, (unsigned long long)h->image,
                     now.game_id, now.ptr_size * 8, now.exe_sha1, (unsigned long long)now.image);
            return 0;
        }
        port_log("state: %s was saved by sha1 %.12s with the image at 0x%llx; this is %.12s at 0x%llx, and "
                 "DW3_STATE_ANY takes it anyway", path, h->exe_sha1, (unsigned long long)h->image, now.exe_sha1,
                 (unsigned long long)now.image);
    }
    if (h->arena != now.arena || h->stack != now.stack) {
        snprintf(err, err_size, "%s: saved with the arena or the game stack at another address (0x%llx, 0x%llx; now "
                 "0x%llx, 0x%llx): its pointers do not hold in this run", path, (unsigned long long)h->arena,
                 (unsigned long long)h->stack, (unsigned long long)now.arena, (unsigned long long)now.stack);
        return 0;
    }
    if (h->rate != now.rate) {
        snprintf(err, err_size, "%s: saved at %lld Hz, this run is at %lld Hz (--refresh %lld)", path,
                 (long long)h->rate, (long long)now.rate, (long long)h->rate);
        return 0;
    }
#ifdef STATE_CANARY_UNKNOWN
    snprintf(err, err_size, "%s: this build has a stack protector whose guard the loader cannot set on this platform",
             path);
    return 0;
#endif
    return 1;
}

/* ---- The whole state, both directions */

static void state_sync(PortState *s, StateHeader *h) {
    uint64_t lo;
    port_state_bytes(s, "header", h, sizeof(*h));
    PORT_STATE_VAR(s, port_frames);
    port_audio_state(s);
    port_overlay_state(s);
    port_arena_state(s);
    psyq_state(s);
    spu_state(s);
    port_framelog_state(s);
    port_script_state(s);
    game_savestate(s); /* the adapter's own (its mods' state) */
    port_fiber_state(s); /* the fiber table and every suspended fiber's stack; the current one is the context's */
    /* the context: the current stack (the game stack, or the fiber's the vsync ended on) above the capture point,
     * then the registers' buffer */
    if (s->loading) {
        if (!port_fiber_current_stack(&state_cur_base, &state_cur_size)) {
            state_cur_base = state_stack;
            state_cur_size = STATE_STACK_SIZE;
        }
    }
    lo = (uint64_t)(state_stack_lo - state_cur_base);
    PORT_STATE_VAR(s, lo);
    if (s->loading) {
        if (lo >= state_cur_size) {
            state_fail(s, "the game stack's bound is outside the stack");
        }
        state_stack_lo = state_cur_base + lo;
        port_state_bytes(s, "stack", state_stack_lo, state_cur_size - (size_t)lo);
    } else {
        port_state_bytes(s, "stack", state_stack_copy, state_stack_copy_n);
    }
    PORT_STATE_VAR(s, port_savestate_ctx);
    if (s->loading && s->pos != s->n) {
        state_fail(s, "%zu bytes after the last block: not a state of this build", s->n - s->pos);
    }
}

/* Writes the state as of the last capture (the end of the current vsync) to `path`; 0 with err set on failure. */
static int state_write(const char *path, char *err, size_t err_size) {
    PortState s;
    StateHeader h;
    char tmp[4096];
    FILE *f;
    long long t0 = port_clock_ns();
    memset(&s, 0, sizeof(s));
    s.path = path;
    state_header_now(&h);
    state_sync(&s, &h);
    snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path, port_process_id());
    f = fopen(tmp, "wb");
    if (f == NULL || fwrite(s.data, 1, s.n, f) != s.n || fclose(f) != 0 || port_file_replace(tmp, path) != 0) {
        snprintf(err, err_size, "%s: cannot write", path);
        free(s.data);
        remove(tmp);
        return 0;
    }
    port_log("state: frame %ld saved to %s (%zu KB, %.1f ms; on fiber %d)", port_frames, path, s.n >> 10,
             (double)(port_clock_ns() - t0) / 1e6, port_fiber_index(port_fiber_current()));
    free(s.data);
    return 1;
}

/* A read state's buffers (its data, its path's copy). */
static void state_drop(PortState *s) {
    free(s->data);
    free((void *)s->path);
    s->data = NULL;
    s->path = NULL;
}

/* Reads and checks a state file into `out` (not applied yet); 0 with err set when it is not usable here. */
static int state_read(const char *path, PortState *out, char *err, size_t err_size) {
    FILE *f = fopen(path, "rb");
    long size;
    StateHeader h;
    memset(out, 0, sizeof(*out));
    out->path = strdup(path); /* kept until applied (the debug channel's request is gone by then) */
    out->loading = 1;
    if (out->path == NULL) {
        port_fatal("state: out of memory");
    }
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        snprintf(err, err_size, "%s: cannot read", path);
        if (f != NULL) {
            fclose(f);
        }
        state_drop(out);
        return 0;
    }
    out->data = malloc(size ? (size_t)size : 1);
    if (out->data == NULL || fread(out->data, 1, (size_t)size, f) != (size_t)size) {
        snprintf(err, err_size, "%s: cannot read", path);
        fclose(f);
        state_drop(out);
        return 0;
    }
    fclose(f);
    out->n = (size_t)size;
    /* the first block is the header ("header", then its size): checked before anything is applied */
    if (out->n < 1 + 6 + 8 + sizeof(h) || out->data[0] != 6 || memcmp(out->data + 1, "header", 6) != 0) {
        snprintf(err, err_size, "%s: not a state file", path);
        state_drop(out);
        return 0;
    }
    memcpy(&h, out->data + 1 + 6 + 8, sizeof(h));
    if (!state_header_check(&h, path, err, err_size)) {
        state_drop(out);
        return 0;
    }
    return 1;
}

/* ---- The options */

int port_savestate_add(const char *spec) {
    const char *colon = strchr(spec, ':');
    StateSave *sv;
    size_t wn;
    if (colon == NULL || colon == spec || colon[1] == '\0' || state_save_count == STATE_MAX_SAVES) {
        return 0;
    }
    sv = &state_saves[state_save_count];
    memset(sv, 0, sizeof(*sv));
    wn = (size_t)(colon - spec);
    if (strspn(spec, "0123456789") == wn) {
        sv->frame = strtol(spec, NULL, 10);
        if (sv->frame <= 0) {
            return 0;
        }
    } else {
        sv->name = strndup(spec, wn);
    }
    sv->path = strdup(colon + 1);
    if (sv->path == NULL || (sv->frame == 0 && sv->name == NULL)) {
        port_fatal("state: out of memory");
    }
    state_save_count++;
    return 1;
}

void port_savestate_set_exit(void) {
    state_save_exit = 1;
}

void port_savestate_set_load(const char *path) {
    state_load_path = path;
}

int port_savestate_wanted(void) {
    return state_save_count > 0 || state_load_path != NULL;
}

/* The game on its own stack from now on (main, with a state option, `required`, or the debug channel): before
 * game_main. */
void port_savestate_arm(int required) {
#ifdef STATE_ASAN
    if (__asan_get_current_fake_stack() != NULL) {
        if (required) {
            port_fatal("state: AddressSanitizer's fake stacks are on (detect_stack_use_after_return): a state cannot "
                       "hold frames on the heap; run with ASAN_OPTIONS=detect_stack_use_after_return=0");
        }
        port_log("state: off in this run (AddressSanitizer's fake stacks: ASAN_OPTIONS=detect_stack_use_after_return=0 "
                 "turns them off)");
        return;
    }
#else
    (void)required;
#endif
    port_savestate_armed = 1;
    port_fiber_set_main_stack(state_stack, STATE_STACK_SIZE); /* the main fiber's stack, for the fibers' states */
}

/* ---- Running the game on its stack */

static void state_game_entry(void) {
#ifdef STATE_ASAN
    __sanitizer_finish_switch_fiber(NULL, &state_main_bottom, &state_main_size);
#endif
    game_main();
    port_exit(0, "game_main returned");
}

/* Calls fn on the stack whose top is `top` (16-byte aligned); fn never returns. */
static void __attribute__((noinline, noreturn)) state_call_on_stack(void (*fn)(void), u8 *top) {
#ifdef STATE_ASAN
    __sanitizer_start_switch_fiber(NULL, state_stack, STATE_STACK_SIZE);
#endif
#if defined(__x86_64__) && defined(_WIN32)
    __asm__ volatile("mov %0, %%rsp\n\tsub $32, %%rsp\n\txor %%ebp, %%ebp\n\tcall *%1\n\tud2" : : "r"(top), "r"(fn)
                     : "memory");
#elif defined(__x86_64__)
    __asm__ volatile("mov %0, %%rsp\n\txor %%ebp, %%ebp\n\tcall *%1\n\tud2" : : "r"(top), "r"(fn) : "memory");
#elif defined(__i386__)
    __asm__ volatile("mov %0, %%esp\n\txor %%ebp, %%ebp\n\tcall *%1\n\tud2" : : "r"(top), "r"(fn) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("mov sp, %0\n\tmov x29, xzr\n\tblr %1\n\tbrk #0" : : "r"(top), "r"(fn) : "memory");
#elif defined(__arm__)
    __asm__ volatile("mov sp, %0\n\tmov r11, #0\n\tblx %1\n\tbkpt #0" : : "r"(top), "r"(fn) : "memory");
#else
#error "save states: no stack switch for this architecture (runtime/savestate.c state_call_on_stack)"
#endif
    __builtin_unreachable();
}

/* The console's reset (reset.c) and a state load leave the current stack (the game stack, or a fiber's) for main's
 * thread stack, before their longjmp: one announcement to AddressSanitizer for the whole move. */
void port_savestate_leave_stack(void) {
#ifdef STATE_ASAN
    if (port_fiber_leave(port_savestate_armed ? state_main_bottom : NULL, port_savestate_armed ? state_main_size : 0)) {
        return;
    }
    if (port_savestate_armed) {
        __sanitizer_start_switch_fiber(NULL, state_main_bottom, state_main_size);
    }
#else
    port_fiber_leave(NULL, 0);
#endif
}

/* After main's setjmp returned from a jump off the game stack (a reset, a load). */
static void state_back_on_main(void) {
#ifdef STATE_ASAN
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);
#endif
}

/* main's last call: game_main, on the game stack when armed; or the state to load, resumed. */
void port_savestate_run(int jumped) {
    if (jumped && port_savestate_armed) {
        state_back_on_main();
    }
    if (state_load_path != NULL || state_load_ready) {
        char err[512];
        PortState *s = &state_loaded;
        StateHeader h;
        long long t0 = port_clock_ns();
        if (!state_load_ready && !state_read(state_load_path, s, err, sizeof(err))) {
            port_fatal("state: %s", err);
        }
        state_load_path = NULL;
        state_load_ready = 0;
        state_sync(s, &h);
        port_log("state: frame %ld loaded from %s (%.1f ms)", port_frames, s->path, (double)(port_clock_ns() - t0) / 1e6);
        state_drop(s);
#ifdef STATE_ASAN
        __sanitizer_start_switch_fiber(NULL, state_cur_base, state_cur_size); /* the stack the context is on */
#endif
        port_fiber_enter_current(); /* Windows: the thread block's bounds for a fiber's stack */
        state_set_canary(h.canary);
        __builtin_longjmp(port_savestate_ctx, 1);
    }
    if (!port_savestate_armed) {
        game_main();
        return;
    }
    state_call_on_stack(state_game_entry, state_stack + STATE_STACK_SIZE - STATE_STACK_TOP_PAD);
}

/* ---- The capture point (pump.c PORT_SAVESTATE_POINT, the end of every vsync) */

void port_savestate_checkpoint(const char *name) {
    state_checkpoint = name;
}

static void state_keep_stack(void) {
    size_t n = (size_t)(state_cur_base + state_cur_size - state_stack_lo);
    if (n > state_stack_copy_cap) {
        state_stack_copy = realloc(state_stack_copy, n);
        if (state_stack_copy == NULL) {
            port_fatal("state: out of memory");
        }
        state_stack_copy_cap = n;
    }
    state_copy_raw(state_stack_copy, state_stack_lo, n);
    state_stack_copy_n = n;
}

static int state_save_due(const StateSave *sv) {
    if (sv->done) {
        return 0;
    }
    if (sv->frame > 0) {
        return sv->frame == port_frames;
    }
    return state_checkpoint != NULL && strcmp(sv->name, state_checkpoint) == 0;
}

void port_savestate_captured(void) {
    int i, due = 0, left = 0;
    /* everything from this function's frame up: port_frame's frame and its callers' (the stack grows down), on the
     * game stack or on the fiber's the vsync ended on */
    state_stack_lo = (u8 *)((uintptr_t)__builtin_frame_address(0) & ~(uintptr_t)63);
    if (!port_fiber_current_stack(&state_cur_base, &state_cur_size)) {
        state_cur_base = state_stack;
        state_cur_size = STATE_STACK_SIZE;
    }
    if (state_stack_lo < state_cur_base || state_stack_lo >= state_cur_base + state_cur_size) {
        port_fatal("state: the vsync is not on the game stack or a fiber's (%p)", (void *)state_stack_lo);
    }
    for (i = 0; i < state_save_count; i++) {
        due |= state_save_due(&state_saves[i]);
    }
    if (due || port_debug_active) {
        state_keep_stack(); /* the debug channel may save from the pause that follows */
    }
    for (i = 0; i < state_save_count; i++) {
        StateSave *sv = &state_saves[i];
        char err[512];
        if (state_save_due(sv)) {
            if (!state_write(sv->path, err, sizeof(err))) {
                port_fatal("state: %s", err);
            }
            sv->done = 1;
        }
        left += !sv->done;
    }
    state_checkpoint = NULL;
    if (port_debug_active) {
        port_debug_state_point(); /* a save the channel asked for while the game ran */
    }
    if (due && left == 0 && state_save_exit) {
        port_exit(0, "state saved");
    }
}

void port_savestate_resumed(void) {
#ifdef STATE_ASAN
    __sanitizer_finish_switch_fiber(NULL, &state_main_bottom, &state_main_size);
#endif
    port_log("state: resumed at the end of frame %ld", port_frames);
}

/* ---- The debug channel (debug.c): a save as of the last capture, a load through main */

int port_savestate_save_now(const char *path, char *err, size_t err_size) {
    if (!port_savestate_armed) {
        snprintf(err, err_size, "save states are off in this run (see its log)");
        return 0;
    }
    if (state_stack_copy_n == 0) {
        snprintf(err, err_size, "no vsync has ended yet");
        return 0;
    }
    return state_write(path, err, err_size);
}

int port_savestate_load_prepare(const char *path, long *frame, char *err, size_t err_size) {
    StateHeader h;
    if (!port_savestate_armed) {
        snprintf(err, err_size, "save states are off in this run (see its log)");
        return 0;
    }
    if (state_load_ready) {
        state_drop(&state_loaded);
        state_load_ready = 0;
    }
    if (!state_read(path, &state_loaded, err, err_size)) {
        return 0;
    }
    memcpy(&h, state_loaded.data + 1 + 6 + 8, sizeof(h));
    *frame = (long)h.frame;
    state_load_ready = 1;
    return 1;
}

void port_savestate_load_request(void) {
    port_log("state: loading %s: back to main", state_loaded.path);
    port_savestate_leave_stack();
    longjmp(port_reset_jmp, 2);
}
