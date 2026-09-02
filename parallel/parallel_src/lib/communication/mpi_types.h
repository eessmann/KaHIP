#pragma once

#include <mpi.h>

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/implicit_lifetime.h"
#include "communication/mpi_error.h"
#include "communication/mpi_failure.h"

namespace parhip::mpi {
namespace detail {
using native_mpi_types = std::tuple<
    char,
    wchar_t,
    signed char,
    unsigned char,
    short,
    unsigned short,
    int,
    unsigned int,
    long,
    unsigned long,
    long long,
    unsigned long long,
    float,
    double,
    long double,
    bool,
    std::complex<float>,
    std::complex<double>,
    std::complex<long double>>;

template <typename T>
using unqualified_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T, typename Tuple>
struct tuple_contains;

template <typename T, typename... Elements>
struct tuple_contains<T, std::tuple<Elements...>>
    : std::bool_constant<(std::is_same_v<T, Elements> || ...)> {};

template <typename T, typename Tuple>
inline constexpr bool tuple_contains_v =
    tuple_contains<T, unqualified_t<Tuple>>::value;

template <typename T, typename Tuple>
struct tuple_index;

template <typename T, typename... Tail>
struct tuple_index<T, std::tuple<T, Tail...>>
    : std::integral_constant<std::size_t, 0> {};

template <typename T, typename Head, typename... Tail>
struct tuple_index<T, std::tuple<Head, Tail...>>
    : std::integral_constant<
          std::size_t,
          1 + tuple_index<T, std::tuple<Tail...>>::value> {};

template <typename T, typename Tuple>
inline constexpr std::size_t tuple_index_v =
    tuple_index<T, unqualified_t<Tuple>>::value;

template <typename T>
inline constexpr bool is_native_mpi_type =
    tuple_contains_v<unqualified_t<T>, native_mpi_types>;

inline auto const native_mpi_handles = std::array{
    MPI_CHAR,
    MPI_WCHAR,
    MPI_SIGNED_CHAR,
    MPI_UNSIGNED_CHAR,
    MPI_SHORT,
    MPI_UNSIGNED_SHORT,
    MPI_INT,
    MPI_UNSIGNED,
    MPI_LONG,
    MPI_UNSIGNED_LONG,
    MPI_LONG_LONG_INT,
    MPI_UNSIGNED_LONG_LONG,
    MPI_FLOAT,
    MPI_DOUBLE,
    MPI_LONG_DOUBLE,
    MPI_CXX_BOOL,
    MPI_CXX_FLOAT_COMPLEX,
    MPI_CXX_DOUBLE_COMPLEX,
    MPI_CXX_LONG_DOUBLE_COMPLEX};

static_assert(std::tuple_size_v<native_mpi_types> ==
              std::tuple_size_v<unqualified_t<decltype(native_mpi_handles)>>,
              "native MPI types and handles must remain aligned");

template <typename T>
  requires is_native_mpi_type<T>
auto native_mpi_handle() noexcept -> MPI_Datatype {
  constexpr auto index =
      tuple_index_v<unqualified_t<T>, native_mpi_types>;
  return native_mpi_handles[index];
}

template <typename Record, typename Members>
struct tuple_members_are_native;

template <typename Record, typename... MemberPointers>
struct tuple_members_are_native<Record, std::tuple<MemberPointers...>>
    : std::bool_constant<
          (is_native_mpi_type<decltype(std::declval<Record&>().*
                                       std::declval<MemberPointers>())> &&
           ...)> {};

template <typename Record, typename Members>
inline constexpr bool tuple_members_are_native_v =
    tuple_members_are_native<Record, unqualified_t<Members>>::value;

template <typename T>
struct aligned_object_delete {
  void operator()(T* object) const noexcept {
    ::operator delete(object, std::align_val_t{alignof(T)});
  }
};
}  // namespace detail

template <typename T>
concept mpi_native_datatype = detail::is_native_mpi_type<T>;

template <typename T>
struct wire_members;

template <typename T>
concept mpi_wire_datatype =
    std::is_standard_layout_v<detail::unqualified_t<T>> &&
    std::is_trivially_copyable_v<detail::unqualified_t<T>> && requires {
      requires detail::is_implicit_lifetime_v<detail::unqualified_t<T>>;
      wire_members<detail::unqualified_t<T>>::value;
      requires detail::tuple_members_are_native_v<
          detail::unqualified_t<T>,
          decltype(wire_members<detail::unqualified_t<T>>::value)>;
    };

template <typename T>
concept mpi_datatype = mpi_native_datatype<T> || mpi_wire_datatype<T>;

class datatype {
public:
  ~datatype() noexcept;

