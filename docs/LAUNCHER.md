# The launcher, the settings file and the mods

How the launcher (`launcher/`) and a game's PC port (the stack's runtime plus the game's adapter) talk to each other,
the settings file they share, the mod manifests and the mod runtime. Building and running the launcher, its screens and
its self-test: `launcher/README.md`. The game is `<id>` throughout (the description's `id`, GAME_CONTRACT.md "1.
game.json": `dw2003` for the first game); `<PREFIX>` is its `env_prefix`. What each of a game's built-in mods does is
the game's documentation, not the stack's.

Code cites this file as `docs/LAUNCHER.md "<heading>"`; keep the headings stable.

## Contract

- The game takes **one option for it, `--config FILE`** (JSON, "Settings file"). Command-line options override the
  file.
- **The game never loads a configuration on its own.** Without `--config` it behaves as the bare binary always did:
  the tests and CI run it that way and must not depend on the machine.
- The launcher edits that file and starts `<id> --config <dir>/settings.json --crash-dir <dir>/crashes --mods-dir
  <dir>/mods` in the settings directory (`SDL_CreateProcess`). Before that it runs `<id> --config FILE --mods-dir
  <dir>/mods --print-settings`, the game's own check of the file (a game without `--config`, `--crash-dir` or
  `--mods-dir` in its usage is too old for the launcher). The game stays startable without the launcher (`--config`, or plain options).
- A game started with `--config` has, by default: a window, memory card files beside the settings file, and the
  watchdog off.
- The disc's SHA-1 check stays the game's (the runtime's `disc.c`, `sha1.c`), against the discs the description lists.
  The launcher compiles `runtime/sha1.c` and `runtime/json.c` in as they are, to verify the disc before storing it and
  to read JSON with the same parser.
- Exit statuses the launcher reports: 0 normal, 1 a fatal error, 4 the watchdog, 64 bad options or settings, a
  signal; on Windows an unhandled exception's code instead of a signal (`0xC0000005` an access violation, ..., named
  by `game_exit_text`), and `abort()` is status 3 there too. On any of them but 0 the game may have written a crash
  report ("Crash report").
- The launcher's screens use the same SDL3 + `SDL_Renderer` as the game, so they could later be drawn inside the
  game's window.

## Settings directory

The launcher picks one directory, the first of these that applies (`launcher/src/settings.cpp`):

