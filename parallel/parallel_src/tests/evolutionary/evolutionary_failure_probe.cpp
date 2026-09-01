#include <mpi.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "configuration.h"
#include "data_structure/graph_access.h"
#include "parallel_mh/exchange/exchanger.h"
#include "parallel_mh/parallel_mh_async.h"
#include "parallel_mh/population.h"
#include "tools/random_functions.h"

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
namespace evolutionary_failure_probe {
enum class mode : unsigned char {
  communicator_duplication,
  sendrecv,
  isend,
  wrong_tag,
  wrong_count,
  wait,
  combine_cross_conditional,
};

inline bool active = false;
inline mode selected = mode::communicator_duplication;
inline MPI_Comm expected_communicator = MPI_COMM_NULL;
inline int duplications = 0;
inline int sendrecvs = 0;
inline int isends = 0;
inline int tests = 0;
inline int waits = 0;
inline int receives = 0;
inline int cancellations = 0;
inline int finalizations = 0;
inline int communicator_size_queries = 0;
inline int broadcasts = 0;
inline bool callback_error = false;

void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto expected_abort_state() noexcept -> bool {
  if (callback_error || finalizations != 0 || cancellations != 0) {
    return false;
  }
  switch (selected) {
    case mode::communicator_duplication:
      return duplications == 1;
    case mode::sendrecv:
      return sendrecvs == 1;
    case mode::isend:
      return isends == 1;
    case mode::wrong_tag:
    case mode::wrong_count:
      return receives == 0;
    case mode::wait:
      return isends == 1 && tests >= 1 && waits == 1;
    case mode::combine_cross_conditional:
      return communicator_size_queries == 1 && broadcasts == 0;
  }
  return false;
}

[[noreturn]] void observed_abort(MPI_Comm communicator,
                                 int error_code) noexcept {
  auto relation = int{MPI_UNEQUAL};
  if (expected_communicator == MPI_COMM_NULL || error_code != EXIT_FAILURE ||
      PMPI_Comm_compare(communicator, expected_communicator, &relation) !=
          MPI_SUCCESS ||
      relation != MPI_IDENT || !expected_abort_state()) {
    write_text(
        "observed evolutionary lifetime MPI_Abort with unexpected state\n");
    std::_Exit(91);
  }
  write_text(
      "observed evolutionary lifetime MPI_Abort on affected communicator\n");
  if (PMPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
    std::_Exit(90);
  }
  std::_Exit(86);
}
}  // namespace evolutionary_failure_probe

static_assert(noexcept(evolutionary_failure_probe::write_text({})));
static_assert(noexcept(evolutionary_failure_probe::expected_abort_state()));
static_assert(noexcept(evolutionary_failure_probe::observed_abort(MPI_COMM_NULL,
                                                                  0)));

extern "C" int MPI_Comm_dup(MPI_Comm communicator, MPI_Comm* duplicate) {
  using namespace evolutionary_failure_probe;
  if (!active) {
    return PMPI_Comm_dup(communicator, duplicate);
  }
  ++duplications;
  if (selected != mode::communicator_duplication ||
      communicator != expected_communicator || duplicate == nullptr) {
    callback_error = true;
  }
  return MPI_ERR_OTHER;
}

