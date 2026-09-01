#include <mpi.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "configuration.h"
#include "data_structure/graph_access.h"
#include "parallel_mh/exchange/exchanger.h"
#include "parallel_mh/parallel_mh_async.h"
#include "parallel_mh/population.h"
#include "partition/initial_partitioning/initial_partitioning.h"
#include "tools/quality_metrics.h"
#include "tools/random_functions.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace evolutionary_lifetime_probe {
enum class phase : unsigned char { inactive, communicator, exchange };

struct send_record final {
  MPI_Request request = MPI_REQUEST_NULL;
  int const* buffer = nullptr;
  int count = 0;
  int destination = -1;
  std::uint64_t checksum = 0;
  bool completed = false;
};

struct counters final {
  int duplications = 0;
  int error_handler_sets = 0;
  int frees = 0;
  int isends = 0;
  int tests = 0;
  int waits = 0;
  int cancellations = 0;
  int sendrecvs = 0;
  int receives = 0;
  int unfinished = 0;
  bool invalid_call = false;
  bool buffer_changed_before_completion = false;
  bool repeated_destination = false;
};

inline constexpr auto maximum_sends = std::size_t{64};
inline phase active_phase = phase::inactive;
inline MPI_Comm source_communicator = MPI_COMM_NULL;
inline MPI_Comm duplicated_communicator = MPI_COMM_NULL;
inline int expected_count = 0;
inline int expected_rank = -1;
inline counters observed{};
inline std::array<send_record, maximum_sends> sends{};

[[nodiscard]] auto checksum(int const* values, int count) noexcept
    -> std::uint64_t {
  auto result = std::uint64_t{1469598103934665603ULL};
  if (values == nullptr || count < 0) {
    return 0;
  }
  for (auto index = 0; index < count; ++index) {
    result ^=
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(values[index]));
    result *= std::uint64_t{1099511628211ULL};
  }
  return result;
}

void reset(phase next,
           MPI_Comm communicator,
           int rank,
           int partition_count) noexcept {
  active_phase = next;
  source_communicator = communicator;
  duplicated_communicator = MPI_COMM_NULL;
  expected_count = partition_count;
  expected_rank = rank;
  observed = {};
  sends = {};
}

void record_send(MPI_Request request,
                 int const* buffer,
                 int count,
                 int destination) noexcept {
  if (observed.isends <= 0 ||
      static_cast<std::size_t>(observed.isends) > sends.size()) {
    observed.invalid_call = true;
    return;
  }
  auto const index = static_cast<std::size_t>(observed.isends - 1);
  for (auto prior = std::size_t{0}; prior < index; ++prior) {
    if (sends[prior].destination == destination) {
      observed.repeated_destination = true;
    }
  }
  sends[index] = {
      .request = request,
      .buffer = buffer,
      .count = count,
      .destination = destination,
      .checksum = checksum(buffer, count),
      .completed = false,
  };
}

void observe_completion(MPI_Request request) noexcept {
  auto const end =
      std::min(static_cast<std::size_t>(observed.isends), sends.size());
  for (auto index = std::size_t{0}; index < end; ++index) {
    auto& send = sends[index];
    if (send.request == request && !send.completed) {
      if (checksum(send.buffer, send.count) != send.checksum) {
        observed.buffer_changed_before_completion = true;
      }
      send.completed = true;
      return;
    }
  }
  observed.invalid_call = true;
}

void finalize_observation() noexcept {
  auto const end =
      std::min(static_cast<std::size_t>(observed.isends), sends.size());
  for (auto index = std::size_t{0}; index < end; ++index) {
    observed.unfinished += sends[index].completed ? 0 : 1;
  }
  active_phase = phase::inactive;
}
}  // namespace evolutionary_lifetime_probe

static_assert(noexcept(evolutionary_lifetime_probe::checksum(nullptr, 0)));
static_assert(noexcept(evolutionary_lifetime_probe::reset(
    evolutionary_lifetime_probe::phase::inactive,
    MPI_COMM_NULL,
    0,
    0)));
static_assert(noexcept(
    evolutionary_lifetime_probe::record_send(MPI_REQUEST_NULL, nullptr, 0,
                                              0)));
