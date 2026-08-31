#pragma once

#include <mpi.h>

#include <boost/hana/for_each.hpp>
#include <boost/hana/tuple.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_error.h"

namespace parhip::mpi {
namespace detail {
using native_mpi_types = boost::mp11::mp_list<
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

template <typename T>
inline constexpr bool is_native_mpi_type =
    boost::mp11::mp_contains<native_mpi_types, unqualified_t<T>>::value;

template <typename T>
  requires is_native_mpi_type<T>
auto native_mpi_handle() noexcept -> MPI_Datatype {
  constexpr auto index =
      boost::mp11::mp_find<native_mpi_types, unqualified_t<T>>::value;
  auto const handles = std::array{
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
  return handles[index];
}
}  // namespace detail

template <typename T>
concept mpi_native_datatype = detail::is_native_mpi_type<T>;

template <typename T>
struct wire_members;

template <typename T>
concept mpi_wire_datatype =
    std::is_standard_layout_v<detail::unqualified_t<T>> &&
    std::is_trivially_copyable_v<detail::unqualified_t<T>> && requires {
      wire_members<detail::unqualified_t<T>>::value;
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

  [[nodiscard]] static auto borrowed(MPI_Datatype handle) noexcept
      -> datatype {
    return datatype{handle, false};
  }
  [[nodiscard]] static auto owned(MPI_Datatype handle) noexcept -> datatype {
    return datatype{handle, true};
  }

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Datatype {
    return handle_;
  }
  [[nodiscard]] auto owns_handle() const noexcept -> bool { return owns_; }

private:
  explicit datatype(MPI_Datatype handle, bool owns) noexcept
      : handle_(handle), owns_(owns) {}
  void reset() noexcept;

  MPI_Datatype handle_ = MPI_DATATYPE_NULL;
  bool owns_ = false;
};

template <mpi_datatype T>
[[nodiscard]] auto make_mpi_datatype() -> datatype {
  using value_type = detail::unqualified_t<T>;
  if constexpr (mpi_native_datatype<value_type>) {
    return datatype::borrowed(detail::native_mpi_handle<value_type>());
  } else {
    alignas(value_type) std::array<std::byte, sizeof(value_type)> sample_storage;
    auto* sample =
        std::start_lifetime_as<value_type>(sample_storage.data());
    MPI_Aint sample_address = 0;
    check(MPI_Get_address(sample, &sample_address),
          "MPI_Get_address(wire record)");

    std::vector<int> block_lengths;
    std::vector<MPI_Aint> offsets;
    std::vector<MPI_Datatype> member_types;
    boost::hana::for_each(wire_members<value_type>::value, [&](auto member) {
      using member_type = detail::unqualified_t<decltype(sample->*member)>;
      static_assert(mpi_native_datatype<member_type>,
                    "wire record members must use native MPI types");
      MPI_Aint member_address = 0;
      check(MPI_Get_address(std::addressof(sample->*member), &member_address),
            "MPI_Get_address(wire member)");
      block_lengths.push_back(1);
      offsets.push_back(member_address - sample_address);
      member_types.push_back(detail::native_mpi_handle<member_type>());
    });

    MPI_Datatype structure = MPI_DATATYPE_NULL;
    check(MPI_Type_create_struct(static_cast<int>(block_lengths.size()),
                                 block_lengths.data(),
                                 offsets.data(),
                                 member_types.data(),
                                 &structure),
          "MPI_Type_create_struct");

    MPI_Datatype resized = MPI_DATATYPE_NULL;
    auto const resize_result = MPI_Type_create_resized(
        structure, 0, static_cast<MPI_Aint>(sizeof(value_type)), &resized);
    MPI_Type_free(&structure);
    check(resize_result, "MPI_Type_create_resized");

    auto const commit_result = MPI_Type_commit(&resized);
    if (commit_result != MPI_SUCCESS) {
      MPI_Type_free(&resized);
      check(commit_result, "MPI_Type_commit");
    }
    return datatype::owned(resized);
  }
}

template <mpi_native_datatype T>
[[nodiscard]] auto get_mpi_datatype() noexcept -> MPI_Datatype {
  return detail::native_mpi_handle<T>();
}
}  // namespace parhip::mpi
