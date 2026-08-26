# Module 2 -- traits as an API surface.

acpp_test(component_traits_ladder component_traits_ladder.cpp)
acpp_test(serialization_traits serialization_traits.cpp)
acpp_test(counter_traits counter_traits.cpp)

# Exercise 2 links EnTT on purpose: "same answer as the real thing" is the claim.
acpp_test(type_list_algebra type_list_algebra.cpp)
target_link_libraries(type_list_algebra PRIVATE EnTT::EnTT)

# ---------------------------------------------------------------------------
# Exercise 2, cost half.
#
# The course says to use clang's -ftime-trace. clang is not installed here (see
# docs/CLAUDE.md), so this measures instantiation depth directly instead, by
# binary-searching the smallest -ftemplate-depth that compiles. Slower to run,
# more precise as an answer, and it works on whatever compiler is configured.
# ---------------------------------------------------------------------------
# Bisects -ftemplate-depth, which MSVC spells differently and bounds
# differently. Labelled "measurement" and excluded from CI regardless.
if(ACPP_GNU_DRIVER)
add_test(NAME type_list_unique_cost COMMAND ${CMAKE_COMMAND}
    "-DCOMPILER=${CMAKE_CXX_COMPILER}"
    "-DSTD=${CMAKE_CXX_STANDARD}"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/type_list_depth_probe.cpp"
    "-DINCLUDES=${PROJECT_SOURCE_DIR}/src@@${PROJECT_SOURCE_DIR}/third_party/entt/src"
    "-DVARIANTS=recursive=-DACPP_UNIQUE_RECURSIVE@@folded=-DACPP_UNIQUE_FOLDED@@entt=-DACPP_UNIQUE_ENTT"
    -DASSERT_DEEPER=recursive
    -DASSERT_SHALLOWER=folded
    -P "${PROJECT_SOURCE_DIR}/cmake/measure_template_cost.cmake")
endif()

# Compiling a 4096-deep binary search several times over is not a unit test.
if(ACPP_GNU_DRIVER)
    set_tests_properties(type_list_unique_cost PROPERTIES TIMEOUT 900 LABELS "measurement")
endif()
