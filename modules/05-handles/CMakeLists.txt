# Module 5 -- bit-packed handles.

acpp_test(handle_layout handle_layout.cpp)
acpp_test(handle_allocator handle_allocator.cpp)
acpp_test(version_wraparound version_wraparound.cpp)
acpp_test(handle_property_test handle_property_test.cpp)

# A split that does not fit its underlying type must not compile: index and
# version masks would overlap and every handle would alias another.
acpp_compile_fail_test(compile_fail_oversized_split oversized_split_fails.cpp
    EXPECT "does not fit in the underlying type")
