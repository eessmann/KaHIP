#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <span>

#include "communication/mpi_fixed_broadcast.h"
#include "definitions.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace fixed_broadcast_probe {
enum class backend : unsigned char { legacy, large_count };

struct operation final {
  backend selected_backend = backend::legacy;
  MPI_Count count = 0;
  MPI_Datatype datatype = MPI_DATATYPE_NULL;
  int root = -1;
  MPI_Comm communicator = MPI_COMM_NULL;
};

struct counters final {
  std::array<operation, 8> operations{};
  int operation_count = 0;
  bool overflow = false;
};

inline bool active = false;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline counters observed{};

void reset(MPI_Comm communicator) noexcept {
  expected_communicator = communicator;
  observed = {};
}

void record(backend selected_backend,
            MPI_Count count,
            MPI_Datatype datatype,
            int root,
            MPI_Comm communicator) noexcept {
  if (!active) {
    return;
  }
  if (observed.operation_count >=
      static_cast<int>(observed.operations.size())) {
    observed.overflow = true;
    return;
  }
  observed.operations[static_cast<std::size_t>(observed.operation_count++)] =
      operation{.selected_backend = selected_backend,
                .count = count,
                .datatype = datatype,
                .root = root,
                .communicator = communicator};
}

class activation final {
 public:
  explicit activation(MPI_Comm communicator) noexcept {
    reset(communicator);
    active = true;
  }

  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace fixed_broadcast_probe

static_assert(noexcept(fixed_broadcast_probe::reset(MPI_COMM_NULL)));
static_assert(noexcept(
    fixed_broadcast_probe::record(fixed_broadcast_probe::backend::legacy,
                                  0,
                                  MPI_DATATYPE_NULL,
                                  0,
                                  MPI_COMM_NULL)));

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  fixed_broadcast_probe::record(fixed_broadcast_probe::backend::legacy, count,
                                datatype, root, communicator);
  return PMPI_Bcast(buffer, count, datatype, root, communicator);
}

#if KAHIP_HAVE_MPI_BCAST_C
extern "C" int MPI_Bcast_c(void* buffer,
                           MPI_Count count,
                           MPI_Datatype datatype,
                           int root,
                           MPI_Comm communicator) {
  fixed_broadcast_probe::record(fixed_broadcast_probe::backend::large_count,
                                count, datatype, root, communicator);
  return PMPI_Bcast_c(buffer, count, datatype, root, communicator);
}
#endif
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
constexpr auto root = 0;

[[nodiscard]] constexpr auto large_value(unsigned long long offset) noexcept
    -> parhip::ULONG {
  return parhip::ULONG{std::numeric_limits<long>::max()} + offset;
}

void require_operation(fixed_broadcast_probe::operation const& operation,
                       fixed_broadcast_probe::backend expected_backend,
                       MPI_Count expected_count,
                       MPI_Datatype expected_datatype,
                       MPI_Comm expected_communicator) {
  REQUIRE(operation.selected_backend == expected_backend);
  REQUIRE(operation.count == expected_count);
  REQUIRE(operation.datatype == expected_datatype);
  REQUIRE(operation.root == root);
  REQUIRE(operation.communicator == expected_communicator);
}
}  // namespace

TEST_CASE("fixed graph-header broadcasts preserve native value domains") {
  auto world_rank = 0;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);
  REQUIRE(world_size >= 1);
  REQUIRE(world_size <= 5);

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  REQUIRE(communicator != MPI_COMM_NULL);

  auto rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);

  auto status = rank == root ? 1 : -1;
  auto header = rank == root ? std::array<parhip::ULONG, 3>{parhip::ULONG{3},
                                                            large_value(17),
                                                            large_value(41)}
                             : std::array<parhip::ULONG, 3>{};
  fixed_broadcast_probe::counters observed{};
  {
    fixed_broadcast_probe::activation const probe{communicator};
    parhip::mpi::broadcast_fixed(status, root,
                                 parhip::mpi::communicator_view{communicator},
                                 "MPI_Bcast(binary graph read status)");
    parhip::mpi::broadcast_fixed(std::span{header}, root,
                                 parhip::mpi::communicator_view{communicator},
                                 "MPI_Bcast(binary graph header)");
    observed = fixed_broadcast_probe::observed;
  }

  REQUIRE(status == 1);
  REQUIRE(header == std::array<parhip::ULONG, 3>{
                        parhip::ULONG{3}, large_value(17), large_value(41)});
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 2);
  require_operation(observed.operations[0],
                    fixed_broadcast_probe::backend::legacy, 1, MPI_INT,
                    communicator);
  require_operation(observed.operations[1],
                    fixed_broadcast_probe::backend::legacy, 3,
                    MPI_UNSIGNED_LONG_LONG, communicator);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("vcycle state broadcast uses map then cut then weight") {
  auto world_rank = 0;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);

  auto partition_map =
      rank == root ? std::array{3, 1, 4, 1, 5} : std::array<int, 5>{};
  auto previous_cut = rank == root ? large_value(73) : parhip::EdgeWeight{};
  auto previous_maximum_block_weight =
      rank == root ? large_value(101) : parhip::NodeWeight{};

  fixed_broadcast_probe::counters observed{};
  {
    fixed_broadcast_probe::activation const probe{communicator};
    parhip::mpi::broadcast_vcycle_state(
        std::span{partition_map}, previous_cut, previous_maximum_block_weight,
        root, parhip::mpi::communicator_view{communicator});
    observed = fixed_broadcast_probe::observed;
  }

  REQUIRE(partition_map == std::array{3, 1, 4, 1, 5});
  REQUIRE(previous_cut == large_value(73));
  REQUIRE(previous_maximum_block_weight == large_value(101));
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 3);
#if KAHIP_HAVE_MPI_BCAST_C
  constexpr auto map_backend = fixed_broadcast_probe::backend::large_count;
