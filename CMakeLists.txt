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
# Hidden by default: the Module 1 ODR exercise is only honest if the project's
# own default visibility is the one that causes the bug.
set_target_properties(acpp PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
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

function(acpp_asm_probe name source)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.s")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${CMAKE_CXX_COMPILER}" ${ACPP_ASM_PROBE_FLAGS}
                "-I${PROJECT_SOURCE_DIR}/src"
                -S "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${out}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        COMMENT "asm probe ${name}.s"
        VERBATIM)
    add_custom_target(${name} ALL DEPENDS "${out}")
    set_property(GLOBAL PROPERTY ACPP_ASM_${name} "${out}")
endfunction()

function(acpp_asm_test test probe symbol)
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
