# Module 1 -- type identity without RTTI.

acpp_test(type_name_probe type_name_probe.cpp)
acpp_test(cmd_dispatch cmd_dispatch.cpp)
acpp_test(constexpr_to_string constexpr_to_string.cpp)

# ---------------------------------------------------------------------------
# Exercise 3: two TUs and one shared object.
#
# The plugin is a real SHARED library, not an OBJECT library folded back into the
# host -- the bug only exists across a dynamic boundary, so simulating one would
# simulate the bug away. It links acpp so that both sides genuinely have their own
# copy of the static library's code; whether those copies merge at load time is
# the entire question.
# ---------------------------------------------------------------------------
add_library(odr_plugin SHARED odr_plugin.cpp)
target_link_libraries(odr_plugin PRIVATE acpp)
set_target_properties(odr_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

acpp_test(odr_across_dso odr_across_dso.cpp)
target_link_libraries(odr_across_dso PRIVATE odr_plugin)
set_target_properties(odr_across_dso PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Without this the host's own copy of the merged symbols never reaches the
    # dynamic symbol table, and the "fixed" case would have nothing to bind to.
    ENABLE_EXPORTS ON)

# ---------------------------------------------------------------------------
# Codegen probes (exercises 1 and 2).
# ---------------------------------------------------------------------------
acpp_asm_probe(type_name_codegen type_name_codegen.cpp)

# The claim: the string_view is two immediates. `lea` of a rodata label plus a
# `mov` of a constant, and nothing else -- no call, no memory dereference, no
# strlen. A budget of 4 instructions leaves room for the return and a
# register-allocation quirk without leaving room for actual work.
acpp_asm_test(codegen_type_name_data type_name_codegen acpp_probe_name_data
    FORBID "\tcall" "strlen" "memchr"
    MAX_LINES 4)

acpp_asm_test(codegen_type_name_size type_name_codegen acpp_probe_name_size
    REQUIRE "mov.*\\$[0-9]+"
    FORBID "\tcall"
    MAX_LINES 3)

# The hash is folded to one immediate; if FNV-1a ran at runtime there would be a
# loop here, and a loop needs a backward branch.
acpp_asm_test(codegen_type_hash type_name_codegen acpp_probe_hash
    FORBID "\tcall" "\tj[a-z]+\t"
    MAX_LINES 3)

# The contrast case. The sequential id genuinely cannot be constant-folded, so
# this one MUST show the thread-safe-static guard. Requiring it here is what
# stops the three checks above from being vacuous: it proves the probe harness
# can tell the difference between compile-time and runtime work.
acpp_asm_test(codegen_type_index_is_not_free type_name_codegen acpp_probe_index
    REQUIRE "\tj[a-z]+\t")

acpp_asm_probe(cmd_dispatch_codegen cmd_dispatch_codegen.cpp)

# Integer compares against immediates (or a jump table). What must NOT be here is
# any trace of string comparison -- that would mean the hash was not folded.
acpp_asm_test(codegen_cmd_dispatch cmd_dispatch_codegen acpp_probe_dispatch
    FORBID "strcmp" "memcmp" "strncmp" "\tcall")

# A literal command at the call site collapses to `mov $3, %eax; ret`.
acpp_asm_test(codegen_cmd_dispatch_constant cmd_dispatch_codegen acpp_probe_dispatch_constant
    REQUIRE "mov.*\\$3"
    FORBID "\tcall"
    MAX_LINES 3)
