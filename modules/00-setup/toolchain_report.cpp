// Module 0 -- the instrument check.
//
// The course opens by insisting you set your instruments up before Module 1,
// and this is the part of that which a program can do: report what this machine
// actually is, and FAIL if it is missing something the rest of the repo assumes.
//
// It earns its place twice over. Every concurrency measurement in this repo is
// meaningless without knowing the core count, and every one of them prints it --
// this is where you find it once, before deciding whether a number is worth
// recording. See docs/pending-verification.md.

#include <atomic>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>

#include <acpp/config.hpp>
#include <acpp/testing.hpp>

namespace {

[[nodiscard]] constexpr const char *compiler_name() noexcept {
#if defined __clang__
    return "clang";
#elif defined __GNUC__
    return "gcc";
#elif defined _MSC_VER
    return "msvc";
#else
    return "unknown";
#endif
}

[[nodiscard]] constexpr int compiler_major() noexcept {
#if defined __clang__
    return __clang_major__;
#elif defined __GNUC__
    return __GNUC__;
#else
    return 0;
#endif
}

[[nodiscard]] constexpr const char *sanitizer_name() noexcept {
#if defined __SANITIZE_THREAD__
    return "thread";
#elif defined __SANITIZE_ADDRESS__
    return "address";
#elif defined __has_feature
#    if __has_feature(thread_sanitizer)
    return "thread";
#    elif __has_feature(address_sanitizer)
    return "address";
#    else
    return "none";
#    endif
#else
    return "none";
#endif
}

} // namespace

int main() {
    acpp::testing::suite suite{"module 00 / toolchain_report"};

    // --- the number every concurrency result in this repo hangs on ----------
    const auto cores = std::thread::hardware_concurrency();

    suite.note("hardware_concurrency() = %u", cores);
    suite.note("compiler               = %s %d, __cplusplus = %ldL", compiler_name(),
               compiler_major(), static_cast<long>(__cplusplus));
    suite.note("sanitizer              = %s", sanitizer_name());
    suite.note("sizeof(void *)         = %zu, CHAR_BIT = %d, endian = %s", sizeof(void *), CHAR_BIT,
               std::endian::native == std::endian::little ? "little" : "big");

#if defined __cpp_lib_hardware_interference_size
    suite.note("hardware_destructive_interference_size = %zu (ACPP_CACHELINE_SIZE = %d)",
               std::hardware_destructive_interference_size, ACPP_CACHELINE_SIZE);
#else
    suite.note("hardware_destructive_interference_size unavailable; ACPP_CACHELINE_SIZE = %d",
               ACPP_CACHELINE_SIZE);
#endif

    if(cores < 2u) {
        suite.note("SINGLE CORE: nothing measured on this machine supports a claim about "
                   "parallel speedup, steal rate or load imbalance. See "
                   "docs/pending-verification.md.");
    } else {
        suite.note("%u cores: the measurements listed in docs/pending-verification.md can be "
                   "re-taken here.", cores);
    }

    // --- assumptions the rest of the repo would be silently wrong without ---

    suite.check(CHAR_BIT == 8, "octets (the bit-packing in Modules 5 and 11 assumes it)");

    // Not pedantry. std::atomic falls back to a lock table when a type is not
    // lock-free, and a "lock-free" queue backed by a hidden mutex would still
    // pass every test in this repo while invalidating every claim Module 9
    // makes about it. Worth failing loudly rather than measuring quietly.
    //
    // is_always_lock_free, NOT is_lock_free(). Three reasons, and the first is
    // the one that broke a CI build:
    //
    //   * is_lock_free() is a runtime call into __atomic_is_lock_free, which
    //     lives in libatomic. gcc links that implicitly; clang does not, so the
    //     member function is an undefined reference at link time.
    //   * is_always_lock_free is a static constexpr bool -- no call, no library.
    //   * it is the stricter question. "Always, for this type on this target" is
    //     what the queue needs; is_lock_free() only answers for one object.
    suite.check(std::atomic<std::int64_t>::is_always_lock_free,
                "atomic<int64_t> is lock-free (Module 9's top/bottom counters)");
    suite.check(std::atomic<std::uint64_t>::is_always_lock_free,
                "atomic<uint64_t> is lock-free (Module 10's packed state word)");
    suite.check(std::atomic<void *>::is_always_lock_free,
                "atomic<T *> is lock-free (Module 9's buffer pointer, Module 10's waiter stack)");
    suite.check(std::atomic<std::size_t>::is_always_lock_free,
                "atomic<size_t> is lock-free (Module 11's join counters)");

#if defined __STDCPP_THREADS__
    suite.check(true, "__STDCPP_THREADS__ is defined (Module 2's counter_traits infers from it)");
#else
    suite.note("__STDCPP_THREADS__ is NOT defined -- counter_traits will infer a non-atomic "
               "counter, which is wrong if this build actually uses threads");
#endif

    suite.check(cores >= 1u, "hardware_concurrency() reported something usable");

    return suite.report();
}
