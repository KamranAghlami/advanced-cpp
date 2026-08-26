# Module 6 -- the sparse set.

acpp_test(sparse_set_invariants sparse_set_invariants.cpp)
acpp_test(in_place_free_list in_place_free_list.cpp)
acpp_test(swap_only_partition swap_only_partition.cpp)
acpp_test(virtual_seam virtual_seam.cpp)

# A measurement, not a test: no add_test(). Build it at -O2 regardless of the
# tree's build type -- timing a -O0 build measures the compiler's mood.
acpp_exercise(sparse_set_bench sparse_set_bench.cpp)
target_compile_options(sparse_set_bench PRIVATE ${ACPP_OPTIMIZE})
