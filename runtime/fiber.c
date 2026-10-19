/* Fibers (include/psxstack/hooks.h "Fibers"; docs/PORT.md "Fibers"): a game's own tasks, each on a host stack of
 * its own, switched by the game's scheduler glue under PC_PORT. A PS1 game that schedules tasks itself (a kernel-TCB
 * context switch in hand-written asm, or Psy-Q's OpenTh/ChangeTh) keeps each task's registers and a small stack
 * (0x100 to 0x2000 bytes); on the host the task's C needs a stack of x86-64 size, so each task runs as a fiber on a
 * static stack of the runtime's (PORT_FIBER_STACK_SIZE), and the game's switch points call port_fiber_switch.
 *
 * Why a hand-written switch and static stacks (DECISIONS "Fibers: a hand-written switch, preemption at the tick's
 * end"): a save state (savestate.c) holds every live fiber's stack and its saved context as bytes at fixed addresses,
 * and resumes the fiber that was running. ucontext's buffers (glibc's point into themselves and carry the signal
 * mask) and Win32 fibers (opaque heap objects) cannot be saved that way; a switch of a dozen instructions that keeps
 * the callee-saved registers on the fiber's own stack and one saved stack pointer can. The switch is the same on
 * every platform; Windows adds the thread block's stack bounds (SEH and the stack probes read them).
 *
 * The main fiber is whatever stack game_main runs on (the thread's, or savestate.c's static game stack when states
 * are on): port_fiber_main(), never created or destroyed. A fiber's entry must not return: the game's task body ends
 * with port_fiber_exit (fatal otherwise, as a PS1 task returning into nothing would be).
 *
 * Preemption (docs/PORT.md "Fibers"): a game's vblank handler may decide to switch tasks (Digimon Digital Card
 * Battle's handleVsyncPreemption returns into the main task when a lower-priority task was interrupted). On the host
 * the handler runs inside the vsync tick (psyq_vsync_tick: the shim's handler call, then pump.c's port_frame), and a
 * switch there would leave the tick half-done on the interrupted fiber; so the handler calls port_fiber_preempt, and
 * the pump performs the switch at the very end of the tick (port_fiber_pump_point): the interrupted fiber is
 * suspended right after its tick, as the PS1's task is suspended right after the interrupt. Every switch is thus a
 * deterministic function of the game's execution: its own switch calls, and the end of a vsync tick. No timer, no
 * thread.
 *
 * AddressSanitizer is told about every switch (the fiber annotations: a frame on a fiber's stack is then a stack
 * frame, not a wild access), as savestate.c does for the game stack. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "port_runtime.h"
#include "savestate.h"

#if defined(__SANITIZE_ADDRESS__)
#define FIBER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FIBER_ASAN 1
#endif
#endif
#ifdef FIBER_ASAN
#include <sanitizer/common_interface_defs.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef PORT_FIBER_MAX
#define PORT_FIBER_MAX 64 /* fibers a game may have alive at once (the main fiber apart) */
#endif
#ifndef PORT_FIBER_STACK_SIZE
#define PORT_FIBER_STACK_SIZE (256u << 10) /* each fiber's stack: PS1 task stacks are at most 8 KB; x86-64 frames at -O0 and under ASan are many times larger */
#endif
#define FIBER_TOP_PAD 64 /* above the first frame: zeros, where an unwinder looking past it finds nothing */

struct PortFiber {
    void *sp;            /* the saved stack pointer while suspended (the callee-saved registers lie above it) */
    void (*entry)(void *);
    void *arg;
    uint8_t *stack;      /* the stack's lowest byte; size bytes; the main fiber's once known (below) */
    size_t size;
    int in_use;
    int index;           /* 0: the main fiber; 1..PORT_FIBER_MAX */
#ifdef FIBER_ASAN
    void *fake_stack;    /* ASan's fake-stack handle while suspended */
#endif
#ifdef _WIN32
    void *tib_base, *tib_limit, *tib_dealloc; /* NT_TIB StackBase/StackLimit and the deallocation stack */
#endif
};

