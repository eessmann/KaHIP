//
// Created by Erich Essmann on 03/09/2024.
//

#ifndef MPI_TYPES_H
#define MPI_TYPES_H
#include <complex>
#include <type_traits>

#include <mpi.h>
namespace mpi {
    namespace detail {
        enum class mpi_data_kinds {
            base,
            composite,
            none
          };

        template<typename DataType>
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
          } else if constexpr (std::is_same_v<DataType,  long long int>) {
              return mpi_data_kinds::base;
          } else if constexpr (std::is_same_v<DataType,  unsigned long long int>) {
              return mpi_data_kinds::base;
          } else {
              return mpi_data_kinds::none;
          }
        }
    }
}

#endif //MPI_TYPES_H
