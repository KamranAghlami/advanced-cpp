# Pattern-check one function's assembly.
#
# The course keeps saying "verify this yourself on Compiler Explorer". There is
# no Compiler Explorer in CI, and a claim nobody re-checks rots. So the claims
# that matter are compiled to assembly at -O2 by acpp_asm_probe() and asserted
# here, on this machine, with this compiler.
#
#   cmake -DASM=<file.s> -DSYMBOL=<name>
#         [-DREQUIRE=<regex>[;<regex>...]] [-DFORBID=<regex>[;<regex>...]]
#         [-DMAX_LINES=<n>] [-DLABEL=<text>]
#         -P cmake/check_asm.cmake
#
# Patterns match against the function body only, one line at a time. A failure
# prints the whole body -- when a codegen assumption breaks you want to see what
# the compiler actually did, not just that it differed.

if(NOT DEFINED ASM OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "check_asm: ASM and SYMBOL are required")
endif()

if(NOT EXISTS "${ASM}")
    message(FATAL_ERROR "check_asm: no such assembly file: ${ASM}")
endif()

if(NOT DEFINED LABEL)
    set(LABEL "${SYMBOL}")
endif()

# Patterns arrive @@-joined; see acpp_asm_test() in the root CMakeLists.
string(REPLACE "@@" ";" REQUIRE "${REQUIRE}")
string(REPLACE "@@" ";" FORBID "${FORBID}")

file(STRINGS "${ASM}" lines)

set(body "")
set(count 0)
set(inside OFF)
set(found OFF)

foreach(line IN LISTS lines)
    if(inside)
        if(line MATCHES "^[ \t]*\\.size[ \t]+${SYMBOL}[,\t ]" OR line MATCHES "^[ \t]*\\.cfi_endproc")
            set(inside OFF)
        else()
            # Assembler bookkeeping and comments: not code, and noisy. The two
            # compilers spell a surprising amount of this differently -- clang
            # tags every label with a `# @name` comment and uses .Lfunc_end,
            # gcc uses .LFE -- so the filter has to cover both.
            if(NOT line MATCHES "^[ \t]*\\.(cfi_|file|loc|p2align|align|type|size|globl|weak|hidden|protected|section|text|data|ident|addrsig|LF|Lfunc|def|scl|endef|seh_)"
               AND NOT line MATCHES "^[ \t]*[#/]"
               AND NOT line MATCHES "^[ \t]*$"
               AND NOT line MATCHES "^\\.L")
                list(APPEND body "${line}")
                math(EXPR count "${count} + 1")
            endif()
        endif()
    # gcc:   `acpp_probe_name_data:`
    # clang: `acpp_probe_name_data:                # @acpp_probe_name_data`
    elseif(line MATCHES "^${SYMBOL}:([ \t]|$)")
        set(inside ON)
        set(found ON)
    endif()
endforeach()

if(NOT found)
    message(FATAL_ERROR "check_asm: symbol '${SYMBOL}' not found in ${ASM}")
endif()

string(REPLACE ";" "\n    " printable "${body}")

set(failures "")

foreach(pattern IN LISTS REQUIRE)
    if(pattern STREQUAL "")
        continue()
    endif()
    set(hit OFF)
    foreach(line IN LISTS body)
        if(line MATCHES "${pattern}")
            set(hit ON)
            break()
        endif()
    endforeach()
    if(NOT hit)
        list(APPEND failures "required pattern never matched: ${pattern}")
    endif()
endforeach()

foreach(pattern IN LISTS FORBID)
    if(pattern STREQUAL "")
        continue()
    endif()
    foreach(line IN LISTS body)
        if(line MATCHES "${pattern}")
            list(APPEND failures "forbidden pattern matched: ${pattern}  <-- ${line}")
        endif()
    endforeach()
endforeach()

if(DEFINED MAX_LINES AND count GREATER MAX_LINES)
    list(APPEND failures "body is ${count} instructions, budget was ${MAX_LINES}")
endif()

if(failures)
    string(REPLACE ";" "\n  - " printable_failures "${failures}")
    message(FATAL_ERROR
        "  FAIL  ${LABEL}\n"
        "  - ${printable_failures}\n"
        "  body of ${SYMBOL} (${count} instructions):\n"
        "    ${printable}\n")
endif()

message(STATUS "  ok    ${LABEL} (${count} instructions)")
