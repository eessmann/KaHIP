#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "parallel_mh/evolutionary_collectives.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace evolutionary_probe {
enum class operation_kind : unsigned char { all_reduce, broadcast };

struct operation final {
  operation_kind kind{};
  MPI_Count count = 0;
  MPI_Datatype datatype = MPI_DATATYPE_NULL;
  MPI_Op reduction = MPI_OP_NULL;
  int root = -1;
  MPI_Comm communicator = MPI_COMM_NULL;
};

struct counters final {
  std::array<operation, 24> operations{};
  int operation_count = 0;
  int rank_queries = 0;
  int size_queries = 0;
  bool overflow = false;
  bool invalid_collective = false;
  bool forbidden_communication = false;
};

inline bool active = false;
inline bool suppress_large_count_payload = false;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline counters observed{};

void reset() noexcept {
  observed = {};
  suppress_large_count_payload = false;
}

void record(operation value) noexcept {
  if (!active) {
    return;
  }
  if (value.communicator != expected_communicator) {
    observed.invalid_collective = true;
  }
  if (observed.operation_count >=
      static_cast<int>(observed.operations.size())) {
    observed.overflow = true;
    return;
  }
  observed.operations[static_cast<std::size_t>(observed.operation_count++)] =
      value;
}

class activation final {
 public:
  explicit activation(MPI_Comm communicator = MPI_COMM_WORLD) noexcept {
    reset();
    expected_communicator = communicator;
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace evolutionary_probe

static_assert(noexcept(evolutionary_probe::reset()));
static_assert(noexcept(evolutionary_probe::record({})));

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  if (evolutionary_probe::active) {
    ++evolutionary_probe::observed.rank_queries;
    if (communicator != evolutionary_probe::expected_communicator ||
        rank == nullptr) {
      evolutionary_probe::observed.invalid_collective = true;
    }
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Comm_size(MPI_Comm communicator, int* size) {
  if (evolutionary_probe::active) {
    ++evolutionary_probe::observed.size_queries;
    if (communicator != evolutionary_probe::expected_communicator ||
        size == nullptr) {
      evolutionary_probe::observed.invalid_collective = true;
    }
  }
  return PMPI_Comm_size(communicator, size);
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op reduction,
                             MPI_Comm communicator) {
  evolutionary_probe::record(
      {.kind = evolutionary_probe::operation_kind::all_reduce,
       .count = count,
       .datatype = datatype,
       .reduction = reduction,
       .root = -1,
       .communicator = communicator});
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, reduction,
                        communicator);
}

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  evolutionary_probe::record(
      {.kind = evolutionary_probe::operation_kind::broadcast,
       .count = count,
       .datatype = datatype,
       .reduction = MPI_OP_NULL,
       .root = root,
       .communicator = communicator});
  return PMPI_Bcast(buffer, count, datatype, root, communicator);
}

#if KAHIP_HAVE_MPI_BCAST_C
extern "C" int MPI_Bcast_c(void* buffer,
                           MPI_Count count,
                           MPI_Datatype datatype,
                           int root,
                           MPI_Comm communicator) {
  evolutionary_probe::record(
      {.kind = evolutionary_probe::operation_kind::broadcast,
       .count = count,
       .datatype = datatype,
       .reduction = MPI_OP_NULL,
       .root = root,
       .communicator = communicator});
  if (evolutionary_probe::active &&
      evolutionary_probe::suppress_large_count_payload) {
    return MPI_SUCCESS;
  }
  return PMPI_Bcast_c(buffer, count, datatype, root, communicator);
}
#endif

extern "C" int MPI_Send(void const* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int destination,
                        int tag,
                        MPI_Comm communicator) {
  if (evolutionary_probe::active) {
    evolutionary_probe::observed.forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Send(buffer, count, datatype, destination, tag, communicator);
}

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (request != nullptr) {
    *request = MPI_REQUEST_NULL;
  }
  if (evolutionary_probe::active) {
    evolutionary_probe::observed.forbidden_communication = true;
    return MPI_ERR_OTHER;
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
  if (evolutionary_probe::active) {
    evolutionary_probe::observed.forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Irecv(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (request != nullptr) {
    *request = MPI_REQUEST_NULL;
  }
  if (evolutionary_probe::active) {
    evolutionary_probe::observed.forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Irecv(buffer, count, datatype, source, tag, communicator,
                    request);
}

extern "C" int MPI_Sendrecv(void const* send_buffer,
                            int send_count,
                            MPI_Datatype send_datatype,
                            int destination,
                            int send_tag,
                            void* receive_buffer,
                            int receive_count,
                            MPI_Datatype receive_datatype,
                            int source,
                            int receive_tag,
                            MPI_Comm communicator,
                            MPI_Status* status) {
  if (evolutionary_probe::active) {
    evolutionary_probe::observed.forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Sendrecv(send_buffer, send_count, send_datatype, destination,
                       send_tag, receive_buffer, receive_count,
                       receive_datatype, source, receive_tag, communicator,
                       status);
}

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (evolutionary_probe::active) {
    evolutionary_probe::observed.forbidden_communication = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Barrier(communicator);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
using objective_type = std::int64_t;
using weight_type = unsigned;
using partition_type = unsigned;

[[nodiscard]] auto objective_datatype() noexcept -> MPI_Datatype {
  if constexpr (std::same_as<objective_type, long>) {
    return MPI_LONG;
  } else if constexpr (std::same_as<objective_type, long long>) {
    return MPI_LONG_LONG_INT;
  } else {
    static_assert(std::same_as<objective_type, long> ||
                  std::same_as<objective_type, long long>);
  }
}

void require_reduction(evolutionary_probe::operation const& operation,
                       MPI_Datatype datatype) {
  REQUIRE(operation.kind == evolutionary_probe::operation_kind::all_reduce);
  REQUIRE(operation.count == 1);
  REQUIRE(operation.datatype == datatype);
  REQUIRE(operation.reduction == MPI_MIN);
  REQUIRE(operation.communicator == MPI_COMM_WORLD);
}

void require_broadcast(evolutionary_probe::operation const& operation,
                       MPI_Count count,
                       int root) {
  REQUIRE(operation.kind == evolutionary_probe::operation_kind::broadcast);
  REQUIRE(operation.count == count);
  REQUIRE(operation.datatype == MPI_UNSIGNED);
  REQUIRE(operation.root == root);
  REQUIRE(operation.communicator == MPI_COMM_WORLD);
}

void require_signature_reduction(evolutionary_probe::operation const& operation,
                                 MPI_Op reduction) {
  REQUIRE(operation.kind == evolutionary_probe::operation_kind::all_reduce);
  REQUIRE(operation.count == 4);
  REQUIRE(operation.datatype == MPI_UINT64_T);
  REQUIRE(operation.reduction == reduction);
  REQUIRE(operation.communicator == MPI_COMM_WORLD);
}

void require_clean_probe(evolutionary_probe::counters const& observed) {
  REQUIRE_FALSE(observed.overflow);
  REQUIRE_FALSE(observed.invalid_collective);
  REQUIRE_FALSE(observed.forbidden_communication);
}
}  // namespace

TEST_CASE("unsigned evolutionary permutation uses MPI_UNSIGNED") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  auto permutation = std::vector<unsigned>(static_cast<std::size_t>(size), 0);
  if (rank == 0) {
    for (auto index = 0; index < size; ++index) {
      permutation[static_cast<std::size_t>(index)] =
          static_cast<unsigned>(size - index - 1);
    }
  }

  evolutionary_probe::counters observed{};
  {
    evolutionary_probe::activation const probe;
    kahip::parallel_mh::broadcast_permutation(MPI_COMM_WORLD, permutation, 0);
    observed = evolutionary_probe::observed;
  }

  REQUIRE(observed.operation_count == 1);
  require_clean_probe(observed);
  REQUIRE(observed.rank_queries == 0);
  REQUIRE(observed.size_queries == 0);
  require_broadcast(observed.operations[0], size, 0);
  for (auto index = 0; index < size; ++index) {
    REQUIRE(permutation[static_cast<std::size_t>(index)] ==
            static_cast<unsigned>(size - index - 1));
  }
}

TEST_CASE(
    "best feasible partition retains wide scalar domains and exact winner") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  constexpr auto objective =
      static_cast<objective_type>(std::numeric_limits<int>::max()) + 4096;
  constexpr auto weight =
      static_cast<weight_type>(std::numeric_limits<int>::max()) + 1024U;
  constexpr auto partition =
      static_cast<partition_type>(std::numeric_limits<int>::max()) + 2048U;
  auto const winner = size - 1;
  auto const local_objective =
      rank == 0 || rank == winner ? objective : objective + 19;
  auto const local_weight = rank == winner ? weight : weight + 7U;
  auto local_map =
      std::array{partition + static_cast<partition_type>(rank * 8),
                 partition + static_cast<partition_type>(rank * 8 + 1),
                 partition + static_cast<partition_type>(rank * 8 + 2)};

  objective_type actual_objective = 0;
  evolutionary_probe::counters observed{};
  {
    evolutionary_probe::activation const probe;
    actual_objective = kahip::parallel_mh::select_and_broadcast_best_partition(
        MPI_COMM_WORLD, local_objective, local_weight,
        std::numeric_limits<weight_type>::max(), local_map.data(),
        local_map.size());
    observed = evolutionary_probe::observed;
  }

  REQUIRE(actual_objective == objective);
  REQUIRE(local_map ==
          std::array{partition + static_cast<partition_type>(winner * 8),
                     partition + static_cast<partition_type>(winner * 8 + 1),
                     partition + static_cast<partition_type>(winner * 8 + 2)});
  REQUIRE(observed.rank_queries == 1);
  REQUIRE(observed.size_queries == 1);
  REQUIRE(observed.operation_count == 8);
  require_clean_probe(observed);
  require_reduction(observed.operations[0], MPI_INT);
  require_reduction(observed.operations[1], objective_datatype());
  require_reduction(observed.operations[2], MPI_UNSIGNED);
  require_reduction(observed.operations[3], MPI_INT);
  require_signature_reduction(observed.operations[4], MPI_MIN);
  require_signature_reduction(observed.operations[5], MPI_MAX);
  require_reduction(observed.operations[6], MPI_INT);
  require_broadcast(observed.operations[7], 3, winner);
}

TEST_CASE(
    "all-infeasible objective fallback breaks an exact tie by lowest rank") {
  auto rank = -1;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);

  constexpr auto objective =
      static_cast<objective_type>(std::numeric_limits<int>::max()) + 8192;
  constexpr auto weight =
      static_cast<weight_type>(std::numeric_limits<int>::max()) + 4096U;
  constexpr auto partition =
      static_cast<partition_type>(std::numeric_limits<int>::max()) + 8192U;
  auto local_map =
      std::array{partition + static_cast<partition_type>(rank * 4),
                 partition + static_cast<partition_type>(rank * 4 + 1)};

  objective_type actual_objective = 0;
  evolutionary_probe::counters observed{};
  {
    evolutionary_probe::activation const probe;
    actual_objective = kahip::parallel_mh::select_and_broadcast_best_partition(
        MPI_COMM_WORLD, objective, weight, weight - 1U, local_map.data(),
        local_map.size());
    observed = evolutionary_probe::observed;
  }

  REQUIRE(actual_objective == objective);
  REQUIRE(local_map == std::array{partition, partition + 1U});
  REQUIRE(observed.rank_queries == 1);
  REQUIRE(observed.size_queries == 1);
  REQUIRE(observed.operation_count == 8);
  require_clean_probe(observed);
  require_reduction(observed.operations[0], MPI_INT);
  require_reduction(observed.operations[1], objective_datatype());
  require_reduction(observed.operations[2], MPI_UNSIGNED);
  require_reduction(observed.operations[3], MPI_INT);
  require_signature_reduction(observed.operations[4], MPI_MIN);
  require_signature_reduction(observed.operations[5], MPI_MAX);
  require_reduction(observed.operations[6], MPI_INT);
  require_broadcast(observed.operations[7], 2, 0);
}

TEST_CASE("maximum feasible objective is not confused with infeasibility") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);

  constexpr auto maximum_objective = std::numeric_limits<objective_type>::max();
  constexpr weight_type upper_bound = 64;
  constexpr partition_type partition = 8192;
  auto const local_objective =
      rank == 0 ? maximum_objective : objective_type{1};
  auto const local_weight =
      rank == 0 || size == 1 ? upper_bound : upper_bound + 1;
  auto local_map =
      std::array{partition + static_cast<partition_type>(rank * 2),
                 partition + static_cast<partition_type>(rank * 2 + 1)};

  objective_type actual_objective = 0;
  evolutionary_probe::counters observed{};
  {
    evolutionary_probe::activation const probe;
    actual_objective = kahip::parallel_mh::select_and_broadcast_best_partition(
        MPI_COMM_WORLD, local_objective, local_weight, upper_bound,
        local_map.data(), local_map.size());
    observed = evolutionary_probe::observed;
  }

  REQUIRE(actual_objective == maximum_objective);
  REQUIRE(local_map == std::array{partition, partition + 1U});
  REQUIRE(observed.rank_queries == 1);
  REQUIRE(observed.size_queries == 1);
  REQUIRE(observed.operation_count == 8);
  require_clean_probe(observed);
  require_reduction(observed.operations[0], MPI_INT);
  require_reduction(observed.operations[1], objective_datatype());
  require_reduction(observed.operations[2], MPI_UNSIGNED);
  require_reduction(observed.operations[3], MPI_INT);
  require_signature_reduction(observed.operations[4], MPI_MIN);
  require_signature_reduction(observed.operations[5], MPI_MAX);
  require_reduction(observed.operations[6], MPI_INT);
  require_broadcast(observed.operations[7], 2, 0);
}

TEST_CASE(
    "forced MPI-3 partition broadcast uses deterministic bounded rounds") {
  auto rank = -1;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);

