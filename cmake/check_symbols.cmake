# Assert what a translation unit did and did not emit.
#
#   cmake -DOBJECT=<file.o> -DNM=<nm> [-DREQUIRE=<regex>[@@...]]
#         [-DFORBID=<regex>[@@...]] [-DLABEL=<text>] -P cmake/check_symbols.cmake
#
# "Only the operations you use get emitted" is a claim about the object file,
# and the object file is the only place it can be checked. Demangled names, so
# the patterns read like C++ rather than like Itanium mangling.

if(NOT DEFINED OBJECT)
    message(FATAL_ERROR "check_symbols: OBJECT is required")
endif()

if(NOT DEFINED NM)
    set(NM nm)
endif()

if(NOT DEFINED LABEL)
    get_filename_component(LABEL "${OBJECT}" NAME)
endif()

string(REPLACE "@@" ";" REQUIRE "${REQUIRE}")
string(REPLACE "@@" ";" FORBID "${FORBID}")

execute_process(
    COMMAND ${NM} -C --size-sort --defined-only "${OBJECT}"
    OUTPUT_VARIABLE symbols
    RESULT_VARIABLE code
    ERROR_VARIABLE err)

if(NOT code EQUAL 0)
    message(FATAL_ERROR "check_symbols: nm failed on ${OBJECT}\n${err}")
endif()

string(REPLACE "\n" ";" lines "${symbols}")

set(failures "")

foreach(pattern IN LISTS REQUIRE)
    if(pattern STREQUAL "")
        continue()
    endif()
    set(hit OFF)
    foreach(line IN LISTS lines)
        if(line MATCHES "${pattern}")
            set(hit ON)
            break()
        endif()
    endforeach()
    if(NOT hit)
        list(APPEND failures "expected a symbol matching: ${pattern}")
    endif()
endforeach()

foreach(pattern IN LISTS FORBID)
    if(pattern STREQUAL "")
        continue()
    endif()
    foreach(line IN LISTS lines)
        if(line MATCHES "${pattern}")
            list(APPEND failures "forbidden symbol emitted: ${line}")
        endif()
    endforeach()
endforeach()

if(failures)
    string(REPLACE ";" "\n  - " printable "${failures}")
    string(REPLACE ";" "\n    " all "${lines}")
    message(FATAL_ERROR "  FAIL  ${LABEL}\n  - ${printable}\n  symbols:\n    ${all}\n")
endif()

list(LENGTH lines count)
message(STATUS "  ok    ${LABEL} (${count} defined symbols)")