static PortFiber fibers[PORT_FIBER_MAX + 1]; /* [0] is the main fiber */
static PortFiber *fiber_current = &fibers[0];
#ifdef FIBER_ASAN
static PortFiber *fiber_prev;                /* the fiber a switch came from (its stack bounds, learnt from ASan) */
#endif
static PortFiber *fiber_pending;             /* port_fiber_preempt's target, switched to at the tick's end */
static uint8_t fiber_stacks[PORT_FIBER_MAX][PORT_FIBER_STACK_SIZE] __attribute__((aligned(64)));
static long fiber_switches;

/* ---- The switch: saves the callee-saved registers on the current stack, stores the stack pointer in *save_sp,
 * loads load_sp and restores from there, returns into the loaded context. A new fiber's stack is laid out by
 * fiber_prepare so that the first switch into it "returns" into fiber_trampoline. */
void port_fiber_swap_(void **save_sp, void *load_sp);

#if defined(__x86_64__) && !defined(_WIN32)
/* SysV: rbx, rbp, r12-r15 are callee-saved, plus the MXCSR and x87 control words. At a function's entry rsp is 8 mod
 * 16, so the trampoline's return address sits at a 16-byte boundary. */
__asm__(".text\n"
        ".globl port_fiber_swap_\n"
        ".type port_fiber_swap_, @function\n"
        "port_fiber_swap_:\n"
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  subq $8, %rsp\n"
        "  stmxcsr (%rsp)\n"
        "  fnstcw 4(%rsp)\n"
        "  movq %rsp, (%rdi)\n"
        "  movq %rsi, %rsp\n"
        "  ldmxcsr (%rsp)\n"
        "  fldcw 4(%rsp)\n"
        "  addq $8, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n"
        ".size port_fiber_swap_, .-port_fiber_swap_\n");
#define FIBER_FRAME_WORDS 7 /* the control words, r15, r14, r13, r12, rbx, rbp */
#define FIBER_RET_BELOW_TOP 16 /* the return address at top - 16 (16-aligned); top - 8 is a zero "caller" */
#elif defined(__x86_64__)
/* Win64: rbx, rbp, rdi, rsi, r12-r15 and xmm6-xmm15 are callee-saved, plus the control words. The caller reserves 32
 * bytes of home space above the return address, which fiber_prepare leaves for the trampoline. */
__asm__(".text\n"
        ".globl port_fiber_swap_\n"
        "port_fiber_swap_:\n"
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %rdi\n"
        "  pushq %rsi\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  subq $168, %rsp\n"
        "  movaps %xmm6, 0(%rsp)\n"
        "  movaps %xmm7, 16(%rsp)\n"
        "  movaps %xmm8, 32(%rsp)\n"
        "  movaps %xmm9, 48(%rsp)\n"
        "  movaps %xmm10, 64(%rsp)\n"
        "  movaps %xmm11, 80(%rsp)\n"
        "  movaps %xmm12, 96(%rsp)\n"
        "  movaps %xmm13, 112(%rsp)\n"
        "  movaps %xmm14, 128(%rsp)\n"
        "  movaps %xmm15, 144(%rsp)\n"
        "  stmxcsr 160(%rsp)\n"
        "  fnstcw 164(%rsp)\n"
        "  movq %rsp, (%rcx)\n"
        "  movq %rdx, %rsp\n"
        "  ldmxcsr 160(%rsp)\n"
        "  fldcw 164(%rsp)\n"
        "  movaps 0(%rsp), %xmm6\n"
        "  movaps 16(%rsp), %xmm7\n"
        "  movaps 32(%rsp), %xmm8\n"
        "  movaps 48(%rsp), %xmm9\n"
        "  movaps 64(%rsp), %xmm10\n"
        "  movaps 80(%rsp), %xmm11\n"
        "  movaps 96(%rsp), %xmm12\n"
        "  movaps 112(%rsp), %xmm13\n"
        "  movaps 128(%rsp), %xmm14\n"
        "  movaps 144(%rsp), %xmm15\n"
        "  addq $168, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rsi\n"
        "  popq %rdi\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n");
