//
// Created by Erich Essmann on 03/09/2024.
//

// mpi_datatype_trait.h
#pragma once

#include <mpi.h>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>


namespace mpi {
namespace details {
// Define an enum to represent the kind of MPI data
enum class mpi_data_kinds { none, base, composite };

// Trait to determine the MPI data kind
template <typename DataType>
struct mpi_data_kind_trait {
	static constexpr mpi_data_kinds kind = mpi_data_kinds::none;
};

// Macro to specialize mpi_data_kind_trait for unique base types
#define MPI_BASE_TYPE_KIND(type)									 \
	template <>														 \
	struct mpi_data_kind_trait<type> {								 \
		static constexpr mpi_data_kinds kind = mpi_data_kinds::base; \
	};

// Specializations for unique base types
MPI_BASE_TYPE_KIND(char)
MPI_BASE_TYPE_KIND(wchar_t)
MPI_BASE_TYPE_KIND(float)
MPI_BASE_TYPE_KIND(double)
MPI_BASE_TYPE_KIND(long double)
MPI_BASE_TYPE_KIND(bool)
MPI_BASE_TYPE_KIND(int8_t)
MPI_BASE_TYPE_KIND(int16_t)
MPI_BASE_TYPE_KIND(int32_t)
MPI_BASE_TYPE_KIND(int64_t)
MPI_BASE_TYPE_KIND(uint8_t)
MPI_BASE_TYPE_KIND(uint16_t)
MPI_BASE_TYPE_KIND(uint32_t)
MPI_BASE_TYPE_KIND(uint64_t)

#undef MPI_BASE_TYPE_KIND
}  // namespace details
// Concept to check if a type is a native MPI datatype
template <typename DataType>
concept mpi_native_datatype = (details::mpi_data_kind_trait<DataType>::kind ==
															 details::mpi_data_kinds::base);

namespace details {
// Trait to get the MPI_Datatype for a given DataType
template <typename DataType>
struct mpi_datatype_trait;

// Macro to specialize mpi_datatype_trait for unique base types
#define MPI_DATATYPE_TRAIT(type, mpi_type_const) \
	template <>                                  \
	struct mpi_datatype_trait<type> {            \
		static MPI_Datatype get_mpi_type() {     \
			return mpi_type_const;               \
		}                                        \
	};

// Map unique base types to MPI_Datatypes
MPI_DATATYPE_TRAIT(char, MPI_CHAR)
MPI_DATATYPE_TRAIT(wchar_t, MPI_WCHAR)
MPI_DATATYPE_TRAIT(float, MPI_FLOAT)
MPI_DATATYPE_TRAIT(double, MPI_DOUBLE)
MPI_DATATYPE_TRAIT(long double, MPI_LONG_DOUBLE)
MPI_DATATYPE_TRAIT(bool, MPI_CXX_BOOL)
MPI_DATATYPE_TRAIT(int8_t, MPI_INT8_T)
MPI_DATATYPE_TRAIT(int16_t, MPI_INT16_T)
MPI_DATATYPE_TRAIT(int32_t, MPI_INT32_T)
MPI_DATATYPE_TRAIT(int64_t, MPI_INT64_T)
MPI_DATATYPE_TRAIT(uint8_t, MPI_UINT8_T)
MPI_DATATYPE_TRAIT(uint16_t, MPI_UINT16_T)
MPI_DATATYPE_TRAIT(uint32_t, MPI_UINT32_T)
MPI_DATATYPE_TRAIT(uint64_t, MPI_UINT64_T)

#undef MPI_DATATYPE_TRAIT

}  // namespace details

// Function to get the MPI_Datatype for a given DataType
// Concept to check if a type is an MPI composite datatype (for completeness)
template <typename DataType>
concept mpi_composite_datatype =
		(details::mpi_data_kind_trait<DataType>::kind ==
		 details::mpi_data_kinds::composite);

// mpi_datatype concept combines native and composite datatypes
template <typename DataType>
concept mpi_datatype =
		mpi_native_datatype<DataType> || mpi_composite_datatype<DataType>;

template <typename DataType>
auto get_mpi_datatype() -> MPI_Datatype {
	if constexpr (mpi_datatype<DataType>) {
		return details::mpi_datatype_trait<DataType>::get_mpi_type();
	} else if constexpr (std::is_same_v<DataType, signed char>) {
		// int8_t may be the same as signed char
		return get_mpi_datatype<int8_t>();
	} else if constexpr (std::is_same_v<DataType, unsigned char>) {
		// uint8_t may be the same as unsigned char
		return get_mpi_datatype<uint8_t>();
	} else if constexpr (std::is_same_v<DataType, short>) {
		// int16_t may be the same as short
		return get_mpi_datatype<int16_t>();
	} else if constexpr (std::is_same_v<DataType, unsigned short>) {
		// uint16_t may be the same as unsigned short
		return get_mpi_datatype<uint16_t>();
	} else if constexpr (std::is_same_v<DataType, int>) {
		// int32_t may be the same as int or long
		if constexpr (sizeof(DataType) == sizeof(int32_t)) {
			return get_mpi_datatype<int32_t>();
		} else if constexpr (sizeof(DataType) == sizeof(int64_t)) {
			return get_mpi_datatype<int64_t>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported integer type");
		}
	} else if constexpr (std::is_same_v<DataType, unsigned int>) {
		// uint32_t may be the same as unsigned int or unsigned long
		if constexpr (sizeof(DataType) == sizeof(uint32_t)) {
			return get_mpi_datatype<uint32_t>();
		} else if constexpr (sizeof(DataType) == sizeof(uint64_t)) {
			return get_mpi_datatype<uint64_t>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported unsigned integer type");
		}
	} else if constexpr (std::is_same_v<DataType, long>) {
		// int64_t may be the same as long or long long
		if constexpr (sizeof(DataType) == sizeof(int32_t)) {
			return get_mpi_datatype<int32_t>();
		} else if constexpr (sizeof(DataType) == sizeof(int64_t)) {
			return get_mpi_datatype<int64_t>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported integer type");
		}
	} else if constexpr (std::is_same_v<DataType, unsigned long>) {
		// uint64_t may be the same as unsigned long or unsigned long long
		if constexpr (sizeof(DataType) == sizeof(uint32_t)) {
			return get_mpi_datatype<uint32_t>();
		} else if constexpr (sizeof(DataType) == sizeof(uint64_t)) {
			return get_mpi_datatype<uint64_t>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported unsigned integer type");
		}
	} else if constexpr (std::is_same_v<DataType, long long>) {
		// int64_t may be the same as long or long long
		if constexpr (sizeof(DataType) == sizeof(int64_t)) {
			return get_mpi_datatype<int64_t>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported integer type");
		}
	} else if constexpr (std::is_same_v<DataType, unsigned long long>) {
		// uint64_t may be the same as unsigned long or unsigned long long
		if constexpr (sizeof(DataType) == sizeof(uint64_t)) {
			return get_mpi_datatype<uint64_t>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported unsigned integer type");
		}
	} else {
		static_assert(sizeof(DataType) == 0,
									"Unsupported data type for MPI communication");
		return MPI_DATATYPE_NULL;  // This line will never be reached due to
															 // static_assert
	}
	return MPI_DATATYPE_NULL;
}

}  // namespace mpi


