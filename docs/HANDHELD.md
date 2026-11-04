# The handheld fork: what dw3-handheld adds

This repository is a fork of `gascarcella/psxstack` (remote `upstream`); `origin` is `al4xdev/dw3-psxstack`. The branch
`dw3-handheld` carries the work that lets the Digimon World 2003 port run on a 32-bit ARM handheld (EmuELEC and other
PortMaster firmwares). The inherited docs belong to upstream and are left as they are; the facts that exist only in
this fork are collected here. The port consumes this repository as a git submodule: its `.gitmodules` points at
`https://github.com/al4xdev/dw3-psxstack.git`, branch `dw3-handheld`.

## What the branch adds

- **A GLES renderer** (`runtime/render_gles.c`, `--renderer gles` or `video.renderer: "gles"`): the VRAM is an RGBA8
  texture drawn offscreen through EGL on a GBM device; the picture leaves through the fbdev present. Built only with
  `-DPSXSTACK_EGL=ON`; without it the run logs that gles needs EGL and the software renderer draws. The options and the
  flow are in docs/RUNTIME.md "The window"; the design notes and measurements are in the port's `docs/gpu/`.
- **A direct framebuffer present** (`runtime/video.c`): `/dev/fb0` is written with NEON 2x scaling, no window and no
  window system. It is chosen by `DW3_VIDEO=fbdev`, or by default when neither `DW3_VIDEO` nor `SDL_VIDEODRIVER` is set
  and `/dev/fb0` is readable and writable. If the open fails the run falls back to SDL.
- **NEON spans in the software rasteriser** (`psyq/gpu.c`) for Gouraud and flat spans; each falls back to scalar when a
  span can read its own row.
- **An SDL2 build** (`-DPSXSTACK_SDL2=ON`): the handheld firmwares ship SDL2, so the audio queue and the window code
  have an SDL2 path beside the SDL3 one. The port's `tools/build_arm.sh` shows the full ARM configuration.
- **Audio** (`runtime/audio.c`): `port_audio_resync` clears the queued audio when an overlay loads, and the SDL2 queue is
  bounded, so the sound does not drift behind the picture after a long load.
- **Signals** (`runtime/platform.c`): SIGTERM, SIGINT and SIGHUP end the run at the end of the next vsync, so the log and
  the record are finished. SDL turns these signals into a quit event the port never reads, which let a killed run keep
  drawing.
- **Log levels** (`runtime/main.c`): `<PREFIX>_LOG` is `off`, `soft` (the default) or `heavy`. `off` silences the
  commentary but the crash report still receives the last lines; `heavy` adds the periodic measurements.
- **CMake**: `PSXSTACK_GAME_OPT` sets the optimisation of the game's own C, which otherwise follows the build type.

## Environment variables

Three are derived from the game's prefix (`<PREFIX>` is `DW3` for this port; docs/DECISIONS.md "Environment variables
derive from the game's prefix"); the others are spelled `DW3_` in the code.

| Variable | Read in | Meaning |
|---|---|---|
| `<PREFIX>_LOG` | `runtime/main.c` | `off`, `soft` or `heavy`; the launcher sets it from the settings file's `debug.logs` |
| `<PREFIX>_CPU_AFFINITY` | `runtime/main.c` | CPU mask (e.g. `0x4`) the process pins itself to; unset or 0 leaves it to the scheduler |
| `<PREFIX>_NO_AUDIO` | `runtime/audio.c` | `1`: the SPU mix is not run and silence is played, to weigh its share of a frame |
| `DW3_VIDEO` | `runtime/video.c` | `fbdev` forces the framebuffer present; any other value uses SDL |
| `DW3_STATE_ANY` | `runtime/savestate.c` | `1`: load a state saved by another build of the same game; for measuring a renderer change, never for play |
| `DW3_PRESENT_LOG` | `runtime/video.c` | file the per-second present time is appended to |
| `DW3_PHASE` | `runtime/phasetime.c` | any value but `0`: per-phase timers |
| `DW3_GPU_LOG` | `psyq/gpu.c` | any value but `0`: pixel mix and ordering-table time, one line a second |
| `DW3_NO_NEON_GOURAUD`, `DW3_NO_NEON_FLAT` | `psyq/gpu.c` | `1`: use the scalar span instead of the NEON one |
| `DW3_SKIP_BIG` | `psyq/gpu.c` | `N`: draw no triangle or rectangle over N pixels; the picture is wrong on purpose |
| `DW3_LOAD_LOG` | `psyq/libgpu.c` | any value but `0`: every VRAM transfer with its rectangle and frame |
| `DW3_GLES_LOG` | `runtime/render_gles.c` | `1`: per-second GLES counters and times |
| `DW3_GLES_NOFLAT`, `DW3_GLES_NOTEX` | `runtime/render_gles.c` | `1`: skip flat or textured primitives |
| `DW3_GLES_TRACE` | `runtime/render_gles.c` | a frame number: every rectangle of that frame is logged (for bisecting) |
| `DW3_GLES_REPLACE` | `runtime/video.c` | `0`: the CPU keeps drawing as well, so the VRAM check can compare the two |
| `DW3_GLES_CHECK` | `runtime/video.c` | `N`: compare the whole GLES target with the shim's VRAM every N frames and log the first difference |
| `DW3_GLES_DUMP`, `DW3_GLES_DUMP_FRAME` | `runtime/video.c` | dump both sides to `/tmp` on the first failed check, or on the frame named |

Everything from `DW3_PRESENT_LOG` down is a measurement switch, not a setting.

## Following upstream

`dw3-handheld` is 44 commits ahead of `main` and 41 behind it (2026-11-04); `main` is what this fork was branched from.
Documentation changes here conflict with upstream edits, which is why this page is a separate file.

To bring upstream in:

```sh
git fetch upstream
git switch dw3-handheld
git merge upstream/main          # or rebase; resolve conflicts, usually in docs/ and runtime/
```

Then rebuild the stack through a consumer and run the host tests listed in docs/RUNTIME.md and `CLAUDE.md`
(`tests/hello_test.py`, `tests/tasks_test.py`, `tests/game_gen_test.py`). The renderer, audio and signal changes touch
`runtime/` and `psyq/`, so the real gate is the port's own build and test run on the ARM target. After the merge, the
port's submodule pointer has to be moved to the new commit.

## See also

- docs/RUNTIME.md "The window": the renderer options, `--renderer gles`.
- docs/LAUNCHER.md "Contract": what the launcher passes to the game.
- docs/PORT.md "Rendering" and "Crash report".
- docs/DECISIONS.md "Environment variables derive from the game's prefix".