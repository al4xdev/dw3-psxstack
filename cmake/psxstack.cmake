# psxstack_add_game(): a PC port from a game's C (GAME_CONTRACT.md "5. The build inputs"; docs/PORT.md). Include this
# file (or add_subdirectory() the stack) and call it once:
#
#   psxstack_add_game(<target>                      # the executable: build/<...>/<target>
#       GAME_JSON <game.json>                       # the description (schema/game.schema.json)
#       UNITS <units.txt>                           # <absolute source>\t<OVERLAY> per C unit; MAIN = the executable's
#       MAIN_UNIT <file>                            # the unit whose main() becomes game_main
#       [OVERLAYS <overlays.txt>]                   # <NAME>\t<tier>\t<file id>\t<symbols file or -> per overlay
#       [TAG_SITES <tag_sites.txt>]                 # the game's tag sites whose overlay is known
#       [EXE_SYMBOLS <symbol_addrs.txt>]            # the EXE's functions and sized data: the state tables
#       [VOLATILE <volatile.txt>]                   # the stable hash's zeroed ranges
#       [GTEMAC <gtemac.h>] [INCLUDE_ASM_GUARD <X>] # the game's GTE macros to translate (the override is written at the
#                                                   #   header's path relative to the game's include dir); its
#                                                   #   include_asm.h's guard. A game may pass no GTEMAC and put a host
#                                                   #   GTE header of its own first in INCLUDE_DIRS
#       INCLUDE_DIRS <dir>...                       # the game's include/ (its common.h, port.h, psyq/*.h) and root
#       [ADAPTER <source>...]                       # the adapter units (psxstack/game.h)
#       [MODS_DIRS <dir>...]                        # directories of mods/<id>/mod.json, copied beside the binary
#       [DEFINES <X>...] [UNIT_COMPILE_OPTIONS <flag>...]
#       [CONFIGURE_DEPENDS <file>...]               # files whose change reruns the configure (the inputs' sources)
#       [RC <file.rc>])                             # Windows: the game's resource script (default: the stack's template)
#
# Options (cache): PSXSTACK_SDL (the window; needs tools/sdl3 and tools/dxc), PSXSTACK_SANITIZE, PSXSTACK_M32,
# PSXSTACK_PSYQ_WERROR, PSXSTACK_ALLOW_UNRESOLVED, PSXSTACK_UNIT_OVERRIDES, PSXSTACK_DXC, PSXSTACK_PYTHON,
# PSXSTACK_TOOLS_DIR, PSXSTACK_VERSION_ROOT (the repository `git describe` names the build after).
#
# What it builds: tools/game_gen.py -> gen/include/psxstack_game_gen.h (configure time); tools/port_gen.py overrides
# (the override headers); the units as an object library compiled through port_gen.py rename (each object's
# .data/.bss into its overlay's section, no linker script); the overlay address tables and the state tables from nm
# of the objects (build time); the runtime, the adapter, the shim, the tables and the units linked into <target>;
# after every link, port_gen.py sections checks that no game data escaped the overlay sections.
cmake_minimum_required(VERSION 3.20)

