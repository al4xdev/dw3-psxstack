#!/usr/bin/env python3
"""psxstack's build generators (cmake/psxstack.cmake runs them; docs/PORT.md "Overlays"). Every input is a file the
game's own tooling writes at configure time (GAME_CONTRACT.md "5. The build inputs"): nothing here reads a game's C.

  tools/port_gen.py overrides --out GEN/include [--gtemac include/psyq/gtemac.h [--gtemac-include psyq/gtemac.h]]
                              [--include-asm-guard INCLUDE_ASM_H]
      # include_asm.h (INCLUDE_ASM/INCLUDE_RODATA empty) and, with --gtemac, the GTE header at the path the game
      # includes it by (default psyq/gtemac.h; psxstack.cmake passes the GTEMAC file's path relative to the game's
      # include directory): the GTE macros on the
      # software GTE (psyq/gte.c) for the host
  tools/port_gen.py rename --id ID --units GEN/units.txt --objcopy objcopy --objdump objdump [--pe] -- cc ...
      # the units' compile launcher: the object's .data/.bss sections renamed into its overlay's (no linker script)
  tools/port_gen.py markers --id ID --overlays GEN/overlays.txt --out GEN/markers.c   # PE: the bracket symbols
  tools/port_gen.py sections --id ID --objdump objdump --objects objs.rsp   # after the link: no game data outside them
  tools/port_gen.py tables --id ID --units U --overlays O [--tag-sites T] --nm nm --objects objs.rsp --out GEN/overlay_tables.c
  tools/port_gen.py state --units U --exe-symbols config/symbol_addrs.txt [--volatile V] --nm nm --cc cc
      [--cflags "-m32"] -I DIR... -D DEF... --objects objs.rsp --out GEN/port_state_tables.c

The input files (tab-separated, `#` comments):
  units.txt      <absolute source path>\t<OVERLAY>        every C unit and its overlay; MAIN = the executable's
  overlays.txt   <NAME>\t<tier>\t<file id>\t<symbols file or ->   every overlay (not MAIN), its slot (1-based), the file
                 ID the game loads it by, and the `name = 0xADDR; // type:func` file of its PS1 functions
  tag_sites.txt  <file:line>\t<tier>\t<OVERLAY | file:<id> | ->\t<0xADDR>   the game's tag sites whose overlay is known
                 (the generic scan finds SLOT_FUNC and LATE_FUNC sites itself and checks them against every table of
                 their tier)
  exe symbols    `name = 0xADDR; // type:func` lines (the EXE's functions) and `size:N` lines (its sized data)
  volatile.txt   <lo> <hi>   byte ranges of the game-state image the stable hash zeroes
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


# ------------------------------------------------------------------------------------------------------------- inputs

def read_table(path):
    """The tab-separated rows of an input file, comments and blank lines dropped."""
    rows = []
    for ln in Path(path).read_text().splitlines():
        ln = ln.split("#", 1)[0].rstrip()
        if ln.strip():
            rows.append(ln.split("\t"))
    return rows


def read_units(path):
    """[(absolute source path, OVERLAY)]"""
    units = []
    for row in read_table(path):
        if len(row) != 2:
            sys.exit(f"port_gen: {path}: a units line is <source>\t<OVERLAY>: {row}")
        units.append((os.path.normpath(row[0]), row[1]))
    return units


def read_overlays(path):
    """[{name, tier, file, symbols}] in the file's order."""
    out = []
    for row in read_table(path):
        if len(row) != 4:
            sys.exit(f"port_gen: {path}: an overlays line is <NAME>\t<tier>\t<file id>\t<symbols file or ->: {row}")
        name, tier, fid, syms = row
        out.append({"name": name, "tier": int(tier), "file": int(fid, 0), "symbols": None if syms == "-" else syms})
    return out


def read_tag_sites(path):
    """[(where, tier, overlay name | ('file', id) | None, address)]"""
    sites = []
    for row in read_table(path):
        if len(row) != 4:
            sys.exit(f"port_gen: {path}: a tag-sites line is <where>\t<tier>\t<overlay>\t<0xADDR>: {row}")
        where, tier, ovl, addr = row
        if ovl == "-":
            o = None
        elif ovl.startswith("file:"):
            o = ("file", int(ovl[5:], 0))
        else:
            o = ovl
        sites.append((where, int(tier), o, int(addr, 0)))
    return sites


def object_unit(obj, units):
    """The (source, overlay) an object was compiled from: CMake names an object after its source's path relative to
    the project (or `__/`-prefixed above it) plus `.o`; the unit whose path ends with the longest matching tail wins."""
    parts = Path(obj).as_posix().split("/")
    stem = re.sub(r"\.(o|obj)$", "", parts[-1])
    best, best_n = None, 0
    for src, ovl in units:
        sp = Path(src).as_posix().split("/")
        if sp[-1] != stem:
            continue
        n = 1
        while n < len(sp) and n < len(parts) and sp[-1 - n] == parts[-1 - n]:
            n += 1
        if n > best_n:
            best, best_n = (src, ovl), n
    if best is None:
        sys.exit(f"port_gen: {obj}: no unit of the units file was compiled into it")
    return best


def read_objects(path):
    return [ln.strip() for ln in Path(path).read_text().replace(";", "\n").splitlines() if ln.strip()]


# ---------------------------------------------------------------------------------------------------------- overrides

