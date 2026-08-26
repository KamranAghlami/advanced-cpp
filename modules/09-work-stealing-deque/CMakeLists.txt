# Module 9 -- the Chase-Lev work-stealing deque.

acpp_test(wsq_semantics wsq_semantics.cpp)
acpp_test(wsq_stress wsq_stress.cpp)

# Exercise 3: the same stress test with one memory order deliberately wrong.
# It is expected to PASS on x86 -- that is the point of running it. See NOTES.md.
acpp_test(wsq_weakened wsq_stress.cpp)
target_compile_definitions(wsq_weakened PRIVATE ACPP_WSQ_WEAKEN_FENCE)

# Exercise 3, second knob: the weakening ARM can actually catch.
#
# Deliberately NOT an acpp_test. Its expected outcome depends on the host --
# on x86 the weakening changes no instruction at all, so it passes there by
# construction, while on AArch64 `stlr` becomes `str` and the interleaving is
# reachable. A test whose expected result depends on the machine is not a
# regression gate, and wiring it into ctest would either make CI red on one
# platform or teach the wrong lesson on the other.
#
# -O2 so the compiler has the same freedom to reorder that the hardware does.
acpp_exercise(wsq_weakened_release wsq_stress.cpp)
target_compile_definitions(wsq_weakened_release PRIVATE ACPP_WSQ_WEAKEN_RELEASE)
target_compile_options(wsq_weakened_release PRIVATE -O2)

# Exercise 5: a measurement, not a test.
acpp_exercise(wsq_bench wsq_bench.cpp)
target_compile_options(wsq_bench PRIVATE -O2)

set_tests_properties(wsq_stress wsq_weakened PROPERTIES TIMEOUT 300)