1. `--config-dir DIR`, else `$<PREFIX>_CONFIG_DIR` (relative to the current directory; created at the first save).
2. A file `portable.txt` beside the launcher's executable: that directory (portable mode).
3. A `settings.json` that already exists in the current directory (local testing; never created there implicitly).
4. The per-user directory, `SDL_GetPrefPath("", "<id>")`: `~/.local/share/<id>/` on Linux (`$XDG_DATA_HOME` when
   set), `%APPDATA%\<id>\` on Windows.

The directory holds `settings.json`, the memory cards, `logs/` (the game's output: `last-run.log`, the previous run's
as `last-run.1.log`), `crashes/` (the game's crash reports) and `mods/` (the user's data mods, "Data mods"). The launcher shows which directory it
chose and why. The game itself never looks for a settings directory: it reads only the file `--config` names.

## Settings file

Schema 1, read by the runtime's `settings.c` (the top level), `input.c` (`input`) and `mods.c` (`mods`).
`<id> --config FILE --print-settings` prints the effective settings (the file, then the command line) with every key
and absolute paths and exits 0, or exits 64 naming the bad key.

```json
{
  "schema": 1,
  "disc": { "path": "game.cue", "sha1": "457cb233..." },
  "video": { "window": true, "scale": 2, "fullscreen": false, "refresh": 50, "renderer": "software" },
  "audio": { "mute": false },
  "memcard1": "card1.mcd",
  "memcard2": "card2.mcd",
  "watchdog": 0,
  "input": {
    "keyboard": { "cross": "X", "start": ["Return", "Keypad Enter"] },
    "gamepad":  { "cross": "south", "up": ["dpup", "lefty-"] },
    "hotkeys":  { "pause": "P", "fullscreen": "F11" }
  },
  "mods": {
    "fast_forward":   { "enabled": true, "hold": "Tab" },
    "skip_dialogues": { "enabled": false, "toggle": "F2", "fast_forward_waits": false }
  },
  "launcher": { }
}
```

### General rules

- **Every key but `schema` is optional;** an absent key keeps its default. `schema` must be 1: a missing one, or any
  other number, ends the game with status 64 (a higher one is named "a newer launcher's").
- **Paths** (`disc.path`, `memcard1`, `memcard2`) are relative to the settings file's directory, or absolute.
- **Errors:** a value of the wrong type or out of range ends the game with status 64 and a message naming the key
  (`port: settings FILE: video.scale: an integer from 1 to 16, not 17`), as a bad option does. **An unknown key is
  logged and ignored** (at every level: top, `disc`, `video`, `audio`, `input`, a PS1 button, a hotkey action, a mod,
  a mod's option), so an older game runs a newer launcher's file.
- **The command line overrides the file:** `--disc`, `--scale`, `--fullscreen`, `--window`, `--mute`,
  `--memcard1|2 PATH|none`, `--watchdog`, `--refresh`, `--fps`.

### Members

| Member | Type | Default | Meaning |
|---|---|---|---|
| `schema` | number | required | Must be 1 |
| `disc.path` | string | none | The disc image (`.cue` or `.bin`). The launcher stores it absolute |
| `disc.sha1` | string | none | The launcher's record of the SHA-1 it verified. The game does not trust it: it checks the disc itself (`disc.c`, with its stamp cache) |
| `video.window` | bool | true | false runs headless (tests, a check run) |
| `video.scale` | integer 1-16 | 2 | The window is 320*scale x 240*scale |
| `video.fullscreen` | bool | false | |
| `video.refresh` | 50 or 60 | the game's nominal rate | The nominal rate, or the game's other one ("The rate") |
| `video.renderer` | `"software"`, `"gpu"` or `"gles"` | `"software"` | The renderer: SDL_Renderer, the hardware renderer (SDL_GPU; it falls back to software when no device can present), or `gles`, the offscreen GLES renderer of a `-DPSXSTACK_EGL` build (`docs/RUNTIME.md` "Rendering"). The launcher's "Renderer" choice offers the first two, which is all a desktop build has: a value it does not know is warned about and reset to `software` |
| `video.internal_scale` | integer 1-8 | 1 | The hardware renderer's resolution, N times the PS1's (1: the software picture). The launcher's "Resolution" slider, shown for the GPU renderer |
| `video.subpixel` | `"off"`, `"on"` or `"perspective"` | `"on"` | Above internal scale 1, the hardware renderer draws the 3D at the GTE's sub-pixel positions (`docs/PORT.md` "Sub-pixel precision") instead of the PS1's whole pixels; `"perspective"` also textures it perspective-correct (not the PS1's look: its affine textures swim on large polygons). The launcher's "3D vertices" choice, shown for the GPU renderer above x1. A game built before it logs the key and ignores it |
| `video.filter` | `"none"`, `"sharp"`, `"scanlines"`, `"crt"` or `"smooth"` | `"none"` | The hardware renderer's present filter (docs/PORT.md "Rendering"): `none` shows the picture's pixels as whole blocks, `sharp` is sharp bilinear, `scanlines` sharp with dark lines, `crt` a TV, `smooth` xBR on the 1x picture (the internal scale does not apply to it). The software renderer shows the picture unfiltered and logs it. The launcher's "Filter" combo, shown for the GPU renderer; it writes the member only once it is set |
| `video.crt` | `{ "scanlines", "mask", "curvature" }`, integers 0-100 | 50, 30, 0 | The filters' parameters: `scanlines` (the lines' strength) for `scanlines` and `crt`, `mask` (the aperture grille) and `curvature` for `crt`. The launcher's sliders under the Filter combo; written once set, as `video.filter` |
| `audio.mute` | bool | false | No audio device |
| `memcard1`, `memcard2` | string or `null` | `"card1.mcd"`, `"card2.mcd"` | A `.mcd` image, created formatted when missing; `null`: no card in that slot. An empty string is an error |
| `watchdog` | integer 0-3600 | 0 | Seconds without a vsync before the game exits 4; 0 is off. The bare binary (no `--config`) keeps its 10 s |
| `input` | object | the game's | "Input bindings" |
| `mods` | object | every mod off | "Mods section" |
| `mod_order` | list of strings | by id | The data mods' ids by priority, the first winning ("Data mods"). The launcher's Earlier/Later buttons write the whole list. Ids of data mods not found are kept |
| `launcher` | object | none | The launcher's own state (e.g. `last_dir`, where its file dialog opens). The game never reads it and `--print-settings` prints it back unchanged |

### Input bindings

`input` has three members; `--print-settings` prints it back as it was given.

- **`keyboard`**: PS1 button -> one key name or a list of them. **`gamepad`**: PS1 button -> one gamepad input name
  or a list. A button named replaces all of that button's entries of that kind; a button that is absent keeps its
  defaults; `""` or `[]` unbinds it. Buttons: `up down left right cross circle square triangle start select l1 r1 l2
  r2`. An unknown button is logged and ignored.
- **Key names are SDL3's scancode names** (`SDL_GetScancodeName`/`SDL_GetScancodeFromName`: `"X"`, `"Return"`,
  `"Keypad Enter"`, `"Right Shift"`, `"F11"`, `"Tab"`). Scancodes name the key's place, whatever the layout. A
  headless build (no SDL) checks only that a name is non-empty.
- **Gamepad input names are ours** (SDL's positional buttons): `south east west north back guide start leftstick
  rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 paddle1 paddle2 paddle3 paddle4 touchpad`,
  and the axes past half way: `lefttrigger righttrigger leftx- leftx+ lefty- lefty+ rightx- rightx+ righty- righty+`
  (`lefty-` is the left stick up). Every connected gamepad is ORed into pad 1.
- **Default map** (the runtime's `input.c`): arrows the D-pad, `X` cross, `C` circle, `Z` square, `S` triangle,
  `Return` and `Keypad Enter` START, `Backspace` and `Right Shift` SELECT, `Q`/`E` L1/R1, `1`/`3` L2/R2; gamepads:
  south cross, east circle, west square, north triangle, back SELECT, start START, the shoulders L1/R1, the triggers
  L2/R2, the D-pad and the left stick the D-pad. The launcher mirrors these defaults for display
  (`launcher/src/input.cpp`).
- **`hotkeys`**: the port's own actions -> a binding. Actions: `pause` (default `"P"`), `fullscreen` (default
  `"F11"`). A mod's bindings are its options, under `mods.<id>`, not here.
- **A binding** is a string or a list; each element is one trigger, any of which fires the action (at most 8). A
  trigger is one input (a string) or a chord (a list of 1 to 4 inputs, all held). An input is a key name, or `"pad:"`
  + a gamepad input name. `""` or `[]`: unbound. Examples: `"F2"`; `["F2", "pad:guide"]`;
  `[["pad:guide", "pad:south"], "Tab"]`.
- **Hotkeys never reach the pad.** A trigger that completes latches until all its inputs are released, and its inputs
  are masked out of the pad while latched: a single-key hotkey is never seen by the game. The first input of a chord
  made only of game buttons does reach the pad until the chord is complete, so chords should start with an input the
  pad map does not use (`pad:guide`, the stick clicks).
- **The pause** (the runtime's `pump.c`) takes effect at the end of the vsync where its key is pressed: the window
  keeps polling and presenting the last image, the audio device is paused, the watchdog is re-armed, and no vsync runs
  (so nothing reaches the game, the log or the record). The key again resumes, with the pace's schedule started over.
- With `--script` the script owns the pad: the keyboard and gamepads are read but never sent to the game (the hotkeys
  still act). `--input-test` tests the active map: the defaults without `--config`, the settings' with it.

### Mods section

`mods`: `<mod id>` -> `{ "enabled": bool, "<option id>": value }`, with the option ids and types of the mod's manifest
("Mod manifest").

- **A mod that is absent, or has no `enabled`, is off.** An absent option keeps the manifest's default.
- An unknown mod id ("no such mod in this build") is logged and ignored, an unknown option too. A value of the wrong
  type or range ends the game with status 64, as elsewhere.
- `--print-settings` prints `mods` resolved: every mod of the game with `enabled` and every option (defaults filled
  in), then the unknown mods as they were given.
- **Under `--script` every mod is off** unless `--script-mods` is given ("Mod runtime").
- The launcher writes `"enabled": true` for a mod switched on, writes an option only when it differs from the
  manifest's default, keeps a switched-off mod's changed options with `"enabled": false`, and keeps unknown mods and
  options.

## Mod manifest

Each mod has a manifest, `mods/<mod id>/mod.json` in the game repo. The game's build copies them beside the binary
(`<build dir>/mods/<mod id>/mod.json`), where the launcher lists them; the launcher also lists the data mods in the settings
directory's `mods/` ("Data mods"). The user-facing text (names, descriptions, labels) lives in the manifests only; the user's values
live in the settings file, never in the manifest.

**Top level:**

| Key | Required | Meaning |
|---|---|---|
| `schema` | yes | 1 |
| `id` | yes | Must equal the mod's directory name; the key under the settings' `mods` |
| `name` | yes | Shown in the launcher |
| `version` | no | A string |
| `description` | no | Shown on the mod's page |
| `kind` | yes | `builtin` (code in the game, from the game's build) or `data` (files: "Data mods") |
| `textures` | data mods | `{ "dir": "textures", "filter": "linear" }`, both optional: the mod is a texture pack (RUNTIME.md "Texture packs"), its PNGs under `dir`, filtered `linear` or `nearest` |
| `requires_port` | no | Integer: the stack's mod interface the mod needs (`PSXSTACK_API`, GAME_CONTRACT.md "Compatibility and versions"; 1 now) |
| `options` | no | A list of options |
| `presets` | no | A list of presets: `{ "id", "name", "description" (optional), "values": { "<option id>": value } }`. The launcher shows a button per preset (pressed while all its values are in place) that sets those values; the game never sees a preset, only the values |

**An option:** `id` (not `enabled`, unique in the mod), `name`, `description` (the launcher's tooltip), `type`,
`default`, and optionally `group` (a heading on the mod's page; ungrouped options come first) and `applies` (`live` or
`restart`; `restart` is marked in the launcher). Types:

| Type | Extra keys | Value in the settings |
|---|---|---|
| `bool` | | `true`/`false` |
| `int`, `float` | `min`, `max`, `step`; `slider_max` with `input_toggle` | A number in range (an integer for `int`) |
| `enum` | `values`: `[{ "id", "label" }]` | One of the value ids |
| `binding` | | The binding grammar of "Input bindings"; the default written the same way |

There is no `string` or `path` type. An `int` or `float` with `min` and `max` is a slider, without them a number field.
**`slider_max` and `input_toggle`** (together, on an `int` or `float` with `min` and `max`): `input_toggle` names a
`bool` option of the same mod; while it is off the option is a slider from `min` to `slider_max` (above `min`, at most
`max`), and a larger value in the settings counts as `slider_max` (the game logs it and uses `slider_max`; the file
keeps the value); while it is on the option is a typed number from `min` to `max`. A preset's value above
`slider_max` must come with the toggle set to `true` in the same preset. Example (`tests/fixtures/mods/skip_dialogues/`):

```json
{
  "schema": 1, "id": "skip_dialogues", "name": "Skip dialogues", "version": "1.0", "kind": "builtin",
  "description": "Text appears at once and advances by itself. Choices still wait for you.",
  "options": [
    { "id": "toggle", "name": "Toggle", "type": "binding", "default": "F2" },
    { "id": "hold",   "name": "Hold",   "type": "binding", "default": "" },
    { "id": "fast_forward_waits", "name": "Also fast-forward cutscene waits", "type": "bool", "default": false,
      "applies": "live" }
  ]
}
```

The built-in mods use the same schema as later data mods, so one renderer in the launcher serves both. A manifest the
launcher cannot use (not JSON, another schema, an `id` that is not its directory's name, a bad option or preset, an
unknown `kind`, a data mod with options or without `textures`) is listed with the reason and cannot be switched on.

## Data mods

A data mod is files, no code: `<settings dir>/mods/<mod id>/mod.json` with `"kind": "data"` and the files it names. The
only kind of data so far is `textures`, a texture pack (RUNTIME.md "Texture packs"; drawn by the hardware renderer).
- **The launcher** lists them on the Mods screen below the built-in mods, in their priority order, each with its
  on/off switch and, on its page, **Earlier**/**Later** (priority) and its directory. A built-in manifest there, an id a
  built-in mod has, or a data mod with options is listed as unusable.
- **The game** finds them through `--mods-dir DIR` (the launcher passes `<settings dir>/mods`; the game never looks for
  a directory itself) and reads each `DIR/<mod id>/mod.json`: schema 1, the id its directory's name, `kind` `data`,
  `requires_port` at most its API; others are logged and skipped. The settings switch one on as a built-in mod
  (`mods.<mod id>.enabled`; an unknown id is not logged for a data mod found there) and `mod_order` orders them, the
  first winning where two replace the same texture; those it does not name come after, by id. Packs given with
  `--texture-pack` come before all of them. Off under `--script` unless `--script-mods`, as every mod. Only the SDL
  build takes `--mods-dir` (the headless build exits 64).

## Mod runtime

The runtime's `mods.c` holds the registry of built-in mods the game's adapter provides (GAME_CONTRACT.md "4. The adapter
units": ids, versions, option types, defaults, ranges, `start`/`frame` callbacks), reads their values from
`mods.<mod id>`, registers each enabled mod's `binding` options as hotkey actions (named `<mod id>.<option>`), and runs
them at every vsync. `<id> --print-mods` prints the registry as JSON; a game's tests require every manifest to agree
with it.

**Rules for a mod that changes the game's behaviour:**

1. Its hook in the game's C sits in an `#ifdef PC_PORT` block that tests a `port_mod_*` flag (declared in the game's
   hook header). Never `if (MACRO_THAT_IS_0)` or `|| MACRO`: an expression that is constant on the PS1 can change the
   old compiler's code. The game's byte-identical build gates every hook.