def include_guard(path):
    m = re.search(r"^\s*#\s*ifndef\s+(\w+)", path.read_text(), flags=re.M)
    return m.group(1) if m else None


# The generated gtemac.h's prelude: the GTE's entry points (port/psyq/gte.c; psyq_internal.h declares them for the
# shim) and the memory accesses of the macros' loads and stores (byte copies: the host is little-endian like the PS1,
# and a struct the game passes may be only halfword-aligned, as gte_ldv0_u's SVECTORs are). A swc2 goes through
# psyq_gte_swc2_ (psyq/gte_shadow.c): the store, and for the screen coordinates the sub-pixel shadow's record.
GTEMAC_PRELUDE = """#include "common.h"

void psyq_gte_mtc2(int reg, u32 v);
u32 psyq_gte_mfc2(int reg);
void psyq_gte_ctc2(int reg, u32 v);
u32 psyq_gte_cfc2(int reg);
void psyq_gte_cmd(u32 op);
void psyq_gte_swc2_(void *p, int reg);

static inline u32 psyq_gte_lw_(const void *p) {
    u32 v;

    __builtin_memcpy(&v, p, 4);
    return v;
}

static inline u32 psyq_gte_lhu_(const void *p) {
    u16 v;

    __builtin_memcpy(&v, p, 2);
    return v;
}

static inline void psyq_gte_sw_(void *p, u32 v) {
    __builtin_memcpy(p, &v, 4);
}

static inline void psyq_gte_sh_(void *p, u32 v) {
    u16 h = (u16)v;

    __builtin_memcpy(p, &h, 2);
}
"""


def split_top(text, sep):
    """Splits at `sep` outside quotes and parentheses."""
    parts, depth, quote, cur = [], 0, False, ""
    for ch in text:
        if ch == '"':
            quote = not quote
        elif not quote and ch == "(":
            depth += 1
        elif not quote and ch == ")":
            depth -= 1
        if ch == sep and depth == 0 and not quote:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return parts


def gte_macro_c(name, params, body):
    """One gte_* macro of include/psyq/gtemac.h for the host: an inline-asm macro becomes the same sequence of GTE
    accesses as calls (port/psyq/gte.c), with the same registers, operands and command words; a composite macro (a
    block of other macros) is kept as it is. Raises ValueError on anything it does not know, so a new macro in
    gtemac.h is translated or fails the configure."""
    body = body.strip()
    if body.startswith("{"):
        return f"#define {name}{params} " + re.sub(r"\s+", " ", body)
    m = re.fullmatch(r"__asm__\s+volatile\s*\((.*)\)", body, flags=re.S)
    if not m:
        raise ValueError(f"{name}: neither an asm statement nor a block")
    sections = split_top(m.group(1), ":")
    asm = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', sections[0]))
    if len(sections) > 1 and sections[1].strip():
        raise ValueError(f"{name}: output operands are not supported")
    operands = []
    if len(sections) > 2 and sections[2].strip():
        for op in split_top(sections[2], ","):
            om = re.fullmatch(r'\s*"r"\s*\((.*)\)\s*', op, flags=re.S)
            if not om:
                raise ValueError(f"{name}: operand {op.strip()!r} is not \"r\"(...)")
            operands.append(om.group(1).strip())
    insns = [i.strip() for i in asm.split(";") if i.strip()]
    pointers, values, temps, out = set(), set(), set(), []

    def gpr(tok):
        if tok.startswith("%"):
            values.add(int(tok[1:]))
            return f"gte_a{tok[1:]}_"
        rm = re.fullmatch(r"\$(\d+)", tok)
        if not rm:
            raise ValueError(f"{name}: register {tok!r}")
        temps.add(int(rm.group(1)))
        return f"gte_r{rm.group(1)}_"

    def cop2(tok):
        rm = re.fullmatch(r"\$(\d+)", tok)
        if not rm or int(rm.group(1)) > 31:
            raise ValueError(f"{name}: GTE register {tok!r}")
        return rm.group(1)

    def mem(tok, adjust=0):
        mm = re.fullmatch(r"(-?\d+)\(%(\d+)\)", tok)
        if not mm:
            raise ValueError(f"{name}: memory operand {tok!r}")
        pointers.add(int(mm.group(2)))
        return f"gte_p{mm.group(2)}_ + {int(mm.group(1)) + adjust}"

    i = 0
    while i < len(insns):
        parts = insns[i].replace(",", " ").split()
        op, a = parts[0], parts[1:]
        if op == "nop":
            pass
        elif op == ".word":
            word = int(a[0], 0)
            if word >> 25 != 0x25:
                raise ValueError(f"{name}: .word {a[0]} is not a GTE command")
            out.append(f"psyq_gte_cmd({word & 0x1FFFFFF:#x});")
        elif op in ("lw", "lhu"):
            out.append(f"{gpr(a[0])} = psyq_gte_{op}_({mem(a[1])});")
        elif op == "lwl":
            # lwl $r, n+3(p); lwr $r, n(p): an unaligned word load at n.
            nxt = insns[i + 1].replace(",", " ").split() if i + 1 < len(insns) else []
            if nxt[:2] != ["lwr", a[0]]:
                raise ValueError(f"{name}: lwl without its lwr")
            out.append(f"{gpr(a[0])} = psyq_gte_lw_({mem(nxt[2])});")
            if mem(a[1], -3) != mem(nxt[2]):
                raise ValueError(f"{name}: lwl/lwr offsets")
            i += 1
        elif op in ("sw", "sh"):
            out.append(f"psyq_gte_{op}_({mem(a[1])}, {gpr(a[0])});")
        elif op == "sll":
            out.append(f"{gpr(a[0])} = {gpr(a[1])} << {int(a[2])};")
        elif op == "sra":
            out.append(f"{gpr(a[0])} = (u32)((s32){gpr(a[1])} >> {int(a[2])});")
        elif op == "or":
            out.append(f"{gpr(a[0])} = {gpr(a[1])} | {gpr(a[2])};")
        elif op in ("mtc2", "ctc2"):
            out.append(f"psyq_gte_{op}({cop2(a[1])}, {gpr(a[0])});")
        elif op in ("mfc2", "cfc2"):
            out.append(f"{gpr(a[0])} = psyq_gte_{op}({cop2(a[1])});")
        elif op == "lwc2":
            out.append(f"psyq_gte_mtc2({cop2(a[0])}, psyq_gte_lw_({mem(a[1])}));")
        elif op == "swc2":
            out.append(f"psyq_gte_swc2_({mem(a[1])}, {cop2(a[0])});")
        else:
            raise ValueError(f"{name}: instruction {insns[i]!r}")
        i += 1
    if pointers & values:
        raise ValueError(f"{name}: an operand used both as an address and as a value")
    decls = [f"u8 *gte_p{n}_ = (u8 *)({operands[n]});" for n in sorted(pointers)]
    decls += [f"u32 gte_a{n}_ = (u32)({operands[n]});" for n in sorted(values)]
    if temps:
        decls.append("u32 " + ", ".join(f"gte_r{t}_" for t in sorted(temps)) + ";")
    stmts = " ".join(decls + out)
    return f"#define {name}{params} do {{ {stmts} }} while (0)"


