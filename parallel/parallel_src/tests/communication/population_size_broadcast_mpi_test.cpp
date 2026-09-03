#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <limits>

#include "parallel_mh/population_size_broadcast.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace population_size_probe {
struct counters final {
  int broadcasts = 0;
  int point_to_point_calls = 0;
  int barriers = 0;
  bool callback_error = false;
};

inline bool active = false;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline counters observed{};

void reset(MPI_Comm communicator) noexcept {
  expected_communicator = communicator;
  observed = {};
}

void observe_broadcast(int count,
                       MPI_Datatype datatype,
                       int root,
                       MPI_Comm communicator) noexcept {
  if (!active) {
    return;
  }
  ++observed.broadcasts;
  int relation = MPI_UNEQUAL;
  if (expected_communicator == MPI_COMM_NULL || count != 1 ||
      datatype != MPI_INT || root != 0 ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT) {
    observed.callback_error = true;
  }
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
}  // namespace population_size_probe

static_assert(noexcept(population_size_probe::reset(MPI_COMM_NULL)));
static_assert(
    noexcept(population_size_probe::observe_broadcast(0,
                                                      MPI_DATATYPE_NULL,
                                                      0,
                                                      MPI_COMM_NULL)));

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  population_size_probe::observe_broadcast(count, datatype, root, communicator);
  return PMPI_Bcast(buffer, count, datatype, root, communicator);
}

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (population_size_probe::active) {
    ++population_size_probe::observed.point_to_point_calls;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (population_size_probe::active) {
    ++population_size_probe::observed.point_to_point_calls;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (population_size_probe::active) {
    ++population_size_probe::observed.barriers;
  }
  return PMPI_Barrier(communicator);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
struct population_case final {
  int root_estimate;
  bool easy_construction;
  int expected;
};

constexpr auto rank_cases = std::array{
    population_case{.root_estimate = -7,
                    .easy_construction = false,
                    .expected = 3},
    population_case{.root_estimate = 17,
                    .easy_construction = false,
                    .expected = 17},
    population_case{.root_estimate = 137,
                    .easy_construction = false,
                    .expected = 100},
    population_case{.root_estimate = 73,
                    .easy_construction = true,
                    .expected = 50},
    population_case{.root_estimate = 50,
                    .easy_construction = true,
                    .expected = 50},
};
}  // namespace

TEST_CASE(
    "population size uses one communicator-scoped broadcast and exact clamp") {
  auto world_rank = 0;
  auto world_size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &world_size) == MPI_SUCCESS);
  REQUIRE(world_size >= 1);
  REQUIRE(world_size <= static_cast<int>(rank_cases.size()));

  auto communicator = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - world_rank,
                         &communicator) == MPI_SUCCESS);
  REQUIRE(communicator != MPI_COMM_NULL);

  auto communicator_rank = -1;
  REQUIRE(MPI_Comm_rank(communicator, &communicator_rank) == MPI_SUCCESS);
  auto const test_case = rank_cases[static_cast<std::size_t>(world_size - 1)];
  auto const local_value = communicator_rank == 0
                               ? test_case.root_estimate
                               : std::numeric_limits<int>::min();

  int actual = 0;
  population_size_probe::counters observed{};
  {
    population_size_probe::activation const probe{communicator};
    actual = kahip::parallel_mh::broadcast_population_size(
        communicator, local_value, test_case.easy_construction);
    observed = population_size_probe::observed;
  }

  REQUIRE(MPI_Comm_free(&communicator) == MPI_SUCCESS);
  REQUIRE(actual == test_case.expected);
  REQUIRE(observed.broadcasts == 1);
  REQUIRE(observed.point_to_point_calls == 0);
  REQUIRE(observed.barriers == 0);
  REQUIRE_FALSE(observed.callback_error);
}
