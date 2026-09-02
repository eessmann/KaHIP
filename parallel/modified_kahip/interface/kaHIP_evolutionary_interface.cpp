#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "../app/configuration.h"
#include "../lib/parallel_mh/parallel_mh_async.h"
#include "parallel_mh/evolutionary_collectives.h"
#include "../lib/tools/quality_metrics.h"
#include "../../shared/random_state.h"
#include "kaHIP_interface.h"
#include "kaHIP_interface_internal.h"
#include "tools/fatal_diagnostics.h"

namespace {
[[nodiscard]] auto exact_public_upper_bound(int const* n,
                                            int const* vertex_weights,
                                            int const* block_count,
                                            double const* imbalance)
    -> std::uint64_t {
  if (n == nullptr || block_count == nullptr || imbalance == nullptr) {
    throw std::invalid_argument(
        "modified kaffpaE requires non-null size, block-count, and imbalance "
        "arguments");
  }
  if (*n < 0 || *block_count <= 0) {
    throw std::invalid_argument(
        "modified kaffpaE requires a non-negative vertex count and a "
        "positive block count");
  }

  auto const scaled_imbalance = 100.0 * *imbalance;
  if (!std::isfinite(scaled_imbalance) || scaled_imbalance < 0.0 ||
      scaled_imbalance >
          static_cast<double>(std::numeric_limits<unsigned>::max())) {
    throw std::overflow_error(
        "modified kaffpaE imbalance percentage exceeds the unsigned int "
        "domain");
  }

  auto total_weight = std::uint64_t{0};
  for (auto node = 0; node < *n; ++node) {
    auto const weight = vertex_weights == nullptr ? 1 : vertex_weights[node];
    if (weight < 0) {
      throw std::invalid_argument(
          "modified kaffpaE requires non-negative vertex weights");
    }
    if (!kahip::random_compat::checked_add(
            total_weight, static_cast<std::uint64_t>(weight))) {
      throw std::overflow_error(
          "modified kaffpaE total vertex weight exceeds the uint64 domain");
    }
  }

  auto const upper_bound = kahip::random_compat::exact_partition_upper_bound(
      total_weight, static_cast<std::uint64_t>(*block_count),
      static_cast<unsigned>(scaled_imbalance));
  if (!upper_bound.has_value()) {
    throw std::overflow_error(
        "modified kaffpaE partition upper bound exceeds the uint64 domain");
  }
  return *upper_bound;
}

[[noreturn]] void abort_kaffpae_boundary(MPI_Comm communicator,
                                         std::exception_ptr failure) noexcept {
  auto active = false;
  auto rank = std::optional<int>{};
  auto lifecycle_error = std::optional<int>{};
  auto initialized = 0;
  auto const initialized_result = MPI_Initialized(&initialized);
  if (initialized_result != MPI_SUCCESS) {
    lifecycle_error = initialized_result;
  } else if (initialized != 0) {
    auto finalized = 0;
    auto const finalized_result = MPI_Finalized(&finalized);
    if (finalized_result != MPI_SUCCESS) {
      lifecycle_error = finalized_result;
    } else {
      active = finalized == 0;
    }
  }

  auto const affected =
      communicator == MPI_COMM_NULL ? MPI_COMM_WORLD : communicator;
  if (active) {
    auto local_rank = 0;
    if (MPI_Comm_rank(affected, &local_rank) == MPI_SUCCESS) {
      rank = local_rank;
    }
  }

  if (lifecycle_error.has_value()) {
    kahip::diagnostics::critical(
        "modified kaffpaE C boundary could not query MPI lifecycle "
        "(raw code ",
        *lifecycle_error, ")");
  }
  try {
    std::rethrow_exception(failure);
  } catch (std::exception const& error) {
    if (rank.has_value()) {
      kahip::diagnostics::critical(
          "modified kaffpaE C boundary: ", error.what(), " (rank ", *rank,
          ")");
    } else {
      kahip::diagnostics::critical("modified kaffpaE C boundary: ",
                                   error.what());
    }
  } catch (...) {
    if (rank.has_value()) {
      kahip::diagnostics::critical(
          "modified kaffpaE C boundary: unknown unrecoverable exception "
          "(rank ",
          *rank, ")");
    } else {
      kahip::diagnostics::critical(
          "modified kaffpaE C boundary: unknown unrecoverable exception");
    }
  }
  if (active) {
    static_cast<void>(MPI_Abort(affected, EXIT_FAILURE));
  }
  std::abort();
}

void kaffpae_impl(int* n,
                  int* vwgt,
                  int* xadj,
                  int* adjcwgt,
                  int* adjncy,
                  int* nparts,
                  double* imbalance,
                  bool suppress_output,
                  bool graph_partitioned,
                  int time_limit,
                  int seed,
                  int mode,
                  MPI_Comm communicator,
                  int* edgecut,
                  double* balance,
                  int* part,
                  std::optional<kahip::modified::NodeWeight>
                      authoritative_upper_bound) {
  using namespace kahip::modified;
  configuration cfg;
  PartitionConfig partition_config;
  partition_config.k = *nparts;
  cfg.standard(partition_config);

  switch (mode) {
    case FAST:
      cfg.fast(partition_config);
      break;
    case ECO:
      cfg.eco(partition_config);
      break;
    case STRONG:
      cfg.strong(partition_config);
      break;
    case FASTSOCIAL:
      cfg.fastsocial(partition_config);
      break;
    case ULTRAFASTSOCIAL:
      cfg.fastsocial(partition_config);
      partition_config.ultra_fast_kaffpaE_interfacecall = true;
      break;
    case ECOSOCIAL:
      cfg.ecosocial(partition_config);
      break;
    case STRONGSOCIAL:
      cfg.strongsocial(partition_config);
      break;
    default:
      cfg.eco(partition_config);
      break;
  }

  partition_config.seed = seed;
  partition_config.k = *nparts;
  partition_config.imbalance = 100 * (*imbalance);
  partition_config.time_limit = time_limit;
  partition_config.kabapE = false;

  graph_access graph;
  internal_build_graph(partition_config, n, vwgt, xadj, adjcwgt, adjncy, graph,
                       authoritative_upper_bound);

  partition_config.kway_adaptive_limits_beta =
      std::log(partition_config.largest_graph_weight);

  if (graph_partitioned) {
    forall_nodes(graph, node) {
      graph.setPartitionIndex(node, part[node]);
    }
    endfor
  }

  partition_config.graph_allready_partitioned = graph_partitioned;
  partition_config.no_new_initial_partitioning = graph_partitioned;

  parallel_mh_async multilevel(communicator);
  multilevel.perform_partitioning(partition_config, graph);

  forall_nodes(graph, node) {
    part[node] = graph.getPartitionIndex(node);
  }
  endfor

  quality_metrics metrics;
  *edgecut = metrics.edge_cut(graph);
  *balance = metrics.balance(graph);

  static_cast<void>(suppress_output);
}
}  // namespace