#define FIBER_FRAME_WORDS (8 + 21) /* 8 registers, then 168 bytes: 10 xmm (16-aligned: 8 + 21 words leaves rsp 16-aligned) */
#define FIBER_RET_BELOW_TOP 56 /* the return address at top - 56 (8 mod 16, as at a call: the frame below it is then 16-aligned for movaps), 32 bytes of home space above it, a zero at top - 8 */
#elif defined(__i386__)
/* SysV i386: ebx, ebp, esi, edi, plus the control words. Arguments on the stack. At entry esp is 12 mod 16. */
__asm__(".text\n"
        ".globl port_fiber_swap_\n"
        ".type port_fiber_swap_, @function\n"
        "port_fiber_swap_:\n"
        "  movl 4(%esp), %eax\n"
        "  movl 8(%esp), %edx\n"
        "  pushl %ebp\n"
        "  pushl %ebx\n"
        "  pushl %esi\n"
        "  pushl %edi\n"
        "  subl $8, %esp\n"
        "  stmxcsr (%esp)\n"
        "  fnstcw 4(%esp)\n"
        "  movl %esp, (%eax)\n"
        "  movl %edx, %esp\n"
        "  ldmxcsr (%esp)\n"
        "  fldcw 4(%esp)\n"
        "  addl $8, %esp\n"
        "  popl %edi\n"
        "  popl %esi\n"
        "  popl %ebx\n"
        "  popl %ebp\n"
        "  ret\n"
        ".size port_fiber_swap_, .-port_fiber_swap_\n");
#define FIBER_FRAME_WORDS 6 /* the control words (2 words), edi, esi, ebx, ebp */
#define FIBER_RET_BELOW_TOP 4 /* the return address at top - 4 (12 mod 16) */
#elif defined(__aarch64__)
/* AAPCS64: x19-x28, x29, x30 and d8-d15 are callee-saved; the saved frame is 176 bytes (16-aligned). Compiled, not
 * run: no AArch64 host has run the stack yet. */
__asm__(".text\n"
        ".globl port_fiber_swap_\n"
        ".type port_fiber_swap_, %function\n"
        "port_fiber_swap_:\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n"
        "  stp x21, x22, [sp, #16]\n"
        "  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n"
        "  stp x27, x28, [sp, #64]\n"
        "  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n"
        "  stp d10, d11, [sp, #112]\n"
        "  stp d12, d13, [sp, #128]\n"
        "  stp d14, d15, [sp, #144]\n"
        "  mov x2, sp\n"
        "  str x2, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n"
        "  ldp x21, x22, [sp, #16]\n"
        "  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n"
        "  ldp x27, x28, [sp, #64]\n"
        "  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n"
        "  ldp d10, d11, [sp, #112]\n"
        "  ldp d12, d13, [sp, #128]\n"
        "  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #176\n"
        "  ret\n"
        ".size port_fiber_swap_, .-port_fiber_swap_\n");
#define FIBER_FRAME_WORDS 22 /* 176 bytes: x19-x28, x29, x30, d8-d15 */
#define FIBER_AARCH64 1
#elif defined(__arm__)
__asm__(".text\n"
        ".arm\n"
        ".globl port_fiber_swap_\n"
        ".type port_fiber_swap_, %function\n"
        "port_fiber_swap_:\n"
        "  push {r4-r11, r12, lr}\n"
        "  vpush {d8-d15}\n"
        "  str sp, [r0]\n"
        "  mov sp, r1\n"
        "  vpop {d8-d15}\n"
        "  pop {r4-r11, r12, pc}\n"
        ".size port_fiber_swap_, .-port_fiber_swap_\n");
#define FIBER_FRAME_WORDS 26 /* 104 bytes: d8-d15 (16 words), r4-r11 (8 words), r12 (1 word), lr (1 word) */
#define FIBER_ARM32 1
#else
#error "fibers: no stack switch for this architecture (runtime/fiber.c port_fiber_swap_)"
#endif

/* Entered by the first switch's `ret`, so its stack pointer may not be what a call leaves: on x86 it realigns. */
#if defined(__x86_64__) || defined(__i386__)
static void fiber_trampoline(void) __attribute__((noreturn, noinline, force_align_arg_pointer));
#else
static void fiber_trampoline(void) __attribute__((noreturn, noinline));
#endif

/* ---- Platform bookkeeping around a switch */

#ifdef _WIN32
static void fiber_tib_read(PortFiber *f) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    f->tib_base = tib->StackBase;
    f->tib_limit = tib->StackLimit;
    f->tib_dealloc = *(void **)((char *)tib + 0x1478); /* TEB DeallocationStack */
}

static void fiber_tib_write(const PortFiber *f) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackBase = f->tib_base;
    tib->StackLimit = f->tib_limit;
    *(void **)((char *)tib + 0x1478) = f->tib_dealloc;
}
#endif

