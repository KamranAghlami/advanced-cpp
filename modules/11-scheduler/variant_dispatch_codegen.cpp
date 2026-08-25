// Module 11, section 11.1 -- switch-on-index versus std::visit.
//
// The source comment on Taskflow's switch says "switch is faster than nested
// if-else due to jump table", which justifies switch over an *if-else chain*.
// It says nothing about std::visit. The course is explicit that the visit
// comparison is inference rather than source-attested, and that the right
// response is a compiler rather than a memory.
//
// So: both spellings, same work, same TU, and the assembly compared. The
// findings are in NOTES.md.

#include <cstdint>
#include <variant>

namespace {

struct alpha {
    int value;
};

struct beta {
    int value;
};

struct gamma {
    int value;
};

struct delta {
    int value;
};

using node_handle = std::variant<alpha, beta, gamma, delta>;

// Four distinct operations, so the cases cannot be folded into one another.
int handle_alpha(const alpha &v) { return v.value + 1; }
int handle_beta(const beta &v) { return v.value * 2; }
int handle_gamma(const gamma &v) { return v.value - 3; }
int handle_delta(const delta &v) { return v.value / 4; }

} // namespace

extern "C" {

// The Taskflow shape: switch on index(), get_if inside each case. Each case can
// call a dedicated function, which keeps per-kind logic in separately
// optimisable bodies rather than in one visitor.
int acpp_probe_switch_dispatch(const node_handle &handle) {
    switch(handle.index()) {
    case 0u:
        return handle_alpha(*std::get_if<alpha>(&handle));
    case 1u:
        return handle_beta(*std::get_if<beta>(&handle));
    case 2u:
        return handle_gamma(*std::get_if<gamma>(&handle));
    case 3u:
        return handle_delta(*std::get_if<delta>(&handle));
    default:
        return 0;
    }
}

// The idiomatic shape: one overload set, one visit.
int acpp_probe_visit_dispatch(const node_handle &handle) {
    struct visitor {
        int operator()(const alpha &v) const { return handle_alpha(v); }
        int operator()(const beta &v) const { return handle_beta(v); }
        int operator()(const gamma &v) const { return handle_gamma(v); }
        int operator()(const delta &v) const { return handle_delta(v); }
    };

    return std::visit(visitor{}, handle);
}

// The straw man the source comment was actually about.
int acpp_probe_if_else_dispatch(const node_handle &handle) {
    if(const auto *v = std::get_if<alpha>(&handle)) {
        return handle_alpha(*v);
    }
    if(const auto *v = std::get_if<beta>(&handle)) {
        return handle_beta(*v);
    }
    if(const auto *v = std::get_if<gamma>(&handle)) {
        return handle_gamma(*v);
    }
    if(const auto *v = std::get_if<delta>(&handle)) {
        return handle_delta(*v);
    }
    return 0;
}

} // extern "C"