  auto local_map = std::array<unsigned, 5>{
      static_cast<unsigned>(rank * 8), static_cast<unsigned>(rank * 8 + 1),
      static_cast<unsigned>(rank * 8 + 2), static_cast<unsigned>(rank * 8 + 3),
      static_cast<unsigned>(rank * 8 + 4)};

  evolutionary_probe::counters observed{};
  {
    evolutionary_probe::activation const probe;
    static_cast<void>(kahip::parallel_mh::select_and_broadcast_best_partition(
        MPI_COMM_WORLD, std::int64_t{17 + rank}, unsigned{32}, unsigned{64},
        local_map.data(), local_map.size(),
        {.mpi3_round_ceiling = 2, .force_mpi3 = true}));
    observed = evolutionary_probe::observed;
  }

  require_clean_probe(observed);
  REQUIRE(observed.rank_queries == 1);
  REQUIRE(observed.size_queries == 1);
  REQUIRE(local_map == std::array<unsigned, 5>{0, 1, 2, 3, 4});
  auto broadcast_counts = std::vector<MPI_Count>{};
  for (auto index = 0; index < observed.operation_count; ++index) {
    auto const& operation =
        observed.operations[static_cast<std::size_t>(index)];
    if (operation.kind == evolutionary_probe::operation_kind::broadcast) {
      broadcast_counts.push_back(operation.count);
      REQUIRE(operation.datatype == MPI_UNSIGNED);
      REQUIRE(operation.root == 0);
      REQUIRE(operation.communicator == MPI_COMM_WORLD);
    }
  }
  REQUIRE(broadcast_counts == std::vector<MPI_Count>{2, 2, 1});
}

