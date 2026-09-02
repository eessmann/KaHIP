#pragma once

#include <type_traits>

namespace parhip::mpi::detail {
namespace implicit_lifetime_detail {
template <typename T>
inline constexpr bool fallback_is_implicit_lifetime_v = [] {
  using value_type = std::remove_cv_t<T>;

  if constexpr (std::is_array_v<value_type>) {
    return fallback_is_implicit_lifetime_v<std::remove_extent_t<value_type>>;
  } else if constexpr (std::is_scalar_v<value_type>) {
    return true;
  } else if constexpr (std::is_class_v<value_type> ||
                       std::is_union_v<value_type>) {
    // P2674: an implicit-lifetime class is either an aggregate with a
    // non-user-provided destructor, or has a trivial eligible constructor and
    // a trivial, non-deleted destructor. Within KaHIP's trivially-copyable
    // storage contract, trivially destructible is a conservative proxy for
    // the aggregate destructor rule. The constructibility traits are likewise
    // conservative because inaccessible trivial constructors report false.
    return std::is_trivially_destructible_v<value_type> &&
           (std::is_aggregate_v<value_type> ||
            std::is_trivially_default_constructible_v<value_type> ||
            std::is_trivially_copy_constructible_v<value_type> ||
            std::is_trivially_move_constructible_v<value_type>);
  } else {
    return false;
  }
}();
}  // namespace implicit_lifetime_detail

template <typename T>
inline constexpr bool is_implicit_lifetime_v =
#if defined(__cpp_lib_is_implicit_lifetime) && \
    __cpp_lib_is_implicit_lifetime >= 202302L
    std::is_implicit_lifetime_v<std::remove_cv_t<T>>;
#else
    implicit_lifetime_detail::fallback_is_implicit_lifetime_v<T>;
#endif
}  // namespace parhip::mpi::detail