struct MyType {
	int a;
	double b;

	friend bool operator==(const MyType& lhs, const MyType& rhs) {
		return std::tie(lhs.a, lhs.b) == std::tie(rhs.a, rhs.b);
	}
	friend bool operator!=(const MyType& lhs, const MyType& rhs) {
		return !(lhs == rhs);
	}
};

// Specialize mpi_data_kind_trait for MyType
namespace mpi::details {
template <>
struct mpi_data_kind_trait<MyType> {
	static constexpr mpi_data_kinds kind = mpi_data_kinds::composite;
};
template <>
struct mpi_datatype_trait<MyType> {
	static MPI_Datatype get_mpi_type() {
		static MPI_Datatype mpi_type = MPI_DATATYPE_NULL;
		if (mpi_type == MPI_DATATYPE_NULL) {
			int block_lengths[2] = {1, 1};
			MPI_Datatype types[2] = {MPI_INT, MPI_DOUBLE};
			MPI_Aint offsets[2];

			offsets[0] = offsetof(MyType, a);
			offsets[1] = offsetof(MyType, b);

			MPI_Type_create_struct(2, block_lengths, offsets, types, &mpi_type);
			MPI_Type_commit(&mpi_type);
		}
		return mpi_type;
	}
};

} // namespace mpi::details
