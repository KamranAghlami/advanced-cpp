# Module 0 -- instrument setup.
#
# These two are smoke tests: they prove the pinned EnTT and Taskflow checkouts
# compile, link and run in this project before Module 1 starts.
#
# Note that exercise code for Modules 1-12 should generally NOT link these
# libraries -- the whole point is to reimplement their techniques. The
# dependencies are here to be read, built, and compared against.

acpp_exercise(entt_smoke entt_smoke.cpp)
target_link_libraries(entt_smoke PRIVATE EnTT::EnTT)
add_test(NAME entt_smoke COMMAND entt_smoke)

acpp_exercise(taskflow_smoke taskflow_smoke.cpp)
target_link_libraries(taskflow_smoke PRIVATE Taskflow::Taskflow)
add_test(NAME taskflow_smoke COMMAND taskflow_smoke)