extern "C" int MPI_Bcast(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int root,
                         MPI_Comm communicator) {
  using namespace evolutionary_failure_probe;
  if (active && selected == mode::combine_cross_conditional) {
    ++broadcasts;
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  return PMPI_Bcast(buffer, count, datatype, root, communicator);
}

extern "C" int MPI_Comm_size(MPI_Comm communicator, int* size) {
  using namespace evolutionary_failure_probe;
  if (active && selected == mode::combine_cross_conditional) {
    ++communicator_size_queries;
    if (communicator != expected_communicator || size == nullptr) {
      callback_error = true;
      return MPI_ERR_OTHER;
    }
  }
  return PMPI_Comm_size(communicator, size);
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
  using namespace evolutionary_failure_probe;
  if (!active) {
    return PMPI_Sendrecv(send_buffer, send_count, send_datatype, destination,
                         send_tag, receive_buffer, receive_count,
                         receive_datatype, source, receive_tag, communicator,
                         status);
  }
  ++sendrecvs;
  if (selected != mode::sendrecv || communicator != expected_communicator ||
      send_buffer == nullptr || receive_buffer == nullptr || send_count != 8 ||
      receive_count != 8 || send_datatype != MPI_INT ||
      receive_datatype != MPI_INT || send_tag != 0 || receive_tag != 0 ||
      destination < 0 || source < 0 || status == nullptr) {
    callback_error = true;
  }
  return MPI_ERR_OTHER;
}

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  using namespace evolutionary_failure_probe;
  if (!active) {
    return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                      request);
  }
  ++isends;
  if ((selected != mode::isend && selected != mode::wait) ||
      communicator != expected_communicator || buffer == nullptr ||
      count != 8 || datatype != MPI_INT || destination < 0 ||
      tag != destination || request == nullptr) {
    callback_error = true;
  }
  if (selected == mode::isend) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Test(MPI_Request* request,
                        int* completed,
                        MPI_Status* status) {
  using namespace evolutionary_failure_probe;
  if (!active) {
    return PMPI_Test(request, completed, status);
  }
  ++tests;
  if (selected != mode::wait || request == nullptr || completed == nullptr) {
    callback_error = true;
    return MPI_ERR_OTHER;
  }
  *completed = 0;
  return MPI_SUCCESS;
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  using namespace evolutionary_failure_probe;
  if (!active) {
    return PMPI_Wait(request, status);
  }
  ++waits;
  if (selected != mode::wait || request == nullptr) {
    callback_error = true;
  }
  return MPI_ERR_OTHER;
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::receives;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Cancel(MPI_Request* request) {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::cancellations;
  }
  return PMPI_Cancel(request);
}

