# The game contract

What a game repository provides to psxstack and what psxstack provides to it. The game is a matching decompilation
whose C also compiles for a 64-bit host; psxstack turns that C into a PC port with a launcher. Code cites this file as
`GAME_CONTRACT.md "<heading>"`; keep the headings stable.

Terms: **the stack** is this repository, pinned by the game as a git submodule at a tag. **The game** is the decomp
repo. **The adapter** is the game's small port-specific part: its description (`game.json`), its hook header, its
adapter units and its mods. **A unit** is one C file of the game. **An overlay** is a code file the PS1 loads into a
fixed address range (**a slot**); **a tier** names a slot (tier 1 = slot 1, ...).

## Layering

```
 game C units  ──includes──▶  game/include/port.h  ──includes──▶  <psxstack/hooks.h>      (the macros, both sides)
      │                                                                   │
      └──calls port_* ────────────────────────────────────────────▶  runtime/ (the stack)
                                                                          │ calls
 game adapter units (port/game/*.c)  ◀─── implements <psxstack/game.h> ───┘
 game adapter also provides: game.json, mods, symbols files, the unit list
```

Rules:
1. **The stack never includes a game header.** The runtime and the shim compile against the stack's own headers
   alone (`include/psxstack/`, the Psy-Q declarations among them: `include/psxstack/psyq/`, DECISIONS "Psy-Q
   declarations: the stack's") and the generated description; only the game's units and its adapter get the game's
   include directories (below). The stack's public headers use `<stdint.h>` types. The macros in `hooks.h` expand inside game code and may use the game's
   `s32`/`u32`/`u8` names there.
2. **The game's C never includes a stack header directly.** Its `include/port.h` (whatever it is called) includes
   `<psxstack/hooks.h>` and adds the game-specific hooks (its own slot-function typedef macros, its mod hooks).
3. **The runtime reaches the game only through `<psxstack/game.h>`**, the adapter interface the game implements, and
   through `game_main`. Anything the runtime needs from the game's objects (a gamestate field, a map ID, the player's
   position) is a function in that interface, implemented in the adapter, never a direct reference from `runtime/`.
4. **No fact about a game lives in the stack**: no address, file ID, overlay name, disc hash, serial, title or
   directory name. They come from `game.json` (generated into a header at configure time) or from the adapter.
5. **The PS1 matching build sees none of this.** The game's own `port.h` holds the PS1 side of every macro (each
   expands to the original code) and includes `<psxstack/hooks.h>` only under `#ifdef PC_PORT`, so the byte-identical
   build does not depend on the stack at all: the submodule may be absent.

## What the game provides

