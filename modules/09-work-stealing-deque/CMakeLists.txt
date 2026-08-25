# Module 9 -- the Chase-Lev work-stealing deque.

acpp_test(wsq_semantics wsq_semantics.cpp)
acpp_test(wsq_stress wsq_stress.cpp)

# Exercise 3: the same stress test with one memory order deliberately wrong.
# It is expected to PASS on x86 -- that is the point of running it. See NOTES.md.
acpp_test(wsq_weakened wsq_stress.cpp)
target_compile_definitions(wsq_weakened PRIVATE ACPP_WSQ_WEAKEN_FENCE)

# Exercise 5: a measurement, not a test.
acpp_exercise(wsq_bench wsq_bench.cpp)
target_compile_options(wsq_bench PRIVATE -O2)

set_tests_properties(wsq_stress wsq_weakened PROPERTIES TIMEOUT 300)
