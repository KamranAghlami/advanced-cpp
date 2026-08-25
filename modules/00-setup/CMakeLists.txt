# Module 0 -- instrument setup.
#
# The course insists the instruments work before Module 1 starts. Three targets:
# two smoke tests proving the pinned checkouts compile, link and run here, and a
# report of what this machine actually is.
#
# Note that exercise code for Modules 1-12 should generally NOT link EnTT or
# Taskflow -- the whole point is to reimplement their techniques. These three
# and four named comparisons elsewhere are the exceptions.

# The instrument check. No dependency on either library: it has to run even when
# the toolchain is the thing that is broken.
acpp_test(toolchain_report toolchain_report.cpp)

acpp_test(entt_smoke entt_smoke.cpp)
target_link_libraries(entt_smoke PRIVATE EnTT::EnTT)

acpp_test(taskflow_smoke taskflow_smoke.cpp)
target_link_libraries(taskflow_smoke PRIVATE Taskflow::Taskflow)