static_assert(noexcept(
    evolutionary_lifetime_probe::observe_completion(MPI_REQUEST_NULL)));
static_assert(noexcept(evolutionary_lifetime_probe::finalize_observation()));

extern "C" int MPI_Comm_dup(MPI_Comm communicator, MPI_Comm* duplicate) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase != phase::communicator) {
    return PMPI_Comm_dup(communicator, duplicate);
  }
  ++observed.duplications;
  if (communicator != source_communicator || duplicate == nullptr) {
    observed.invalid_call = true;
  }
  auto const result = PMPI_Comm_dup(communicator, duplicate);
  if (result == MPI_SUCCESS && duplicate != nullptr) {
    duplicated_communicator = *duplicate;
  }
  return result;
}

extern "C" int MPI_Comm_set_errhandler(MPI_Comm communicator,
                                       MPI_Errhandler error_handler) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase == phase::communicator) {
    ++observed.error_handler_sets;
    if (communicator != duplicated_communicator ||
        error_handler != MPI_ERRORS_RETURN) {
      observed.invalid_call = true;
    }
  }
  return PMPI_Comm_set_errhandler(communicator, error_handler);
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase == phase::communicator) {
    ++observed.frees;
    if (communicator == nullptr || *communicator != duplicated_communicator) {
      observed.invalid_call = true;
    }
  }
  return PMPI_Comm_free(communicator);
}

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase != phase::exchange) {
    return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                      request);
  }
  ++observed.isends;
  if (communicator != source_communicator || buffer == nullptr ||
      count != expected_count || datatype != MPI_INT || destination < 0 ||
      destination == expected_rank || tag != destination ||
      request == nullptr) {
    observed.invalid_call = true;
  }
  auto const result = PMPI_Isend(buffer, count, datatype, destination, tag,
                                 communicator, request);
  if (result == MPI_SUCCESS && request != nullptr) {
    record_send(*request, static_cast<int const*>(buffer), count, destination);
  }
  return result;
}

extern "C" int MPI_Test(MPI_Request* request,
                        int* completed,
                        MPI_Status* status) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase != phase::exchange) {
    return PMPI_Test(request, completed, status);
  }
  ++observed.tests;
  if (request == nullptr || completed == nullptr) {
    observed.invalid_call = true;
    return MPI_ERR_ARG;
  }
  auto const original = *request;
  auto const result = PMPI_Test(request, completed, status);
  if (result == MPI_SUCCESS && *completed != 0) {
    observe_completion(original);
  }
  return result;
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase != phase::exchange) {
    return PMPI_Wait(request, status);
  }
  ++observed.waits;
  if (request == nullptr) {
    observed.invalid_call = true;
    return MPI_ERR_ARG;
  }
  auto const original = *request;
  auto const result = PMPI_Wait(request, status);
  if (result == MPI_SUCCESS) {
    observe_completion(original);
  }
  return result;
}

