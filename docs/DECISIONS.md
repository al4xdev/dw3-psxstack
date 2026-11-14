# Decisions

A few lines each: what and why. Only decisions that change how the stack works.

## The name: psxstack (2026-10-07)
Short, free on GitHub, and a clean prefix everywhere the code said `dw3`: CMake `PSXSTACK_*`, the C++ namespace, the
library target. Nothing with "Psy" in it, since Psy-Q is SN Systems' name.

## One stack repo, consumed as a pinned git submodule (2026-10-07)
A separate repo that releases like a library, not a monorepo of games: the game repos have disc-derived gitignored
data and their own CI, and two of the candidate decomps are other people's projects. A submodule rather than
FetchContent (needs network in CI and worktrees) or subtree (muddles history). A game's `worktree_init.sh` links the
sibling clone so worktrees build offline; CI checks the submodule out at the pin.

## The game description is compiled in, not read at run time (2026-10-07)
`game.json` is read once at configure time and generated into `psxstack_game_gen.h`; the port, the launcher and the
self-test get their constants from it. Single binaries, no file lookup at start, and the self-test fixtures carry no
real hash of their own.

## Psy-Q headers: the game's, for now (2026-10-07; superseded 2026-10-08, below)
The shim compiles against the game's recovered `include/psyq/*.h` through the include path the game passes
(`INCLUDE_DIRS`). The stack owns its own Psy-Q declarations only when the second game arrives and two sets have to
agree; doing it first would mean a third copy with no way to test it.

