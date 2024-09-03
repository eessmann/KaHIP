//
// Created by Erich Essmann on 03/09/2024.
//

#ifndef MPI_TYPES_H
#define MPI_TYPES_H
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
          if constexpr (std::is_same_v<DataType, void>) {
            return mpi_data_kinds::base;
          }
        }
    }
}

#endif //MPI_TYPES_H