/* After port_fiber_swap_ returned into the now-current fiber (its own resume, or the trampoline's start). */
static void fiber_after_switch(void) {
#ifdef FIBER_ASAN
    const void *bottom = NULL;
    size_t size = 0;
    __sanitizer_finish_switch_fiber(fiber_current->fake_stack, &bottom, &size);
    fiber_current->fake_stack = NULL;
    if (fiber_prev != NULL && fiber_prev->stack == NULL && bottom != NULL) {
        fiber_prev->stack = (uint8_t *)bottom; /* the main fiber's stack, as ASan knows it */
        fiber_prev->size = size;
    }
#endif
}

/* The switch itself, from the current fiber to `to` (in use, not the current). `destroyed`: the current fiber ends
 * here and never resumes. */
static void fiber_switch_to(PortFiber *to, int destroyed) {
    PortFiber *from = fiber_current;
    fiber_switches++;
    if (port_trace) {
        port_log("fiber: switch %d -> %d%s", from->index, to->index, destroyed ? " (the first ends)" : "");
    }
    fiber_current = to;
#ifdef FIBER_ASAN
    fiber_prev = from;
    __sanitizer_start_switch_fiber(destroyed ? NULL : &from->fake_stack, to->stack, to->size);
#else
    (void)destroyed;
#endif
#ifdef _WIN32
    fiber_tib_read(from);
    fiber_tib_write(to);
#endif
    port_fiber_swap_(&from->sp, to->sp);
    fiber_after_switch();
}

/* A new fiber's stack: the frame port_fiber_swap_ pops (zero registers, the current control words) under the
 * trampoline's address as the return address, and a zero word at the top for an unwinder. */
static void fiber_prepare(PortFiber *f) {
    uintptr_t top = ((uintptr_t)f->stack + f->size - FIBER_TOP_PAD) & ~(uintptr_t)15;
    uintptr_t *w;
    memset((void *)(top - 256), 0, 256);
#ifdef FIBER_AARCH64
    w = (uintptr_t *)(top - FIBER_FRAME_WORDS * sizeof(uintptr_t));
    w[11] = (uintptr_t)fiber_trampoline; /* x30: the return address of the first switch */
    f->sp = w;
#elif defined(FIBER_ARM32)
    w = (uintptr_t *)(top - FIBER_FRAME_WORDS * sizeof(uintptr_t));
    w[25] = (uintptr_t)fiber_trampoline; /* pc: the return address of the first switch */
    f->sp = w;
#else
    *(uintptr_t *)(top - FIBER_RET_BELOW_TOP) = (uintptr_t)fiber_trampoline;
    w = (uintptr_t *)(top - FIBER_RET_BELOW_TOP - FIBER_FRAME_WORDS * sizeof(uintptr_t));
    {
        /* the control words of the creating fiber: the fiber starts with the same rounding and exceptions masked */
        uint32_t mxcsr = 0, cw = 0;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        __asm__ volatile("fnstcw %0" : "=m"(cw));
#if defined(__x86_64__) && defined(_WIN32)
        ((uint32_t *)w)[160 / 4] = mxcsr; /* the xmm area's 160 bytes, then the control words, then the registers */
        ((uint32_t *)w)[164 / 4] = cw;
#else
        ((uint32_t *)w)[0] = mxcsr;
        ((uint32_t *)w)[1] = cw;
#endif
    }
    f->sp = w;
#endif
#ifdef _WIN32
    f->tib_base = (void *)((uintptr_t)f->stack + f->size);
    f->tib_limit = f->stack;
    f->tib_dealloc = f->stack;
#endif
}

static void fiber_trampoline(void) {
    PortFiber *f = fiber_current;
    fiber_after_switch();
    f->entry(f->arg);
    port_fatal("fiber %d: its entry returned (a task ends with port_fiber_exit)", f->index);
}

/* ---- The game's interface (hooks.h) */

