cmake_minimum_required(VERSION 3.20)

project(advanced-cpp LANGUAGES CXX)
# Cached so CI (and you) can build against the libraries' actual floor with
# -DCMAKE_CXX_STANDARD=20. See docs/CLAUDE.md.
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard revision to build with")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# EnTT and Taskflow at the commits the course text is pinned to. See docs/CLAUDE.md.
include(cmake/dependencies.cmake)

find_package(Threads REQUIRED)

# Module 4. The number of distinct std:: names src/acpp/stl re-exports -- the
# library's complete dependency on the standard library, pinned so that adding
# one is a visible decision. Checked by the `stl_manifest` test.
# Not a cache variable: it is a fact about the source, not a knob, and a cached
# one would keep an old value across a re-configure -- which is precisely the
# silent drift this check exists to prevent.
set(ACPP_STL_MANIFEST_SIZE 201)

# ---------------------------------------------------------------------------
# src/ is the shared library the course builds up: Module 11's executor is made
# of Module 9's queue and Module 10's notifier, Module 7's storage extends
# Module 6's sparse set. It used to be a GLOB_RECURSE into an executable; that
# stopped being tenable the moment more than one main() needed the same code.
#
# Mostly headers, so INTERFACE would almost do -- except type_index deliberately
# needs a real TU to demonstrate the cross-DSO ODR trap in Module 1.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ACPP_SOURCES CONFIGURE_DEPENDS "src/acpp/*.cpp")

