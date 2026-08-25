#ifndef ACPP_STL_TYPE_TRAITS_HPP
#define ACPP_STL_TYPE_TRAITS_HPP

// Module 4 -- the freestanding seam. One file per standard header we depend on.
//
// The `using` list below IS the dependency manifest: it is the complete,
// auditable inventory of what this library needs from <type_traits>. Drop a
// replacement at acpp/ext/stl/type_traits.hpp on the include path and __has_include
// picks it up -- no build flag, no fork, no patch to rebase.

#if __has_include(<acpp/ext/stl/type_traits.hpp>)
#    include <acpp/ext/stl/type_traits.hpp>
#else
#    include <type_traits>

namespace acpp::stl {

using std::integral_constant;
using std::bool_constant;
using std::true_type;
using std::false_type;
using std::conditional;
using std::conditional_t;
using std::enable_if;
using std::enable_if_t;
using std::is_same;
using std::is_same_v;
using std::is_empty_v;
using std::is_final_v;
using std::is_const_v;
using std::is_pointer_v;
using std::is_enum_v;
using std::is_integral_v;
using std::is_signed_v;
using std::is_unsigned_v;
using std::is_void_v;
using std::is_class_v;
using std::is_function_v;
using std::is_array_v;
using std::is_reference_v;
using std::is_lvalue_reference_v;
using std::is_trivially_copyable_v;
using std::is_trivially_destructible_v;
using std::is_standard_layout_v;
using std::is_default_constructible_v;
using std::is_nothrow_default_constructible_v;
using std::is_copy_constructible_v;
using std::is_move_constructible_v;
using std::is_move_assignable_v;
using std::is_copy_assignable_v;
using std::is_constructible_v;
using std::is_nothrow_constructible_v;
using std::is_nothrow_move_constructible_v;
using std::is_nothrow_move_assignable_v;
using std::is_nothrow_swappable_v;
using std::is_convertible_v;
using std::is_base_of_v;
using std::is_invocable_v;
using std::is_invocable_r_v;
using std::invoke_result_t;
using std::remove_cv_t;
using std::remove_const_t;
using std::remove_reference_t;
using std::remove_cvref_t;
using std::remove_pointer_t;
using std::remove_extent_t;
using std::decay_t;
using std::add_pointer_t;
using std::add_const_t;
using std::add_lvalue_reference_t;
using std::make_unsigned_t;
using std::make_signed_t;
using std::underlying_type_t;
using std::common_type_t;
using std::void_t;
using std::type_identity;
using std::type_identity_t;
using std::aligned_storage_t;
using std::alignment_of_v;

} // namespace acpp::stl

#endif

#endif // ACPP_STL_TYPE_TRAITS_HPP
