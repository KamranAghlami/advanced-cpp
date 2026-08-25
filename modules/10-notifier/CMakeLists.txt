# Module 10 -- sleeping without lost wakeups.

acpp_test(lost_wakeup lost_wakeup.cpp)
acpp_test(notifier_state_layout notifier_state_layout.cpp)
acpp_test(notifier_protocol notifier_protocol.cpp)

# Exercise 5: idle CPU under a bursty load. A measurement, not a test.
acpp_exercise(notifier_idle notifier_idle.cpp)
target_compile_options(notifier_idle PRIVATE -O2)

set_tests_properties(lost_wakeup notifier_protocol PROPERTIES TIMEOUT 300)