### 1. `game.json`, the game description
`schema/game.schema.json`; `examples/dw2003.game.json`. Static facts about the game, read once at configure time by
`psxstack_add_game()` and generated into `psxstack_game_gen.h` (C and C++): nothing reads the file at run time, so
the port and the launcher stay single binaries. Contents:
- **Identity:** `id` (the exe's name, the settings and cache directory names, the section prefix), `title`,
  `env_prefix` (`<PREFIX>_CONFIG_DIR`, `<PREFIX>_GAME`, …), `launcher.about`, `launcher.disc_hint`.
- **Discs:** the accepted images: label, serial, SHA-1 of the whole BIN, BIN size, region. Both the port's check and
  the launcher's check use this list; the error text names the label.
- **Timing:** `video.rate` (the nominal vsyncs per second: 50 PAL, 60 NTSC), the rates the launcher offers and the
  text that explains the alternative.
- **Memory:** `memory.ram` (base, size), `memory.slots` (name, base, size, in address order, contiguous), `memory.heap`
  (PS1 start and end, the host heap size; optional: a game whose heap is its own data, an array in its `.bss`, has
  none, and the arena is then the slots alone), `memory.bios_standin` (addresses the game reads from the BIOS ROM and what
  the stand-in holds).
- **Sections:** nothing. The section prefix is `id` (`dw2003_data_<ovl>`); the overlay names come from the unit list.

#### The generated header
`tools/game_gen.py GAME.json --out psxstack_game_gen.h` (and `--cmake` for the id and title as CMake variables, `--check`
to validate). Valid as C and as C++; `tests/game_gen_test.py` compiles it both ways. Its macros, which the runtime, the
launcher and the game's adapter use by these exact names:

| Macro | Value |
|---|---|
| `PSXSTACK_GAME_ID`, `PSXSTACK_GAME_ID_UPPER`, `PSXSTACK_GAME_TITLE` | `id` (and upper-cased), `title`, as string literals |
| `PSXSTACK_GAME_ENV_PREFIX` | `env_prefix`, default `id` upper-cased; the launcher's variables are `<PREFIX>_GAME`, `<PREFIX>_CONFIG_DIR`, `<PREFIX>_LAUNCHER_FAKE_GAME`, `<PREFIX>_SELFTEST_DISC`, `<PREFIX>_SELFTEST_GAME`, `<PREFIX>_SELFTEST_VIDEO_DRIVER` |
| `PSXSTACK_GAME_RATE`, `PSXSTACK_GAME_RATE_COUNT`, `PSXSTACK_GAME_RATES`, `PSXSTACK_GAME_RATE_NOTE` | The nominal rate (50 or 60), the rates offered (an `int` array initializer, the nominal one among them), the note shown for a rate other than the nominal (`""` when none) |
| `PSXSTACK_GAME_RAM_BASE`, `PSXSTACK_GAME_RAM_SIZE` | `memory.ram`, as `u` constants |
| `PORT_SLOT_COUNT`; `PORT_SLOT<n>_BASE`, `PORT_SLOT<n>_SIZE`, `PORT_SLOT<n>_NAME` for n = 1.. | `memory.slots`, in order: slot n is `tier` n in the hook macros |
| `PORT_HEAP_PRESENT`; `PORT_HEAP_START_ADDR`, `PORT_HEAP_END_ADDR`, `PORT_HEAP_SIZE` | 1 with a `memory.heap`, then the PS1 bounds and the host region's size (`host_size`, default 4 MB); 0 without one: no bounds, `PORT_HEAP_SIZE` 0, and the `HEAP_*` hooks refuse to compile |
| `PsxstackGameDisc` (a struct: `label, serial, sha1, cue, region` strings and `size`), `PSXSTACK_GAME_DISC_COUNT`, `PSXSTACK_GAME_DISCS` | `discs`, as an array initializer; `cue` is `""` when absent |
| `PsxstackGameBiosStandin` (`address`, `text`), `PSXSTACK_GAME_BIOS_STANDIN_COUNT`, `PSXSTACK_GAME_BIOS_STANDINS` | `memory.bios_standin`; with none the count is 0 and the initializer holds one empty entry |
| `PSXSTACK_GAME_ABOUT`, `PSXSTACK_GAME_DISC_HINT`, `PSXSTACK_GAME_WEBSITE` | `launcher.*`; `about` defaults to the title, the others to `""` |

A disc's `label` is a noun phrase, since the launcher writes "This is <label>." and "<file> is not <label>". The
generator checks what the schema cannot: the slots are in address order and contiguous with each other and with the
heap, the host heap is at least the PS1's, the rates include the nominal one.

### 2. The hook header
The game's `include/port.h` (included by its `common.h`, as today) keeps the **PS1 side** of every macro itself: each
expands to exactly the code the unit had, with the game's own constants, and nothing of the stack is needed to build
the executable. Only the host side comes from the stack:
```c
#ifndef PC_PORT
#define PLATFORM_WAIT()                                 /* the PS1 side, as today: the original code, verbatim */
#define SLOT_FUNC(type, addr) ((type)(addr))
...
#else
#include <psxstack/hooks.h>   /* the host side: PLATFORM_WAIT, OVERLAY_COPY, SLOT_FUNC, OVERLAY_FN, LATE_FUNC/LATE_CALL,
                                 SLOT_PTR, HEAP_*, PTR_*, BIOS_PTR, PORT_SCRATCHPAD_STACK_*; the port_* they call */
extern int port_mod_skip_dialogues;  /* the game's mod hooks, read only inside #ifdef PC_PORT blocks */
...
#endif
#define WSTAG_ENTRY(addr) SLOT_FUNC(void *(*)(), addr)   /* the game's own typed slot-function macros, both sides */
#define OVERLAY_ENTRY(addr) SLOT_FUNC(s32 (*)(void), addr)
```
The macro names and meanings of `hooks.h` are stable API (table in `docs/PORT.md "Hook macros"` once it moves here).
A game that schedules its own tasks (a hand-written context switch over kernel thread blocks, Psy-Q's
`OpenTh`/`ChangeTh`) replaces its switch glue under `PC_PORT` with the stack's fibers (`hooks.h` "Fibers":
`port_fiber_create`, `port_fiber_switch`, `port_fiber_exit`, `port_fiber_destroy`, `port_fiber_preempt` from a vblank
handler, `port_fiber_main`, `port_fiber_current`); the task records, priorities, waits and wake-ups stay the game's C
(docs/PORT.md "Fibers").
`hooks.h` gets the slot constants from `psxstack_game_gen.h` ("The generated header"): `PORT_SLOT<n>_BASE`,
`PORT_SLOT<n>_SIZE`, `port_slot<n>` for every slot in `memory.slots`, `PORT_HEAP_START_ADDR`, `PORT_HEAP_END_ADDR`. The
`tier` argument of `OVERLAY_COPY`, `OVERLAY_FN`, `LATE_FUNC` and `SLOT_PTR` is the slot's 1-based index. The PS1 side's
constants in the game's header must equal the description's; the adapter's build checks it with `_Static_assert`s.

