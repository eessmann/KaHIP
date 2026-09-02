#include <cmath>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>

#include "../app/configuration.h"
#include "../lib/parallel_mh/parallel_mh_async.h"
#include "../lib/tools/quality_metrics.h"
#include "kaHIP_interface.h"
#include "kaHIP_interface_internal.h"
#include "tools/fatal_diagnostics.h"

namespace {
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
                  int* part) {
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
  internal_build_graph(partition_config, n, vwgt, xadj, adjcwgt, adjncy, graph);

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
    kaffpae_impl(n, vwgt, xadj, adjcwgt, adjncy, nparts, imbalance,
                 suppress_output, graph_partitioned, time_limit, seed, mode,
                 communicator, edgecut, balance, part);
  } catch (...) {
    abort_kaffpae_boundary(communicator, std::current_exception());
  }
}
