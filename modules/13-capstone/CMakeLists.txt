# Capstone -- a reactive dataflow engine over Modules 5-11.

acpp_test(dataflow_engine dataflow_engine.cpp)

acpp_exercise(dataflow_bench dataflow_bench.cpp)
target_compile_options(dataflow_bench PRIVATE -O2)

set_tests_properties(dataflow_engine PROPERTIES TIMEOUT 300)
