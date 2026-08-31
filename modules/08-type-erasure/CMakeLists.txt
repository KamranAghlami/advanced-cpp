# Module 8 -- type erasure, three ways.

acpp_test(small_any small_any.cpp)
acpp_test(delegate_semantics delegate_semantics.cpp)

# Exercise 1 links EnTT deliberately: the table is only meaningful against the
# real entt::delegate and entt::poly.
acpp_exercise(erasure_table erasure_table.cpp)
target_link_libraries(erasure_table PRIVATE EnTT::EnTT)
target_compile_options(erasure_table PRIVATE ${ACPP_OPTIMIZE})

# ---------------------------------------------------------------------------
# Exercise 3: does a delegate call inline?
# ---------------------------------------------------------------------------
acpp_asm_probe(delegate_codegen delegate_probe.cpp)

# A locally-constructed delegate over a compile-time target collapses to the
# target's body: `lea (%rdi,%rdi,2)` is x*3, and that is the whole function.
acpp_asm_test(codegen_delegate_inlined delegate_codegen acpp_probe_delegate_inlined
    FORBID "\tcall" "\tjmp[a-z]*[ \t]+\\*"
    MAX_LINES 4)

# The finding worth pinning, in two halves, because they do not hold on the same
# set of compilers.
#
# Half one, asserted everywhere: the LOCAL std::function's DISPATCH is
# devirtualised. _M_invoke's address is written into _Any_data and then never
# called -- the target is reached directly, by name. This is the half that
# corrects the folklore ("delegates inline, std::function does not"), and it
# holds on every toolchain measured, gcc 11.4 included.
acpp_asm_test(codegen_std_function_local_devirtualises delegate_codegen acpp_probe_std_function_local
    FORBID "call[a-z]*[ \t]+\\*" "jmp[a-z]*[ \t]+\\*")

# Half two: the std::function OBJECT vanishes outright, leaving the same two
# instructions as the delegate. That needs the optimiser to dead-store-eliminate
# _Any_data across a non-trivial destructor, which is a strictly harder job than
# devirtualising the call and is where the toolchains diverge.
#
# gcc 13.3 and clang 18 do it. gcc 11.4 does not: it devirtualises the call but
# still builds _Any_data on the stack and calls _M_manager(__destroy_functor) to
# tear it down -- 20 instructions at -O2, and the stack protector that shows up
# in the failure output is a consequence of that live stack buffer, not a cause.
# Measured 2026-08-31; see modules/08-type-erasure/NOTES.md.
#
# gcc 12 was never measured, so the cut is at the version that was. If this test
# fails on gcc >= 13 or on clang, the folklore became true again and the
# recommendation in NOTES.md needs revisiting.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    message(STATUS
        "codegen_std_function_local_is_also_free skipped on gcc "
        "${CMAKE_CXX_COMPILER_VERSION}: it devirtualises the call but keeps the "
        "object. Needs gcc >= 13; the dispatch half still runs.")
else()
    acpp_asm_test(codegen_std_function_local_is_also_free delegate_codegen acpp_probe_std_function_local
        FORBID "\tcall" "\tjmp[a-z]*[ \t]+\\*"
        MAX_LINES 4)
endif()

# For a bound member the delegate's own indirection must go; whether the target
# body inlines is the optimiser's business, so a direct call by name is allowed
# and an indirect one is not.
acpp_asm_test(codegen_delegate_member delegate_codegen acpp_probe_delegate_member
    FORBID "call[a-z]*[ \t]+\\*" "jmp[a-z]*[ \t]+\\*")

# Stored: one indirect call and almost nothing else. A tail `jmp *%rax` counts.
acpp_asm_test(codegen_delegate_stored delegate_codegen acpp_probe_delegate_stored
    REQUIRE "(call|jmp)[a-z]*[ \t]+\\*"
    MAX_LINES 5)

# std::function stored: also indirect, but it must first check for an empty
# target, because the standard requires it to throw. That check is the price of
# the interface, and it is why the delegate has none.
acpp_asm_test(codegen_std_function_stored delegate_codegen acpp_probe_std_function_stored
    REQUIRE "(call|jmp)[a-z]*[ \t]+\\*" "\tje|\tjne|\ttest|\tcmp")

# ---------------------------------------------------------------------------
# Exercise 2: only the operations you use get emitted.
# ---------------------------------------------------------------------------
add_library(any_emission OBJECT any_emission.cpp)
target_link_libraries(any_emission PRIVATE acpp)
target_compile_options(any_emission PRIVATE ${ACPP_OPTIMIZE})

# dumpbin /symbols exists, but its output shares no format with nm and the
# demangled names differ entirely. The claim -- that no vtable is emitted for a
# type never placed in an any -- is about the compiler's instantiation rules,
# which one toolchain can settle.
if(ACPP_GNU_DRIVER)
add_test(NAME symbols_any_vtables COMMAND ${CMAKE_COMMAND}
    "-DOBJECT=$<TARGET_OBJECTS:any_emission>"
    "-DNM=${CMAKE_NM}"
    "-DLABEL=symbols_any_vtables"
    # The vtable for a type that was never put into an any must not exist.
    "-DFORBID=never_instantiated"
    -P "${PROJECT_SOURCE_DIR}/cmake/check_symbols.cmake")
endif()
