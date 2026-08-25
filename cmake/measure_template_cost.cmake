# Measure what a metaprogram costs the compiler.
#
#   cmake -DCOMPILER=<c++> -DSTD=<20|23> -DSOURCE=<file.cpp>
#         -DINCLUDES=<dir>[@@<dir>...] -DVARIANTS=<name>=<defs>[@@...]
#         [-DITERATIONS=<n>] [-DASSERT_DEEPER=<name>] [-DASSERT_SHALLOWER=<name>]
#         -P cmake/measure_template_cost.cmake
#
# Two numbers per variant:
#
#   depth  the smallest -ftemplate-depth the TU still compiles at, found by
#          binary search. This is the direct answer to "how deep does this
#          instantiate?" -- clang's -ftime-trace is not available here (see
#          docs/CLAUDE.md) and would answer it less precisely anyway.
#   time   wall-clock seconds for ITERATIONS compiles. CMake's clock has
#          one-second resolution, hence the repetition; treat it as a ratio
#          between variants on one machine, never as an absolute.

if(NOT DEFINED COMPILER OR NOT DEFINED SOURCE OR NOT DEFINED VARIANTS)
    message(FATAL_ERROR "measure_template_cost: COMPILER, SOURCE and VARIANTS are required")
endif()

if(NOT DEFINED ITERATIONS)
    set(ITERATIONS 4)
endif()

string(REPLACE "@@" ";" VARIANTS "${VARIANTS}")
string(REPLACE "@@" ";" INCLUDES "${INCLUDES}")

set(include_flags "")
foreach(dir IN LISTS INCLUDES)
    list(APPEND include_flags "-I${dir}")
endforeach()

# Returns TRUE in <out> if the TU compiles at the given -ftemplate-depth.
function(compiles_at depth defs out)
    execute_process(
        COMMAND "${COMPILER}" "-std=c++${STD}" "-ftemplate-depth=${depth}"
                ${include_flags} ${defs} -fsyntax-only "${SOURCE}"
        RESULT_VARIABLE code
        OUTPUT_QUIET ERROR_QUIET)
    if(code EQUAL 0)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(report "")

foreach(variant IN LISTS VARIANTS)
    string(FIND "${variant}" "=" split)
    string(SUBSTRING "${variant}" 0 ${split} name)
    math(EXPR after "${split} + 1")
    string(SUBSTRING "${variant}" ${after} -1 defs)
    string(REPLACE " " ";" defs "${defs}")

    # Sanity: it has to compile at all before a depth means anything.
    set(ceiling 1024)
    compiles_at(${ceiling} "${defs}" ok)
    if(NOT ok)
        message(FATAL_ERROR "measure_template_cost: variant '${name}' does not compile at depth ${ceiling}")
    endif()

    # Binary search the boundary: `low` never compiles, `high` always does.
    # `if(high GREATER low + 1)` would silently misparse -- CMake's comparison
    # takes exactly two operands and does no arithmetic -- so the bound is
    # computed first.
    set(low 0)
    set(high ${ceiling})
    math(EXPR floor_plus_one "${low} + 1")
    while(high GREATER floor_plus_one)
        math(EXPR mid "(${low} + ${high}) / 2")
        compiles_at(${mid} "${defs}" ok)
        if(ok)
            set(high ${mid})
        else()
            set(low ${mid})
        endif()
        math(EXPR floor_plus_one "${low} + 1")
    endwhile()

    string(TIMESTAMP started "%s" UTC)
    foreach(i RANGE 1 ${ITERATIONS})
        execute_process(
            COMMAND "${COMPILER}" "-std=c++${STD}" ${include_flags} ${defs} -fsyntax-only "${SOURCE}"
            OUTPUT_QUIET ERROR_QUIET)
    endforeach()
    string(TIMESTAMP finished "%s" UTC)
    math(EXPR elapsed "${finished} - ${started}")

    message(STATUS "  ${name}: depth=${high}  ${ITERATIONS} compiles in ${elapsed}s")
    set(depth_of_${name} ${high})
    list(APPEND report "${name}=${high}")
endforeach()

# The measurement is only a test if it can fail. The claim under test is that
# the recursive formulation's instantiation depth grows with the list and the
# folded one's does not.
if(DEFINED ASSERT_DEEPER AND DEFINED ASSERT_SHALLOWER)
    if(NOT depth_of_${ASSERT_DEEPER} GREATER depth_of_${ASSERT_SHALLOWER})
        message(FATAL_ERROR
            "expected '${ASSERT_DEEPER}' (depth ${depth_of_${ASSERT_DEEPER}}) to instantiate deeper "
            "than '${ASSERT_SHALLOWER}' (depth ${depth_of_${ASSERT_SHALLOWER}})")
    endif()
    message(STATUS "  ok    ${ASSERT_DEEPER} instantiates deeper than ${ASSERT_SHALLOWER}")
endif()