#else
  constexpr auto map_backend = fixed_broadcast_probe::backend::legacy;
#endif
  require_operation(observed.operations[0], map_backend, 5, MPI_INT,
                    communicator);
  require_operation(observed.operations[1],
                    fixed_broadcast_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, communicator);
  require_operation(observed.operations[2],
                    fixed_broadcast_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, communicator);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("vcycle map uses deterministic bounded MPI-3 rounds") {
  auto world_rank = 0;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);

  auto partition_map =
      rank == root ? std::array{2, 7, 1, 8, 2} : std::array<int, 5>{};
  auto previous_cut = rank == root ? large_value(113) : parhip::EdgeWeight{};
  auto previous_maximum_block_weight =
      rank == root ? large_value(127) : parhip::NodeWeight{};

  fixed_broadcast_probe::counters observed{};
  {
    fixed_broadcast_probe::activation const probe{communicator};
    parhip::mpi::broadcast_vcycle_state(
        std::span{partition_map}, previous_cut, previous_maximum_block_weight,
        root, parhip::mpi::communicator_view{communicator},
        {.mpi3_round_ceiling = 2, .force_mpi3 = true});
    observed = fixed_broadcast_probe::observed;
  }

  REQUIRE(partition_map == std::array{2, 7, 1, 8, 2});
  REQUIRE(previous_cut == large_value(113));
  REQUIRE(previous_maximum_block_weight == large_value(127));
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 5);
  require_operation(observed.operations[0],
                    fixed_broadcast_probe::backend::legacy, 2, MPI_INT,
                    communicator);
  require_operation(observed.operations[1],
                    fixed_broadcast_probe::backend::legacy, 2, MPI_INT,
                    communicator);
  require_operation(observed.operations[2],
                    fixed_broadcast_probe::backend::legacy, 1, MPI_INT,
                    communicator);
  require_operation(observed.operations[3],
                    fixed_broadcast_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, communicator);
  require_operation(observed.operations[4],
                    fixed_broadcast_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, communicator);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("vcycle state retains a zero-length map collective") {
  auto world_rank = 0;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  auto rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);

  auto previous_cut = rank == root ? large_value(131) : parhip::EdgeWeight{};
  auto previous_maximum_block_weight =
      rank == root ? large_value(137) : parhip::NodeWeight{};

  fixed_broadcast_probe::counters observed{};
  {
    fixed_broadcast_probe::activation const probe{communicator};
    parhip::mpi::broadcast_vcycle_state(
        std::span<int>{}, previous_cut, previous_maximum_block_weight, root,
        parhip::mpi::communicator_view{communicator},
        {.mpi3_round_ceiling = 2, .force_mpi3 = true});
    observed = fixed_broadcast_probe::observed;
  }

  REQUIRE(previous_cut == large_value(131));
  REQUIRE(previous_maximum_block_weight == large_value(137));
  REQUIRE_FALSE(observed.overflow);
  REQUIRE(observed.operation_count == 3);
  require_operation(observed.operations[0],
                    fixed_broadcast_probe::backend::legacy, 0, MPI_INT,
                    communicator);
  require_operation(observed.operations[1],
                    fixed_broadcast_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, communicator);
  require_operation(observed.operations[2],
                    fixed_broadcast_probe::backend::legacy, 1,
                    MPI_UNSIGNED_LONG_LONG, communicator);

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
}