PortFiber *port_fiber_create(void (*entry)(void *), void *arg) {
    int i;
    for (i = 1; i <= PORT_FIBER_MAX; i++) {
        PortFiber *f = &fibers[i];
        if (!f->in_use) {
            memset(f, 0, sizeof(*f));
            f->index = i;
            f->in_use = 1;
            f->entry = entry;
            f->arg = arg;
            f->stack = fiber_stacks[i - 1];
            f->size = PORT_FIBER_STACK_SIZE;
            fiber_prepare(f);
            if (port_trace) {
                port_log("fiber: create %d (entry %p)", i, (void *)(uintptr_t)entry);
            }
            return f;
        }
    }
    port_fatal("fiber: more than %d fibers alive (PORT_FIBER_MAX)", PORT_FIBER_MAX);
}

PortFiber *port_fiber_main(void) {
    return &fibers[0];
}

PortFiber *port_fiber_current(void) {
    return fiber_current;
}

int port_fiber_index(const PortFiber *f) {
    return f->index;
}

static void fiber_check_target(const PortFiber *to, const char *what) {
    if (to == NULL || to < fibers || to > fibers + PORT_FIBER_MAX || (to != &fibers[0] && !to->in_use)) {
        port_fatal("fiber: %s to a fiber that does not exist", what);
    }
}

void port_fiber_switch(PortFiber *to) {
    fiber_check_target(to, "switch");
    if (to == fiber_current) {
        return;
    }
    fiber_switch_to(to, 0);
}

void port_fiber_exit(PortFiber *to) {
    PortFiber *from = fiber_current;
    fiber_check_target(to, "exit");
    if (from == &fibers[0]) {
        port_fatal("fiber: the main fiber cannot exit (game_main returns instead)");
    }
    if (to == from) {
        port_fatal("fiber: exit into the exiting fiber %d", from->index);
    }
    if (port_trace) {
        port_log("fiber: exit %d", from->index);
    }
    from->in_use = 0; /* the slot is free; the switch below still writes its sp on the freed stack, harmlessly */
    if (fiber_pending == from) {
        fiber_pending = NULL;
    }
    fiber_switch_to(to, 1);
    port_fatal("fiber: an exited fiber resumed"); /* unreachable: nothing switches to a free slot */
}

void port_fiber_destroy(PortFiber *f) {
    if (f == NULL || f == &fibers[0] || !f->in_use) {
        return;
    }
    if (f == fiber_current) {
        port_fatal("fiber: destroy of the running fiber %d (port_fiber_exit ends it)", f->index);
    }
    if (port_trace) {
        port_log("fiber: destroy %d", f->index);
    }
    if (fiber_pending == f) {
        fiber_pending = NULL;
    }
    f->in_use = 0;
}

void port_fiber_preempt(PortFiber *to) {
    fiber_check_target(to, "preempt");
    fiber_pending = to;
}

/* ---- The runtime's side (port_runtime.h) */

/* pump.c, at the very end of every vsync tick: the switch a vblank handler asked for. */
void port_fiber_pump_point(void) {
    PortFiber *to = fiber_pending;
    if (to != NULL) {
        fiber_pending = NULL;
        if (to->in_use || to == &fibers[0]) {
            port_fiber_switch(to);
        }
    }
}

int port_fiber_on_fiber(void) {
    return fiber_current != &fibers[0];
}

/* The stack of the current fiber: 0 for the main fiber (its stack is the caller's business). */
int port_fiber_current_stack(uint8_t **lo, size_t *size) {
    if (fiber_current == &fibers[0]) {
        return 0;
    }
    *lo = fiber_current->stack;
    *size = fiber_current->size;
    return 1;
}

void port_fiber_set_main_stack(uint8_t *lo, size_t size) {
    fibers[0].stack = lo;
    fibers[0].size = size;
}

/* Before a longjmp off the current fiber onto the main thread's stack (reset.c, a state load): announces one switch
 * to AddressSanitizer when the game is on a fiber, and makes the main fiber current, so that the main fiber's own
 * announcement (savestate.c) is not a second one. `bottom`/`size`: where the jump lands when known (the main
 * thread's stack under savestate.c's game stack); else the main fiber's recorded stack. Returns 1 when announced. */
int port_fiber_leave(const void *bottom, size_t size) {
    if (fiber_current == &fibers[0]) {
        return 0;
    }
    if (port_trace) {
        port_log("fiber: leaving fiber %d for the main thread's stack", fiber_current->index);
    }
#ifdef FIBER_ASAN
    if (bottom == NULL) {
        bottom = fibers[0].stack;
        size = fibers[0].size;
    }
    __sanitizer_start_switch_fiber(NULL, bottom, size);
#else
    (void)bottom;
    (void)size;
#endif
    fiber_current = &fibers[0];
    return 1;
}