get_filename_component(PSXSTACK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

option(PSXSTACK_ALLOW_UNRESOLVED "Link with --unresolved-symbols=ignore-all (a build without the shim)" OFF)
option(PSXSTACK_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(PSXSTACK_M32 "Build a 32-bit binary (-m32, gcc-multilib): its --log must equal the 64-bit build's" OFF)
option(PSXSTACK_SDL "The window (runtime/video.c, input.c) over SDL3 (tools/sdl3: scripts/setup.sh sdl3 dxc)" OFF)
option(PSXSTACK_SDL2 "The window, input and audio over SDL2 and fbdev (R36S / embedded Linux)" OFF)
option(PSXSTACK_EGL "The GLES renderer (runtime/render_gles.c): offscreen EGL on a GBM device" OFF)
option(PSXSTACK_PSYQ_WERROR "-Werror for the shim too (the runtime always has it)" ON)
set(PSXSTACK_UNIT_OVERRIDES "" CACHE STRING
    "Experiments: units to build from another file, as <unit path>=<path> entries (the game's tree is untouched)")
set(PSXSTACK_PYTHON "" CACHE FILEPATH "The Python that runs the generators (default: python3 on PATH)")
set(PSXSTACK_TOOLS_DIR "" CACHE PATH "A tools/ directory holding sdl3/, dxc/, ... (else the stack's tools/)")
set(PSXSTACK_VERSION_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH "The repository `git describe` names the build after")
set(PSXSTACK_DXC "" CACHE FILEPATH "DXC for the hardware renderer's shaders (default: tools/dxc/bin/dxc)")

# ---- The built tools: PSXSTACK_TOOLS_DIR, the stack's tools/, the stack's main checkout's (a worktree).
set(PSXSTACK_TOOL_DIRS "")
if(PSXSTACK_TOOLS_DIR)
    list(APPEND PSXSTACK_TOOL_DIRS "${PSXSTACK_TOOLS_DIR}")
endif()
list(APPEND PSXSTACK_TOOL_DIRS "${PSXSTACK_ROOT}/tools")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PSXSTACK_ROOT}" rev-parse --path-format=absolute --git-common-dir
                    OUTPUT_VARIABLE _psx_common OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _r)
    if(_r EQUAL 0 AND _psx_common)
        get_filename_component(_psx_main "${_psx_common}" DIRECTORY)
        list(APPEND PSXSTACK_TOOL_DIRS "${_psx_main}/tools")
    endif()
endif()
list(REMOVE_DUPLICATES PSXSTACK_TOOL_DIRS)

if(PSXSTACK_PYTHON)
    set(_psx_python "${PSXSTACK_PYTHON}")
else()
    find_program(_psx_python NAMES python3 python REQUIRED)
endif()
set(PSXSTACK_PORT_GEN "${PSXSTACK_ROOT}/tools/port_gen.py")
set(PSXSTACK_GAME_GEN "${PSXSTACK_ROOT}/tools/game_gen.py")

