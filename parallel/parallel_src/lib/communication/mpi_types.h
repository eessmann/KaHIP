//
// Created by Erich Essmann on 03/09/2024.
//

// mpi_datatype_trait.h
#pragma once

#include <mpi.h>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <mutex>

#include <cista/reflection/arity.h>
#include <cista/reflection/for_each_field.h>
#include <cista/serialization.h>

namespace mpi {
namespace details {
// Define an enum to represent the kind of MPI data
enum class mpi_data_kinds { none, base, composite };

// Trait to determine the MPI data kind
template <typename DataType>
struct mpi_data_kind_trait {
	static constexpr mpi_data_kinds kind = mpi_data_kinds::none;
};

// Mutex for thread safety; needed for the cleanup function
inline std::mutex mpi_type_mutex;

// Function to get the list of custom MPI datatypes
inline std::vector<MPI_Datatype>& get_custom_mpi_types() {
	static std::vector<MPI_Datatype> custom_types;
	return custom_types;
}

// Declare the keyval for the attribute
inline constinit int mpi_type_cleanup_keyval = MPI_KEYVAL_INVALID;

// Cleanup function called during MPI_Finalize
inline int mpi_type_cleanup(MPI_Comm comm,
							int keyval,
							void* attribute_val,
							void* extra_state) {
	std::scoped_lock lock{mpi_type_mutex};

	auto& custom_types = get_custom_mpi_types();
	for (MPI_Datatype& datatype : custom_types) {
		if (datatype != MPI_DATATYPE_NULL) {
			MPI_Type_free(&datatype);
			datatype = MPI_DATATYPE_NULL;
		}
	}
	custom_types.clear();

	// Free the keyval
	MPI_Comm_free_keyval(&mpi_type_cleanup_keyval);
	mpi_type_cleanup_keyval = MPI_KEYVAL_INVALID;

	return MPI_SUCCESS;
}

// Initialize the attribute and set the cleanup function
inline void initialize_mpi_type_cleanup() {
	// Use a static function-local variable to ensure initialization occurs only
	// once
	static bool initialized = []() {
		int mpi_error = MPI_Comm_create_keyval(MPI_NULL_COPY_FN, mpi_type_cleanup, &mpi_type_cleanup_keyval, nullptr);
		if (mpi_error != MPI_SUCCESS) {
			throw std::runtime_error("MPI_Comm_create_keyval() failed");
		}

		// Set an attribute on MPI_COMM_SELF
		mpi_error =
				MPI_Comm_set_attr(MPI_COMM_SELF, mpi_type_cleanup_keyval, nullptr);
		if (mpi_error != MPI_SUCCESS) {
			throw std::runtime_error("MPI_Comm_set_attr() failed");
		}

		return true;
	}();
	(void)initialized;  // Suppress unused variable warning
}

// Detect aggregate types and set kind to composite
template <typename T>
	requires std::is_aggregate_v<T>
struct mpi_data_kind_trait<T> {
	static constexpr mpi_data_kinds kind = mpi_data_kinds::composite;
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
MPI_BASE_TYPE_KIND(signed char)
MPI_BASE_TYPE_KIND(unsigned char)
MPI_BASE_TYPE_KIND(short)
MPI_BASE_TYPE_KIND(unsigned short)
MPI_BASE_TYPE_KIND(int)
MPI_BASE_TYPE_KIND(unsigned int)
MPI_BASE_TYPE_KIND(long)
MPI_BASE_TYPE_KIND(unsigned long)
MPI_BASE_TYPE_KIND(long long)
MPI_BASE_TYPE_KIND(unsigned long long)
MPI_BASE_TYPE_KIND(float)
MPI_BASE_TYPE_KIND(double)
MPI_BASE_TYPE_KIND(long double)
MPI_BASE_TYPE_KIND(bool)
MPI_BASE_TYPE_KIND(std::complex<double>)

#undef MPI_BASE_TYPE_KIND

// Trait to get the MPI_Datatype for a given DataType
template <typename DataType>
struct mpi_datatype_trait;

// Macro to specialize mpi_datatype_trait for unique base types
#define MPI_DATATYPE_TRAIT(type, mpi_type_const)	\
	template <>										\
	struct mpi_datatype_trait<type> {				\
		static MPI_Datatype get_mpi_type() {		\
			return mpi_type_const;					\
		}											\
	};

// Map unique base types to MPI_Datatypes
MPI_DATATYPE_TRAIT(char, MPI_CHAR)
MPI_DATATYPE_TRAIT(wchar_t, MPI_WCHAR)
MPI_DATATYPE_TRAIT(signed char, MPI_SIGNED_CHAR)
MPI_DATATYPE_TRAIT(unsigned char, MPI_UNSIGNED_CHAR)
MPI_DATATYPE_TRAIT(short, MPI_SHORT)
MPI_DATATYPE_TRAIT(unsigned short, MPI_UNSIGNED_SHORT)
MPI_DATATYPE_TRAIT(int, MPI_INT)
MPI_DATATYPE_TRAIT(unsigned int, MPI_UNSIGNED)
MPI_DATATYPE_TRAIT(long, MPI_LONG)
MPI_DATATYPE_TRAIT(unsigned long, MPI_UNSIGNED_LONG)
MPI_DATATYPE_TRAIT(long long, MPI_LONG_LONG_INT)
MPI_DATATYPE_TRAIT(unsigned long long, MPI_UNSIGNED_LONG_LONG)
MPI_DATATYPE_TRAIT(float, MPI_FLOAT)
MPI_DATATYPE_TRAIT(double, MPI_DOUBLE)
MPI_DATATYPE_TRAIT(long double, MPI_LONG_DOUBLE)
MPI_DATATYPE_TRAIT(bool, MPI_CXX_BOOL)
MPI_DATATYPE_TRAIT(std::complex<double>, MPI_CXX_DOUBLE_COMPLEX)

#undef MPI_DATATYPE_TRAIT

}  // namespace details

// Concept to check if a type is a native MPI datatype
template <typename DataType>
concept mpi_native_datatype = (details::mpi_data_kind_trait<DataType>::kind == details::mpi_data_kinds::base);

template <typename DataType>
concept mpi_composite_datatype =
		(details::mpi_data_kind_trait<DataType>::kind == details::mpi_data_kinds::composite);

// mpi_datatype concept combines native and composite datatypes
template <typename DataType>
concept mpi_datatype =
		mpi_native_datatype<DataType> || mpi_composite_datatype<DataType>;

template <typename DataType>
auto get_mpi_datatype() -> MPI_Datatype {
	if constexpr (mpi_native_datatype<DataType>) {
		return details::mpi_datatype_trait<DataType>::get_mpi_type();
	} else if constexpr (mpi_composite_datatype<DataType>) {
		return details::mpi_datatype_trait<DataType>::get_mpi_type();
	} else if constexpr (std::is_same_v<DataType, int8_t>) {
		// int8_t may be the same as signed char
		return get_mpi_datatype<signed char>();
	} else if constexpr (std::is_same_v<DataType, uint8_t>) {
		// uint8_t may be the same as unsigned char
		return get_mpi_datatype<unsigned char>();
	} else if constexpr (std::is_same_v<DataType, int16_t>) {
		// int16_t may be the same as short
		return get_mpi_datatype<short>();
	} else if constexpr (std::is_same_v<DataType, uint16_t>) {
		// uint16_t may be the same as unsigned short
		return get_mpi_datatype<unsigned short>();
	} else if constexpr (std::is_same_v<DataType, int32_t>) {
		// int32_t may be the same as int or long
		if constexpr (sizeof(int32_t) == sizeof(int)) {
			return get_mpi_datatype<int>();
		} else if constexpr (sizeof(int32_t) == sizeof(long)) {
			return get_mpi_datatype<long>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported 32-bit integer type");
		}
	} else if constexpr (std::is_same_v<DataType, uint32_t>) {
		// uint32_t may be the same as unsigned int or unsigned long
		if constexpr (sizeof(uint32_t) == sizeof(unsigned int)) {
			return get_mpi_datatype<unsigned int>();
		} else if constexpr (sizeof(uint32_t) == sizeof(unsigned long)) {
			return get_mpi_datatype<unsigned long>();
		} else {
			static_assert(sizeof(DataType) == 0,
										"Unsupported 32-bit unsigned integer type");
		}
	} else if constexpr (std::is_same_v<DataType, int64_t>) {
		// int64_t may be the same as long or long long
		if constexpr (sizeof(int64_t) == sizeof(long)) {
			return get_mpi_datatype<long>();
		} else if constexpr (sizeof(int64_t) == sizeof(long long)) {
			return get_mpi_datatype<long long>();
		} else {
			static_assert(sizeof(DataType) == 0, "Unsupported 64-bit integer type");
		}
	} else if constexpr (std::is_same_v<DataType, uint64_t>) {
		// uint64_t may be the same as unsigned long or unsigned long long
		if constexpr (sizeof(uint64_t) == sizeof(unsigned long)) {
			return get_mpi_datatype<unsigned long>();
		} else if constexpr (sizeof(uint64_t) == sizeof(unsigned long long)) {
			return get_mpi_datatype<unsigned long long>();
		} else {
			static_assert(sizeof(DataType) == 0,
										"Unsupported 64-bit unsigned integer type");
		}
	} else {
		static_assert(sizeof(DataType) == 0,
									"Unsupported data type for MPI communication");
		return MPI_DATATYPE_NULL;  // This line will never be reached due to
															 // static_assert
	}
}

// Handle composite types using reflection
template <typename DataType>
	requires mpi_composite_datatype<DataType>
struct mpi::details::mpi_datatype_trait<DataType> {
	static MPI_Datatype get_mpi_type() {
		// Function-local static variable initialized via lambda
		static MPI_Datatype mpi_type = []() -> MPI_Datatype {
			MPI_Datatype mpi_type = MPI_DATATYPE_NULL;

			constexpr size_t num_fields = cista::arity<DataType>();
			std::vector<int> block_lengths(num_fields, 1);
			std::vector<MPI_Datatype> types;
			std::vector<MPI_Aint> offsets;

			DataType const tmp{};

			cista::for_each_field(tmp, [&types, &offsets, &tmp](auto&& field) {
				using field_type = std::decay_t<decltype(field)>;
				types.push_back(mpi::get_mpi_datatype<field_type>());
				auto offset = reinterpret_cast<std::ptrdiff_t>(&field) -
											reinterpret_cast<std::ptrdiff_t>(&tmp);
				offsets.push_back(offset);
			});

			// Create the MPI datatype
			int mpi_error = MPI_Type_create_struct(
					static_cast<int>(num_fields), block_lengths.data(), offsets.data(),
					types.data(), &mpi_type);
			if (mpi_error != MPI_SUCCESS) {
				throw std::runtime_error("MPI_Type_create_struct() failed");
			}

			mpi_error = MPI_Type_commit(&mpi_type);
			if (mpi_error != MPI_SUCCESS) {
				throw std::runtime_error("MPI_Type_commit() failed");
			}

			// Register the datatype and initialize cleanup
			{
				std::scoped_lock lock{mpi::details::mpi_type_mutex};
				mpi::details::get_custom_mpi_types().push_back(mpi_type);
				mpi::details::initialize_mpi_type_cleanup();
			}

			return mpi_type;
		}();

		return mpi_type;
	}
};

}  // namespace mpi

// Example struct
struct MyType {
	int a{};
	double b{};
	double c{};

	friend bool operator==(const MyType& lhs, const MyType& rhs) {
		return std::tie(lhs.a, lhs.b, lhs.c) == std::tie(rhs.a, rhs.b, rhs.c);
	}
	friend bool operator!=(const MyType& lhs, const MyType& rhs) {
		return !(lhs == rhs);
	}
};