def cmd_overrides(args):
    """The override headers (first on the include path): INCLUDE_ASM/INCLUDE_RODATA empty, and with --gtemac every
    gte_* macro of the game's gtemac.h translated for the host: its MIPS sequence becomes the same register accesses
    and commands on the software GTE (psyq/gte.c: psyq_gte_mtc2/mfc2/ctc2/cfc2/cmd). They carry the real headers'
    include guards, so the real ones are skipped wherever they are included from."""
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    guard = args.include_asm_guard or "INCLUDE_ASM_H"
    write(out / "include_asm.h", f"/* Generated by tools/port_gen.py overrides; do not edit. */\n#ifndef {guard}\n"
          f"#define {guard}\n#define INCLUDE_ASM(FOLDER, NAME)\n#define INCLUDE_RODATA(FOLDER, NAME)\n#endif\n")
    n = 0
    if args.gtemac:
        real = Path(args.gtemac)
        text = re.sub(r"/\*.*?\*/", "", real.read_text(), flags=re.S)
        text = re.sub(r"\\\n", " ", text)
        macros = re.findall(r"^[ \t]*#[ \t]*define[ \t]+(gte_\w+)(\([^)]*\))(.*)$", text, flags=re.M)
        body = "".join(gte_macro_c(nm, a, b) + "\n" for nm, a, b in macros)
        g = include_guard(real) or "PSYQ_GTEMAC_H"
        rel = args.gtemac_include or "psyq/gtemac.h"
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        write(out / rel, f"/* Generated by tools/port_gen.py overrides from {real.name}; do not edit.\n"
              f" * Every GTE macro runs on the software GTE (psyq/gte.c) with the original's registers and commands. */\n"
              f"#ifndef {g}\n#define {g}\n{GTEMAC_PRELUDE}\n{body}#endif\n")
        n = len(macros)
    print(f"port_gen overrides: include_asm.h"
          + (f", {args.gtemac_include or 'psyq/gtemac.h'} ({n} gte_* macros on the software GTE)" if n else "")
          + f" -> {out}")


# ----------------------------------------------------------------------------------------------------------- sections
# The per-overlay data sections (docs/PORT.md "Overlays"): every unit's writable sections are renamed, right after its
# compile (`rename`, the units' compile launcher), into its overlay's, so that the linker collects them per overlay
# without a script, bracketed by __start_<id>_{data,bss}_<ovl> / __stop_... for the overlay manager's snapshot
# (runtime/overlay.c; the EXE's units are the overlay "main").
#   ELF: an orphan output section whose name is a C identifier (<id>_data_<ovl>, <id>_bss_<ovl>) gets GNU ld's
#        __start_/__stop_ symbols by itself, and lands after .data/.bss, outside GNU_RELRO (a PIE link is fine).
#   PE:  one .psxdata and one .psxbss output section (eight characters: a PE image section name's limit); lld sorts
#        their chunks by the `$` suffix, so the generated marker objects (`markers`: .psxdata$<ovl>_0 and _2, empty
#        labelled chunks carrying the __start_/__stop_ symbols) bracket the units' renamed chunks (.psxdata$<ovl>_1).
#        GNU objcopy reads COFF (llvm-objcopy cannot rename COFF sections).
# .data.rel.ro* (const data with relocations, PIE) is left alone: read-only after relocation, not game-writable.
RENAMED_SECTION = re.compile(r"^\.(data|bss)($|[.$].*)")
RELRO_SECTION = re.compile(r"^\.data\.rel\.ro($|\..*)")
WRITABLE_SECTION = re.compile(r"^(\.(data|bss|sdata|sbss|tdata|tbss)($|[.$].*)|COMMON)$")
PE_PREFIX = ".psx"