extern "C" int MPI_Cancel(MPI_Request* request) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase == phase::exchange) {
    ++observed.cancellations;
  }
  return PMPI_Cancel(request);
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
  using namespace evolutionary_lifetime_probe;
  if (active_phase == phase::exchange) {
    ++observed.sendrecvs;
    if (communicator != source_communicator || send_buffer == nullptr ||
        receive_buffer == nullptr || send_count != expected_count ||
        receive_count != expected_count || send_datatype != MPI_INT ||
        receive_datatype != MPI_INT || destination < 0 || source < 0 ||
        send_tag != 0 || receive_tag != 0 || status == nullptr) {
      observed.invalid_call = true;
    }
  }
  return PMPI_Sendrecv(send_buffer, send_count, send_datatype, destination,
                       send_tag, receive_buffer, receive_count,
                       receive_datatype, source, receive_tag, communicator,
                       status);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  using namespace evolutionary_lifetime_probe;
  if (active_phase == phase::exchange) {
    ++observed.receives;
    if (communicator != source_communicator || buffer == nullptr ||
        count != expected_count || datatype != MPI_INT || source < 0 ||
        tag != expected_rank || status == nullptr) {
      observed.invalid_call = true;
    }
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
void build_cycle_graph(graph_access& graph, int rank) {
  constexpr auto nodes = NodeID{8};
  graph.start_construction(nodes, 2 * nodes);
  graph.set_partition_count(2);
  for (auto node = NodeID{0}; node < nodes; ++node) {
    auto const created = graph.new_node();
    graph.setNodeWeight(created, 1);
    graph.setPartitionIndex(created,
                            static_cast<PartitionID>(
                                (node + static_cast<unsigned>(rank)) % 2));
    auto const previous = (node + nodes - 1) % nodes;
    auto const next = (node + 1) % nodes;
    auto const first = graph.new_edge(node, previous);
    graph.setEdgeWeight(first, 1);
    auto const second = graph.new_edge(node, next);
    graph.setEdgeWeight(second, 1);
  }
  graph.finish_construction();
}

[[nodiscard]] auto make_config() -> PartitionConfig {
  auto config = PartitionConfig{};
  config.k = 2;
  auto defaults = configuration{};
  defaults.standard(config);
  config.mh_pool_size = 64;
  config.mh_num_ncs_to_compute = 0;
  config.mh_optimize_communication_volume = false;
  config.mh_penalty_for_unconnected = false;
  config.largest_graph_weight = 8;
  config.upper_bound_partition = 8;
  return config;
}

[[nodiscard]] auto make_individual(graph_access& graph,
                                   int rank) -> Individuum {
  auto result = Individuum{
      .partition_map = new int[graph.number_of_nodes()],
      .objective = rank + 1,
      .cut_edges = new std::vector<EdgeID>{},
  };
  for (auto node = NodeID{0}; node < graph.number_of_nodes();
       ++node) {
    result.partition_map[node] = graph.getPartitionIndex(node);
  }
  for (auto node = NodeID{0}; node < graph.number_of_nodes();
       ++node) {
    for (auto edge = graph.get_first_edge(node);
         edge < graph.get_first_invalid_edge(node); ++edge) {
      if (result.partition_map[node] !=
          result.partition_map[graph.getEdgeTarget(edge)]) {
        result.cut_edges->push_back(edge);
      }
    }
  }
  return result;
}

void reset_graph_partition(graph_access& graph, int rank) {
  for (auto node = NodeID{0}; node < graph.number_of_nodes();
       ++node) {
    graph.setPartitionIndex(node,
                            static_cast<PartitionID>(
                                (node + static_cast<unsigned>(rank)) % 2));
  }
}

[[nodiscard]] auto partition_vector(graph_access& graph)
    -> std::vector<PartitionID> {
  auto result = std::vector<PartitionID>(
      static_cast<std::size_t>(graph.number_of_nodes()));
  for (auto node = NodeID{0}; node < graph.number_of_nodes();
       ++node) {
    result[node] = graph.getPartitionIndex(node);
  }
  return result;
}

[[nodiscard]] auto expected_unsent_destinations(int seed,
                                                int rank,
                                                int size)
    -> std::vector<int> {
  random_functions::setSeed(seed);
  auto sent = std::vector<bool>(static_cast<std::size_t>(size), false);
  sent[static_cast<std::size_t>(rank)] = true;
  auto destinations = std::vector<int>{};
  destinations.reserve(static_cast<std::size_t>(size - 1));
  while (destinations.size() < static_cast<std::size_t>(size - 1)) {
    auto target = rank;
    while (sent[static_cast<std::size_t>(target)]) {
      target = random_functions::nextInt(0, size - 1);
    }
    sent[static_cast<std::size_t>(target)] = true;
    destinations.push_back(target);
  }
  return destinations;
}
}  // namespace