### 3. The hooks in the game's C
The game's units carry the `PLATFORM_WAIT()` at every busy-wait an interrupt ends, the pointer macros where a pointer
meets an integer, `OVERLAY_COPY` at the overlay loads, the slot macros at every fixed address, and `#ifdef PC_PORT`
blocks for the mod hooks and the 64-bit `sizeof` fixes. This is the per-game cost of a port; the stack documents the
patterns (`docs/PORT.md "Compiling the game C for the host"`) and `tools/port_inventory.py` counts and gates them, but
nothing in the stack removes the need. A fork of a third-party decomp carries them as a small rebased patch set.

### 4. The adapter units (`<psxstack/game.h>`)
C the game compiles into the port alongside the runtime. It implements:
- `int game_main(void);` the game's `main`, renamed by the build (`-Dmain=game_main` on that one unit).
- `void game_apply_rate(long rate);` called after the settings are read and before `game_main`: the game's own
  response to a non-default rate (dw2003: `records_60hz = 1` for 60). May be empty.
- **State probes** (the replay scripts, the debug channel, the checkpoint hash): `game_state_stage()`,
  `game_state_file()`, `game_state_map()`, `game_state_random_index()`, `game_state_player_pos(double *x, double *y)`,
  `game_state_image_size()` and `game_state_image(uint8_t *out)` (the PS1 bytes a checkpoint hashes, pointers as PS1
  addresses), `game_state_volatile_count()` and `game_state_volatile()` (the ranges the stable hash zeroes),
  `game_state_read(addr, size, is_signed, out)` and `game_state_host(addr, size)`: a PS1 address outside the arena
  (the EXE's globals, an overlay's objects, memcard state) mapped to the host object by the adapter, from the tables
  the build generates for it. The stack maps the arena itself and asks the adapter for everything else.
- **The mods:** `game_mod_count()` and `game_mods()` return the game's `PortMod` records (`<psxstack/mods.h>`): id,
  version, options, `start`/`frame` callbacks and an optional `status` (text for the window title). Their logic and
  hooks are the game's; the option types, the manifest reader, the hotkeys, `--print-mods`, the settings section and
  fast-forward (the one mod every game has) are the stack's. The manifests (`mods/<id>/mod.json`) sit in the game
  repo and the build copies them next to the binary; `port_video_widescreen_enable()` and `port_video_widescreen(on)`
  let a game mod show the scenes it names wide, at the window's aspect up to 16:9 (the hardware renderer's wide
  canvas: docs/PORT.md "Rendering"), `port_video_widescreen_pad()` how far past the display's edges its 2D may draw,
  and `port_video_vram_extend()` gives it a 1024x1024 VRAM for the textures that takes;
  `port_fast_forward_request()` lets a game mod ask for
  fast-forward.
- **Save states:** `game_savestate(PortState *s)` names the adapter's own state that lives across vsyncs outside the
  game's sections (its mods' statics: a save in progress, a remainder carried between battles) with
  `PORT_STATE_VAR` (`<psxstack/savestate.h>`), in a fixed order; the same function saves and loads. The mods' options
  and toggles are settings, not state. The game's own data needs nothing: every writable byte of its units is in the
  renamed sections (`port_gen.py sections` proves it at every link), which a state holds whole.
- **Weak stand-ins** for data the game's residual asm owned (`asmdata.c`), placed in the right overlay section.
Every function has a weak default in the stack (`runtime/game_defaults.c`) that returns 0 or does nothing, so a new game
starts with an empty adapter and adds probes as its tests need them. They are functions rather than arrays so that
the weak defaults also work on COFF (Windows).

