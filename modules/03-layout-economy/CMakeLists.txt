# Module 3 -- layout economy.

acpp_test(compressed_pair_layout compressed_pair_layout.cpp)
acpp_test(allocator_aware_ring allocator_aware_ring.cpp)
acpp_test(layout_assertions layout_assertions.cpp)

# The checkpoint, as a test: the tag-less pair must be ill-formed, and for the
# stated reason rather than by accident.
acpp_compile_fail_test(compile_fail_tagless_pair tagless_pair_fails.cpp
    EXPECT "duplicate base|more than once as a direct base")