2. **Mods are off under `--script`** unless the run passes `--script-mods`, so the replays, goldens and records stay
   the bare binary's.
3. A hotkey never reaches the pad: no `I` line in the frame log, no change to a record.
4. A mod's own state lives in the runtime's variables. A game global a mod must set is set before
   `port_overlay_init()` (the reset's snapshot); a later write breaks the reset check and is undone by a reset.
5. A run with a mod on needs its own expected results: a game's random generator usually steps once a frame, so any
   skipping shifts later rolls (as a faster player would).

**The pace split** (the runtime's `pump.c`): `port_rate` is the nominal rate (50 or 60: the vsyncs per second the game
is made for, which sets the audio's samples per vsync); the pace (`port_pace_set`; 0 = unthrottled) is the wall
clock's vsyncs per second. Every change of the pace starts the schedule over, so lowering it does not stall the game.
`--fps N` sets both.

## Launcher

`<id>-launcher` (`launcher/`) is C++17 with Dear ImGui on SDL3 + `SDL_Renderer`; it is its own CMake project and the
game stays C. Every name, string, hash and number it shows comes from the game's description (`launcher/src/brand.h`
over the generated header). It finds the settings directory, edits `settings.json` (keeping every member it does not
edit, unknown ones included), and starts the game. Screens: **Disc** (file dialog, drag-and-drop, or a typed path; only
the described discs' SHA-1s are accepted), **Play** (starts the game; on an error shows the exit status, the crash
report and the last lines of output, "Crash report"), **Settings** (scale, fullscreen, the rate when the game offers
two, renderer, mute, memory cards), **Controls** (keyboard, gamepad, hotkeys), **Mods** (an on/off switch per mod and a
page generated from its manifest; the data mods in the settings directory's `mods/` in priority order, "Data mods"). Build, run, details and the self-test: `launcher/README.md`.

## Crash report

The game writes a crash report when it dies or stops on an error (`docs/PORT.md` "Crash report"): the launcher passes
`--crash-dir <settings dir>/crashes`, streams the game's whole output to `<settings dir>/logs/last-run.log` (the
previous run's kept as `last-run.1.log`) and, when the game ends with an error, reads the report the game named
(`port: crash report: PATH`) and shows its path on the Play screen. **Copy** puts on the clipboard, in this order: the
launcher's version and the platform, the game's path and version, the command, the result in words, the report's
text, the last 40 lines of the output and the log file's path: what to paste into an issue. **Open folder** opens the
crash directory. A game too old for `--crash-dir` is reported as too old, like one without `--config`. On Windows the
same, with the minidump (`crash-<date>.dmp`) beside the report in `%APPDATA%\<id>\crashes\` for a tester to attach.

## The rate

A setting, not a mod: `video.refresh` (or `--refresh N`), default the description's `video.rate` (50 PAL, 60 NTSC). The
launcher offers the description's `video.rates` and shows its `rate_note` when the other rate is chosen; a game with one
rate has no Refresh row. The runtime sets `port_rate` and the pace to the rate (735 or 882 audio samples a vsync) and the
CD drive's vsync rate (its sectors and XA frames per second stay the same), then calls the adapter's
`game_apply_rate(rate)` before `port_overlay_init()` (so the reset's snapshot holds whatever it set): that is where a
game runs its own mode for the other rate, as Digimon World 2003 does with the flag its NTSC patch sets (its docs
describe it). `--fps N` alone is not that mode: it speeds everything up, music and the play-time clock included.