def section_name(name):
    return name.lower()


def game_section(game_id, name, kind, fmt):
    """The output section (ELF) or chunk group (PE) of an overlay's writable data; kind 'data' or 'bss'."""
    s = section_name(name)
    return f"{game_id}_{kind}_{s}" if fmt == "elf" else f"{PE_PREFIX}{kind}${s}_1"


def bracket_symbol(game_id, name, kind, which):
    return f"__{which}_{game_id}_{kind}_{section_name(name)}"


def is_game_section(game_id, name):
    return name.startswith(f"{game_id}_data_") or name.startswith(f"{game_id}_bss_") or name.startswith(PE_PREFIX)


def object_sections(objdump, obj):
    """('elf' | 'pe', [(section name, size, flags)]) of an object, from `objdump -h` (GNU binutils, which reads both)."""
    r = subprocess.run([objdump, "-h", obj], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"port_gen: {objdump} -h {obj}: {r.stderr.strip()}")
    fmt, rows, name, size = None, [], None, 0
    for ln in r.stdout.splitlines():
        m = re.search(r"file format (\S+)", ln)
        if m:
            fmt = "pe" if "pe" in m.group(1) else "elf"
            continue
        p = ln.split()
        if len(p) >= 3 and p[0].isdigit() and not ln.startswith(" " * 10):
            name, size = p[1], int(p[2], 16)
        elif name is not None and ln.startswith(" " * 10):
            rows.append((name, size, set(f.strip() for f in ln.split(","))))
            name = None
    if fmt is None:
        sys.exit(f"port_gen: {objdump} -h {obj}: no file format line")
    return fmt, rows


def cmd_rename(args):
    """The game units' compile launcher (C_COMPILER_LAUNCHER): the unit's writable sections (.data*, .bss*, but not
    .data.rel.ro*) end up in its overlay's (game_section), the overlay found from the source file in the units file.
    ELF (GCC): the compile, then GNU objcopy renames the object's sections. PE (--pe, clang): the compile itself puts
    them there, through a forced include of `#pragma clang section data=... bss=...` for the overlay; GNU objcopy is
    not used on COFF objects: it drops the IMAGE_SCN_LNK_COMDAT flag of every section it rewrites, after which lld
    discards the COMDAT .pdata$fn/.xdata$fn (the x64 unwind tables: a crash report's stack and any SEH unwinding stop
    at the first game frame). After the compile, objdump -h checks that no writable section stayed outside."""
    cmd = args.command[1:] if args.command[:1] == ["--"] else args.command
    units = {src: ovl for src, ovl in read_units(args.units)}
    src = next((a for a in cmd if os.path.normpath(os.path.abspath(a)) in units), None)
    if src is None:
        sys.exit(f"port_gen rename: no unit of {args.units} in the compile command: {' '.join(cmd)}")
    ovl = units[os.path.normpath(os.path.abspath(src))]
    if args.pe:
        hdr = Path(args.units).parent / "sections" / f"{section_name(ovl)}.h"
        text = ("/* Generated by tools/port_gen.py rename; do not edit: this unit's .data/.bss in its overlay's groups. */\n"
                f'#pragma clang section data="{game_section(args.id, ovl, "data", "pe")}" '
                f'bss="{game_section(args.id, ovl, "bss", "pe")}"\n')
        if not hdr.exists() or hdr.read_text() != text:
            hdr.parent.mkdir(parents=True, exist_ok=True)
            tmp = hdr.with_suffix(f".{os.getpid()}.tmp")
            tmp.write_text(text)
            os.replace(tmp, hdr)  # parallel compiles of the overlay's units: each writes the same bytes, atomically
        cmd = [cmd[0], "-include", str(hdr), *cmd[1:]]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(r.returncode)
    if "-o" not in cmd:
        return  # not an object compile (a preprocess only, ...): nothing to rename
    out = cmd[cmd.index("-o") + 1]
    fmt, rows = object_sections(args.objdump, out)
    if args.pe:
        left = [name for name, size, _ in rows if size > 0 and RENAMED_SECTION.match(name)]
        if left:
            sys.exit(f"port_gen rename: {out}: writable sections the overlay's pragma did not cover: {', '.join(left)}")
        return
    renames = []
    for name, _, _ in rows:
        if RENAMED_SECTION.match(name) and not RELRO_SECTION.match(name):
            kind = "bss" if name.startswith(".bss") else "data"
            renames += ["--rename-section", f"{name}={game_section(args.id, ovl, kind, fmt)}"]
    if renames:
        subprocess.run([args.objcopy, *renames, out], check=True)


