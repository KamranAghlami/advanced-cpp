# Module 8 -- type erasure, three ways.

acpp_test(small_any small_any.cpp)
acpp_test(delegate_semantics delegate_semantics.cpp)

# Exercise 1 links EnTT deliberately: the table is only meaningful against the
# real entt::delegate and entt::poly.
acpp_exercise(erasure_table erasure_table.cpp)
target_link_libraries(erasure_table PRIVATE EnTT::EnTT)
target_compile_options(erasure_table PRIVATE -O2)

# ---------------------------------------------------------------------------
# Exercise 3: does a delegate call inline?
# ---------------------------------------------------------------------------
acpp_asm_probe(delegate_codegen delegate_probe.cpp)

# A locally-constructed delegate over a compile-time target collapses to the
# target's body: `lea (%rdi,%rdi,2)` is x*3, and that is the whole function.
acpp_asm_test(codegen_delegate_inlined delegate_codegen acpp_probe_delegate_inlined
    FORBID "\tcall" "\tjmp[a-z]*[ \t]+\\*"
    MAX_LINES 4)

# The finding worth pinning: gcc devirtualises a LOCAL std::function just as
# completely. If this ever starts failing, the folklore became true again and
# the recommendation in NOTES.md needs revisiting.
acpp_asm_test(codegen_std_function_local_is_also_free delegate_codegen acpp_probe_std_function_local
    FORBID "\tcall" "\tjmp[a-z]*[ \t]+\\*"
    MAX_LINES 4)

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
target_compile_options(any_emission PRIVATE -O2)

add_test(NAME symbols_any_vtables COMMAND ${CMAKE_COMMAND}
    "-DOBJECT=$<TARGET_OBJECTS:any_emission>"
    "-DNM=${CMAKE_NM}"
    "-DLABEL=symbols_any_vtables"
    # The vtable for a type that was never put into an any must not exist.
    "-DFORBID=never_instantiated"
    -P "${PROJECT_SOURCE_DIR}/cmake/check_symbols.cmake")
