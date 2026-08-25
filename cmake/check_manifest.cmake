# Count the standard-library names a freestanding shim re-exports, and require
# the count to be the documented one.
#
#   cmake -DDIRECTORY=<src/acpp/stl> -DEXPECT=<n> -P cmake/check_manifest.cmake
#
# The `using std::` list in a shim directory is a dependency manifest, and its
# whole value is that somebody has read it. A manifest that can grow silently is
# just an include list with extra steps -- so the count is pinned, and adding a
# dependency means changing a number in the root CMakeLists and noticing.

if(NOT DEFINED DIRECTORY OR NOT DEFINED EXPECT)
    message(FATAL_ERROR "check_manifest: DIRECTORY and EXPECT are required")
endif()

file(GLOB headers "${DIRECTORY}/*.hpp")

set(names "")
foreach(header IN LISTS headers)
    file(STRINGS "${header}" lines REGEX "^using std::")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^using std::([A-Za-z_0-9]+).*$" "\\1" name "${line}")
        list(APPEND names "${name}")
    endforeach()
endforeach()

list(LENGTH names total)
list(REMOVE_DUPLICATES names)
list(LENGTH names distinct)
list(LENGTH headers files)

if(NOT distinct EQUAL EXPECT)
    list(SORT names)
    string(REPLACE ";" "\n    " printable "${names}")
    message(FATAL_ERROR
        "  FAIL  the standard-library manifest changed: ${distinct} distinct names, expected ${EXPECT}.\n"
        "  If that is intended, update ACPP_STL_MANIFEST_SIZE in the root CMakeLists -- the point\n"
        "  of the number is that someone reads the diff.\n"
        "  current manifest (${files} headers, ${total} entries):\n    ${printable}\n")
endif()

message(STATUS "  ok    standard-library manifest: ${distinct} distinct names across ${files} headers")