def cmd_markers(args):
    """PE only: a C file whose file-scope asm defines the bracket symbols of the EXE and every overlay as empty labelled
    chunks in the groups .psxdata$<ovl>_0 / _2 and .psxbss$<ovl>_0 / _2 (ELF needs none: GNU ld makes the symbols)."""
    names = ["MAIN"] + [o["name"] for o in read_overlays(args.overlays)]
    lines = []
    for name in names:
        s = section_name(name)
        for kind, flags in (("data", "dw"), ("bss", "bw")):
            for n, which in ((0, "start"), (2, "stop")):
                sym = bracket_symbol(args.id, name, kind, which)
                lines.append(f'    ".section {PE_PREFIX}{kind}${s}_{n},\\"{flags}\\"\\n"\n'
                             f'    ".globl {sym}\\n{sym}:\\n"')
    text = ["/* Generated by tools/port_gen.py markers; do not edit. The __start_/__stop_ symbols of the EXE's and each",
            " * overlay's .data/.bss on PE: empty chunks that lld's `$` sorting puts around the units' renamed sections",
            f" * ({PE_PREFIX}data$<ovl>_1, {PE_PREFIX}bss$<ovl>_1; port_gen.py rename). docs/PORT.md explains. */",
            "__asm__(", "\n".join(lines), ");", ""]
    write(args.out, "\n".join(text))
    print(f"port_gen markers: the EXE and {len(names) - 1} overlays -> {args.out}")


def cmd_sections(args):
    """Checks the game's objects after the link: every writable section of a game object (.data*, .bss*, COMMON, ...)
    must be a renamed one (`rename`), i.e. inside the ranges the overlay manager snapshots and the console's reset
    restores (runtime/overlay.c). A section left out would keep its value across a reset: a game global the reset
    misses. .data.rel.ro* (PIE: const after relocation) is allowed as is. Lists, with -v, the other writable sections
    (asan_globals, .init_array: the runtime's, not game data)."""
    objects = read_objects(args.objects)
    bad, game, other = [], {}, {}
    for obj in objects:
        _, rows = object_sections(args.objdump, obj)
        for name, size, flags in rows:
            if size == 0 or "ALLOC" not in flags or "READONLY" in flags or "CODE" in flags:
                continue
            if is_game_section(args.id, name):
                key = name.split("$")[0] if name.startswith(PE_PREFIX) else name
                game[key] = game.get(key, 0) + size
            elif WRITABLE_SECTION.match(name) and not RELRO_SECTION.match(name):
                bad.append(f"{obj}: {name} (0x{size:X} bytes)")
            else:
                other[name] = other.get(name, 0) + size
    md, mb = f"{args.id}_data_main", f"{args.id}_bss_main"
    main_data = game.get(md, 0) + game.get(f"{PE_PREFIX}data", 0)
    main_bss = game.get(mb, 0) + game.get(f"{PE_PREFIX}bss", 0)
    print(f"port_gen sections: {sum(game.values())} bytes of the game's writable data in {len(game)} non-empty "
          f"overlay sections ({PE_PREFIX + 'data' if f'{PE_PREFIX}data' in game else 'main .data'} {main_data}, "
          f"{PE_PREFIX + 'bss' if f'{PE_PREFIX}bss' in game else 'main .bss'} {main_bss}); {len(bad)} outside")
    if args.verbose:
        for k in sorted(other):
            print(f"  not game data: {k}: {other[k]} bytes")
    for b in bad:
        print(f"  OUTSIDE {b}")
    if bad:
        sys.exit(f"port_gen sections: {len(bad)} writable game section(s) not renamed into an overlay's: "
                 f"tools/port_gen.py rename must cover them")


# ------------------------------------------------------------------------------------------------------------- tables

def nm_globals(nm, objects):
    """{object path: set of global function (T) symbols}."""
    out = {}
    for obj in objects:
        r = subprocess.run([nm, "--defined-only", "-g", obj], capture_output=True, text=True, check=True)
        out[obj] = {ln.split()[2] for ln in r.stdout.splitlines() if len(ln.split()) == 3 and ln.split()[1] in "TtWw"}
    return out


def symbol_funcs(path):
    """{address: symbol} of the `type:func` lines of an overlay's symbol file."""
    funcs = {}
    if path and Path(path).exists():
        for ln in Path(path).read_text().splitlines():
            m = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*//(.*)", ln)
            if m and "type:func" in m.group(3):
                funcs.setdefault(int(m.group(2), 16), m.group(1))
    return funcs


def generic_tag_sites(units):
    """SLOT_FUNC(type, 0x...) and LATE_FUNC(tier, 0x...) sites in the units' sources: the overlay is unknown (a
    SLOT_FUNC is for whatever the slot holds; LATE_FUNC names its tier), so each is checked against every table of its
    tier. A game's tag-sites file adds the sites whose overlay it knows (GAME_CONTRACT.md)."""
    sites = []
    for src, _ in units:
        p = Path(src)
        if not p.exists():
            continue
        text = p.read_text(errors="replace")
        for m in re.finditer(r"SLOT_FUNC\([^,]+,\s*0x([0-9A-Fa-f]+)\)", text):
            sites.append((f"{src}:{text.count(chr(10), 0, m.start()) + 1}", None, None, int(m.group(1), 16)))
        for m in re.finditer(r"LATE_FUNC\(\s*(\d+),\s*0x([0-9A-Fa-f]+)", text):
            sites.append((f"{src}:{text.count(chr(10), 0, m.start()) + 1}", int(m.group(1)), None, int(m.group(2), 16)))
    return sites


