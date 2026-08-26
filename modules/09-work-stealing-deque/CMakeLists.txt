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

# The harness that makes the knob above testable. wsq_stress keeps a deep queue,
# so the owner and thieves never touch the same slot and the publish race cannot
# be observed; this one holds the queue 0-2 deep so that they always do.
#
# The UNWEAKENED build is a real regression test and gates CI everywhere: it is a
# stress shape wsq_stress does not cover, and it must never corrupt on any
# machine. -O2 because a race test at -O0 is testing a different program.
acpp_test(wsq_publish_race wsq_publish_race.cpp)
target_compile_options(wsq_publish_race PRIVATE -O2)

# The weakened build, and like wsq_weakened_release it is NOT a ctest: it is
# expected to FAIL on a weakly-ordered machine and to pass on x86, so its correct
# outcome depends on the host. On an M1 it corrupts on ~1 run in 4.
acpp_exercise(wsq_publish_race_weakened wsq_publish_race.cpp)
target_compile_definitions(wsq_publish_race_weakened PRIVATE ACPP_WSQ_WEAKEN_RELEASE)
target_compile_options(wsq_publish_race_weakened PRIVATE -O2)

# Exercise 5: a measurement, not a test.
acpp_exercise(wsq_bench wsq_bench.cpp)
target_compile_options(wsq_bench PRIVATE -O2)

set_tests_properties(wsq_stress wsq_weakened wsq_publish_race PROPERTIES TIMEOUT 300)
