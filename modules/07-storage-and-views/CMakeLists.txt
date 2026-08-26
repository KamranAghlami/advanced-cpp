# Module 7 -- storage and view iteration.

acpp_test(paged_payload paged_payload.cpp)
acpp_test(view_composition view_composition.cpp)

# Exercise 2's comparison: a measurement, not a test.
acpp_exercise(view_bench view_bench.cpp)
target_compile_options(view_bench PRIVATE ${ACPP_OPTIMIZE})

# Exercise 3: "verify at -O2 that the tuple machinery is entirely eliminated".
acpp_asm_probe(view_codegen view_codegen.cpp)

# A single-pool each() must be a strided loop and nothing else: no call into
# tuple construction, no memory traffic for a tuple that was never needed.
acpp_asm_test(codegen_view_single_pool view_codegen acpp_probe_single_pool_sum
    FORBID "\tcall" "_ZNSt.*tuple" "operator new")

# Paged indexing is a shift and a mask because the page size is a compile-time
# power of two. A division instruction here would falsify storage.hpp's comment.
acpp_asm_test(codegen_view_paged_access view_codegen acpp_probe_paged_access
    FORBID "\tdiv" "\tidiv" "\tcall"
    REQUIRE "sar|shr|sal|shl|and")

# The two-pool loop keeps the filter -- so it may branch and it may call into
# contains(). What it must NOT do is materialise a tuple in memory.
acpp_asm_test(codegen_view_two_pool view_codegen acpp_probe_two_pool_sum
    FORBID "_ZNSt.*tupleI.*C[12]E" "operator new")