def cmd_tables(args):
    units = read_units(args.units)
    overlays = read_overlays(args.overlays)
    objects = read_objects(args.objects)
    globals_of = nm_globals(args.nm, objects)
    by_overlay = {}
    for obj, syms in globals_of.items():
        by_overlay.setdefault(object_unit(obj, units)[1], set()).update(syms)
    stats = {"funcs": {}, "skipped_static_or_asm": 0, "unnamed_global": 0}
    tables, skipped = [], []
    for o in overlays:
        funcs = symbol_funcs(o["symbols"])
        host = by_overlay.get(o["name"], set())
        rows = sorted((a, s) for a, s in funcs.items() if s in host)
        for a, s in sorted(funcs.items()):
            if s not in host:
                stats["skipped_static_or_asm"] += 1
                skipped.append((o["name"], s, a))
        stats["unnamed_global"] += len(host - set(funcs.values()) - {"main"})
        stats["funcs"][o["tier"]] = stats["funcs"].get(o["tier"], 0) + len(rows)
        tables.append((o, rows))
    # every tag site resolves?
    defined = {o["name"]: {a for a, _ in rows} for o, rows in tables}
    by_file = {o["file"]: o["name"] for o in overlays}
    by_tier = {}
    for o, rows in tables:
        by_tier.setdefault(o["tier"], set()).update(a for a, _ in rows)
    sites = generic_tag_sites(units) + (read_tag_sites(args.tag_sites) if args.tag_sites else [])
    bad, checked, loose = [], 0, 0
    for where, tier, ovl, addr in sites:
        if isinstance(ovl, tuple):
            ovl = by_file.get(ovl[1])
        if ovl is not None:
            checked += 1
            if addr not in defined.get(ovl, set()):
                bad.append(f"{where}: 0x{addr:08X} not a host function of {ovl}")
        else:
            loose += 1
            pool = by_tier.get(tier, set()) if tier is not None else set().union(*by_tier.values()) if by_tier else set()
            if addr not in pool:
                bad.append(f"{where}: 0x{addr:08X} (tier {tier if tier is not None else '?'}, overlay unknown) is in "
                           f"no table of that tier")
    out = ["/* Generated by tools/port_gen.py tables from the overlays' symbol files and nm of the game's objects;",
           " * do not edit. The only place that maps a PS1 address to a host function (SLOT_FUNC). */",
           "#include <stddef.h>", "#include \"port_runtime.h\"", ""]
    for _, rows in tables:
        out += [f"void {s}(void);" for _, s in rows]  # the real prototypes are in the game's headers; any one will do
    out.append("")
    for o, rows in tables:
        out.append(f"static const PortOverlayFunc port_funcs_{section_name(o['name'])}[] = {{")
        out += [f"    {{ 0x{a:08X}u, (PortFn){s} }}," for a, s in rows]
        out += ["    { 0, NULL },", "};"]
    out.append("")
    for o, _ in tables:
        syms = [bracket_symbol(args.id, o["name"], k, w) for k in ("data", "bss") for w in ("start", "stop")]
        out.append("extern char " + ", ".join(f"{s}[]" for s in syms) + ";")
    out += ["", "const PortOverlay port_overlays[] = {"]
    for o, rows in tables:
        b = [bracket_symbol(args.id, o["name"], k, w) for k in ("data", "bss") for w in ("start", "stop")]
        out.append(f"    {{ {o['tier']}, 0x{o['file']:X}, \"{o['name']}\", port_funcs_{section_name(o['name'])}, "
                   f"{len(rows)}, {b[0]}, {b[1]}, {b[2]}, {b[3]} }},")
    out += ["};", f"const int port_overlay_count = {len(tables)};", ""]
    write(args.out, "\n".join(out))
    per_tier = ", ".join(f"tier {t} {n}" for t, n in sorted(stats["funcs"].items()))
    print(f"port_gen tables: {len(tables)} overlays; host functions: {per_tier or 'none'}; "
          f"skipped {stats['skipped_static_or_asm']} symbol-file functions with no global symbol on the host (static, "
          f"or still INCLUDE_ASM); {stats['unnamed_global']} global functions without a symbol-file address (not in "
          f"any table); tag sites: {checked} checked against their overlay, {loose} against every table of their tier")
    if args.report:
        for name, s, a in skipped:
            print(f"  skipped {name} {s} 0x{a:08X}")
    for b in bad:
        print(f"  UNRESOLVED {b}")
    if bad:
        sys.exit(f"port_gen tables: {len(bad)} tag site(s) do not resolve")


# -------------------------------------------------------------------------------------------------------------- state

def exe_symbols(path):
    """-> (funcs {name: address}, data {name: (address, PS1 size)}) from the EXE's symbol file: the `type:func`
    lines, and the other lines that carry a `size:`."""
    funcs, data = {}, {}
    for ln in Path(path).read_text().splitlines():
        m = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*//(.*)", ln)
        if not m:
            continue
        name, addr, attrs = m.group(1), int(m.group(2), 16), m.group(3)
        if "type:func" in attrs:
            funcs.setdefault(name, addr)
        else:
            s = re.search(r"\bsize:(0x[0-9A-Fa-f]+|\d+)", attrs)
            if s:
                data.setdefault(name, (addr, int(s.group(1), 0)))
    return funcs, data


def volatile_ranges(path):
    """[(lo, hi)] of the volatile file: `lo hi` per line (hex or decimal)."""
    out = []
    for row in read_table(path):
        p = " ".join(row).split()
        if len(p) != 2:
            sys.exit(f"port_gen state: {path}: a line is <lo> <hi>: {row}")
        lo, hi = int(p[0], 0), int(p[1], 0)
        if not 0 <= lo < hi:
            sys.exit(f"port_gen state: {path}: not a [lo, hi) range: {row}")
        out.append((lo, hi))
    return out


