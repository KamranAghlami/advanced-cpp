#!/usr/bin/env bash
# Run the CI matrix locally, plus the sanitizer legs.
#
#   scripts/verify.sh            everything
#   scripts/verify.sh quick      gcc/clang x C++20/23 only
#   scripts/verify.sh -R <regex> pass through a ctest filter
#
# Tests labelled "measurement" are excluded: they compile the same TU dozens of
# times to bracket an instantiation depth, which is minutes of work and not a
# pass/fail signal about the change in front of you. Run them with
#   ctest --test-dir build -L measurement
#
# Build trees live under build/verify/<leg> so they do not disturb ./build.
#
# Do not run this concurrently with another build of this project. Every build
# tree shares SOURCE_DIR=third_party/{entt,taskflow}, and CMake's git download
# step wipes and re-clones that directory whenever a tree's stamp file is
# missing -- so a fresh configure here will briefly delete headers another build
# is reading. Legs run sequentially here for the same reason.
#
# The sanitizer legs need reduced ASLR entropy or the runtime aborts at startup
# with "unexpected memory mapping"; `setarch -R` does that per process, which is
# preferable to changing a sysctl on the machine.

set -uo pipefail

cd "$(dirname "$0")/.."
root=$(pwd)
mode=${1:-full}
[[ $mode == quick || $mode == full ]] && shift || mode=full
extra=("$@")

legs_quick=(
    "gcc-20:g++:20:"
    "gcc-23:g++:23:"
    "clang-20:clang++:20:"
    "clang-23:clang++:23:"
)
legs_san=(
    "asan-ubsan:clang++:23:-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    "tsan:clang++:23:-fsanitize=thread -fno-omit-frame-pointer -g"
)

legs=("${legs_quick[@]}")
[[ $mode == full ]] && legs+=("${legs_san[@]}")

failed=()

for leg in "${legs[@]}"; do
    IFS=: read -r name cxx std flags <<< "$leg"

    if ! command -v "$cxx" > /dev/null; then
        echo "SKIP $name ($cxx not installed)"
        continue
    fi

    dir="$root/build/verify/$name"
    log="$dir/verify.log"
    mkdir -p "$dir"

    printf '%-12s ' "$name"

    if ! CXX=$cxx cmake -S "$root" -B "$dir" -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_CXX_STANDARD="$std" \
            -DCMAKE_CXX_FLAGS="$flags" > "$log" 2>&1; then
        echo "FAIL (configure) -- $log"; failed+=("$name"); continue
    fi

    if ! cmake --build "$dir" -j >> "$log" 2>&1; then
        echo "FAIL (build) -- $log"; failed+=("$name"); continue
    fi

    # setarch is Linux-only, and the ASLR conflict it works around is a Linux
    # sanitizer problem. On macOS the runtimes start fine without it.
    runner=()
    if [[ -n $flags && $(uname -s) == Linux ]] && command -v setarch > /dev/null; then
        runner=(setarch "$(uname -m)" -R)
    fi

    if ! UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1 \
            "${runner[@]}" ctest --test-dir "$dir" --output-on-failure -LE measurement "${extra[@]}" >> "$log" 2>&1; then
        echo "FAIL (test) -- $log"; failed+=("$name"); continue
    fi

    echo "ok  ($(grep -oE '[0-9]+ tests passed, [0-9]+ tests failed out of [0-9]+' "$log" | tail -1))"
done

if ((${#failed[@]})); then
    printf 'FAILED: %s\n' "${failed[*]}"
    exit 1
fi

echo "all legs passed"