## Psy-Q declarations: the stack's (2026-10-08)
The second game (Digimon Digital Card Battle) has no recovered `include/psyq/` at all: its `game.h` redeclares every
Psy-Q type and prototype in its own names (`Rect16`, `s32 LoadImage(s16 *, s32)`), so the shim could not compile
against that tree. Now `include/psxstack/psyq/*.h` is the one set the shim compiles against (the first game's
recovered declarations, MIT, moved in: written from the games' use of the API and public documentation, Sony's
names, no SDK file), and the runtime and the shim see no game include directory at all (the units get the full
list, the adapter the game's as quote-only directories, so a game's PS1 `include/stdarg.h` cannot shadow the host's).
A game keeps its own declarations for its matching build; the two meet only at link time, so `tools/psyq_decls.py`
(the inventory's `decls` command) compares the function types both sides declare in the DWARF and fails on a
register-ABI difference: a parameter's or the return's width or kind, or the count. Names and typedefs are free.

## The hooks in the game's C are the game's (2026-10-07)
`PLATFORM_WAIT`, the pointer macros, `OVERLAY_COPY`, the slot macros and the `#ifdef PC_PORT` blocks are the per-game
cost of a port, about a hundred lines for dw2003. The stack documents the patterns and gates them; it does not try to
hide them. A third-party decomp is forked and carries them as a small rebased patch set.

## N slots, data-driven (2026-10-07)
The overlay manager and the arena take the slot table from the description instead of two hard-wired tiers. dw2003
has two slots plus the stage files, Digimon World 2 has one; the macro `tier` argument becomes the slot's 1-based
index, so the game C does not change.

## Versioning (2026-10-07)
Semver tags; `PSXSTACK_API` counts incompatible contract changes and replaces `PORT_MODS_API` (manifests keep
`requires_port`). The settings file's `schema` stays 1; changing it is a MAJOR.

## The game's hook header keeps the PS1 side; the stack's hooks.h is the host side only (2026-10-07)
Phase 0 had the game's `port.h` include `<psxstack/hooks.h>` for both sides. The matching build must not depend on
the stack at all (a clone without the submodule still rebuilds the executable byte for byte), so the PS1 expansions
stay in the game's header and `<psxstack/hooks.h>` is included only under `#ifdef PC_PORT`. The game's constants and
the description's are tied by `_Static_assert`s in the adapter.

## The launcher's self-test carries its fixtures (2026-10-07)
The four test manifests live as files under `tests/fixtures/mods/` (readable, diffable), and the launcher's CMake
compiles them into the binary as string constants. The self-test keeps needing no file beside the executable, which
is what lets it run inside a release package and under Wine.

## One generator for the game description: tools/game_gen.py (2026-10-08)
Phase 1 had the first game generate the same header with its own names (`PortGameDisc`, `PSXSTACK_GAME_LAUNCHER_ABOUT`).
One header, one generator, the stack's names: `PsxstackGameDisc`, `PSXSTACK_GAME_ABOUT`; the arena's derived numbers
(`PORT_SLOT<n>_OFS`, `port_slot<n>`, `PORT_HEAP_OFS`, `PORT_ARENA_SIZE`) and the id's token form
(`PSXSTACK_GAME_ID_IDENT`, for pasted symbol names) come from it too, so nothing in the runtime computes a game fact.

## Sections named by the game's id on ELF, fixed on PE (2026-10-08)
The overlay sections are `<id>_data_<ovl>`/`<id>_bss_<ovl>` on ELF (the bracket symbols `__start_<id>_...`). On PE
they are the chunk groups of two fixed output sections, `.psxdata`/`.psxbss`: a PE image section name is eight
characters, which an id-prefixed name would not fit; the symbols still carry the id.

## Environment variables derive from the game's prefix (2026-10-08)
`<PREFIX>_PORT_TRACE`, `_FAST_FORWARD`, `_CHECKPOINT_DIR`, `_RESET_CHECK`, `_CRASH_AT`, `_PRESENT_READBACK`,
`_GPU_VRAM_CHECK`, `_PRIM_DUMP`, `_MCP_GAME`: `PSXSTACK_GAME_ENV_PREFIX` pasted at compile time, so the first game's
names (`DW3_PORT_TRACE`) and tests stay as they were, and two games on one machine never share a variable.

## The build's inputs are files the game writes (2026-10-08)
`tools/port_gen.py` reads no game C: the units, the overlays (slot, file ID, symbol file), the tag sites whose overlay
the game knows and the volatile ranges come as tab-separated files from the game's own tool
(dw2003recomp: `tools/port_inputs.py`), named in `psxstack_add_game()`. The stack checks what it can on its own
(`SLOT_FUNC`/`LATE_FUNC` sites against every table of their tier); a game adds precision, never knowledge the stack
would have to carry.

## The MCP server and the inventory are configured, not forked (2026-10-08)
`tools/mcp/server.py` takes the game on its command line (the game's `.mcp.json`: root, description, binaries,
symbol files, disc) and its self-test runs on fixture symbol files; `tools/port_inventory.py` is a module a game's
wrapper configures (`configure()`: sources, headers, include directories, symbol files, regions from the
description) and extends with its own commands (the first game: `structs`, `object-sizes`).

## examples/hello is the stack's smoke test (2026-10-08)
A disc-free Psy-Q program (two polygons, a pad read, the sound library on) built through `psxstack_add_game()` with
the first game's recovered Psy-Q headers copied beside it (MIT; `docs/THIRD_PARTY.md`), run headless with a
screenshot whose hash CI checks. It proves the generators, the sections, the shim and the runtime without any game,
so the stack's CI needs no disc and no private data. `discs` may be empty for it (the port then takes no `--disc`).

## Direct3D 12 on Windows, DXIL signed on Linux (2026-10-08)
The Windows build embeds each shader as SPIR-V and as DXIL (the same HLSL through the pinned DXC, whose `libdxil.so`
signs DXIL on the Linux host: D3D12 rejects an unsigned shader, so the build refuses one) and lets SDL pick the backend
in its own order, D3D12 before Vulkan: no Vulkan driver needed on Windows, and no setting (SDL's `SDL_GPU_DRIVER`
switches it). The HLSL needed no change: SDL builds the D3D12 root signature itself. Measured under Wine (the first
game's issue #67): Wine's vkd3d and vkd3d-proton draw the Vulkan build's pictures byte for byte. Wine's own vkd3d
refuses every swapchain composition, so a refused `SDL_SetGPUSwapchainParameters` keeps the claimed swapchain (SDR,
vsync) instead of falling back to software. Linux stays SPIR-V only.

## Sub-pixel precision: the GTE's fractions followed by value at addPrim (2026-10-08)
For the hardware renderer above scale 1, the GTE's 16.16 screen coordinates are kept in a shadow (`psyq/gte_shadow.c`;
`docs/PORT.md` "Sub-pixel precision"), never in anything the game reads, so the stream and the frame hash cannot
change. The copies games make from a vertex cache into packets are plain C the shim cannot watch (PGXP's memory
shadow is not available to a native port), so a linked polygon's vertex words are matched by value against the last
run of SXY stores (one mesh) and the result is keyed by the packet word's host address, validated against the word and
its integer when drawn. Words two vertices of a mesh share take their mean: no game-side hook is needed (the first
game's owner chose this over a hook at its copies, which would have made them exact).

## Save states hold the native stack, at fixed addresses (2026-10-08)
A state is the end of a vsync: the game's sections, the arena, every module's state through one sync function per
module (`savestate.h`; the same function saves and loads, so the directions cannot drift), and the game's own C stack
and registers. The game is native code, so its frames and objects hold host pointers; rather than translate them,
a state is valid only for the binary that saved it, at the same addresses: ELF builds are non-PIE (they already were,
for the debug channel), the game runs on a static stack of the runtime's when states may be used, the context is
`__builtin_setjmp`'s unmangled buffer at the end of `port_frame`, and the header names the binary (SHA-1) and the
addresses, refused on a mismatch. A rebuild therefore invalidates every state; making one again is a replay to the
point (the first game's battle: about 10 s headless), which is what its tests do. The heap is never in a state (the
memory cards are media, like the disc); host state (the window, the audio device, files, caches) is the loading
run's.

## Sub-pixel vertices are on by default above internal scale 1 (2026-10-08)
`video.subpixel` (`--subpixel`) defaults to `"on"`: a player who chooses a higher internal resolution expects the 3D
not to wobble by whole 1x pixels, scale 1 and the software renderer cannot change, and the scale itself is already an
opt-in (the first game's owner's decision, dw2003recomp #68). `"off"` keeps the PS1's whole pixels.

## Present filters: one pass, the default untouched (2026-10-08)
The hardware renderer's filters (`--filter`, `video.filter`; docs/PORT.md "Rendering") are each one pixel shader in
place of the present's own, with no intermediate target at the output's size: their cost is the output's size times a
few texel reads, and no memory that grows with the internal scale. (`smooth` adds one first pass at the 1x image's
size, 640x576 at most: its edge decisions per source pixel, read four times per output pixel, made it 12 times
faster.) `none` keeps the present's nearest shaders, so the default picture cannot change. A
filter must be continuous in its sampling position: many window sizes put pixel centres exactly on a boundary between
source pixels or lines, where devices round differently (a two-line CRT kernel differed by 154 between NVIDIA and
lavapipe; a continuous one by 2). The software renderer is never filtered: it stays the reference picture.

## Widescreen: a wide canvas beside the VRAM (2026-10-08)
A 16:9 picture comes from the hardware renderer, not from the game: a game that projects through the GTE already sends
geometry past the display's edges (its culling has a margin) and the GPU clips it, so the renderer draws every unit of
a display buffer a second time into a wider canvas in a strip of its target beside the VRAM (the PS1's VRAM has no room:
textures sit beside the buffers), with full-screen untextured 2D stretched. The game's command stream, the frame hash
and the software picture do not change; the VRAM part stays exact. Rejected: squeezing the GTE's projection (it
changes what the game draws and stretches every 2D sprite). The game names the scenes (`port_video_widescreen`); a
scene whose 2D ends at the screen's edge stays 4:3. The first game's issue #71 has the measurements.

## More texture room: Psy-Q's 2 MB VRAM layout, textures only (2026-10-26)
A game that draws more than its PS1 screen held needs textures the 1 MB VRAM has no room for (the first game's field:
3 more 128x128 map tiles for a 16:9 picture, the first game's issue #78). `port_video_vram_extend` makes `gpu.c`'s
VRAM 1024x1024 in Psy-Q's 2 MB layout: its `getTPage` already puts y's bit 9 into the texture page's bit 11 and
`getClut` into the CLUT's bit 15, so the game's code reaches rows 512-1023 with its own macros, and the 1 MB GPU (every
build that does not ask) ignores both bits as before. Only textures and CLUTs live there: the drawing
area and the display stay in rows 0-511 (E3/E4 have 9 bits of y), so the hardware renderer's target keeps its size and
only its VRAM copy, which every texel comes from, grows. Rejected: a second texture memory behind a port-only command
(a new primitive tag, another sampler, the same cost for nothing more), and room found in the game's own VRAM (what a
stage overlay loads varies by map, so every map would have to be measured, and a later change could take it).

## Texture keys by content, SHA-1, per VRAM word (2026-10-08)
Texture replacement (docs/PORT.md "Texture replacement") names a texture by the transfer that loaded it and by its
CLUT's content, never by VRAM position: the first game loads one image at up to 11 places and reuses places for
others. The hash is SHA-1 truncated to 64 bits (`runtime/sha1.c`, no new third-party code; hashing all the first
game's transfers costs nothing measurable). Ownership is tracked per VRAM word, not per transfer, because 30% of the
first game's textured draws sample the intact rest of a transfer that a later one partly covered.

## Texture packs are data mods named by key; scale 1 stays exact (2026-10-08)
A pack (docs/RUNTIME.md "Texture packs") is a data mod whose PNG file names are the dump's keys, so an artist edits a
dump in place and nothing else maps files to textures; the first pack given wins. A replacement's alpha thresholds
stand for the PS1's texel 0 and bit 15. At internal scale 1 its colour is rounded back to 5 bits, so an unedited dump
used as a pack leaves the target equal to the software VRAM (checked over the first game's replays); above scale 1 its
8 bits are kept.

## The user's data mods: --mods-dir, mods.<id>.enabled, mod_order (2026-10-08)
Texture packs are installed as data mods in the settings directory's `mods/` (docs/LAUNCHER.md "Data mods"). The game
still never looks for a directory: the launcher passes `--mods-dir`, as it passes `--config` and `--crash-dir`. A data
mod is switched on like a built-in one (`mods.<id>.enabled`), and one new top-level key, `mod_order`, holds their
priority, because data mods compete where built-in mods do not (two packs replacing the same texture).

## A game may have no stack heap (2026-10-08)
`memory.heap` is optional (GAME_CONTRACT.md "1. game.json"). The second game's heap is a static array in its own
`.bss`, managed by its own allocator: it is game data already (snapshotted, reset and saved with the sections), and
moving it into the arena would only serve `PTR_TO_S32`, which that game needs for a handful of fields. So the arena
can be the slots alone (`PORT_HEAP_PRESENT` 0), and the `HEAP_*` hooks then fail to compile with a message rather
than pointing at nothing. The alternative, a heap region placed before the slots in the arena, was rejected: a host
heap larger than the PS1's cannot grow in place there, and the first game's layout (heap after the slots) stays the
only arena shape to reason about. The description's other conventions (the override header's name, the Psy-Q
library map, `check.sh`'s paths) became configurable at the same time (issue #28).

## The replay runners live in the stack; a game keeps its probes and its pins (2026-10-08)
The emulator harness (PCSX-Redux headless with a pad script, the record and its cross-core comparison, the boot
check, the emulator installer) and the port's replay test (two runs byte-identical, the `-m32` and sanitizer builds,
the cross-core view against the expected file) were the first game's `tests/replay/`, `tests/port/run.py` and
`scripts/setup.sh redux`, and nothing in them was about the game but a dozen addresses and paths. They are now
`tools/replay/` (#27), driven by `game.json` plus two small game-side pieces: a Lua chunk of probes (the same state
the adapter's `game_state_*` report, read from the emulated RAM) and a configuration call per driver. A second game
starts with them instead of copying. The emulator and the disc stay the game's pins: the stack downloads nothing by
itself, and the record and script formats are unchanged (the first game's expected files are read as they are).

## Fibers: a hand-written switch, preemption at the tick's end (2026-10-08)
The second game (Digimon Digital Card Battle) schedules up to 32 tasks itself, cooperatively and from its vblank
handler; Psy-Q's `OpenTh`/`ChangeTh` games are the same shape. On the host each task is a fiber on a static stack of
the runtime's (`runtime/fiber.c`; docs/PORT.md "Fibers"). The switch is hand-written (x86-64 SysV and Win64, i386,
AArch64) rather than `ucontext` or Win32 fibers, because a save state must hold every live fiber as bytes at fixed
addresses and resume the one that was running: a saved stack pointer and the callee-saved registers on the fiber's
own stack can be saved and restored, glibc's `ucontext_t` (self-pointing, with the signal mask) and `CreateFiber`'s
heap object cannot. A vblank handler's switch (`port_fiber_preempt`) is performed by the pump at the end of the vsync
tick, not where the handler runs: the handler is inside the tick, and a switch there would leave the tick half-done on
the interrupted fiber; at the tick's end the interrupted fiber is suspended right after its tick, as the PS1's task
right after the interrupt. So every switch is a deterministic function of the game's execution, and a run with
fibers is as repeatable as one without.