add_library(acpp STATIC ${ACPP_SOURCES})
target_include_directories(acpp PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(acpp PUBLIC Threads::Threads)

# PUBLIC, so every exercise inherits them. The tree is clean under these today,
# and the point of turning them on is that it stays that way -- a warning found
# by somebody's IDE and not by CI is a warning that comes back.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(acpp PUBLIC -Wall -Wextra)
endif()
# Hidden by default: the Module 1 ODR exercise is only honest if the project's
# own default visibility is the one that causes the bug.
#
# ...but `-fvisibility-inlines-hidden` has to come off on Mach-O, and the reason
# is a finding rather than a nuisance. It demotes an inline function's static
# local from `weak external` to `weak private external`. On ELF that is free --
# the symbol stays exported and the loader still merges it. On Mach-O, dyld's
# coalescing of weak definitions across images is the *only* thing making a
# function-local static process-wide, so hiding it gives every image its own copy
# of acpp::sequential_counter<>::next()::value -- two counters, handing out the
# same small integers to different types. Measured on Apple clang 21 (arm64):
# host alpha=0, and a type the plugin registered at load time also 0. The
# collision is silent, and worse than the bug Module 1 is about: the ODR test's
# agreement check still passes, because the *memoised* per-type static coalesces
# even when the counter behind it does not. With the flag off: alpha=2, beta=3,
# which is exactly the ordering odr_across_dso.cpp predicts for ELF.
#
# The bug reproduction is unaffected -- naive::type_index carries an explicit
# ACPP_HIDDEN, so it splits by attribute, not by this flag.
# See modules/01-type-identity/NOTES.md.
if(APPLE)
    set(ACPP_INLINES_HIDDEN OFF)
else()
    set(ACPP_INLINES_HIDDEN ON)
endif()

set_target_properties(acpp PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ${ACPP_INLINES_HIDDEN}
    POSITION_INDEPENDENT_CODE ON)

# The template's hello-world, kept as a build canary for the bare toolchain.
add_executable(${PROJECT_NAME} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE "src" ${CMAKE_BINARY_DIR})

# One executable per exercise, so several main()s can coexist.
#   acpp_exercise(<name> <source>...)
# Shared code that later modules reuse lives in src/acpp and is linked in.
function(acpp_exercise name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE acpp)
endfunction()

# Registers an exercise and gates CI on it. Benchmarks and codegen probes use
# acpp_exercise() alone -- they are not pass/fail.
#   acpp_test(<name> <source>...)
function(acpp_test name)
    acpp_exercise(${name} ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---------------------------------------------------------------------------
# Codegen probes.
#
#   acpp_asm_probe(<name> <source>)          compile <source> to <name>.s at -O2
#   acpp_asm_test(<test> <probe> <symbol> [REQUIRE ...] [FORBID ...] [MAX_LINES n])
#
# The course asks, repeatedly, for claims to be verified on Compiler Explorer.
# These two turn that into something CI re-runs: the probe emits assembly with
# the project's own compiler, and the test pattern-matches one function's body.
# Codegen checks are a compiler-and-flags-specific claim, so they are worth
# exactly what the flags say and no more -- which is why the flags are visible.
# ---------------------------------------------------------------------------
set(ACPP_ASM_PROBE_FLAGS -O2 -std=c++${CMAKE_CXX_STANDARD}
    CACHE STRING "flags used for the -S codegen probes")

# The probe PATTERNS are x86-64 AT&T assembly with ELF symbol names: `mov $16`,
# `sar`, `jmp *%rax`, and a bare `acpp_probe_x:` label. None of that survives a
# move to AArch64 (different mnemonics entirely) or to Mach-O (which prefixes C
# symbols with an underscore).
#
# So the probes are generated everywhere -- reading the assembly is useful on any
# target -- but the pattern ASSERTIONS only run where the patterns mean anything.
# Skipping loudly beats sixteen red tests that say nothing about the code.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT APPLE)
    set(ACPP_ASM_ASSERTIONS ON)
else()
    set(ACPP_ASM_ASSERTIONS OFF)
    message(STATUS
        "codegen assertions disabled: they are x86-64/ELF patterns and this is "
        "${CMAKE_SYSTEM_PROCESSOR}${APPLE_SUFFIX}. The .s files are still produced; see "
        "docs/pending-verification.md for what to do with them.")
endif()

function(acpp_asm_probe name source)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.s")
    # DEPFILE, not just DEPENDS on the source. Without it the probe is stale the
    # moment a header changes -- and a stale codegen check is worse than none,
    # because it keeps passing against assembly nobody generated any more. Cost
    # us one wrong conclusion about view iteration before it was noticed.
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${CMAKE_CXX_COMPILER}" ${ACPP_ASM_PROBE_FLAGS}
                "-I${PROJECT_SOURCE_DIR}/src"
                -MD -MF "${out}.d" -MT "${out}"
                -S "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${out}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPFILE "${out}.d"
        COMMENT "asm probe ${name}.s"
        VERBATIM)
    add_custom_target(${name} ALL DEPENDS "${out}")
    set_property(GLOBAL PROPERTY ACPP_ASM_${name} "${out}")
endfunction()

function(acpp_asm_test test probe symbol)
    if(NOT ACPP_ASM_ASSERTIONS)
        return()
    endif()

    cmake_parse_arguments(ARG "" "MAX_LINES" "REQUIRE;FORBID" ${ARGN})
    get_property(asm GLOBAL PROPERTY ACPP_ASM_${probe})
    set(defs "-DASM=${asm}" "-DSYMBOL=${symbol}" "-DLABEL=${test}")
    # Patterns are joined with @@ rather than passed as a CMake list: a list
    # expands to several arguments on a COMMAND line, and these are regexes that
    # legitimately contain every other punctuation character.
    if(ARG_REQUIRE)
        string(JOIN "@@" joined ${ARG_REQUIRE})
        list(APPEND defs "-DREQUIRE=${joined}")
    endif()
    if(ARG_FORBID)
        string(JOIN "@@" joined ${ARG_FORBID})
        list(APPEND defs "-DFORBID=${joined}")
    endif()
    if(ARG_MAX_LINES)
        list(APPEND defs "-DMAX_LINES=${ARG_MAX_LINES}")
    endif()
    add_test(NAME ${test} COMMAND ${CMAKE_COMMAND} ${defs}
             -P "${PROJECT_SOURCE_DIR}/cmake/check_asm.cmake")
endfunction()

# ---------------------------------------------------------------------------
# Negative compilation.
#
#   acpp_compile_fail_test(<test> <source> [DEFS ...] [EXPECT <regex>])
#
# Several checkpoints in the course are of the form "give the exact code that
# breaks without this". Those are claims, and the only honest way to keep them
# is to compile them and require the failure.
# ---------------------------------------------------------------------------
function(acpp_compile_fail_test test source)
    cmake_parse_arguments(ARG "" "EXPECT" "DEFS" ${ARGN})
    set(defs "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DSTD=${CMAKE_CXX_STANDARD}"
             "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/${source}"
             "-DINCLUDES=${PROJECT_SOURCE_DIR}/src" "-DLABEL=${test}")
    if(ARG_DEFS)
        string(JOIN "@@" joined ${ARG_DEFS})
        list(APPEND defs "-DDEFS=${joined}")
    endif()
    if(ARG_EXPECT)
        list(APPEND defs "-DEXPECT=${ARG_EXPECT}")
    endif()
    add_test(NAME ${test} COMMAND ${CMAKE_COMMAND} ${defs}
             -P "${PROJECT_SOURCE_DIR}/cmake/check_compile_fails.cmake")
endfunction()

add_subdirectory(modules/00-setup)
add_subdirectory(modules/01-type-identity)
add_subdirectory(modules/02-traits)
add_subdirectory(modules/03-layout-economy)
add_subdirectory(modules/04-freestanding-shim)
add_subdirectory(modules/05-handles)
add_subdirectory(modules/06-sparse-set)
add_subdirectory(modules/07-storage-and-views)
add_subdirectory(modules/08-type-erasure)
add_subdirectory(modules/09-work-stealing-deque)
add_subdirectory(modules/10-notifier)
add_subdirectory(modules/11-scheduler)
add_subdirectory(modules/12-parallel-algorithms)
add_subdirectory(modules/13-capstone)
