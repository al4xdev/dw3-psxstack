/* The settings file (`--config FILE`; settings.c): the contract between the launcher and the game
 * (docs/LAUNCHER.md "Contract" to "Settings file", schema 1). The game reads a settings file only when `--config` names it: the
 * bare binary (the tests, CI) never depends on the machine. Every path in the file is relative to the file's own
 * directory (or absolute); the launcher owns the lookup of that directory. main.c applies the values, then the
 * command line's options override them. */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#include <stdio.h>

#include "json.h"

#define PORT_SETTINGS_SCHEMA 1

typedef struct PortSettings {
    char *file;      /* the settings file, absolute */
    char *dir;       /* its directory, absolute: relative paths in the file start here */
    char *disc;      /* disc.path, absolute; NULL: none */
    char *disc_sha1; /* disc.sha1 as stored (the launcher's record; the game checks the disc itself); NULL: none */
    int window;      /* video.window (default 1) */
    int scale;       /* video.scale, 1..16 (default 2) */
    int fullscreen;  /* video.fullscreen (default 0) */
    int refresh;     /* video.refresh, 50 or 60 (default 50) */
    int gpu;         /* video.renderer: "gpu" (the hardware renderer built into the window; issue #31) */
    int gles;        /* video.renderer: "gles" (the offscreen GLES renderer of a -DPSXSTACK_EGL build). Both 0 is
                      * "software", the default: a build that has no EGL and no GPU still runs */
    int internal_scale; /* video.internal_scale, 1..8 (default 1): the hardware renderer's resolution, x 1024x512 */
    int subpixel;    /* video.subpixel: 0 "off", 1 "on" (default), 2 "perspective": the GTE's sub-pixel positions above
                      * internal scale 1 (2: textured perspective-correct too) */
    int filter;      /* video.filter: PORT_FILTER_* (default 0, "none"): the hardware renderer's present filter */
    int crt_scanlines, crt_mask, crt_curvature; /* video.crt: the filters' parameters, 0..100 (default 50, 30, 0) */
    int mute;        /* audio.mute (default 0) */
    int watchdog;    /* watchdog: seconds, 0 off (default 0: a player's game is never killed by the debugging aid) */
    char *memcard[2]; /* memcard1/2, absolute; NULL: no card (null in the file); default card1.mcd/card2.mcd */
    PortJson *root;            /* the parsed file (owns the values below) */
    const PortJson *input;     /* input: { keyboard, gamepad, hotkeys } (input.c); NULL: the defaults */
    const PortJson *mods;      /* mods: { <id>: { enabled, <option>: value } } (mods.c); NULL: every mod off */
    const PortJson *mod_order; /* mod_order: the data mods' ids by priority (mods_data.c); NULL: by id */
    const PortJson *launcher;  /* launcher: the launcher's own state, never read by the game (printed back as is) */
} PortSettings;

extern PortSettings port_settings;
extern int port_settings_loaded; /* --config was given */

/* Reads and checks `path` into port_settings. A file that cannot be read, is not JSON, has another schema, or has a
 * value of the wrong type or out of range ends the run with status 64 and a message naming the key; an unknown key is
 * logged and ignored (a newer launcher's), and the `launcher` object is the launcher's own, never read. */
void port_settings_load(const char *path);
/* `s` (the effective settings: main.c's values after the command line) as a settings file, every key written,
 * absolute paths, `input` and `launcher` as given, `mods` resolved (port_mods_print): `--print-settings`. Loading its output gives the same output (the round trip). */
void port_settings_print(FILE *f, const PortSettings *s);
/* A bad value in the settings: "port: settings FILE: KEY: message" on stderr, exit 64 (as a bad option). For the
 * readers of the sections (input.c, mods.c) too. */
void port_settings_fail(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
/* `path` made absolute against the current directory (not resolved: no symlinks followed, it need not exist). */
char *port_settings_abspath(const char *path);

#endif /* PORT_SETTINGS_H */