/* The console's reset: every fiber dropped (the game starts over on the main fiber). */
void port_fiber_reset(void) {
    int i;
    for (i = 1; i <= PORT_FIBER_MAX; i++) {
        fibers[i].in_use = 0;
    }
    fiber_pending = NULL;
    fiber_current = &fibers[0];
}

/* ---- Save states: the fiber table and every suspended fiber's live stack; the current fiber's is the context's
 * (savestate.c). Loading puts the table back first, then each stack; savestate.c then enters the current fiber. */

typedef struct FiberRecord {
    uint64_t sp;     /* the saved stack pointer as an offset from the stack's start; 0 for the running one */
    uint64_t entry;  /* code address: fixed with the image */
    uint64_t arg;    /* a pointer into the image or the arena, as the game gave it */
    uint32_t in_use;
    uint32_t pad;
} FiberRecord;

void port_fiber_state(PortState *s) {
    FiberRecord rec[PORT_FIBER_MAX + 1];
    int32_t current = fiber_current->index, pending = fiber_pending != NULL ? fiber_pending->index : -1;
    int i;
    memset(rec, 0, sizeof(rec));
    for (i = 0; i <= PORT_FIBER_MAX; i++) {
        PortFiber *f = &fibers[i];
        rec[i].in_use = (uint32_t)(i == 0 || f->in_use);
        rec[i].entry = (uint64_t)(uintptr_t)f->entry;
        rec[i].arg = (uint64_t)(uintptr_t)f->arg;
        rec[i].sp = f != fiber_current && f->sp != NULL && f->stack != NULL ? (uint64_t)((uint8_t *)f->sp - f->stack) : 0;
    }
    port_state_bytes(s, "fibers", rec, sizeof(rec));
    PORT_STATE_VAR(s, current);
    PORT_STATE_VAR(s, pending);
    if (port_state_loading(s)) {
        if (current < 0 || current > PORT_FIBER_MAX || pending > PORT_FIBER_MAX) {
            port_fatal("state: the fiber table is not this build's");
        }
        for (i = 1; i <= PORT_FIBER_MAX; i++) {
            PortFiber *f = &fibers[i];
            f->in_use = (int)rec[i].in_use;
            f->index = i;
            f->entry = (void (*)(void *))(uintptr_t)rec[i].entry;
            f->arg = (void *)(uintptr_t)rec[i].arg;
            f->stack = fiber_stacks[i - 1];
            f->size = PORT_FIBER_STACK_SIZE;
            f->sp = NULL;
#ifdef FIBER_ASAN
            f->fake_stack = NULL;
#endif
#ifdef _WIN32
            f->tib_base = (void *)((uintptr_t)f->stack + f->size);
            f->tib_limit = f->stack;
            f->tib_dealloc = f->stack;
#endif
        }
        fiber_current = &fibers[current];
        fiber_pending = pending >= 0 ? &fibers[pending] : NULL;
#ifdef FIBER_ASAN
        fiber_prev = NULL;
#endif
    }
    /* every suspended fiber's stack from its saved pointer up (the main fiber's too, when it is suspended inside a
     * switch: its stack is savestate.c's game stack then) */
    for (i = 0; i <= PORT_FIBER_MAX; i++) {
        PortFiber *f = &fibers[i];
        char tag[32];
        uint64_t sp = rec[i].sp;
        if (!rec[i].in_use || i == current || sp == 0) {
            continue;
        }
        if (f->stack == NULL || sp >= f->size) {
            port_fatal("state: fiber %d: its saved stack pointer is outside its stack", i);
        }
        snprintf(tag, sizeof(tag), "fiber%d", i);
        f->sp = f->stack + sp;
        port_state_bytes(s, tag, f->sp, f->size - (size_t)sp);
    }
}

/* Windows: the thread block's stack bounds for the fiber a state load resumes (savestate.c, before its jump). */
void port_fiber_enter_current(void) {
#ifdef _WIN32
    if (fiber_current != &fibers[0]) {
        fiber_tib_write(fiber_current);
    }
#endif
}

long port_fiber_switch_count(void) {
    return fiber_switches;
}
