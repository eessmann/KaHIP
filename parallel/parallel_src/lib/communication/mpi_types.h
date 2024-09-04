//
// Created by Erich Essmann on 03/09/2024.
//

#ifndef MPI_TYPES_H
#define MPI_TYPES_H
#include <concepts>
#include <type_traits>

#include <mpi.h>

namespace mpi {
namespace details {
enum class mpi_data_kinds { none, base, composite };

template <typename DataType>
constexpr auto get_data_kind() -> mpi_data_kinds {
  if constexpr (std::is_same_v<DataType, char>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, wchar_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, signed short>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, signed int>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, signed long>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, signed char>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, unsigned char>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, unsigned short>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, unsigned int>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, unsigned long int>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, float>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, double>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, long double>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, bool>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, int8_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, int16_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, int32_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, int64_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, uint8_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, uint16_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, uint32_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, uint64_t>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, long long int>) {
    return mpi_data_kinds::base;
  } else if constexpr (std::is_same_v<DataType, unsigned long long int>) {
    return mpi_data_kinds::base;
  } else {
    return mpi_data_kinds::none;
  }
}
}  // namespace detail

template <typename DataType>
concept mpi_native_datatype = requires {
  details::get_data_kind<DataType>() == details::mpi_data_kinds::base;
};

template <typename DataType>
concept mpi_composite_datatype = requires {
  details::get_data_kind<DataType>() == details::mpi_data_kinds::composite;
};


template <mpi_native_datatype DataType>
constexpr auto get_mpi_native_datatype() -> MPI_Datatype {
    if constexpr (std::is_same_v<DataType, char>) {
		return MPI_CHAR;
	} else if constexpr (std::is_same_v<DataType, wchar_t>) {
		return MPI_WCHAR;
	} else if constexpr (std::is_same_v<DataType, signed short>) {
		return MPI_SHORT;
	} else if constexpr (std::is_same_v<DataType, signed int>) {
		return MPI_INT;
	} else if constexpr (std::is_same_v<DataType, signed long>) {
		return MPI_LONG;
	} else if constexpr (std::is_same_v<DataType, signed char>) {
		return MPI_SIGNED_CHAR;
	} else if constexpr (std::is_same_v<DataType, unsigned char>) {
		return MPI_UNSIGNED_CHAR;
	} else if constexpr (std::is_same_v<DataType, unsigned short>) {
		return MPI_UNSIGNED_SHORT;
	} else if constexpr (std::is_same_v<DataType, unsigned int>) {
		return MPI_UNSIGNED;
	} else if constexpr (std::is_same_v<DataType, unsigned long int>) {
		return MPI_UNSIGNED_LONG;
	} else if constexpr (std::is_same_v<DataType, float>) {
		return MPI_FLOAT;
	} else if constexpr (std::is_same_v<DataType, double>) {
		return MPI_DOUBLE;
	} else if constexpr (std::is_same_v<DataType, long double>) {
		return MPI_LONG_DOUBLE;
	} else if constexpr (std::is_same_v<DataType, bool>) {
		return MPI_CXX_BOOL;
	} else if constexpr (std::is_same_v<DataType, int8_t>) {
		return MPI_INT8_T;
	} else if constexpr (std::is_same_v<DataType, int16_t>) {
		return MPI_INT16_T;
	} else if constexpr (std::is_same_v<DataType, int32_t>) {
		return MPI_INT32_T;
	} else if constexpr (std::is_same_v<DataType, int64_t>) {
		return MPI_INT64_T;
	} else if constexpr (std::is_same_v<DataType, uint8_t>) {
		return MPI_UINT8_T;
	} else if constexpr (std::is_same_v<DataType, uint16_t>) {
		return MPI_UINT16_T;
	} else if constexpr (std::is_same_v<DataType, uint32_t>) {
		return MPI_UINT32_T;
	} else if constexpr (std::is_same_v<DataType, uint64_t>) {
		return MPI_UINT64_T;
	} else if constexpr (std::is_same_v<DataType, long long int>) {
		return MPI_LONG_LONG_INT;
	} else if constexpr (std::is_same_v<DataType, unsigned long long int>) {
	  return MPI_UNSIGNED_LONG_LONG;
	} else {
	  return MPI_DATATYPE_NULL;
	};
}

template <mpi_composite_datatype DataType>
constexpr auto get_mpi_composite_datatype() -> MPI_Datatype;

template <typename DataType>
concept mpi_datatype = mpi_native_datatype<DataType> || mpi_composite_datatype<DataType>;

template <typename ContainerType>
concept container = requires(ContainerType a, ContainerType const b) {
  requires std::regular<ContainerType>;
  requires std::swappable<ContainerType>;
  requires std::destructible<typename ContainerType::value_type>;
  requires std::same_as<typename ContainerType::reference,
			typename ContainerType::value_type&>;
  requires std::same_as<typename ContainerType::const_reference,
			const typename ContainerType::value_type&>;
  requires std::forward_iterator<typename ContainerType::iterator>;
  requires std::forward_iterator<typename ContainerType::const_iterator>;
  requires std::signed_integral<typename ContainerType::difference_type>;
  requires std::same_as<typename ContainerType::difference_type,
			typename std::iterator_traits<
			    typename ContainerType::iterator>::difference_type>;
  requires std::same_as<
      typename ContainerType::difference_type,
      typename std::iterator_traits<
	  typename ContainerType::const_iterator>::difference_type>;
  { a.begin() } -> std::same_as<typename ContainerType::iterator>;
  { a.end() } -> std::same_as<typename ContainerType::iterator>;
  { b.begin() } -> std::same_as<typename ContainerType::const_iterator>;
  { b.end() } -> std::same_as<typename ContainerType::const_iterator>;
  { a.cbegin() } -> std::same_as<typename ContainerType::const_iterator>;
  { a.cend() } -> std::same_as<typename ContainerType::const_iterator>;
  { a.size() } -> std::same_as<typename ContainerType::size_type>;
  { a.max_size() } -> std::same_as<typename ContainerType::size_type>;
  { a.empty() } -> std::same_as<bool>;
};

template <mpi_datatype DataType>
constexpr auto get_mpi_datatype() -> MPI_Datatype {
  if constexpr (details::get_data_kind<DataType>() == details::mpi_data_kinds::base) {
    return get_mpi_native_datatype<DataType>();
  } else if constexpr (details::get_data_kind<DataType>() == details::mpi_data_kinds::composite) {
    return get_mpi_composite_datatype<DataType>(); // Point of customization for user-defined types
  } else {
    return MPI_PACKED; // Fallback to serialization of object
  }

};

template <typename ContainerType>
concept mpi_container = container<ContainerType> &&
			mpi_datatype<typename ContainerType::value_type>;
}  // namespace mpi

#endif  // MPI_TYPES_H