function(_psxstack_run)
    execute_process(COMMAND "${_psx_python}" ${ARGN} RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT r EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed:\n${out}${err}")
    endif()
    string(STRIP "${out}" out)
    if(out)
        message(STATUS "${out}")
    endif()
endfunction()

function(psxstack_add_game target)
    cmake_parse_arguments(G "" "GAME_JSON;UNITS;MAIN_UNIT;OVERLAYS;TAG_SITES;EXE_SYMBOLS;VOLATILE;GTEMAC;INCLUDE_ASM_GUARD;RC"
                          "INCLUDE_DIRS;ADAPTER;MODS_DIRS;DEFINES;UNIT_COMPILE_OPTIONS;CONFIGURE_DEPENDS" ${ARGN})
    foreach(req GAME_JSON UNITS MAIN_UNIT)
        if(NOT G_${req})
            message(FATAL_ERROR "psxstack_add_game(${target}): ${req} is required")
        endif()
    endforeach()
    if(G_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "psxstack_add_game(${target}): unknown arguments: ${G_UNPARSED_ARGUMENTS}")
    endif()
    set(GEN "${CMAKE_BINARY_DIR}/gen")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${G_GAME_JSON}" "${G_UNITS}" "${PSXSTACK_GAME_GEN}"
                 "${PSXSTACK_PORT_GEN}" "${PSXSTACK_ROOT}/schema/game.schema.json" ${G_CONFIGURE_DEPENDS})
    foreach(f G_OVERLAYS G_TAG_SITES G_EXE_SYMBOLS G_VOLATILE G_GTEMAC)
        if(${f})
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${${f}}")
        endif()
    endforeach()

    # ---- The game's description -> the generated header and the CMake variables.
    _psxstack_run("${PSXSTACK_GAME_GEN}" "${G_GAME_JSON}" --out "${GEN}/include/psxstack_game_gen.h"
                  --cmake "${GEN}/psxstack_game.cmake")
    include("${GEN}/psxstack_game.cmake")   # PSXSTACK_GAME_ID, PSXSTACK_GAME_TITLE, PSXSTACK_GAME_ENV_PREFIX, PSXSTACK_GAME_RATE
    set(ID "${PSXSTACK_GAME_ID}")
    message(STATUS "Game: ${PSXSTACK_GAME_TITLE} (${ID}; ${G_GAME_JSON}) -> ${target}")

    # ---- The override headers.
    set(_ov overrides --out "${GEN}/include")
    if(G_GTEMAC)
        list(APPEND _ov --gtemac "${G_GTEMAC}")
        # The override goes where `#include "<path>"` finds it first: the header's path under the game's include dir.
        foreach(d IN LISTS G_INCLUDE_DIRS)
            file(RELATIVE_PATH _gte_rel "${d}" "${G_GTEMAC}")
            if(NOT _gte_rel MATCHES "^\\.\\." AND NOT IS_ABSOLUTE "${_gte_rel}")
                list(APPEND _ov --gtemac-include "${_gte_rel}")
                break()
            endif()
        endforeach()
    endif()
    if(G_INCLUDE_ASM_GUARD)
        list(APPEND _ov --include-asm-guard "${G_INCLUDE_ASM_GUARD}")
    endif()
    _psxstack_run("${PSXSTACK_PORT_GEN}" ${_ov})

    # ---- The units (units.txt), with PSXSTACK_UNIT_OVERRIDES applied; the compile launcher reads the same file.
    file(STRINGS "${G_UNITS}" _lines)
    set(UNITS "")
    set(_units_text "")
    foreach(line IN LISTS _lines)
        if(line MATCHES "^[ \t]*#" OR line STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" parts "${line}")
        list(GET parts 0 unit)
        list(GET parts 1 overlay)
        foreach(override IN LISTS PSXSTACK_UNIT_OVERRIDES)
            string(REPLACE "=" ";" op "${override}")
            list(GET op 0 from)
            list(GET op 1 to)
            get_filename_component(from "${from}" ABSOLUTE)
            if(unit STREQUAL from)
                message(STATUS "unit override: ${unit} <- ${to}")
                set(unit "${to}")
            endif()
        endforeach()
        list(APPEND UNITS "${unit}")
        string(APPEND _units_text "${unit}\t${overlay}\n")
    endforeach()
    set(UNITS_FILE "${GEN}/units.txt")
    file(WRITE "${UNITS_FILE}" "${_units_text}")
    list(LENGTH UNITS n_units)
    if(NOT G_OVERLAYS)
        set(G_OVERLAYS "${GEN}/overlays.txt")
        file(WRITE "${G_OVERLAYS}" "")
    endif()

    # ---- Flags shared by everything built here. The game's include directories go to the units (every one of them,
    # first) and, as quote-only directories, to the adapter (its `#include "gamestate.h"`); the runtime and the shim
    # see only the stack's own headers and the generated ones (the shim compiles against include/psxstack/psyq/, not
    # a game's recovered headers: DECISIONS "Psy-Q declarations: the stack's"). -iquote keeps a game's own
    # `include/stdarg.h` (a PS1 build's) from shadowing the host's <stdarg.h> inside the adapter.
    set(GAME_DEFINES PC_PORT ${G_DEFINES})
    set(STACK_INCLUDES "${GEN}/include" "${PSXSTACK_ROOT}/include" "${PSXSTACK_ROOT}/include/psxstack")
    set(GAME_INCLUDES ${STACK_INCLUDES} ${G_INCLUDE_DIRS})
    set(ADAPTER_INCLUDE_FLAGS "")
    foreach(d IN LISTS G_INCLUDE_DIRS)
        list(APPEND ADAPTER_INCLUDE_FLAGS "-iquote" "${d}")
    endforeach()
    set(COMMON_FLAGS -fsigned-char -fwrapv -fno-strict-aliasing)
    if(PSXSTACK_SANITIZE)
        list(APPEND COMMON_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    # Non-PIE on ELF, for the debug channel's sake: the channel (runtime/debug.c), the MCP server (tools/mcp) and tests
    # over them resolve host symbols with nm's link-time addresses, which a PIE would relocate at load. Nothing else
    # needs it (the arena needs no link address, the overlay sections are orphans outside GNU_RELRO). PE has no such
    # flag (ASLR relocates the image; --debug is refused there).
    if(NOT WIN32)
        list(APPEND COMMON_FLAGS -fno-pie)
        add_link_options(-no-pie)
    endif()
    if(PSXSTACK_M32)
        if(WIN32)
            message(FATAL_ERROR "PSXSTACK_M32: the Windows build is x86_64 only")
        endif()
        add_compile_options(-m32)
        add_link_options(-m32)
    endif()

    # ---- The game's units: one object library, each compiled through port_gen.py rename (C_COMPILER_LAUNCHER).
    find_program(PSXSTACK_OBJCOPY NAMES ${CMAKE_OBJCOPY} objcopy NO_CMAKE_FIND_ROOT_PATH)
    find_program(PSXSTACK_OBJDUMP NAMES ${CMAKE_OBJDUMP} objdump NO_CMAKE_FIND_ROOT_PATH)
    if(NOT PSXSTACK_OBJCOPY OR NOT PSXSTACK_OBJDUMP)
        message(FATAL_ERROR "GNU objcopy and objdump (binutils) are needed on the host: the units' section renaming")
    endif()
    set(RENAME_FLAGS "")
    if(WIN32)
        execute_process(COMMAND "${PSXSTACK_OBJDUMP}" --info OUTPUT_VARIABLE _info ERROR_QUIET)
        if(NOT _info MATCHES "pe-x86-64")
            message(FATAL_ERROR "${PSXSTACK_OBJDUMP} cannot read PE objects (x86_64-pe is not among its targets): the "
                                "units' section checks need a host binutils with it (Fedora's and Ubuntu's have it)")
        endif()
        list(APPEND RENAME_FLAGS --pe)
    endif()
    set(game_lib ${target}_game)
    add_library(${game_lib} OBJECT ${UNITS})
    set(RENAME_LAUNCHER "${_psx_python}" "${PSXSTACK_PORT_GEN}" rename --id "${ID}" --objcopy "${PSXSTACK_OBJCOPY}"
        --objdump "${PSXSTACK_OBJDUMP}" --units "${UNITS_FILE}" ${RENAME_FLAGS} --)
    if(CMAKE_C_COMPILER_LAUNCHER)
        list(APPEND RENAME_LAUNCHER ${CMAKE_C_COMPILER_LAUNCHER})
    endif()
    set_target_properties(${game_lib} PROPERTIES C_COMPILER_LAUNCHER "${RENAME_LAUNCHER}")
    set_target_properties(${game_lib} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON C_STANDARD_REQUIRED ON)
    target_compile_definitions(${game_lib} PRIVATE ${GAME_DEFINES})
    target_include_directories(${game_lib} BEFORE PRIVATE ${GAME_INCLUDES})
    target_compile_options(${game_lib} PRIVATE ${COMMON_FLAGS} -fno-builtin -fno-common -fdata-sections
                           -ffunction-sections -Wall -Wno-pointer-sign -Wno-unused-but-set-variable -Wno-unused-variable
                           ${G_UNIT_COMPILE_OPTIONS})
    # PSXSTACK_GAME_OPT: the game's own C, which otherwise follows the build type (and is at GCC's -O0 when the port
    # sets none). A port on a slow core can ask for -O2 or -O3 here; a matching decompilation leaves it empty.
    if(PSXSTACK_GAME_OPT)
        target_compile_options(${game_lib} PRIVATE ${PSXSTACK_GAME_OPT})
    endif()
    # GCC 14+ makes some old-C constructs errors; the probe (tools/port_inventory.py) keeps them warnings the same way.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
        target_compile_options(${game_lib} PRIVATE -fpermissive)
    endif()
    # Clang (the Windows cross build) makes the same constructs errors, each under its own flag: back to warnings.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(${game_lib} PRIVATE -Wno-error=implicit-function-declaration -Wno-error=implicit-int
                               -Wno-error=int-conversion -Wno-error=incompatible-pointer-types
                               -Wno-error=incompatible-function-pointer-types -Wno-error=return-type)
    endif()
    get_filename_component(main_unit "${G_MAIN_UNIT}" ABSOLUTE)
    set_source_files_properties("${main_unit}" PROPERTIES COMPILE_DEFINITIONS "main=game_main")

    # ---- The overlay address tables and the state tables: from nm of the compiled units.
    file(GENERATE OUTPUT "${GEN}/game_objects.rsp" CONTENT "$<TARGET_OBJECTS:${game_lib}>")
    set(_tag "")
    if(G_TAG_SITES)
        set(_tag --tag-sites "${G_TAG_SITES}")
    endif()
    add_custom_command(OUTPUT "${GEN}/overlay_tables.c"
                       COMMAND "${_psx_python}" "${PSXSTACK_PORT_GEN}" tables --id "${ID}" --units "${UNITS_FILE}"
                               --overlays "${G_OVERLAYS}" ${_tag} --nm "${CMAKE_NM}"
                               --objects "${GEN}/game_objects.rsp" --out "${GEN}/overlay_tables.c"
                       DEPENDS $<TARGET_OBJECTS:${game_lib}> "${PSXSTACK_PORT_GEN}" "${G_OVERLAYS}" ${G_TAG_SITES}
                               ${G_CONFIGURE_DEPENDS}
                       COMMENT "port_gen.py tables (the overlay address tables)" VERBATIM)
    set(TABLE_SRCS "${GEN}/overlay_tables.c")
    if(G_EXE_SYMBOLS)
        set(_vol "")
        if(G_VOLATILE)
            set(_vol --volatile "${G_VOLATILE}")
        endif()
        set(_inc "")
        foreach(i IN LISTS GAME_INCLUDES)
            list(APPEND _inc -I "${i}")
        endforeach()
        set(_def "")
        foreach(d IN LISTS G_DEFINES)
            list(APPEND _def -D "${d}")
        endforeach()
        add_custom_command(OUTPUT "${GEN}/port_state_tables.c"
                           COMMAND "${_psx_python}" "${PSXSTACK_PORT_GEN}" state --units "${UNITS_FILE}"
                                   --exe-symbols "${G_EXE_SYMBOLS}" ${_vol} --nm "${CMAKE_NM}" --cc "${CMAKE_C_COMPILER}"
                                   "--cflags=$<$<BOOL:${PSXSTACK_M32}>:-m32>" ${_inc} ${_def}
                                   --objects "${GEN}/game_objects.rsp" --out "${GEN}/port_state_tables.c"
                           DEPENDS $<TARGET_OBJECTS:${game_lib}> "${PSXSTACK_PORT_GEN}" "${G_EXE_SYMBOLS}" ${G_VOLATILE}
                           COMMENT "port_gen.py state (the EXE's function and data tables)" VERBATIM)
        list(APPEND TABLE_SRCS "${GEN}/port_state_tables.c")
    endif()

    # ---- The Psy-Q shim.
    file(GLOB PSYQ_SRCS CONFIGURE_DEPENDS "${PSXSTACK_ROOT}/psyq/*.c")

    # ---- The build's version stamp (port_version, port_commit: `--version`, the start line, the crash report).
    add_custom_target(${target}_version COMMAND "${CMAKE_COMMAND}" -DROOT=${PSXSTACK_VERSION_ROOT}
                      -DOUT=${GEN}/port_version.c -DPREFIX=port -P "${PSXSTACK_ROOT}/cmake/version.cmake"
                      BYPRODUCTS "${GEN}/port_version.c" COMMENT "cmake/version.cmake (the build's version stamp)" VERBATIM)
    # A release build keeps the debug info in the build tree (the packaging strips the shipped binary and saves it as
    # the release's .debug file, which symbolizes crash reports). -g changes no code.
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_C_FLAGS_RELEASE MATCHES "(^| )-g( |$)")
        string(APPEND CMAKE_C_FLAGS_RELEASE " -g")
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}" PARENT_SCOPE)
    endif()

    # ---- The binary: the runtime, the adapter, the tables, the shim, the units (and on PE the bracket symbols).
    file(GLOB RUNTIME_SRCS CONFIGURE_DEPENDS "${PSXSTACK_ROOT}/runtime/*.c")
    set(PLATFORM_SRCS "")
    if(WIN32)
        _psxstack_run("${PSXSTACK_PORT_GEN}" markers --id "${ID}" --overlays "${G_OVERLAYS}" --out "${GEN}/markers.c")
        list(APPEND PLATFORM_SRCS "${GEN}/markers.c")
    endif()
    add_executable(${target} ${RUNTIME_SRCS} ${G_ADAPTER} ${TABLE_SRCS} "${GEN}/port_version.c" ${PLATFORM_SRCS}
                   ${PSYQ_SRCS} $<TARGET_OBJECTS:${game_lib}>)
    add_dependencies(${target} ${target}_version)
    set_target_properties(${target} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON C_STANDARD_REQUIRED ON)
    target_compile_definitions(${target} PRIVATE ${GAME_DEFINES})
    target_include_directories(${target} BEFORE PRIVATE "${PSXSTACK_ROOT}/psyq" "${PSXSTACK_ROOT}/runtime" ${STACK_INCLUDES})
    target_compile_options(${target} PRIVATE ${COMMON_FLAGS} -Wall -Wextra)
    set_source_files_properties(${RUNTIME_SRCS} ${TABLE_SRCS} PROPERTIES COMPILE_OPTIONS "-Werror")
    if(G_ADAPTER)
        set_source_files_properties(${G_ADAPTER} PROPERTIES COMPILE_OPTIONS "-Werror;${ADAPTER_INCLUDE_FLAGS}")
    endif()
    if(PSXSTACK_PSYQ_WERROR)
        set_source_files_properties(${PSYQ_SRCS} PROPERTIES COMPILE_OPTIONS "-Werror")
    endif()
    # The software GPU runs every frame: optimised in every build (-O3: its span loops rely on inlining and on the
    # vectorised select loops; psyq/gpu.c). The SPU core renders every audio frame: -O2 (runtime/spu*.c).
    set_property(SOURCE "${PSXSTACK_ROOT}/psyq/gpu.c" APPEND PROPERTY COMPILE_OPTIONS -O3 -Wno-aggressive-loop-optimizations)
    # The movie decoder and the geometry engine run every frame of a cutscene or a battle setup, and the rest of
    # this target has no -O flag (GCC's -O0). -O2 here is the cheap experiment; the game's own C stays as it was.
    set_property(SOURCE "${PSXSTACK_ROOT}/psyq/mdec.c" "${PSXSTACK_ROOT}/psyq/gte.c" "${PSXSTACK_ROOT}/psyq/libgte.c"
                 APPEND PROPERTY COMPILE_OPTIONS -O2)
    file(GLOB SPU_SRCS CONFIGURE_DEPENDS "${PSXSTACK_ROOT}/runtime/spu*.c")
    set_property(SOURCE ${SPU_SRCS} APPEND PROPERTY COMPILE_OPTIONS -O2)
    # The link map, for reading. After every link the game's objects are checked: a writable section the compile
    # launcher did not rename would survive the console's reset (runtime/reset.c; port_gen.py sections).
    target_link_options(${target} PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/${target}.map")
    add_custom_command(TARGET ${target} POST_BUILD
                       COMMAND "${_psx_python}" "${PSXSTACK_PORT_GEN}" sections --id "${ID}"
                               --objdump "${PSXSTACK_OBJDUMP}" --objects "${GEN}/game_objects.rsp"
                       COMMENT "port_gen.py sections (the game's data is all in the reset's ranges)" VERBATIM)
    # The mods' manifests beside the binary, where the launcher lists them: the stack's (mods/) and the game's.
    set(_copies "")
    foreach(d IN ITEMS "${PSXSTACK_ROOT}/mods" ${G_MODS_DIRS})
        list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_directory "${d}" "${CMAKE_BINARY_DIR}/mods")
    endforeach()
    add_custom_target(${target}_mods ALL ${_copies} COMMENT "the mods' manifests -> ${CMAKE_BINARY_DIR}/mods" VERBATIM)
    if(PSXSTACK_ALLOW_UNRESOLVED)
        target_link_options(${target} PRIVATE "-Wl,--unresolved-symbols=ignore-all")
    endif()
    target_link_libraries(${target} PRIVATE m)
    # Windows: a GUI-subsystem executable (no console window behind the game when the launcher starts it) with the
    # manifest resource (windows/: the UTF-8 code page for the narrow fopen paths, long paths, per-monitor DPI
    # awareness), configured from the game's id and compiled by the toolchain's windres.
    if(WIN32)
        enable_language(RC)
        if(G_RC)
            target_sources(${target} PRIVATE "${G_RC}")
        else()
            configure_file("${PSXSTACK_ROOT}/windows/game.manifest.in" "${GEN}/windows/${target}.manifest" @ONLY)
            configure_file("${PSXSTACK_ROOT}/windows/game.rc.in" "${GEN}/windows/${target}.rc" @ONLY)
            target_sources(${target} PRIVATE "${GEN}/windows/${target}.rc")
        endif()
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
    endif()

    # ---- The window (PSXSTACK_SDL): SDL3 as a CMake package (SDL3_DIR or CMAKE_PREFIX_PATH when given, else the
    # pinned build in a tools/ directory). Off by default: the headless build and the tests stay SDL-free.
    if(PSXSTACK_SDL)
        if(PSXSTACK_M32)
            message(FATAL_ERROR "PSXSTACK_SDL: tools/sdl3 is a 64-bit build; configure the -m32 variant without the window")
        endif()
        set(_hints "")
        foreach(d IN LISTS PSXSTACK_TOOL_DIRS)
            list(APPEND _hints "${d}/sdl3")
        endforeach()
        find_package(SDL3 3.2 CONFIG REQUIRED HINTS ${_hints})
        message(STATUS "SDL3 ${SDL3_VERSION}: ${SDL3_DIR}")
        target_link_libraries(${target} PRIVATE SDL3::SDL3)
        target_compile_definitions(${target} PRIVATE PSXSTACK_SDL)
        # The hardware renderer's shaders (runtime/render_gpu.c): shaders/<name>.<vert|frag>.hlsl compiled by DXC to
        # SPIR-V for SDL_GPU's Vulkan backend at build time, then embedded as C arrays (cmake/embed.cmake) under
        # gen/shaders/<name>_<stage>_spv.h; for Windows also to DXIL for its D3D12 backend (<name>_<stage>_dxil.h),
        # signed by DXC's libdxil.so on this host (embed.cmake refuses an unsigned one: D3D12 rejects it). DXC writes
        # the same bytes on every machine, so nothing compiled is committed.
        set(_dxc "${PSXSTACK_DXC}")
        if(NOT _dxc)
            foreach(d IN LISTS PSXSTACK_TOOL_DIRS)
                if(EXISTS "${d}/dxc/bin/dxc")
                    set(_dxc "${d}/dxc/bin/dxc")
                    break()
                endif()
            endforeach()
        endif()
        if(NOT _dxc OR NOT EXISTS "${_dxc}")
            message(FATAL_ERROR "PSXSTACK_SDL: no DXC for the hardware renderer's shaders: scripts/setup.sh dxc "
                                "(or -DPSXSTACK_DXC=<path to dxc>)")
        endif()
        message(STATUS "DXC: ${_dxc}")
        file(GLOB SHADERS CONFIGURE_DEPENDS "${PSXSTACK_ROOT}/shaders/*.hlsl")
        file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS "${PSXSTACK_ROOT}/shaders/*.hlsli")   # #included by the shaders
        set(SHADER_HEADERS "")
        foreach(src IN LISTS SHADERS)
            get_filename_component(name "${src}" NAME_WLE)   # present.frag
            if(name MATCHES "\\.vert$")
                set(profile vs_6_0)
            elseif(name MATCHES "\\.frag$")
                set(profile ps_6_0)
            else()
                message(FATAL_ERROR "${src}: a shader is <name>.vert.hlsl or <name>.frag.hlsl")
            endif()
            string(REPLACE "." "_" sid "${name}")
            set(spv "${GEN}/shaders/${name}.spv")
            set(header "${GEN}/shaders/${sid}_spv.h")
            add_custom_command(OUTPUT "${spv}"
                               COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN}/shaders"
                               COMMAND "${_dxc}" -T ${profile} -E main -spirv -fspv-target-env=vulkan1.0 -WX
                                       -Fo "${spv}" "${src}"
                               DEPENDS "${src}" ${SHADER_INCLUDES} COMMENT "dxc ${name}.hlsl -> SPIR-V" VERBATIM)
            add_custom_command(OUTPUT "${header}"
                               COMMAND "${CMAKE_COMMAND}" -DIN=${spv} -DOUT=${header} -DNAME=${sid}_spv
                                       -P "${PSXSTACK_ROOT}/cmake/embed.cmake"
                               DEPENDS "${spv}" "${PSXSTACK_ROOT}/cmake/embed.cmake" VERBATIM)
            list(APPEND SHADER_HEADERS "${header}")
            if(WIN32)
                set(dxil "${GEN}/shaders/${name}.dxil")
                set(header "${GEN}/shaders/${sid}_dxil.h")
                add_custom_command(OUTPUT "${dxil}"
                                   COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN}/shaders"
                                   COMMAND "${_dxc}" -T ${profile} -E main -WX -Fo "${dxil}" "${src}"
                                   DEPENDS "${src}" COMMENT "dxc ${name}.hlsl -> DXIL" VERBATIM)
                add_custom_command(OUTPUT "${header}"
                                   COMMAND "${CMAKE_COMMAND}" -DIN=${dxil} -DOUT=${header} -DNAME=${sid}_dxil -DDXIL=ON
                                           -P "${PSXSTACK_ROOT}/cmake/embed.cmake"
                                   DEPENDS "${dxil}" "${PSXSTACK_ROOT}/cmake/embed.cmake" VERBATIM)
                list(APPEND SHADER_HEADERS "${header}")
            endif()
        endforeach()
        add_custom_target(${target}_shaders DEPENDS ${SHADER_HEADERS})
        add_dependencies(${target} ${target}_shaders)
        target_include_directories(${target} PRIVATE "${GEN}/shaders")
    endif()
    if(PSXSTACK_SDL2)
        target_include_directories(${target} PRIVATE "/usr/include/SDL2")
        target_link_libraries(${target} PRIVATE SDL2)
        target_compile_definitions(${target} PRIVATE PSXSTACK_SDL2)
        target_sources(${target} PRIVATE "${PSXSTACK_ROOT}/runtime/fbdev_present.c")
        # The present is every frame (the measured 10 ms / 23 ms). -O3: the NEON expand below relies on it.
        set_property(SOURCE "${PSXSTACK_ROOT}/runtime/fbdev_present.c" APPEND PROPERTY COMPILE_OPTIONS -O3)
    endif()
    if(PSXSTACK_EGL)
        # The GLES renderer (runtime/render_gles.c): offscreen EGL on a GBM device, no window (docs/gpu in the game
        # repository). It needs the EGL, GLES and GBM headers, which embedded images have and desktops often do not,
        # which is why it is its own option instead of riding on PSXSTACK_SDL2.
        target_sources(${target} PRIVATE "${PSXSTACK_ROOT}/runtime/render_gles.c")
        target_compile_definitions(${target} PRIVATE PSXSTACK_EGL)
        target_link_libraries(${target} PRIVATE EGL GLESv2 gbm)
    endif()
    message(STATUS "psxstack: ${target}: ${n_units} units")
endfunction()