### 5. The build inputs
Passed to `psxstack_add_game(<target> ...)` (`cmake/psxstack.cmake`, which documents every argument):
- `GAME_JSON`: the description.
- `UNITS`: a file of `<absolute source path>\t<OVERLAY>` lines, every C unit and its overlay (`MAIN` for the
  executable's), from the game's own tool (dw2003recomp: `tools/port_inputs.py`, which knows the tier list,
  `config/wstag_c.txt` and the stage tables).
- `MAIN_UNIT`: the unit whose `main` becomes `game_main`.
- `OVERLAYS`: a file of `<NAME>\t<tier>\t<file id>\t<symbols file or ->` lines: every overlay, its slot (1-based), the
  file ID the game loads it by, and the `name = 0xADDR; // type:func` file of its PS1 functions (the overlay address
  tables). Optional for a program without overlays.
- `TAG_SITES` (optional): `<file:line>\t<tier>\t<OVERLAY | file:<id> | ->\t<0xADDR>` lines, the tag sites whose
  overlay the game knows, checked against that overlay's table; the stack finds `SLOT_FUNC`/`LATE_FUNC` sites itself
  and checks them against every table of their tier.
- `EXE_SYMBOLS` (optional): the EXE's symbol file (`type:func` and `size:` lines): the state tables for the probes.
- `VOLATILE` (optional): `<lo> <hi>` lines, the game-state image ranges the stable hash zeroes.
- `GTEMAC` (optional): the game's `gtemac.h`, translated onto the software GTE and written, first on the include
  path, at the header's path relative to the game's include directory (`psyq/gtemac.h`, `gte.h`); a game whose macros
  the translator cannot take passes none and puts a host GTE header of its own first in `INCLUDE_DIRS`
  (docs/PORT.md "Compiling the game C for the host"). `INCLUDE_ASM_GUARD`: its `include_asm.h`'s guard.
- `INCLUDE_DIRS`: the game's `include/` and root (its `common.h`, its own Psy-Q declarations). The units get them
  first on their include path, the adapter as quote-only directories (`-iquote`); the runtime and the shim never see
  them. The game's Psy-Q declarations must agree with the stack's in ABI: `tools/psyq_decls.py`, the inventory's
  `decls` command (DECISIONS "Psy-Q declarations: the stack's").
- `ADAPTER`: the adapter units; `MODS_DIRS`: directories of `<id>/mod.json` manifests (the stack's `mods/` is added).
- `DEFINES`, `UNIT_COMPILE_OPTIONS`, `CONFIGURE_DEPENDS` (the inputs' sources, so a change reruns the configure), `RC`.
Everything comes from tracked files: a port configures from a fresh clone with no disc. Options are cache variables
(`PSXSTACK_SDL`, `PSXSTACK_SANITIZE`, `PSXSTACK_M32`, `PSXSTACK_TOOLS_DIR`, `PSXSTACK_VERSION_ROOT`, ...).

**The generated header** (`psxstack_game_gen.h`, `tools/game_gen.py`): `PSXSTACK_GAME_ID` (and `_ID_UPPER`,
`_ID_IDENT`, the id as a bare token), `_TITLE`, `_ENV_PREFIX`, `_RATE`, `_RATE_COUNT`, `_RATES`, `_RATE_NOTE`,
`_RAM_BASE`, `_RAM_SIZE`; `PORT_SLOT_COUNT`, `PORT_SLOT<n>_BASE/SIZE/NAME/OFS`, `port_slot<n>`, `PORT_HEAP_START_ADDR`,
`PORT_HEAP_END_ADDR`, `PORT_HEAP_OFS`, `PORT_HEAP_SIZE`, `PORT_ARENA_SIZE`; `PsxstackGameDisc`, `PSXSTACK_GAME_DISC_COUNT`,
`PSXSTACK_GAME_DISCS`; `PsxstackGameBiosStandin`, `PSXSTACK_GAME_BIOS_STANDIN_COUNT`, `PSXSTACK_GAME_BIOS_STANDINS`;
`PSXSTACK_GAME_ABOUT`, `_DISC_HINT`, `_WEBSITE`. The runtime's environment variables are `<PREFIX>_PORT_<NAME>`.

### 6. Tests
The game owns its oracles: goldens, replays, the emulator harness's configuration, the M1 test's configuration. The
stack provides the formats and the runners (`tools/replay/`, docs/RUNTIME.md "The replay runners"): the layer-2
script grammar and the step engine (`runtime/script.c` on the port, `tools/replay/run.lua` in the emulator: the same
semantics frame for frame), the per-frame log and record shapes (`framelog.c`, docs/RUNTIME.md "The record"), the
emulator driver (`tools/replay/emulator.py`: PCSX-Redux headless, the record and its cross-core comparison, `run`,
`check` and `boot`), the port's replay test (`tools/replay/port_test.py`: two runs byte-identical, the `-m32` and
sanitizer builds, the cross-core view against the expected file), the emulator installer (`tools/replay/redux.sh`),
the debug-channel gate, the renderer comparison, the settings round trip, the launcher self-test. A game's test that
needs the stack's internals talks to the debug channel, not to the C. The stack's own smoke test is `examples/hello`
(`tests/hello_test.py`): a Psy-Q program with no game and no disc.

What a game supplies to the replay runners (the first game: `tests/replay/replay.py`, `tests/replay/probes.lua`,
`tests/port/run.py`):
- **The probes**, a Lua chunk the emulator runs with the stack's memory accessors in scope (`PSXSTACK_REPLAY`):
  `image_addr`/`image_size` (the checkpoint image: the bytes a checkpoint hashes, the same `game_state_image()`
  gives on the port), `stage()`, `file()`, `map()`, `random_index()`, optionally `player_pos()` (the `walk` step),
  `booted()` (the boot check) and `pad_held()`/`pad_pressed()` (verbose state lines). They mirror the adapter's
  `game_state_*` probes, so a script means the same in the emulator and on the port.
- **The drivers' configuration** (`emulator.configure()`, `port_test.configure()`): the repository root, `game.json`
  (the id names the binary, `env_prefix` the environment variables `<PREFIX>_REPLAY_*` and `<PREFIX>_JOBS`, the first
  slot's base `wait_stage`'s `word0`), where the emulator is installed, the disc image, the scripts and expected
  files, the probes, the volatile ranges (the bytes of the image zeroed for the stable hash: the same ranges
  `VOLATILE` gives the build), an optional retail BIOS, the paths whose last commit names the matching tree; for
  the port test the venv, the UBSan suppressions, the scripts whose `-m32` log must equal the `-m64` log and why
  the others' may differ, and two hooks for the game's own checks (`before_scripts`, `after_script`).
- **The pins:** the emulator (`redux.sh`'s zip or AppImage, its SHA-256, the runtime sysroot's packages for an old
  host glibc) and the disc are the game's choices, in its own setup script; the stack downloads nothing by itself.
- **The scripts and the expected files**, recorded in the emulator (`run --record`), and whatever Lua the game loads
  before `run.lua` (`--prelude`: a patch, a trace).

## What the stack provides

- **The runtime:** the arena at the PS1's distances from `memory.slots`, the N-slot overlay manager with data reset
  and address tables, the interrupt pump and timing at `video.rate`, the reset, the disc source with the SHA-1 check
  over `discs[]`, the memory cards, settings (schema 1, `--config`), the per-frame log and record, the input script,
  the window and input over SDL3, the software GPU, the hardware renderer, the SPU and audio, the crash report, the
  debug channel, save states (`--save-state`, `--load-state`), fibers for a game's own tasks (each on a host stack
  of the runtime's, switched where the game's scheduler says and at the end of a vsync tick a vblank handler asked
  for; held whole by save states).
- **The Psy-Q shim:** the libraries' behavior over the hardware models. It implements what its games call; a new
  game's missing function stops with `port_unimplemented(name)` and is added to the shim (the shim grows per game and
  stays generic).
- **The launcher:** settings directory and file, disc check, video/audio/memcard/input/mods screens, crash reports,
  the self-test; every string, name and hash from `psxstack_game_gen.h`.
- **The build:** `psxstack_add_game()`, the section renaming without a linker script, the post-link checks, the
  Windows cross-build, the host-compile gate (`port_inventory.py probe/link/structs`), the version header.
- **Packaging:** the AppImage and the Windows zip with PDBs, driven by `id` and `title`.
- **Tools:** the MCP server over the debug channel, the symbols from the game's symbol files.

## Compatibility and versions
- The stack is tagged semver `vMAJOR.MINOR.PATCH`. The game pins a tag. `PSXSTACK_API` (an integer in
  `<psxstack/game.h>`) counts incompatible changes to this contract; it replaces `PORT_MODS_API`, and a manifest's
  `requires_port` is checked against it.
- The settings file keeps `schema` 1; a change there is a stack MAJOR.
- A game's adapter compiles against exactly one stack version; the game's CI builds the pin, nothing else.
