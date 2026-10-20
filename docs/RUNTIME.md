# The runtime: building, running, options and formats

A game built through `psxstack_add_game()` (`cmake/psxstack.cmake`; docs/PORT.md for the architecture) is one host
binary named after the game: the game's C, its adapter, the runtime (`runtime/`) and the Psy-Q shim (`psyq/`). This
page is the runtime's manual: the build options, the command line, the settings file, the window, what the build
generates, the runtime's parts, the per-frame log and the record, the SPU core and the audio output. `<game>` is the
game's id (the first game's: `dw2003`); `<PREFIX>` its environment prefix (`DW3`).

## Build and run
```sh
cmake -S port -B build/port -G Ninja      # the game's port directory; CMake >= 3.20; Ninja; GCC (or Clang) with GNU ld
cmake --build build/port                  # ~30 s from scratch with -j6 for the first game
build/port/<game> --max-frames 60         # exit 0 at the frame cap; 3 from port_unimplemented; 2 PLATFORM_HALT; 4 watchdog
build/port/<game> --trace                 # every tick, overlay load/resolve and stub call, to stderr
build/port/<game> --max-frames 600 --log run.log --record run.json   # the per-frame log and the record (below)
build/port/<game> --disc <disc>.cue --script scripts/new_game.json --log run.log --record run.json
build/port/<game> --help                  # every option (--cd-speed instant|realistic, --no-disc-check, ...)
build/port/<game> --disc <disc>.cue --debug /tmp/game.sock   # the debug channel (runtime/debug.c; tools/mcp drives it)
build/port/<game> --disc <disc>.cue --script s.json --save-state <checkpoint>:s.state --save-state-exit   # a state
build/port/<game> --disc <disc>.cue --load-state s.state [--script s.json] [--window]   # the run goes on from it
```
Options (CMake cache): `-DPSXSTACK_SANITIZE=ON` (`-fsanitize=address,undefined`; needs libasan/libubsan installed),
`-DPSXSTACK_M32=ON` (a 32-bit binary: `-m32` on every compile and link; needs gcc-multilib),
`-DPSXSTACK_ALLOW_UNRESOLVED=ON` (link with undefined symbols ignored), `-DPSXSTACK_UNIT_OVERRIDES="<unit>=<path>;..."`
(experiments: build a unit from another file, the tree untouched), `-DPSXSTACK_PSYQ_WERROR=OFF`,
`-DPSXSTACK_SDL=ON` (the window: below), `-DPSXSTACK_TOOLS_DIR=<dir>` (where `sdl3/`, `dxc/`, ... were built),
`-DPSXSTACK_VERSION_ROOT=<repo>` (the repository `git describe` names the build after: the game's),
`-DPSXSTACK_PYTHON=<python>` (the generators' interpreter).

The sanitizer and 32-bit builds go into their own build directories:
```sh
cmake -S port -B build/port-san -G Ninja -DPSXSTACK_SANITIZE=ON && cmake --build build/port-san
build/port-san/<game> --max-frames 600    # any ASan/UBSan report goes to stderr (and ASan's exits non-zero)
cmake -S port -B build/port-m32 -G Ninja -DPSXSTACK_M32=ON && cmake --build build/port-m32
build/port-m32/<game> --max-frames 600 --log m32.log && build/port/<game> --max-frames 600 --log m64.log && cmp m32.log m64.log
```
The `-m32` build is the layout check: pointers are 4 bytes there, as on the PS1, so a run whose log differs from the
64-bit build's has a pointer-size bug on one side (the log holds no host address).

## The settings file (`--config FILE`)
What the launcher starts the game with (docs/LAUNCHER.md "Contract" to "Settings file", schema 1; `runtime/settings.c`): the disc, the
window (`video.window`, `scale`, `fullscreen`, `refresh`), `audio.mute`, the memory cards (default `card1.mcd` and
`card2.mcd` beside the file, created formatted when missing; `null` for no card), the watchdog (default off), and the
`input` and `mods` sections. Paths in the file are relative to its directory. The options given on the command line
override the file. The game reads a settings file only when `--config` names it: the bare binary, which the tests run,
never depends on the machine. A bad value exits 64 naming the key; an unknown key is logged and ignored.
```sh
build/port-sdl/<game> --config <settings dir>/settings.json                   # as the launcher starts it
build/port/<game> --config settings.json --print-settings                     # the effective settings, every key
```

## The window (`-DPSXSTACK_SDL=ON`)
SDL3 (zlib licence) shows the PS1's display and reads the keyboard and
gamepads. It is optional: the default build has no SDL and stays headless (the tests use it). SDL3 is pinned
(`scripts/setup.sh` `SDL3_VER`/`SDL3_SHA256`) and built from its release tarball into `tools/sdl3/` as a static library,
so the binary needs nothing beside it; SDL loads X11/Wayland/ALSA/PulseAudio/... with `dlopen` at run time.
```sh
scripts/setup.sh sdl3 dxc                 # ~70 s with 2 jobs; again: "already installed"
cmake -S port -B build/port-sdl -G Ninja -DPSXSTACK_SDL=ON && cmake --build build/port-sdl
build/port-sdl/<game> --disc <disc>.cue --window            # 640x480, 50 vsyncs per second (PAL)
build/port-sdl/<game> --disc <disc>.cue --scale 3 --fullscreen
build/port-sdl/<game> --disc <disc>.cue --script scripts/new_game.json --window   # watch a replay
build/port-sdl/<game> --disc <disc>.cue --window --renderer gpu   # the window through the hardware renderer
SDL_VIDEO_DRIVER=offscreen build/port-sdl/<game> --input-test --fps 0   # no display: the input self-test (exit 0)
```
SDL's backends follow the `-dev` headers present when `setup.sh sdl3` runs: missing optional ones are turned off one by
one (logged), and with no X11/Wayland headers at all only the `offscreen` and `dummy` video drivers are built (enough
for the tests). For a desktop window install the headers (Debian/Ubuntu: `libx11-dev libxext-dev libwayland-dev
libxkbcommon-dev wayland-protocols libasound2-dev libpulse-dev libudev-dev`), then `rm -rf tools/sdl3 && scripts/setup.sh
sdl3`. Another SDL3 (3.2 or newer) is used when CMake is pointed to it (`-DSDL3_DIR=<dir with SDL3Config.cmake>` or
`CMAKE_PREFIX_PATH`). The window is 64-bit only (`PSXSTACK_M32` with `PSXSTACK_SDL` is refused).

**The picture** (`runtime/video.c`): every vsync the display area that `PutDispEnv` set (`psyq.h` "The video output":
`psyq_gpu_display`) is read from the VRAM (`psyq_gpu_vram`, the software GPU's) and converted to 32-bit pixels at its
own size: 15-bit (one VRAM pixel per screen pixel, 5-bit components widened as `(c << 3) | (c >> 2)`, the mask bit not
shown) or 24-bit (`isrgb24`, the movies and the title: 3 bytes per screen pixel, so a 320-pixel line spans 480 VRAM
pixels; `disp.w` counts screen pixels), any width up to 640 and height up to 576 (the title's 320x480 interlaced frame
is shown whole, both fields), wrapping in the VRAM as the GPU does; black while `SetDispMask(0)` holds. The image is
drawn at 4:3, nearest-neighbour, as tall as an integer multiple of its lines fits the window (centred; scaled to fit when
the window is smaller than the image). `--scale N` opens a 320N x 240N window (default 2; with an even N a 240-line and
a 480-line display come out the same size), `--fullscreen` (F11 toggles). `--screenshot FRAME:PATH` (repeatable; any
build, also headless) writes the image of vsync FRAME as a binary PPM: the texture's pixels, independent of the window.
**The renderer** (docs/PORT.md "Rendering"): `--renderer software` (the default) presents through
SDL_Renderer; `--renderer gpu` (or `video.renderer: "gpu"`) through the hardware renderer, `runtime/render_gpu.c` on SDL_GPU
(Vulkan; on Windows Direct3D 12 first, `SDL_GPU_DRIVER=vulkan` or `direct3d12` picks one). `--renderer gles` (or
`video.renderer: "gles"`) is a third one, `runtime/render_gles.c`, built only with `-DPSXSTACK_EGL`: it renders the VRAM
offscreen through EGL on a GBM device - no window, no DRM master, no mode change - and the picture leaves through the
same fbdev present. `--renderer` overrides the file; with neither, the file alone decides. It is opened before any SDL_Renderer (on Wayland a window that had an OpenGL renderer cannot be claimed by
Vulkan); when it cannot present (no Vulkan driver, NVIDIA on the offscreen driver, a shader that does not load) the run
logs `renderer: gpu unavailable (<why>); software` and uses SDL_Renderer. Its rasteriser draws the game itself into a
VRAM target of its own (`runtime/render_gpu.c`'s header comment) and a 15-bit display is presented from that target; 24-bit
displays (the movies, the title) stay the software image. At the internal scale of 1 the picture is the software
path's, pixel for pixel; `--internal-scale N` (`video.internal_scale`, 2 to 8) draws at N times the resolution (no
dithering, 8-bit colour; a device that cannot allocate the targets gets a lower scale, logged). Above scale 1 the 3D is
drawn at the GTE's sub-pixel positions (`--subpixel on`, the default; `video.subpixel`; `docs/PORT.md` "Sub-pixel
precision"), so it moves smoothly instead of a whole 1x pixel at a time; `--subpixel off` keeps the PS1's whole pixels,
`--subpixel perspective` also textures the 3D perspective-correct (off by default: it is not the PS1's look).
The log's last renderer line counts the polygon vertices drawn and those at their sub-pixel position. Its shaders need DXC at build time (`scripts/setup.sh dxc`, or `-DPSXSTACK_DXC=<path>`):
configuring the SDL build without it fails with that hint; the Windows build also compiles them to DXIL, and fails on a
DXC that cannot sign it (no `libdxil.so` beside it). `--gpu-screenshot FRAME[@WxH]:PATH` (repeatable) writes the hardware renderer's picture
of vsync FRAME as a PPM: the image itself, or with `@WxH` its present into a W x H output, letterboxed as the window
would be; a run that has no device opens one for it (headless too: `SDL_VIDEO_DRIVER=offscreen`), and logs the shot as
skipped when none opens. `--screenshot` and the debug channel's screenshot stay the software image.
`--filter NAME[:KEY=V,...]` (`video.filter`, `video.crt`) is the hardware renderer's present filter: `none` (the
default: the picture's pixels as whole blocks), `sharp` (sharp bilinear), `scanlines[:scanlines=0..100]` or
`crt[:scanlines=,mask=,curvature=]` (a parameter not named keeps `video.crt`'s value; defaults 50, 30, 0) or `smooth`
(xBR on the 1x software image, whatever the internal scale); it applies to
the window and to `--gpu-screenshot`'s `@WxH` pictures, and a software-renderer run logs that its picture stays
unfiltered (docs/PORT.md "Rendering").
**Widescreen** (docs/PORT.md "Rendering"): when a game mod calls `port_video_widescreen_enable` (from its start) and
`port_video_widescreen(1)` (every vsync of the scenes that widen), the hardware renderer presents those scenes 16:9
inside the window, the rest 4:3; the window opens 16:9 when the mod is on and `video.renderer` is `gpu`. With the
software renderer the picture stays 4:3 and the run logs `widescreen: needs the GPU renderer` once.
`<PREFIX>_PORT_PRESENT_READBACK=FRAME:PATH` reads SDL_Renderer's output back (a game's renderer test compares the two
present paths). `<PREFIX>_PORT_GPU_VRAM_CHECK=N` compares the rasteriser's whole target with the software VRAM every N
vsyncs and logs any difference (with `--renderer gpu --window`, or `--gpu-screenshot` headless).
`<PREFIX>_PORT_SUBPIXEL_LOG=PATH` (with `_FROM`, `_TO`: frames) turns the GTE shadow on and logs the projected vertices
(`docs/PORT.md` "Sub-pixel precision"). `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` picks Mesa's software Vulkan driver.
`--debug SOCKET` (any build) opens the debug channel on a Unix socket (`runtime/debug.c`'s header comment is the protocol;
`tools/mcp/` drives it): pause, step, wait, the pad, memory by host or PS1 address, screenshots, the hash, reset, quit,
each between two vsyncs; it turns the watchdog and the default frame cap off; `--debug-hold` starts the game paused at
its first vsync until the client resumes it (docs/PORT.md "Debug channel").

**Real time**: with a window the vsyncs are paced to `--fps N` per second (default: the description's rate; 0: unthrottled) against `CLOCK_MONOTONIC`; a run more than 0.1 s late starts the pace over instead of hurrying. Only the
time between vsyncs changes: the log and the record of a window run are the headless run's, byte for byte. Without a
window nothing is paced.

**Input** (`runtime/input.c`): the keyboard and every gamepad SDL sees are ORed into pad 1, a digital pad (`psyq_pad_set`),
once per vsync. Keys (by position): arrows the D-pad; X cross, C circle, Z square, S triangle; Enter (or keypad Enter)
START; Backspace (or right Shift) SELECT; Q L1, E R1, 1 L2, 3 R2; F11 fullscreen, P pause. Gamepads (SDL's positional
buttons, the PlayStation layout): south cross, east circle, west square, north triangle, back SELECT, start START,
shoulders L1/R1, triggers L2/R2, the D-pad and the left stick the D-pad. All of it is rebindable in the settings
(`input.keyboard`, `input.gamepad`, `input.hotkeys`; docs/LAUNCHER.md "Input bindings"), and the mods add hotkeys. A
hotkey never reaches the pad: a completed trigger masks its inputs until they are all released. **The pause** stops the
game between two vsyncs (the window keeps presenting the last image, the audio device pauses) until the key again. Every change of the pad goes to the log's `I` lines and the
record's `inputs`, as a script's do. With `--script` the script owns the pad: the window's input never reaches it.
Closing the window (or SIGINT/SIGTERM, which SDL turns into a quit) ends the run: `port_exit(0, "window closed")`, the
log and the record written. `--input-test` checks the path: it injects every key of the map (`SDL_PushEvent`), then
attaches a virtual SDL gamepad and presses each of its buttons, triggers and stick directions, and a chord, one per
frame with its release in the next, then every hotkey trigger, and requires the buttons sent to `psyq_pad_set` to be the
map's (none for a hotkey's, which must fire; with `--script`: none sent, and the script's log stays byte-identical to a
run without the test), then (without a script) the pause's round trip; exit 0 when all the checks pass , 6 otherwise. With `--config` it tests the settings' map. The first game's CI runs it on SDL's offscreen driver, with the defaults and with a rebound settings file.

**The mods** (`runtime/mods.c`; docs/LAUNCHER.md "Mod manifest", "Mod runtime"): the registry (the stack's
fast_forward, then the game's `game_mods`); each has a manifest `mods/<id>/mod.json` (the stack's `mods/`, the game's
directories), copied beside the binary (`build/port*/mods/`) for the launcher. `--print-mods` prints the
registry; the settings' `mods.<id>` enable and configure them; under `--script` they are off unless `--script-mods`.

**Two rates** (`runtime/pump.c`): the nominal rate (`port_rate`, the description's; `--fps N` sets it: the audio's samples per vsync) and
the pace (the wall clock's vsyncs per second; `port_pace_set`, which starts the schedule over on every change, so a
fast-forward that ends never makes the game wait for the vsyncs it ran ahead).

**Fast-forward** (the `fast_forward` mod; docs/LAUNCHER.md "Fast-forward"): enabled in the settings, hold `Tab` (or a
toggle binding) runs the game at the nominal rate times its speed (default `4x`), presents at most 60 images a second
and mutes the audio device; the game, its log and its record are unchanged.

## Texture dump
`--dump-textures DIR` (the SDL build, either renderer, headless too; docs/PORT.md "Texture replacement") writes every
texture a primitive samples, the first time it samples it, under DIR:
- `<overlay>/<image>-<clut>-<4|8>bpp-<w>x<h>.png`: an indexed PNG whose palette is the CLUT, or
  `<overlay>/<image>-15bpp-<w>x<h>.png` (RGBA). `<image>` and `<clut>` are the key's two 16-digit hashes; w x h is the
  whole transfer in texels at that depth (an atlas: the image of a whole page, recoloured by one CLUT). `<overlay>` is
  the tier-1 overlay that first sampled it (`main` without one); directories are only for people.
- Colours are 5-bit channels widened as `(c << 3) | (c >> 2)`; alpha is 0 for the texel `0x0000` (transparent), 128 for
  a texel with bit 15 set (semi-transparent where the primitive is) and 255 otherwise.
- `index.json`: `{"schema": 1, "textures": [...]}`, one entry per key: `file`, `image`, `clut` (not for 15-bit),
  `depth`, `size` [w, h], `first_vsync`, `overlays` (where it was sampled, at most 8), `vram` [x, y, w, h] (the
  transfer's place when first sampled, in VRAM words), `clut_xy`, `uv` [u0, v0, u1, v1] (the texels primitives
  sampled, inclusive, in the image's own texels: the part of an atlas that is this key's), `draws` (primitives),
  `semi` (a semi-transparent primitive sampled it). It is rewritten at the end of every vsync that added a key and at
  exit.
- **A dump continues:** an existing `index.json` is read back at the start, existing files are never rewritten, and
  the counts and ranges add up over the runs.

## Texture packs
`--texture-pack DIR` (repeatable; the SDL build) loads a texture pack, which the hardware renderer's rasteriser draws
instead of the game's textures (`--renderer gpu`, at any internal scale; docs/PORT.md "Texture replacement"). A pack
is a data mod:
```
DIR/mod.json
DIR/textures/**/<image>-<clut>-<4|8>bpp-<w>x<h>.png                   replaces the whole image (under that CLUT)
DIR/textures/**/<image>-<clut>-<4|8>bpp-<w>x<h>@<u>,<v>,<uw>x<vh>.png  only that sub-rectangle (in its texels)
DIR/textures/**/<image>-15bpp-<w>x<h>.png
```
```json
{ "schema": 1, "id": "hd_field", "name": "HD field sprites", "version": "1.0", "kind": "data", "requires_port": 1,
  "description": "...", "textures": { "dir": "textures", "filter": "linear" } }
```
- **The file name is the key**, as the dump names it ("Texture dump"); directories under `textures/` (`dir`, default
  `textures`) are free, and other PNGs are ignored (logged). A file named like a dump replaces it: an unedited dump is
  a pack that changes nothing at internal scale 1.
- **Any size**: a file is stretched over the whole image or its sub-rectangle; integer multiples of the original are
  best. PNG, RGBA or indexed.
- **Alpha**: below 64 transparent (as the texel `0x0000`), 64 to 191 semi-transparent (bit 15: blended where the
  primitive is), 192 and above opaque. The colour is used in 8 bits above internal scale 1, in 5 bits at it.
- **Which file**: a primitive takes a sub-rectangle file whose rectangle holds all its texels before the whole image's
  file, and the first pack given before later ones; with none it samples the VRAM as before (also under a texture
  window, and for render-to-texture and copies, which have no key).
- **`filter`**: `linear` (the default; with mipmaps) or `nearest`.
- Files are decoded on first use (a decode over 4 ms is logged: a hitch) and kept up to 1 GB of textures, then the
  ones unused for longest are released. Without `--renderer gpu` a pack is loaded but nothing draws it (logged).
- **As a player installs them:** a pack is a data mod in the settings directory's `mods/`, switched on and ordered in
  the launcher, which passes `--mods-dir` (docs/LAUNCHER.md "Data mods"); `--texture-pack` is the direct way (an
  artist's, a test's), its packs before the data mods.

## What the build generates (`build/port/gen/`, by `tools/port_gen.py`)
At configure time (the inputs are the files the game's tooling writes: `units.txt`, `overlays.txt`, `tag_sites.txt`,
`volatile.txt`; GAME_CONTRACT.md "5"):
- `include/psxstack_game_gen.h` (`tools/game_gen.py`): the game description as C macros; `psxstack_game.cmake` the
  id, title, prefix and rate as CMake variables.
- `include/include_asm.h` (empty `INCLUDE_ASM`/`INCLUDE_RODATA`) and, when the game gives its `GTEMAC`,
  `include/psyq/gtemac.h` (every `gte_*` macro translated from its MIPS sequence into calls of the software GTE,
  `psyq/gte.c`), first on the include path with the real headers' guards.
- `units.txt`: `<source path>\t<overlay>` per unit (`MAIN` for the EXE's) with the unit overrides applied, what the
  compile launcher reads.
- `markers.c` (PE only, `port_gen.py markers`): the `__start_<id>_*`/`__stop_<id>_*` symbols of the EXE's and each
  overlay's `.data`/`.bss` as empty labelled chunks in the groups `.psxdata$<ovl>_0`/`_2` and `.psxbss$<ovl>_0`/`_2`.

Each unit is compiled through `port_gen.py rename` (CMake's `C_COMPILER_LAUNCHER` on the units; a launcher of the
user's such as ccache runs after it): the object's writable sections (`.data*`, `.bss*`; not `.data.rel.ro*`, const
after relocation) go into the overlay's: on ELF `<id>_data_<ovl>` and
`<id>_bss_<ovl>` (`<ovl>` = `main` for the EXE), orphan output sections with C-identifier names, for which GNU ld
makes `__start_`/`__stop_` by itself and which it places after `.data`/`.bss`, outside GNU_RELRO (the ELF link stays non-PIE for the debug channel's `nm` addresses, docs/PORT.md
"Compiling the game C for the host"); on PE `.psxdata$<ovl>_1` and `.psxbss$<ovl>_1`, chunk groups of the two output sections
`.psxdata` and `.psxbss` (eight characters: a PE image section name's limit) that lld sorts by their `$` suffix, so that they lie between the markers above (two output
sections in all, not one per overlay). No linker script (lld for PE takes none; DECISIONS "Overlay sections by
renaming, no linker script"). On ELF the launcher runs the compile, then the host's GNU objcopy renames the
sections. On PE (`--pe`) it forces the overlay's `#pragma clang section data=... bss=...` into the compile
(`build/port-win/gen/sections/<ovl>.h`), and no objcopy touches the object: GNU objcopy drops the
`IMAGE_SCN_LNK_COMDAT` flag of every COFF section it rewrites, after which lld discards the unit's COMDAT
`.pdata$fn`/`.xdata$fn` (the x64 unwind tables; a crash report's stack walk and any SEH unwinding then stop at the
first game frame), and llvm-objcopy cannot rename COFF sections at all. Either way the launcher checks the object
with GNU objdump, which reads COFF too (Fedora's and Ubuntu's binutils have the `x86_64-pe` target; CMake checks
`objdump --info`). A game's stand-in data names its overlay's section explicitly the same way (`<id>_data_<ovl>`).

At build time, after the units are compiled:
- `overlay_tables.c`: per overlay `{ tier, file ID, name, [{ PS1 address, host function }], section bounds }`.
  The addresses are the `type:func` lines of each overlay's symbol file (`overlays.txt`); a function is in the
  table only if `nm` of the overlay's objects shows it as a global (a `static` function, or one still `INCLUDE_ASM`,
  has no host symbol). The file IDs come from `overlays.txt` too. The generator also checks every tag site
  (`SLOT_FUNC` and `LATE_FUNC` sites it finds itself; the game's `tag_sites.txt`) against the tables and fails the
  build if one does not resolve.
- `port_state_tables.c` (`port_gen.py state`, when the game gives `EXE_SYMBOLS`): the EXE's functions (`{ PS1
  address, host function }`, the `type:func` lines of its symbol file that nm finds in the EXE's objects), its data
  symbols with a `size:` there (`{ PS1 address, PS1 size, host object, layout-identical length }`), and the volatile
  ranges (`volatile.txt`). A data symbol is layout-identical as a whole when its `sizeof` at `-m64` is its PS1 size
  (a pointer or a `long` makes it larger at -m64); the generator measures that size by compiling the defining units
  at `-m64` (to assembly, ~1 s) in every build, so the `-m32` build's table is the same, and it fails if a
  pointer-free object's size differs between the two.

After the link (`port_gen.py sections`, POST_BUILD): every writable section of a game object (`.data*`, `.bss*`,
`COMMON` of the units' objects, read with `objdump -h`) must be a renamed one, i.e. inside the
ranges the console's reset restores; one left out fails the build (`-v` lists the other writable sections, such as
ASan's `asan_globals`). The link map (`build/port/<target>.map`, `-Wl,-Map`) is written for reading.

## The runtime
**Arena** (docs/PORT.md "Memory arena"): one static block, `port_arena`, mirroring the PS1 from the first slot
up: the description's slots at their distances, then the heap when the description has one (`host_size`, larger than
the PS1's because 64-bit structs are bigger); `port_slot<n>`, `port_heap_start` and `port_heap_end` are macros on it
(the generated header). A
pointer's PS1-style address (`PTR_TO_S32`) is `PORT_SLOT1_BASE + offset`. An
ordering-table tag (`PTR_TO_U32`) is a pointer's word offset in the **tag window**, the units' `.data`/`.bss` regions
plus the arena (a few MB, more under ASan; measured by `port_overlay_init`, under 64 MB by a startup check), since
a game may keep an ordering table static; the shim's `DrawOTag` walks tags from `port_tag_base`. No alignment or link
address is assumed by the arena: the arrangement a PE build can use as it is. The ELF link is non-PIE all the same
(`cmake/psxstack.cmake`): the debug channel's symbols are `nm`'s addresses, and a save state holds host addresses
("Save states").

**Overlay manager**: every overlay is linked in. `OVERLAY_COPY` (a game's overlay `memcpy` sites) calls
`port_overlay_load(tier, file, ...)`: a file with a table becomes the tier's current overlay and gets its `.data` and
`.bss` restored from the startup snapshot (`.bss`: zero; what the PS1's copy of the file did); a file without one
(data files) is copied into the slot buffer, exactly the PS1's memcpy. `OVERLAY_FN`/`LATE_CALL`
call `port_overlay_resolve(tier, addr)`: a tag (an address inside the PS1's RAM) is looked up in the tier's
current overlay's table (fatal if absent: static, still asm, or a data file is loaded); anything else is a host
function pointer and comes back unchanged. **The copy's time:** on the PS1 the copy is LIBC2's byte-loop `memcpy`
(about 12 cycles a byte), so 0x19000 bytes take ~1.8 frames, and an emulator's checkpoints can see the state
between the copy and the overlay's start-up. `port_overlay_load` therefore runs `size * 12 / 677376` vsync ticks
(`port_wait`) after a copy.

**Game-state probes** (the adapter's `game_state_*`, GAME_CONTRACT.md "4"): what a replay runner reads from PS1 RAM
(the stage, the file, the map, the random index, the player's position) read from the host's objects by the game's
adapter; the slot's first word is the runtime's (`port_overlay_load` keeps the first word of every file it copies,
per tier). `game_state_read(addr, size, signed, &v)` (the script's `wait_mem`, the debug channel's `peek_ps1` below
the arena) maps a PS1 address only where the adapter maps it (layout-identical objects, explicit field tables);
anything else returns 0 (unmapped).

**The checkpoint hash**: the adapter's `game_state_image` (its PS1 bytes, pointers as PS1 addresses), the emulator's
dump. The record has its SHA-1 and the SHA-1 with the volatile ranges zeroed (`gamestate_sha1_stable`).

**Pump**: a frame is one vsync tick (`psyq_vsync_tick`), from the game's `VSync()` or from `PLATFORM_WAIT()` ->
`port_wait()`, or from LIBCD's `StGetNext` once per 5000 empty polls (the movie player spins without a wait hook).
Each tick runs `port_frame` (`pump.c`): the CD tick (`psyq_cd_tick`), the frame log, the window's input (with a
window), the script's step, the screenshots and the window's present (`video.c`), the exit at `--max-frames` (default
600; none with `--script`), and with a window the real-time pace ("The window"). `<PREFIX>_PORT_CHECKPOINT_DIR=<dir>` writes each checkpoint's PS1
image as `cpNN_<name>.bin`. A watchdog (`--watchdog SEC`,
default 10) exits 4 when no `port_wait()` ran for that long: a loop that no hook reaches (see below).

**The console's reset** (`reset.c`; the script's `reset` step, an emulator's hard reset): the step releases
the pad and ends its frame (the input trace, the end-of-script check), then `port_reset_request`
longjmps from the vsync tick, however deep in the game's stack (a `VSync`, a `PLATFORM_WAIT`, `StGetNext`'s polling
tick, an overlay copy's ticks), to `main()`, which calls `port_reset_state` and `game_main()` again. The reset puts
back: every game global (the EXE's and every overlay's `.data`/`.bss` from the startup snapshot, static locals
included; no overlay current; `port_overlay_reset`), the arena (zero: the PS1's RAM is cleared), the shim
(`psyq_reset`: each library's `psyq_<lib>_reset`; the CD's sector source and timing model, the vsync hook, the GPU walk
window, the memory cards' contents and the trace setting stay). The frame count, the frame log's sequences (the next
frame records `(0, 0)` and map 0 as a change, as an emulator's listener records the cleared RAM), the checkpoints,
the input trace and the script's next step go on; the log gets an `R` line. `<PREFIX>_PORT_RESET_CHECK=1` (or `--trace`)
proves the restore: at startup (the last setup call before `game_main`) and after every reset, every game section is
compared with its startup snapshot and the arena with zero, fatal on a difference; with `port_gen.py sections` (no
game data outside the sections) that is the whole of the game's writable state.

## Save states (`--save-state`, `--load-state`)
`runtime/savestate.c` (docs/PORT.md "Save states" for the design): the whole machine at the end of a vsync in a file,
and a later run of **the same binary** going on from it.
- `--save-state WHEN:FILE`: at the end of vsync `WHEN` (a frame number, `port_frames`), or, when `WHEN` is not a
  number, at the end of the frame in which the script's checkpoint `WHEN` ran; repeatable (16 at most), each written
  once. `--save-state-exit` ends the run (status 0, reason `state saved`) once every one is written. A run with them is
  otherwise unchanged: its log and record are the plain run's.
- `--load-state FILE`: after the setup (the disc, the cards, the log, the window, the script), the state replaces
  power-on: the game goes on from the vsync after the saved one. With `--script` naming the script that was running
  when the state was saved, the script goes on from its saved step, so the run's record is the straight run's and its
  log is the straight log's lines after the saved frame. With another script, that script starts at the loaded frame
  (its `max_frames` counted from there); without one, the pad is the window's (or none). `--max-frames` still counts
  from frame 0 (the saved frame count goes on).
- **What must match:** the binary (its SHA-1), its pointer size and the addresses of its image, the arena and the
  game stack; the rate (`--refresh`). Anything else is refused with a message and status 1. The disc, the memory cards,
  `--cd-speed`, the window and the renderer are the loading run's: a state is not tied to them (the CD timing of a
  read in progress follows the loading run's `--cd-speed`).
  `DW3_STATE_ANY=1` lifts the binary and image check, for measuring a change that cannot move the game's data (a
  renderer, a timer) without replaying the game to the same scene: a battle state costs minutes to make. The arena and
  the stack are still checked. It is a measurement switch, not a way to run: a state loaded this way can hang or
  misbehave without saying so, which is what happened the first time it was tried.
- **Through the debug channel:** `save_state` and `load_state` (docs/PORT.md "Debug channel and the MCP server"; the MCP
  tools `state_save` and `state_load`): the same files.
- **The cost** (the first game, its first battle): about 8 MB, saved or loaded in under 0.1 s; the sanitizer build's
  is 27 MB (its sections carry ASan's redzones), 1.5 s, and needs `ASAN_OPTIONS=detect_stack_use_after_return=0`.
- **When states are on:** with `--save-state`, `--load-state` or `--debug`, the game runs on a stack of the runtime's
  (8 MB, static) instead of the main thread's; without them nothing of it exists but one branch in `port_frame`. A
  game's fibers (docs/PORT.md "Fibers") always run on static stacks of the runtime's, so a state holds them in any
  case, and a load resumes the fiber the vsync ended on.
  Under AddressSanitizer with its fake stacks on, `--debug` alone runs without states (logged).

**The state file.** A sequence of blocks, each `<tag length: 1 byte><tag><size: 8 bytes, host order><bytes>`, in an
order fixed by the code: each module's sync function (`<module>_state(PortState *)`, `include/psxstack/savestate.h`)
names its variables with `port_state_bytes`, and a load checks every block's tag and size against the build reading
it. The blocks, in order:
| Blocks | What |
|---|---|
| `header` | `PSXSTATE`, the format (1), the pointer size, the binary's SHA-1, the game's id, the addresses of the image, the arena and the game stack, the rate, the frame, the stack protector's canary (fixed widths: any build reads any state's header) |
| `port_frames`, `audio_vsync` | The frame count; the vsyncs the audio rendered (each vsync's sample count) |
| one per game section, `port_current`, `port_word0` | The EXE's and every overlay's `.data` and `.bss` (named by the overlay), the current overlay and the first word per tier |
| `arena` | The PS1's RAM: the slots and the heap (when the description has one) |
| `psyq_*`, `gpu_vram`, `gpu`, `gte_*`, `snd`, `sspu`, `mdec`, ... | The shim (`psyq_state`): LIBETC, LIBCD (with the stream ring and the XA decoder), LIBPAD, LIBGPU's display, the GPU's VRAM and drawing state, LIBGS, the GTE's registers, LIBPRESS and the MDEC, LIBSND and LIBSPU, LIBMCRD's command |
| `spu` | The SPU: registers, voices, its RAM |
| `count`, `overlay_seq`, `map_seq`, `checkpoints`, `inputs`, ... | The record so far and what the next frame's log compares with |
| `name`, `index`, `started`, `held`, ... | The script's name and progress |
| the adapter's (`game_savestate`) | The game's own (its mods' state across vsyncs) |
| `fibers`, `current`, `pending`, `fiber<n>` | The fiber table (every slot's entry, argument and saved stack pointer), the fiber the vsync ended on, the switch a vblank handler asked for, and each suspended fiber's stack from its saved pointer up (the main fiber's too when it is suspended) |
| `lo`, `stack`, `port_savestate_ctx` | The current stack (the game stack, or the fiber's the vsync ended on) from the capture point up, and `__builtin_setjmp`'s buffer |
A state holds the game's data: it is generated from the user's disc, never committed.

## The per-frame log (`--log FILE`)
Text, line-buffered, one line per frame and one per event; nothing in it depends on the host (no time, no address),
so two runs, and the `-m32` and `-m64` builds, write the same bytes. `<frame>` is `port_frames`: the vsync ticks so
far (an event between two ticks carries the last tick's number, as an emulator's listener would see it at the next).
| Line | Meaning |
|---|---|
| `# <game> port frame log 1 (port/README.md)` | The header (the format's version) |
| `F <frame> st <stage> fl <file> map 0x<map> prims <n> hash <8 hex>` | Every frame: the adapter's stage, file and map, and the primitive stream of the frame (`psyq_gpu_take_hash`: the primitives DrawOTag walked since the previous frame, their FNV-1a hash over the words the GPU reads: a textured polygon's padding halves and, for a 15-bit texture, its unused CLUT field are hashed as 0, since the game leaves them to stale packet-buffer bytes that differ between builds) |
| `S <frame> stage <stage> file <file>` | An overlay-sequence entry: `(stage, file)` changed (frame 1 always) |
| `M <frame> map 0x<map>` | A map-sequence entry: the map changed (frame 1 always) |
| `L <frame> tier <t> file 0x<id> <name> word0 0x<8 hex> size 0x<n>` | A file copied into a slot (`port_overlay_load`): the overlay's name, or `(data)`; its first word; its size |
| `C <frame> <name> stage <stage> map 0x<map> rnd <index> sha1 <40 hex> stable <40 hex>` | A checkpoint |
| `I <frame> buttons 0x<4 hex>` | The script's pad changed (`port_framelog_input`; PS1 bit order, active high) |
| `R <frame> reset` | The console's reset (the script's `reset` step): the next frame runs the game from `main()` again |
| `X <frame> status <status> <reason>` | The exit (`port_exit`) |

## The record (`--record FILE`)
JSON written at exit, with the keys of the first game's emulator records (its `replay.py` reads it as it is):
`runner` (`"port"`), `status` (the exit status), `reason`, `frames`, `checkpoints` (`name`, `frame`, `stage`, `map`,
`random_index`, `gamestate_sha1`, `gamestate_sha1_stable`, as the emulator runner records them), `overlay_sequence`
(`{frame, stage, file}`), `map_sequence` (`{frame, map}`), and `inputs` (`{frame, buttons: [names]}`, the names sorted) once the script has called `port_framelog_input`. The sequences follow the emulator runner's vsync
listener: an entry at every change, the first frame always (`{1, 0, 0}`, map 0).

## The replay runners (`tools/replay/`)
The emulator harness and the port's replay test a game configures (GAME_CONTRACT.md "6. Tests"; the first game's
`tests/replay/replay.py`, `tests/replay/probes.lua` and `tests/port/run.py` are the reference). Nothing here is run
by the stack's CI: it needs a game, its disc and its pinned emulator.
```sh
<game>/scripts/setup.sh redux                 # the game's setup calls tools/replay/redux.sh with its pins (below)
<game>/tests/replay/replay.py boot            # the boot check: the disc boots until the probes' booted() holds
<game>/tests/replay/replay.py run tests/replay/scripts/new_game.json --record   # record a script's expected file
<game>/tests/replay/replay.py run ... --repeat 2 [--interpreter] [--prelude x.lua] [-v]   # determinism; the other core
<game>/tests/replay/replay.py check [-j N]    # every script with an expected file, against it (the layer-2 test)
<game>/tests/port/run.py [--m32] [--sanitize] [--exe <game>.exe --wine]   # the port replays them (the M1 test)
```
- **`emulator.py`** drives PCSX-Redux headless (`-no-ui -stdout -testmode -run`, fresh memory cards per run, the
  pad script as a Lua table, `run.lua` as the `-dofile` chunk or a game's wrapper that loads its own Lua first),
  hashes each checkpoint image (SHA-1, and the stable SHA-1 with the game's volatile ranges zeroed), and builds the
  record: `script`, `script_sha1`, `emulator`, `bios`, `tree_commit`, `frames`, `checkpoints`, `overlay_sequence`,
  `map_sequence`, `inputs` ("The record" above: the port writes the same shape). `check` compares a run with the
  expected file whole; `--interpreter` and `--prelude` compare the **cross-core view** (checkpoint names, stages,
  maps and stable hashes; the overlay and map sequences without frames), the part of a record that neither the CPU
  core nor the port's timing changes. The emulator's environment: `PSXSTACK_REPLAY_SCRIPT`, `_OUT`, `_PROBES`,
  `_SLOT1_BASE`, `_SPEED`, `_VERBOSE`, and the same under the game's prefix.
- **`run.lua`** is the step engine in the emulator (`runtime/script.c` is the same engine on the port): every vsync it
  records the (stage, file) and map transitions, applies the held buttons through the pad override, advances the
  steps (`press` with `repeat`/`until`, `wait_stage`/`wait_map`/`wait_mem`/`wait_frames`, `walk`, `reset`,
  `checkpoint`, `vram`), and writes `result.json`. It reads the game's state through the probes chunk
  (`PSXSTACK_REPLAY_PROBES`), loaded with `PSXSTACK_REPLAY` (`u8`..`s32` over the emulated RAM) in scope.
- **`boot_check.lua`** waits for the probes' `booted()` (`PSXSTACK_BOOT_FRAMES`, default 3000) and prints
  `boot check: OK` or `FAIL`.
- **`port_test.py`** builds the port (`cmake -S <port> -B build/port -G Ninja`, and `build/port-m32`,
  `build/port-san` with `--m32`/`--sanitize`), runs each script twice (`--disc --script --log --record --spu-trace`,
  the checkpoint dumps in `<PREFIX>_PORT_CHECKPOINT_DIR`) and requires the logs, records and SPU traces identical,
  the cross-core view equal to the expected file's, the `-m32` build's view equal (and its log equal for the scripts
  the game names), and no sanitizer report; `--exe --wine` runs a Windows build under Wine instead. Two hooks take
  the game's own checks: `before_scripts(variants, out)` once after the build, `after_script(name, out, binary,
  run1)` per script.
- **`redux.sh`** installs a pinned PCSX-Redux from a release zip, an AppImage or a URL (SHA-256 checked) into a
  directory: `app/` (the extracted AppImage), the wrapper `pcsx-redux`, and when the host's glibc is older than the
  build's, a runtime sysroot from a list of pinned Debian packages (`--sysroot-debs FILE --mirror URL`) that the
  wrapper runs the binary through. The pins are the game's setup script's.

## The SPU core
`runtime/spu.c` and `runtime/spu_dsp.c` are the PS1's sound chip, our own from psx-spx (the first game's docs/SOUND.md section 6 has
what is modelled, the readings taken where psx-spx is silent, and the checks): the register file by offset from
`0x1F801C00` (`spu_write16`/`spu_read16`), 512 KB of SPU RAM (`spu_dma_write`, the FIFO), 24 voices (ADPCM, pitch
with the 4-point interpolation, ADSR, volume sweeps, noise, PMON), the mix with its clamps, the reverb at 22,050 Hz,
the CD input (`spu_cd_input`) and the capture buffers. `spu_render(out, frames)` renders 44,100 Hz stereo; time moves
only there (a register write acts between two samples), so the output is a function of the writes and the frame
counts. `spu_set_write_hook` sees every write and DMA block (the trace writer). LIBSND drives it (`psyq/libsnd*.c`); the first game's `tests/spu/` holds its unit goldens (from a Python model of
the same psx-spx text) and its checks against PCSX-Redux. 53× real time at `-O2`, 13× at `-O0`.

## Audio (`--wav`, the SDL3 audio device)
`runtime/audio.c` renders the SPU core once per vsync (`port_audio_frame`, `pump.c`'s vsync pre-hook: at the start of the
tick, before the game's VSyncCallback handler, where LIBSND's flush reads the envelopes and writes the SPU): vsync n gets `floor((n + 1) * 44100 / rate) - floor(n * 44100 / rate)` stereo frames
with `rate` = `--fps` (the description's rate by default: 882 frames a vsync at 50, 735 at 60; `--fps 0`: the nominal rate). So the audio lasts exactly as long as the vsyncs at their nominal pace and its pitch never changes (a PAL game
paced at 60 plays its music 20 % faster). It renders whether anything listens or not: LIBSND reads the voices'
envelopes back, so the game must not run differently with or without an output. The samples depend only on the SPU
writes and the vsync count: two runs give the same bytes.
```sh
build/port/<game> --disc <disc>.cue --script scripts/new_game.json --wav run.wav   # any build, headless
build/port-sdl/<game> --disc <disc>.cue --window            # sound through SDL3's default device
build/port-sdl/<game> --disc <disc>.cue --window --mute     # no device
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=out.raw build/port-sdl/<game> --disc <disc>.cue --window --max-frames 3000
```
`--wav FILE`: 44,100 Hz, stereo, signed 16-bit little-endian PCM; the header's sizes are written at exit
(`port_exit`), so the file is `44 + vsyncs × 882 × 4` bytes (every vsync of the run, the last one too). The window's device (unless `--mute`) is an SDL3 audio stream fed the
same samples. Its clock and the window's pace (`CLOCK_MONOTONIC`, `pump.c`) drift apart; the queue (frames put and
not yet played, measured before each vsync's put) is held without touching the game's timing or the samples: a moving
average above the band (target ± one vsync, target = the device's period + two vsyncs: 63 ms ± 20 ms at 1,024 frames)
speeds the stream's playback up by 0.2 % (`SDL_SetAudioStreamFrequencyRatio`, 3.5 cents) until it is back at the
target, below the band slows it down as much; an empty queue (the host was late) is refilled with the target's worth
of silence; above target + three vsyncs (an unthrottled `--fps 0`, a device that stalled) the vsync's samples are
dropped. The log (stderr) gets the device, the watermarks, and every 10 s and at exit the queue's minimum, maximum and
mean, the ratio changes, refills and drops. Without a device (none on the host, or SDL fails) the run goes on silent.
SDL's `disk` driver plays in real time into a file (S16LE stereo at 44,100 Hz) and `dummy` discards: headless tests of
the path (a CI can run the input self-test with `dummy`).