TEST_CASE("evolutionary driver owns an errors-return communicator") {
  auto rank = -1;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);

  auto caller = MPI_COMM_NULL;
  REQUIRE(PMPI_Comm_split(MPI_COMM_WORLD, 0, rank, &caller) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_set_errhandler(caller, MPI_ERRORS_RETURN) == MPI_SUCCESS);

  evolutionary_lifetime_probe::reset(
      evolutionary_lifetime_probe::phase::communicator, caller, rank, 0);
  {
    auto driver = parallel_mh_async{caller};
    static_cast<void>(driver);
  }
  evolutionary_lifetime_probe::finalize_observation();
  auto const observed = evolutionary_lifetime_probe::observed;

  CHECK(observed.duplications == 1);
  CHECK(observed.error_handler_sets == 1);
  CHECK(observed.frees == 1);
  CHECK_FALSE(observed.invalid_call);
  REQUIRE(PMPI_Comm_free(&caller) == MPI_SUCCESS);
}

TEST_CASE("evolutionary gossip owns payloads through exact P2P completion") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE((size == 2 || size == 3 || size == 5));

  auto communicator = MPI_COMM_NULL;
  REQUIRE(PMPI_Comm_split(MPI_COMM_WORLD, 0, size - rank, &communicator) ==
          MPI_SUCCESS);
  REQUIRE(PMPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) ==
          MPI_SUCCESS);

  auto graph = graph_access{};
  build_cycle_graph(graph, rank);
  auto config = make_config();
  auto island = population{communicator, config};
  auto initial = make_individual(graph, rank);
  island.insert(graph, initial);
  random_functions::setSeed(127 + rank);

  evolutionary_lifetime_probe::reset(
      evolutionary_lifetime_probe::phase::exchange, communicator, rank,
      static_cast<int>(graph.number_of_nodes()));
  {
    auto exchange = exchanger{communicator};
    exchange.diversify_population(config, graph, island, false);
    for (auto iteration = 0; iteration < size + 1; ++iteration) {
      exchange.push_best(config, graph, island);
      REQUIRE(PMPI_Barrier(communicator) == MPI_SUCCESS);
      exchange.recv_incoming(config, graph, island);
      REQUIRE(PMPI_Barrier(communicator) == MPI_SUCCESS);
    }
    exchange.finish(static_cast<std::size_t>(graph.number_of_nodes()));
  }
  evolutionary_lifetime_probe::finalize_observation();
  auto const local = evolutionary_lifetime_probe::observed;

  auto local_values = std::array<int, 8>{
      local.isends,
      local.tests,
      local.waits,
      local.cancellations,
      local.sendrecvs,
      local.receives,
      local.unfinished,
      local.invalid_call || local.buffer_changed_before_completion ? 1 : 0,
  };
  auto global = std::array<int, 8>{};
  REQUIRE(PMPI_Allreduce(local_values.data(), global.data(),
                         static_cast<int>(local_values.size()), MPI_INT,
                         MPI_SUM, communicator) == MPI_SUCCESS);

  CHECK(global[0] >= size);
  CHECK(global[1] + global[2] >= global[0]);
  CHECK(global[3] == 0);
  CHECK(global[4] == size);
  CHECK(global[5] >= global[0]);
  CHECK(global[6] == 0);
  CHECK(global[7] == 0);
  REQUIRE(PMPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("evolutionary gossip contacts only unsent peers before a reset") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE((size == 2 || size == 3 || size == 5));

  auto communicator = MPI_COMM_NULL;
  REQUIRE(PMPI_Comm_split(MPI_COMM_WORLD, 0, size - rank, &communicator) ==
          MPI_SUCCESS);
  REQUIRE(PMPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) ==
          MPI_SUCCESS);

  auto graph = graph_access{};
  build_cycle_graph(graph, rank);
  auto config = make_config();
  auto island = population{communicator, config};
  auto initial = make_individual(graph, rank);
  island.insert(graph, initial);
  auto const random_seed = 127 + rank;
  auto const expected_destinations =
      expected_unsent_destinations(random_seed, rank, size);
  random_functions::setSeed(random_seed);

  evolutionary_lifetime_probe::reset(
      evolutionary_lifetime_probe::phase::exchange, communicator, rank,
      static_cast<int>(graph.number_of_nodes()));
  {
    auto exchange = exchanger{communicator};
    // More calls than the logarithmic push budget make duplicate selection
    // observable while the exact issued-count drain keeps teardown finite.
    for (auto iteration = 0; iteration < size + 2; ++iteration) {
      exchange.push_best(config, graph, island);
    }
    exchange.finish(static_cast<std::size_t>(graph.number_of_nodes()));
  }
  evolutionary_lifetime_probe::finalize_observation();

  auto const local_repeat =
      evolutionary_lifetime_probe::observed.repeated_destination ? 1 : 0;
  auto repeated_ranks = 0;
  REQUIRE(PMPI_Allreduce(&local_repeat, &repeated_ranks, 1, MPI_INT, MPI_SUM,
                         communicator) == MPI_SUCCESS);
  CHECK(repeated_ranks == 0);
  REQUIRE(evolutionary_lifetime_probe::observed.isends ==
          static_cast<int>(expected_destinations.size()));
  for (auto index = std::size_t{0}; index < expected_destinations.size();
       ++index) {
    CHECK(evolutionary_lifetime_probe::sends[index].destination ==
          expected_destinations[index]);
  }
  CHECK_FALSE(evolutionary_lifetime_probe::observed.invalid_call);
  CHECK(evolutionary_lifetime_probe::observed.unfinished == 0);
  REQUIRE(PMPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE("empty evolutionary exchange finishes without sentinel traffic") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE((size == 2 || size == 3 || size == 5));

  auto communicator = MPI_COMM_NULL;
  REQUIRE(PMPI_Comm_split(MPI_COMM_WORLD, 0, size - rank, &communicator) ==
          MPI_SUCCESS);
  REQUIRE(PMPI_Comm_rank(communicator, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) ==
          MPI_SUCCESS);

  evolutionary_lifetime_probe::reset(
      evolutionary_lifetime_probe::phase::exchange, communicator, rank, 0);
  {
    auto exchange = exchanger{communicator};
    exchange.finish(0);
  }
  evolutionary_lifetime_probe::finalize_observation();

  CHECK(evolutionary_lifetime_probe::observed.isends == 0);
  CHECK(evolutionary_lifetime_probe::observed.receives == 0);
  CHECK(evolutionary_lifetime_probe::observed.unfinished == 0);
  CHECK_FALSE(evolutionary_lifetime_probe::observed.invalid_call);
  REQUIRE(PMPI_Comm_free(&communicator) == MPI_SUCCESS);
}

