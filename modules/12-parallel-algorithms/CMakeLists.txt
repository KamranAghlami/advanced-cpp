# Module 12 -- parallel algorithm design.

acpp_test(corun_deadlock corun_deadlock.cpp)
acpp_test(pipeline_ordering pipeline_ordering.cpp)

# Exercise 1: a measurement, not a test.
acpp_exercise(partitioner_bench partitioner_bench.cpp)
target_compile_options(partitioner_bench PRIVATE ${ACPP_OPTIMIZE})

set_tests_properties(corun_deadlock pipeline_ordering PROPERTIES TIMEOUT 300)
