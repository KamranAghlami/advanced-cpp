# Module 4 -- the freestanding shim.

# The same TU, twice, with no difference but the include path. That is the claim
# the module makes, so building it both ways is the test.
acpp_test(seam_default seam_swap.cpp)

acpp_test(seam_fixed seam_swap.cpp)
target_include_directories(seam_fixed BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ext")

# Exercise 2 against EnTT itself. entt_ext/ supplies entt/ext/stl/vector.hpp, so
# EnTT's own stl/vector.hpp finds it through __has_include and never reaches
# <vector>. How far up the library that gets is recorded in NOTES.md.
acpp_test(entt_on_fixed_vector entt_on_fixed_vector.cpp)
target_link_libraries(entt_on_fixed_vector PRIVATE EnTT::EnTT)
target_include_directories(entt_on_fixed_vector BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/entt_ext" "${CMAKE_CURRENT_SOURCE_DIR}/ext")

# Exercise 1, kept honest. The `using` list is the dependency manifest, and a
# manifest is only useful if it cannot grow unnoticed.
add_test(NAME stl_manifest COMMAND ${CMAKE_COMMAND}
    "-DDIRECTORY=${PROJECT_SOURCE_DIR}/src/acpp/stl"
    "-DEXPECT=${ACPP_STL_MANIFEST_SIZE}"
    -P "${PROJECT_SOURCE_DIR}/cmake/check_manifest.cmake")