TEST_CASE(
    "standard evolutionary configuration selects recursive initial "
    "partitioning") {
  auto config = PartitionConfig{};
  config.k = 2;
  config.initial_partitioning_type =
      INITIAL_PARTITIONING_BIPARTITION;

  auto defaults = configuration{};
  defaults.standard(config);

  CHECK(config.initial_partitioning_type ==
        INITIAL_PARTITIONING_RECPARTITION);
}

TEST_CASE("reusing an evolutionary driver resets first-run state") {
  auto rank = -1;
  auto size = 0;
  REQUIRE(PMPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(PMPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE((size == 2 || size == 3 || size == 5));

  auto communicator = MPI_COMM_NULL;
  REQUIRE(PMPI_Comm_split(MPI_COMM_WORLD, 0, rank, &communicator) ==
          MPI_SUCCESS);
  REQUIRE(PMPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) ==
          MPI_SUCCESS);

  auto graph = graph_access{};
  build_cycle_graph(graph, rank);
  auto config = make_config();
  config.seed = 19;
  config.time_limit = 0.0;
  config.mh_pool_size = 3;
  config.mh_enable_quickstart = true;
  config.mh_disable_combine = true;
  config.mh_diversify = false;
  config.local_partitioning_repetitions = 1;

  auto driver = parallel_mh_async{communicator};
  driver.perform_partitioning(config, graph);
  auto const first_partition = partition_vector(graph);
  auto metrics = quality_metrics{};
  auto const first_cut = metrics.edge_cut(graph);

  reset_graph_partition(graph, rank);
  driver.perform_partitioning(config, graph);
  auto const second_partition = partition_vector(graph);
  auto const second_cut = metrics.edge_cut(graph);

  CHECK(second_partition == first_partition);
  CHECK(second_cut == first_cut);
  REQUIRE(PMPI_Comm_free(&communicator) == MPI_SUCCESS);
}
