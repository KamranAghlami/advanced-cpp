# Module 11 -- scheduler and graph representation.

# Links Taskflow deliberately: exercise 1 asks for sizeof(tf::Node), and the
# comparison is only meaningful against the real type.
acpp_test(node_layout node_layout.cpp)
target_link_libraries(node_layout PRIVATE Taskflow::Taskflow)
acpp_test(edge_partition edge_partition.cpp)
acpp_test(dag_executor dag_executor.cpp)

acpp_test(exception_propagation exception_propagation.cpp)

set_tests_properties(dag_executor exception_propagation PROPERTIES TIMEOUT 300)

# Module 10's exercise 6, which needs an executor to exist. A measurement.
acpp_exercise(sticky_victim_bench sticky_victim_bench.cpp)
target_compile_options(sticky_victim_bench PRIVATE ${ACPP_OPTIMIZE})

# 11.1: the switch-vs-visit question, asked of a compiler rather than of memory.
acpp_asm_probe(variant_dispatch_codegen variant_dispatch_codegen.cpp)

# Both must dispatch without calling into variant machinery -- the interesting
# question is whether they differ, and NOTES.md reports the answer.
acpp_asm_test(codegen_switch_dispatch variant_dispatch_codegen acpp_probe_switch_dispatch
    FORBID "__throw_bad_variant_access" "	call")
acpp_asm_test(codegen_visit_dispatch variant_dispatch_codegen acpp_probe_visit_dispatch
    FORBID "__throw_bad_variant_access")
