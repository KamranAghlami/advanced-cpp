# Assert that a translation unit does NOT compile, and optionally that the
# diagnostic says what it should.
#
#   cmake -DCOMPILER=<c++> -DSTD=<20|23> -DSOURCE=<file.cpp>
#         [-DINCLUDES=<dir>[@@...]] [-DDEFS=<-Dx>[@@...]]
#         [-DEXPECT=<regex>] [-DLABEL=<text>]
#         -P cmake/check_compile_fails.cmake
#
# "This is ill-formed" is a claim like any other, and the only way to keep it
# honest is to compile it. A negative test that silently starts compiling is
# worse than no test: it means the invariant it was guarding is gone.

if(NOT DEFINED COMPILER OR NOT DEFINED SOURCE)
    message(FATAL_ERROR "check_compile_fails: COMPILER and SOURCE are required")
endif()

if(NOT DEFINED LABEL)
    get_filename_component(LABEL "${SOURCE}" NAME)
endif()

string(REPLACE "@@" ";" INCLUDES "${INCLUDES}")
string(REPLACE "@@" ";" DEFS "${DEFS}")

set(flags "")
foreach(dir IN LISTS INCLUDES)
    if(NOT dir STREQUAL "")
        list(APPEND flags "-I${dir}")
    endif()
endforeach()
foreach(def IN LISTS DEFS)
    if(NOT def STREQUAL "")
        list(APPEND flags "${def}")
    endif()
endforeach()

execute_process(
    COMMAND "${COMPILER}" "-std=c++${STD}" ${flags} -fsyntax-only "${SOURCE}"
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(code EQUAL 0)
    message(FATAL_ERROR "  FAIL  ${LABEL}: expected a compile error, but it compiled cleanly")
endif()

if(DEFINED EXPECT AND NOT EXPECT STREQUAL "")
    if(NOT "${err}${out}" MATCHES "${EXPECT}")
        message(FATAL_ERROR
            "  FAIL  ${LABEL}: it failed, but not for the stated reason.\n"
            "  expected the diagnostic to match: ${EXPECT}\n"
            "  got:\n${err}${out}")
    endif()
endif()

message(STATUS "  ok    ${LABEL} is ill-formed, as intended")