def nm_defined(nm, objects):
    """{object: {symbol: type letter}} of the defined global symbols. Names and types only: no sizes (`nm -S`), which
    COFF objects do not carry; the sizes come from probe_sizes."""
    out = {}
    for obj in objects:
        r = subprocess.run([nm, "--defined-only", "-g", obj], capture_output=True, text=True, check=True)
        syms = {}
        for ln in r.stdout.splitlines():
            p = ln.split()
            if len(p) == 3:
                syms[p[2]] = p[1]
        out[obj] = syms
    return out


def probe_sizes(cc, cflags, includes, defines, by_unit):
    """{symbol: sizeof} under `cc cflags`: compiles each unit (to assembly only) followed by a function whose asm
    statements print `# PSXSIZE <sym> <sizeof>` as comments (an "i" operand with the %c modifier: the number alone,
    on GCC and clang for any target), and reads them back. No object format is involved: COFF carries no symbol sizes
    for `nm -S`, and the assembly's data directives differ per target; a comment is the same everywhere.
    cmd_state calls it twice: at -m64 (the pointer test of the layout-identical rule) and with the build's own flags
    (this build's sizes); measuring both the same way keeps the -m32 build's table equal to the -m64 build's."""
    from concurrent.futures import ThreadPoolExecutor
    ver = subprocess.run([cc, "-dumpversion"], capture_output=True, text=True).stdout.strip()
    flags = [cc, *cflags, "-S", "-o", "-", "-w", "-x", "c", "-std=gnu99", "-DPC_PORT", *[f"-D{d}" for d in defines],
             "-fsigned-char", "-fno-builtin", "-fno-common", *[f"-I{i}" for i in includes]]
    is_gcc = "clang" not in subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.lower()
    if is_gcc and ver.split(".")[0].isdigit() and int(ver.split(".")[0]) >= 14:
        flags.append("-fpermissive")

    def one(item):
        src, syms = item
        text = (f'#include "{src}"\nvoid psx_size_probe__(void) {{\n'
                + "".join(f'    __asm__ volatile("# PSXSIZE {s} %c0" : : "i"((long)sizeof({s})));\n' for s in sorted(syms))
                + "}\n")
        r = subprocess.run(flags + ["-"], input=text, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"port_gen state: the size probe ({' '.join(cflags)}) of {src} failed:\n{r.stderr[-2000:]}")
        found = {m.group(1): int(m.group(2)) for m in re.finditer(r"# PSXSIZE (\w+) (\d+)", r.stdout)}
        missing = sorted(syms - found.keys())
        if missing:
            sys.exit(f"port_gen state: the size probe of {src} printed no size for {', '.join(missing)}")
        return found

    sizes = {}
    with ThreadPoolExecutor(max_workers=2) as ex:
        for s in ex.map(one, sorted(by_unit.items())):
            sizes.update(s)
    return sizes


def cmd_state(args):
    """port_state_tables.c for the game's state probes and the runtime's checkpoint hash: the EXE's functions ({PS1
    address, host function}), the EXE's sized data symbols ({PS1 address, PS1 size, host object, the length of the
    layout-identical prefix by the default rule}: game_state_read), and the volatile ranges (the stable hash).
    The default rule: a whole object is layout-identical when its -m64 sizeof equals its PS1 size (any pointer, or
    long, makes it larger at -m64); its prefix is 0 otherwise (the game's adapter lists the pointer-bearing objects'
    identical prefixes). Only symbols that nm shows as globals of the EXE's objects are listed; their sizes come from
    compiling the defining units (probe_sizes), not from the objects."""
    units = read_units(args.units)
    objects = [o for o in read_objects(args.objects) if object_unit(o, units)[1] == "MAIN"]
    defined = nm_defined(args.nm, objects)
    funcs, data = exe_symbols(args.exe_symbols)
    host_funcs = {s for syms in defined.values() for s, t in syms.items() if t in "TW"}
    frows = sorted((a, s) for s, a in funcs.items() if s in host_funcs)
    by_unit = {}
    for obj, syms in defined.items():
        src = object_unit(obj, units)[0]
        for s, t in syms.items():
            if s in data and t in "DdBbRrGgSsVv":
                by_unit.setdefault(src, set()).add(s)
    cflags = [f for f in (args.cflags or "").split() if f]
    has_m64 = subprocess.run([args.cc, "-m64", "-x", "c", "-c", "/dev/null", "-o", "/dev/null"],
                             capture_output=True).returncode == 0
    if has_m64:
        m64 = probe_sizes(args.cc, ["-m64"], args.include, args.define, by_unit)
    else:
        m64 = None
    owner = m64 if (cflags == ["-m64"] or not cflags) and m64 is not None else probe_sizes(args.cc, cflags, args.include, args.define, by_unit)
    if m64 is None:
        m64 = owner
    drows, bad = [], []
    for s in sorted(owner, key=lambda s: data[s][0]):
        addr, ps1 = data[s]
        identical = m64.get(s) == ps1
        if identical and owner[s] != ps1:
            bad.append(f"{s}: -m64 size 0x{m64[s]:X} is the PS1 size but this build's is 0x{owner[s]:X}")
        drows.append((addr, ps1, s, ps1 if identical else 0))
    if bad:
        sys.exit("port_gen state: layout differs between -m32 and -m64 for a pointer-free object:\n  " + "\n  ".join(bad))
    vol = volatile_ranges(args.volatile) if args.volatile else []
    out = ["/* Generated by tools/port_gen.py state from the EXE's symbol file, nm of the EXE's objects and the",
           " * volatile ranges; do not edit. The game's state probes (psxstack/game.h) use it. */",
           "#include <stddef.h>", "#include \"psxstack/game.h\"", ""]
    out += [f"void {s}(void);" for _, s in frows]
    out += [f"extern char {s}[];" for _, _, s, _ in drows]
    out += ["", "const PortExeFunc port_exe_funcs[] = {"]
    out += [f"    {{ 0x{a:08X}u, (PortFn){s} }}," for a, s in frows]
    out += ["    { 0u, NULL },", "};", f"const int port_exe_func_count = {len(frows)};", "",
            "const PortExeData port_exe_data[] = {"]
    out += [f"    {{ 0x{a:08X}u, 0x{n:X}, \"{s}\", {s}, 0x{p:X} }}," for a, n, s, p in drows]
    out += ["    { 0u, 0u, NULL, NULL, 0u },", "};", f"const int port_exe_data_count = {len(drows)};", "",
            "/* [lo, hi) byte ranges of the game-state image the stable hash zeroes */",
            "const PortRange port_gamestate_volatile[] = {"]
    out += [f"    {{ 0x{lo:X}, 0x{hi:X} }}," for lo, hi in vol]
    out += ["    { 0u, 0u },", "};", f"const int port_gamestate_volatile_count = {len(vol)};", ""]
    write(args.out, "\n".join(out))
    print(f"port_gen state: {len(frows)} EXE functions, {len(drows)} sized EXE data symbols "
          f"({sum(1 for r in drows if r[3])} layout-identical by size), {len(vol)} volatile ranges -> {args.out}")