void kahip::modified::kaffpaE_with_upper_bound(
    int* n,
    int* vwgt,
    int* xadj,
    int* adjcwgt,
    int* adjncy,
    int* nparts,
    double* imbalance,
    bool suppress_output,
    bool graph_partitioned,
    int time_limit,
    int seed,
    int mode,
    MPI_Comm communicator,
    std::uint64_t authoritative_upper_bound,
    int* edgecut,
    double* balance,
    int* part) {
  auto const narrowed =
      kahip::random_compat::checked_narrow<NodeWeight>(
          authoritative_upper_bound);
  if (!narrowed.has_value()) {
    ::kahip::parallel_mh::detail::abort_evolutionary_collective(
        communicator, "evolutionary partition upper bound",
        "ParHIP upper bound exceeds the modified KaHIP weight domain");
  }
  kaffpae_impl(n, vwgt, xadj, adjcwgt, adjncy, nparts, imbalance,
               suppress_output, graph_partitioned, time_limit, seed, mode,
               communicator, edgecut, balance, part, *narrowed);
}

void kaffpaE(int* n,
             int* vwgt,
             int* xadj,
             int* adjcwgt,
             int* adjncy,
             int* nparts,
             double* imbalance,
             bool suppress_output,
             bool graph_partitioned,
             int time_limit,
             int seed,
             int mode,
             MPI_Comm communicator,
             int* edgecut,
             double* balance,
             int* part) noexcept {
  try {
    auto const upper_bound =
        exact_public_upper_bound(n, vwgt, nparts, imbalance);
    kahip::modified::kaffpaE_with_upper_bound(
        n, vwgt, xadj, adjcwgt, adjncy, nparts, imbalance, suppress_output,
        graph_partitioned, time_limit, seed, mode, communicator, upper_bound,
        edgecut, balance, part);
  } catch (...) {
    abort_kaffpae_boundary(communicator, std::current_exception());
  }
}