TEST_CASE("empty partition payload is a valid collective no-op") {
  auto rank = -1;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);

  evolutionary_probe::counters observed{};
  auto best_objective = objective_type{};
  {
    evolutionary_probe::activation const probe;
    best_objective = kahip::parallel_mh::select_and_broadcast_best_partition(
        MPI_COMM_WORLD, objective_type{23 + rank}, unsigned{1}, unsigned{2},
        static_cast<unsigned*>(nullptr), 0);
    observed = evolutionary_probe::observed;
  }

  require_clean_probe(observed);
  REQUIRE(observed.rank_queries == 1);
  REQUIRE(observed.size_queries == 1);
  REQUIRE(best_objective == objective_type{23});
  REQUIRE(observed.operation_count == 7);
  for (auto index = 0; index < observed.operation_count; ++index) {
    REQUIRE(observed.operations[static_cast<std::size_t>(index)].kind ==
            evolutionary_probe::operation_kind::all_reduce);
  }
}

#if KAHIP_HAVE_MPI_BCAST_C
TEST_CASE("MPI-4 partition broadcast preserves a count above INT_MAX") {
  auto rank = -1;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);

  auto partition = static_cast<unsigned>(rank);
  constexpr auto large_count =
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U;
  evolutionary_probe::counters observed{};
  {
    evolutionary_probe::activation const probe;
    evolutionary_probe::suppress_large_count_payload = true;
    static_cast<void>(kahip::parallel_mh::select_and_broadcast_best_partition(
        MPI_COMM_WORLD, std::int64_t{7 + rank}, unsigned{1}, unsigned{2},
        &partition, large_count));
    observed = evolutionary_probe::observed;
  }

  require_clean_probe(observed);
  REQUIRE(observed.rank_queries == 1);
  REQUIRE(observed.size_queries == 1);
  REQUIRE(observed.operation_count == 8);
  require_broadcast(observed.operations[7], static_cast<MPI_Count>(large_count),
                    0);
}
#endif