# --------------------------------------------------------------------------------------------------------------- main

def write(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("overrides", help="include_asm.h and (with --gtemac) the game's GTE header for the host build")
    p.add_argument("--out", required=True, help="directory")
    p.add_argument("--gtemac", help="the game's gtemac.h to translate onto the software GTE")
    p.add_argument("--gtemac-include", help="the path the game includes it by, relative to its include directory "
                   "(the override is written there; default psyq/gtemac.h)")
    p.add_argument("--include-asm-guard", help="the include guard of the game's include_asm.h (default INCLUDE_ASM_H)")
    p.set_defaults(fn=cmd_overrides)
    p = sub.add_parser("rename", help="the units' compile launcher: compile, then rename .data/.bss into the overlay's")
    p.add_argument("--id", required=True, help="the game's id (the section prefix)")
    p.add_argument("--objcopy", default=os.environ.get("OBJCOPY", "objcopy"), help="GNU objcopy (ELF: the rename)")
    p.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"), help="GNU objdump (reads ELF and COFF)")
    p.add_argument("--units", required=True, help="units.txt")
    p.add_argument("--pe", action="store_true",
                   help="a COFF compile (clang): the overlay's `#pragma clang section` forced in, no objcopy")
    p.add_argument("command", nargs=argparse.REMAINDER, help="-- the compile command")
    p.set_defaults(fn=cmd_rename)
    p = sub.add_parser("markers", help="PE: the __start_/__stop_ bracket symbols of every overlay's sections (C file)")
    p.add_argument("--id", required=True)
    p.add_argument("--overlays", required=True, help="overlays.txt")
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_markers)
    p = sub.add_parser("sections", help="check the objects: every game object's writable data in its overlay's section")
    p.add_argument("--id", required=True)
    p.add_argument("--objdump", default=os.environ.get("OBJDUMP", "objdump"), help="GNU objdump")
    p.add_argument("--objects", required=True, help="a file listing the game's objects (one per line or ;-separated)")
    p.add_argument("-v", "--verbose", action="store_true", help="also list the other writable sections")
    p.set_defaults(fn=cmd_sections)
    p = sub.add_parser("tables", help="the overlay address tables (needs the compiled objects)")
    p.add_argument("--id", required=True)
    p.add_argument("--units", required=True)
    p.add_argument("--overlays", required=True)
    p.add_argument("--tag-sites", help="the game's tag sites whose overlay is known (optional)")
    p.add_argument("--nm", default=os.environ.get("NM", "nm"))
    p.add_argument("--objects", required=True, help="a file listing the game's objects (one per line or ;-separated)")
    p.add_argument("--out", required=True)
    p.add_argument("--report", action="store_true", help="list every skipped function")
    p.set_defaults(fn=cmd_tables)
    p = sub.add_parser("state", help="the EXE's function and data tables for the game-state probes (needs the objects)")
    p.add_argument("--units", required=True)
    p.add_argument("--exe-symbols", required=True, help="the EXE's symbol file (type:func and size: lines)")
    p.add_argument("--volatile", help="the volatile ranges file (optional)")
    p.add_argument("--nm", default=os.environ.get("NM", "nm"))
    p.add_argument("--cc", default=os.environ.get("CC", "cc"), help="the C compiler (the size probes)")
    p.add_argument("--cflags", default="", help="this build's width flags for the size probe (-m32; default: -m64)")
    p.add_argument("-I", dest="include", action="append", default=[], help="an include directory (repeatable)")
    p.add_argument("-D", dest="define", action="append", default=[], help="a define (repeatable)")
    p.add_argument("--objects", required=True, help="a file listing the game's objects (one per line or ;-separated)")
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_state)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
