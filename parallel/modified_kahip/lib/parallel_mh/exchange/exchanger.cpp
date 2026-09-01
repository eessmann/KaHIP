/******************************************************************************
 * exchanger.cpp
 *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "exchanger.h"

#include <mpi.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <vector>

#include "parallel_mh/evolutionary_collectives.h"
#include "parallel_mh/population_size_broadcast.h"
#include "tools/quality_metrics.h"
#include "tools/random_functions.h"

namespace kahip::modified {
auto exchanger::pending_send::operator=(pending_send&& other) noexcept
    -> pending_send& {
  if (this != &other) {
    if (request != MPI_REQUEST_NULL) {
      ::kahip::parallel_mh::detail::abort_evolutionary_collective(
          communicator, "evolutionary pending-send move assignment",
          "live MPI request would lose its completion ownership");
    }
    payload = std::move(other.payload);
    request = std::exchange(other.request, MPI_REQUEST_NULL);
    communicator = std::exchange(other.communicator, MPI_COMM_NULL);
  }
  return *this;
}

exchanger::exchanger(MPI_Comm communicator)
    : m_prev_best_objective(std::numeric_limits<EdgeWeight>::max()),
      m_max_num_pushes(1),
      m_rank(-1),
      m_size(0),
      m_communicator(communicator) {
  if (!::kahip::parallel_mh::detail::mpi_runtime_is_active()) {
    ::kahip::parallel_mh::detail::abort_evolutionary_lifecycle(
        "evolutionary exchange requires an active MPI runtime");
  }
  if (m_communicator == MPI_COMM_NULL) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        MPI_COMM_WORLD, "evolutionary exchange construction",
        "exchange requires a live intracommunicator");
  }

  auto is_intercommunicator = 0;
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Comm_test_inter(m_communicator, &is_intercommunicator),
      m_communicator, "MPI_Comm_test_inter(evolutionary exchange)");
  if (is_intercommunicator != 0) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, "evolutionary exchange construction",
        "exchange requires an intracommunicator");
  }

  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Comm_rank(m_communicator, &m_rank), m_communicator,
      "MPI_Comm_rank(evolutionary exchange)");
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Comm_size(m_communicator, &m_size), m_communicator,
      "MPI_Comm_size(evolutionary exchange)");
  if (m_rank < 0 || m_rank >= m_size || m_size <= 0) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, "evolutionary exchange construction",
        "MPI returned an invalid evolutionary communicator rank or size");
  }

  m_max_num_pushes =
      m_size > 2 ? static_cast<int>(std::ceil(std::log2(m_size))) : 1;
  std::cout << "max num pushes " << m_max_num_pushes << std::endl;
  m_already_sent_to.assign(static_cast<std::size_t>(m_size), false);
  m_already_sent_to[static_cast<std::size_t>(m_rank)] = true;
  m_issued_sends.assign(static_cast<std::size_t>(m_size), 0);
  m_consumed_receives.assign(static_cast<std::size_t>(m_size), 0);
}

exchanger::~exchanger() noexcept {
  if (!m_finished) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, "evolutionary rumor exchange teardown",
        "exchanger destroyed before explicit finish drained all messages");
  }
}

auto exchanger::observe_graph_order(std::size_t graph_order,
                                    std::string_view operation) -> int {
  if (m_finished) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, operation,
        "evolutionary exchange used after explicit finish");
  }
  if (m_graph_order_observed && graph_order != m_graph_order) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, operation,
        "evolutionary exchange graph order changed during its lifetime");
  }
  m_graph_order = graph_order;
  m_graph_order_observed = true;
  return ::kahip::parallel_mh::detail::checked_count(graph_order,
                                                     m_communicator, operation);
}

void exchanger::validate_partition_status(MPI_Status const& status,
                                          int expected_source,
                                          int expected_tag,
                                          int expected_count,
                                          std::string_view operation) const {
  if (status.MPI_SOURCE != expected_source || expected_source < 0 ||
      expected_source >= m_size) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, operation,
        "rumor message source does not match the requested peer");
  }
  if (status.MPI_TAG != expected_tag) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, operation,
        "rumor message tag does not match receiver rank");
  }
  auto received_count = 0;
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Get_count(&status, MPI_INT, &received_count), m_communicator,
      "MPI_Get_count(evolutionary partition payload)");
  if (received_count != expected_count) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, operation,
        "rumor message count does not match the graph order");
  }
}

void exchanger::diversify_population(PartitionConfig& config,
                                     graph_access& graph,
                                     population& island,
                                     bool replace) {
  static_cast<void>(
      observe_graph_order(static_cast<std::size_t>(graph.number_of_nodes()),
                          "MPI_Sendrecv(evolutionary permutation exchange)"));
  auto permutation = std::vector<unsigned>(static_cast<std::size_t>(m_size), 0);
  if (m_rank == ROOT) {
    random_functions::circular_permutation(permutation);
  }
  ::kahip::parallel_mh::broadcast_permutation(m_communicator, permutation,
                                              ROOT);

  auto canonical = permutation;
  std::ranges::sort(canonical);
  auto expected = std::vector<unsigned>(canonical.size());
  std::iota(expected.begin(), expected.end(), 0U);
  if (canonical != expected) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, "MPI_Bcast(evolutionary permutation)",
        "evolutionary permutation is not a bijection of communicator ranks");
  }

  auto const destination =
      static_cast<int>(permutation[static_cast<std::size_t>(m_rank)]);
  auto const source_position =
      std::ranges::find(permutation, static_cast<unsigned>(m_rank));
  auto const source =
      static_cast<int>(std::distance(permutation.begin(), source_position));

  auto input = Individuum{};
  auto output = Individuum{};
  if (config.mh_diversify_best) {
    island.get_best_individuum(input);
  } else {
    island.get_random_individuum(input);
  }
  exchange_individum(config, graph, source, destination, input, output);
  if (replace) {
    island.replace(input, output);
  } else {
    island.insert(graph, output);
  }
}

void exchanger::quick_start(PartitionConfig& config,
                            graph_access& graph,
                            population& island) {
  static_cast<void>(
      observe_graph_order(static_cast<std::size_t>(graph.number_of_nodes()),
                          "evolutionary quick-start"));
  auto const plan = ::kahip::parallel_mh::quick_start_population_plan(
      config.mh_pool_size, m_size);
  if (!plan.has_value()) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, "evolutionary quick-start",
        "quick-start requires a positive communicator size");
  }
  std::cout << "creating " << plan->local_creations << std::endl;
  for (auto index = 0U; index < plan->local_creations; ++index) {
    auto individual = Individuum{};
    island.createIndividuum(config, graph, individual, true);
    island.insert(graph, individual);
  }

  auto diversify_config = config;
  diversify_config.mh_diversify_best = false;
  for (auto index = 0U; index < plan->diversifications; ++index) {
    diversify_population(diversify_config, graph, island, false);
  }
}

void exchanger::exchange_individum(PartitionConfig const& config,
                                   graph_access& graph,
                                   int source,
                                   int destination,
                                   Individuum& input,
                                   Individuum& output) {
  auto const graph_count =
      observe_graph_order(static_cast<std::size_t>(graph.number_of_nodes()),
                          "MPI_Sendrecv(evolutionary permutation exchange)");
  if (source < 0 || source >= m_size || destination < 0 ||
      destination >= m_size || input.partition_map == nullptr) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        m_communicator, "MPI_Sendrecv(evolutionary permutation exchange)",
        "permutation exchange arguments are invalid");
  }

  auto partition_map = std::make_unique<int[]>(
      static_cast<std::size_t>(graph.number_of_nodes()));
  auto cut_edges = std::make_unique<std::vector<EdgeID>>();
  auto status = MPI_Status{};
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Sendrecv(input.partition_map, graph_count, MPI_INT, destination, 0,
                   partition_map.get(), graph_count, MPI_INT, source, 0,
                   m_communicator, &status),
      m_communicator, "MPI_Sendrecv(evolutionary permutation exchange)");
  validate_partition_status(status, source, 0, graph_count,
                            "MPI_Sendrecv(evolutionary permutation exchange)");

  forall_nodes(graph, node){forall_out_edges(
      graph, edge, node){auto const target = graph.getEdgeTarget(edge);
  if (partition_map[node] != partition_map[target]) {
    cut_edges->push_back(edge);
  }
}
endfor
}  // namespace kahip::modified
endfor output.objective = m_qm.objective(config, graph, partition_map.get());
output.partition_map = partition_map.release();
output.cut_edges = cut_edges.release();
}

void exchanger::push_best(PartitionConfig& config,
                          graph_access& graph,
                          population& island) {
  static_cast<void>(config);
  auto const graph_count =
      observe_graph_order(static_cast<std::size_t>(graph.number_of_nodes()),
                          "MPI_Isend(evolutionary rumor)");
  auto best = Individuum{};
  island.get_best_individuum(best);
  if (::kahip::parallel_mh::objective_improved(best.objective,
                                               m_prev_best_objective)) {
    m_prev_best_objective = best.objective;
    std::ranges::fill(m_already_sent_to, false);
    m_already_sent_to[static_cast<std::size_t>(m_rank)] = true;
    m_cur_num_pushes = 0;
    std::cout << "rank " << m_rank
              << ": pool improved *************************************** "
              << best.objective << std::endl;
  }

  auto something_to_do =
      std::ranges::any_of(m_already_sent_to, [](bool sent) { return !sent; });
  if (m_cur_num_pushes > m_max_num_pushes)
    something_to_do = false;
  if (something_to_do) {
    auto payload =
        std::vector<int>(static_cast<std::size_t>(graph.number_of_nodes()));
    forall_nodes(graph, node) {
      payload[static_cast<std::size_t>(node)] = graph.getPartitionIndex(node);
    }
    endfor

    auto target = m_rank;
    // Retain the paper's asynchronous rumor selection and exact draw order.
    while (m_already_sent_to[static_cast<std::size_t>(target)]) {
      target = random_functions::nextInt(0, m_size - 1);
    }
    auto& issued = m_issued_sends[static_cast<std::size_t>(target)];
    if (issued == std::numeric_limits<std::uint64_t>::max()) {
      ::kahip::parallel_mh::detail::abort_evolutionary_collective(
          m_communicator, "MPI_Isend(evolutionary rumor)",
          "evolutionary rumor send count exceeds uint64_t");
    }
    m_pending_sends.emplace_back(std::move(payload), MPI_REQUEST_NULL,
                                 m_communicator);
    auto& pending = m_pending_sends.back();
    ::kahip::parallel_mh::detail::check_mpi(
        MPI_Isend(pending.payload.data(), graph_count, MPI_INT, target, target,
                  m_communicator, &pending.request),
        m_communicator, "MPI_Isend(evolutionary rumor)");
    ++issued;
    ++m_cur_num_pushes;
    m_already_sent_to[static_cast<std::size_t>(target)] = true;
  }
  retire_completed_sends();
}

void exchanger::retire_completed_sends() {
  std::erase_if(m_pending_sends, [&](pending_send& pending) {
    auto complete = 0;
    ::kahip::parallel_mh::detail::check_mpi(
        MPI_Test(&pending.request, &complete, MPI_STATUS_IGNORE),
        m_communicator, "MPI_Test(evolutionary rumor)");
    return complete != 0;
  });
}

void exchanger::receive_available(PartitionConfig& config,
                                  graph_access& graph,
                                  population& island,
                                  MPI_Status const& probe_status,
                                  int graph_count) {
  validate_partition_status(probe_status, probe_status.MPI_SOURCE, m_rank,
                            graph_count, "MPI_Recv(evolutionary rumor)");
  auto const source = probe_status.MPI_SOURCE;
  auto partition_map = std::make_unique<int[]>(
      static_cast<std::size_t>(graph.number_of_nodes()));
  auto cut_edges = std::make_unique<std::vector<EdgeID>>();
  auto receive_status = MPI_Status{};
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Recv(partition_map.get(), graph_count, MPI_INT, source, m_rank,
               m_communicator, &receive_status),
      m_communicator, "MPI_Recv(evolutionary rumor)");
  validate_partition_status(receive_status, source, m_rank, graph_count,
                            "MPI_Recv(evolutionary rumor)");

  forall_nodes(graph, node){forall_out_edges(
      graph, edge, node){auto const target = graph.getEdgeTarget(edge);
  if (partition_map[node] != partition_map[target]) {
    cut_edges->push_back(edge);
  }
}
endfor
}
endfor auto output = Individuum{};
output.objective = m_qm.objective(config, graph, partition_map.get());
output.partition_map = partition_map.release();
output.cut_edges = cut_edges.release();
island.insert(graph, output);

auto& consumed = m_consumed_receives[static_cast<std::size_t>(source)];
if (consumed == std::numeric_limits<std::uint64_t>::max()) {
  ::kahip::parallel_mh::detail::abort_evolutionary_collective(
      m_communicator, "MPI_Recv(evolutionary rumor)",
      "evolutionary rumor receive count exceeds uint64_t");
}
++consumed;
if (::kahip::parallel_mh::objective_improved(output.objective,
                                             m_prev_best_objective)) {
  m_prev_best_objective = output.objective;
  std::cout << "rank " << m_rank
            << ": pool improved (inc) "
               "**************************************** "
            << output.objective << std::endl;
  std::ranges::fill(m_already_sent_to, false);
  m_already_sent_to[static_cast<std::size_t>(m_rank)] = true;
  m_cur_num_pushes = 0;
}
m_already_sent_to[static_cast<std::size_t>(source)] = true;
}

void exchanger::recv_incoming(PartitionConfig& config,
                              graph_access& graph,
                              population& island) {
  auto const graph_count =
      observe_graph_order(static_cast<std::size_t>(graph.number_of_nodes()),
                          "MPI_Recv(evolutionary rumor)");
  auto available = 0;
  auto probe_status = MPI_Status{};
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, m_communicator, &available,
                 &probe_status),
      m_communicator, "MPI_Iprobe(evolutionary rumor)");
  while (available != 0) {
    receive_available(config, graph, island, probe_status, graph_count);
    ::kahip::parallel_mh::detail::check_mpi(
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, m_communicator, &available,
                   &probe_status),
        m_communicator, "MPI_Iprobe(evolutionary rumor)");
  }
}

void exchanger::finish(std::size_t graph_order) {
  auto const graph_count =
      observe_graph_order(graph_order, "evolutionary rumor exchange finish");
  auto incoming = std::vector<std::uint64_t>(static_cast<std::size_t>(m_size));
  ::kahip::parallel_mh::detail::check_mpi(
      MPI_Alltoall(m_issued_sends.data(), 1, MPI_UINT64_T, incoming.data(), 1,
                   MPI_UINT64_T, m_communicator),
      m_communicator, "MPI_Alltoall(evolutionary rumor counts)");

  for (auto source = 0; source < m_size; ++source) {
    auto& consumed = m_consumed_receives[static_cast<std::size_t>(source)];
    auto const expected = incoming[static_cast<std::size_t>(source)];
    if (consumed > expected) {
      ::kahip::parallel_mh::detail::abort_evolutionary_collective(
          m_communicator, "evolutionary rumor exchange finish",
          "consumed rumor count exceeds the sender's issued count");
    }
    while (consumed < expected) {
      auto probe_status = MPI_Status{};
      ::kahip::parallel_mh::detail::check_mpi(
          MPI_Probe(source, MPI_ANY_TAG, m_communicator, &probe_status),
          m_communicator, "MPI_Probe(evolutionary rumor drain)");
      validate_partition_status(probe_status, source, m_rank, graph_count,
                                "MPI_Recv(evolutionary rumor drain)");
      auto payload = std::vector<int>(graph_order);
      auto receive_status = MPI_Status{};
      ::kahip::parallel_mh::detail::check_mpi(
          MPI_Recv(payload.data(), graph_count, MPI_INT, source, m_rank,
                   m_communicator, &receive_status),
          m_communicator, "MPI_Recv(evolutionary rumor drain)");
      validate_partition_status(receive_status, source, m_rank, graph_count,
                                "MPI_Recv(evolutionary rumor drain)");
      ++consumed;
    }
  }

  for (auto& pending : m_pending_sends) {
    ::kahip::parallel_mh::detail::check_mpi(
        MPI_Wait(&pending.request, MPI_STATUS_IGNORE), m_communicator,
        "MPI_Wait(evolutionary rumor)");
  }
  m_pending_sends.clear();
  m_finished = true;
}
}  // namespace kahip::modified
