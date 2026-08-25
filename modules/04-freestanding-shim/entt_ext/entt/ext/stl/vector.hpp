#ifndef ACPP_MODULE04_ENTT_EXT_STL_VECTOR_HPP
#define ACPP_MODULE04_ENTT_EXT_STL_VECTOR_HPP

// Module 4, exercise 2 -- the replacement EnTT picks up.
//
// EnTT's own src/entt/stl/vector.hpp opens with
//     #if __has_include(<entt/ext/stl/vector.hpp>)
// so putting this file on the include path is the entire integration. No build
// flag, no -D wall, no patch to rebase when the pin moves.
//
// It forwards to the same fixed-capacity vector the acpp seam uses, because
// maintaining two of them would be the thing this pattern exists to avoid.

#include <acpp/ext/stl/vector.hpp>

namespace entt::stl {

using acpp::stl::fixed_vector;

template<typename Type, typename = void>
using vector = acpp::stl::fixed_vector<Type>;

} // namespace entt::stl

#endif