extern "C" int MPI_Finalize() {
  if (evolutionary_failure_probe::active) {
    ++evolutionary_failure_probe::finalizations;
    return MPI_SUCCESS;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  evolutionary_failure_probe::observed_abort(communicator, error_code);
}
// KAHIP_PMPI_CALLBACK_REGION_END

namespace {
[[nodiscard]] auto parse_mode(std::string_view value)
    -> evolutionary_failure_probe::mode {
  using mode = evolutionary_failure_probe::mode;
  if (value == "communicator-duplication") {
    return mode::communicator_duplication;
  }
  if (value == "sendrecv")
    return mode::sendrecv;
  if (value == "isend")
    return mode::isend;
  if (value == "wrong-tag")
    return mode::wrong_tag;
  if (value == "wrong-count")
    return mode::wrong_count;
  if (value == "wait")
    return mode::wait;
  if (value == "combine-cross-conditional")
    return mode::combine_cross_conditional;
  evolutionary_failure_probe::write_text(
      "unknown evolutionary lifetime failure mode\n");
  std::_Exit(2);
}

void build_cycle_graph(kahip::modified::graph_access& graph, int rank) {
  constexpr auto nodes = kahip::modified::NodeID{8};
  graph.start_construction(nodes, 2 * nodes);
  graph.set_partition_count(2);
  for (auto node = kahip::modified::NodeID{0}; node < nodes; ++node) {
    auto const created = graph.new_node();
    graph.setNodeWeight(created, 1);
    graph.setPartitionIndex(created,
                            static_cast<kahip::modified::PartitionID>(
                                (node + static_cast<unsigned>(rank)) % 2));
    for (auto const target :
         std::array{(node + nodes - 1) % nodes, (node + 1) % nodes}) {
      auto const edge = graph.new_edge(node, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
}

[[nodiscard]] auto make_config() -> kahip::modified::PartitionConfig {
  auto config = kahip::modified::PartitionConfig{};
  config.k = 2;
  auto defaults = kahip::modified::configuration{};
  defaults.standard(config);
  config.mh_pool_size = 64;
  config.mh_num_ncs_to_compute = 0;
  config.mh_optimize_communication_volume = false;
  config.mh_penalty_for_unconnected = false;
  config.largest_graph_weight = 8;
  config.upper_bound_partition = 8;
  return config;
}

[[nodiscard]] auto make_individual(kahip::modified::graph_access& graph,
                                   int rank) -> kahip::modified::Individuum {
  auto result = kahip::modified::Individuum{
      .partition_map = new int[graph.number_of_nodes()],
      .objective = rank + 1,
      .cut_edges = new std::vector<kahip::modified::EdgeID>{},
  };
  for (auto node = kahip::modified::NodeID{0}; node < graph.number_of_nodes();
       ++node) {
    result.partition_map[node] = graph.getPartitionIndex(node);
  }
  for (auto node = kahip::modified::NodeID{0}; node < graph.number_of_nodes();
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
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    return 2;
  }
  auto world_rank = -1;
  auto world_size = 0;
  if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size != 2) {
    return 3;
  }
  auto communicator = MPI_COMM_NULL;
  if (MPI_Comm_split(MPI_COMM_WORLD, 0, 1 - world_rank, &communicator) !=
          MPI_SUCCESS ||
      communicator == MPI_COMM_NULL ||
      MPI_Comm_set_errhandler(communicator, MPI_ERRORS_RETURN) != MPI_SUCCESS) {
    return 4;
  }
  auto rank = -1;
  if (MPI_Comm_rank(communicator, &rank) != MPI_SUCCESS) {
    return 5;
  }

  auto const selected = parse_mode(argv[1]);
  evolutionary_failure_probe::selected = selected;
  evolutionary_failure_probe::expected_communicator = communicator;
  if (selected == evolutionary_failure_probe::mode::communicator_duplication) {
    evolutionary_failure_probe::active = true;
    auto driver = kahip::modified::parallel_mh_async{communicator};
    static_cast<void>(driver);
  }

  auto graph = kahip::modified::graph_access{};
  build_cycle_graph(graph, rank);
  auto config = make_config();
  auto island = kahip::modified::population{communicator, config};
  auto initial = make_individual(graph, rank);
  island.insert(graph, initial);
  kahip::modified::random_functions::setSeed(71 + rank);
  using mode = evolutionary_failure_probe::mode;

  if (selected == mode::combine_cross_conditional) {
    // Model asynchronous entry: only one process calls combine_cross. The peer
    // waits in the PMPI harness solely so the intercepted abort can emit one
    // deterministic marker without replacing the real MPI_Abort semantics.
    if (rank != 0) {
      static_cast<void>(PMPI_Barrier(MPI_COMM_WORLD));
      std::_Exit(85);
    }
    config.mh_cross_combine_original_k = true;
    auto output = kahip::modified::Individuum{};
    evolutionary_failure_probe::active = true;
    island.combine_cross(config, graph, initial, output);
  }

  auto exchange = kahip::modified::exchanger{communicator};
  if (selected == mode::wrong_tag || selected == mode::wrong_count) {
    auto payload = std::vector<int>(selected == mode::wrong_count
                                        ? graph.number_of_nodes() + 1
                                        : graph.number_of_nodes(),
                                    rank);
    auto request = MPI_REQUEST_NULL;
    auto const destination = 1 - rank;
    auto const tag = selected == mode::wrong_tag ? 947 : destination;
    if (PMPI_Isend(payload.data(), static_cast<int>(payload.size()), MPI_INT,
                   destination, tag, communicator, &request) != MPI_SUCCESS ||
        PMPI_Barrier(communicator) != MPI_SUCCESS) {
      return 6;
    }
    evolutionary_failure_probe::active = true;
    exchange.recv_incoming(config, graph, island);
  } else if (selected == mode::sendrecv) {
    evolutionary_failure_probe::active = true;
    exchange.diversify_population(config, graph, island, false);
  } else if (selected == mode::isend) {
    evolutionary_failure_probe::active = true;
    exchange.push_best(config, graph, island);
  } else if (selected == mode::wait) {
    evolutionary_failure_probe::active = true;
    exchange.push_best(config, graph, island);
    exchange.finish(static_cast<std::size_t>(graph.number_of_nodes()));
  }

  evolutionary_failure_probe::write_text(
      "evolutionary lifetime operation returned without fail-fast\n");
  std::_Exit(92);
}