  datatype(datatype const&) = delete;
  auto operator=(datatype const&) -> datatype& = delete;
  datatype(datatype&& other) noexcept;
  auto operator=(datatype&& other) noexcept -> datatype&;

  [[nodiscard]] static auto borrowed(
      MPI_Datatype handle,
      MPI_Comm failure_communicator = MPI_COMM_WORLD) noexcept
      -> datatype {
    return datatype{handle, false, failure_communicator};
  }
  [[nodiscard]] static auto owned(
      MPI_Datatype handle,
      MPI_Comm failure_communicator = MPI_COMM_WORLD) noexcept -> datatype {
    return datatype{handle, true, failure_communicator};
  }

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Datatype {
    return handle_;
  }
  [[nodiscard]] auto owns_handle() const noexcept -> bool { return owns_; }

private:
  explicit datatype(MPI_Datatype handle,
                    bool owns,
                    MPI_Comm failure_communicator) noexcept
      : handle_(handle),
        owns_(owns),
        failure_communicator_(failure_communicator) {}
  void reset() noexcept;

  MPI_Datatype handle_ = MPI_DATATYPE_NULL;
  bool owns_ = false;
  MPI_Comm failure_communicator_ = MPI_COMM_WORLD;
};

template <mpi_datatype T>
[[nodiscard]] auto make_mpi_datatype(
    MPI_Comm failure_communicator = MPI_COMM_WORLD) -> datatype {
  using value_type = detail::unqualified_t<T>;
  if constexpr (mpi_native_datatype<value_type>) {
    return datatype::borrowed(
        detail::native_mpi_handle<value_type>(), failure_communicator);
  } else {
    auto sample =
        std::unique_ptr<value_type, detail::aligned_object_delete<value_type>>{
            static_cast<value_type*>(::operator new(
                sizeof(value_type), std::align_val_t{alignof(value_type)}))};
    MPI_Aint sample_address = 0;
    check_or_abort(MPI_Get_address(sample.get(), &sample_address),
                   failure_communicator,
                   "MPI_Get_address(wire record)");

    std::vector<int> block_lengths;
    std::vector<MPI_Aint> offsets;
    std::vector<MPI_Datatype> member_types;
    constexpr auto member_count = std::tuple_size_v<detail::unqualified_t<
        decltype(wire_members<value_type>::value)>>;
    block_lengths.reserve(member_count);
    offsets.reserve(member_count);
    member_types.reserve(member_count);

    auto append_member = [&](auto member) {
      using member_type =
          detail::unqualified_t<decltype(sample.get()->*member)>;
      static_assert(mpi_native_datatype<member_type>,
                    "wire record members must use native MPI types");
      MPI_Aint member_address = 0;
      check_or_abort(
          MPI_Get_address(std::addressof(sample.get()->*member),
                          &member_address),
          failure_communicator,
          "MPI_Get_address(wire member)");
      block_lengths.push_back(1);
      offsets.push_back(member_address - sample_address);
      member_types.push_back(detail::native_mpi_handle<member_type>());
    };
    std::apply(
        [&](auto... members) { (append_member(members), ...); },
        wire_members<value_type>::value);

    MPI_Datatype structure = MPI_DATATYPE_NULL;
    check_or_abort(MPI_Type_create_struct(
                       static_cast<int>(block_lengths.size()),
                       block_lengths.data(),
                       offsets.data(),
                       member_types.data(),
                       &structure),
                   failure_communicator,
                   "MPI_Type_create_struct");

    MPI_Datatype resized = MPI_DATATYPE_NULL;
    check_or_abort(MPI_Type_create_resized(
                       structure,
                       0,
                       static_cast<MPI_Aint>(sizeof(value_type)),
                       &resized),
                   failure_communicator,
                   "MPI_Type_create_resized");
    check_or_abort(MPI_Type_free(&structure),
                   failure_communicator,
                   "MPI_Type_free(wire structure)");
    check_or_abort(MPI_Type_commit(&resized),
                   failure_communicator,
                   "MPI_Type_commit");
    return datatype::owned(resized, failure_communicator);
  }
}

template <mpi_native_datatype T>
[[nodiscard]] auto get_mpi_datatype() noexcept -> MPI_Datatype {
  return detail::native_mpi_handle<T>();
}
}  // namespace parhip::mpi
