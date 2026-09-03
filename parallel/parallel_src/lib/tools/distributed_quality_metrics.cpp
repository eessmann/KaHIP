/******************************************************************************
 * distributed_quality_metrics.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "communication/mpi_fixed_reduction.h"
#include "definitions.h"
#include "distributed_quality_metrics.h"
namespace parhip {
namespace {
[[nodiscard]] auto validated_local_block_count(
    PartitionID k,
    mpi::communicator_view communicator,
    std::string_view zero_diagnostic,
    std::string_view capacity_diagnostic) noexcept -> std::size_t {
  mpi::require_live_intracommunicator(
      communicator, "local quality metric requires a live intracommunicator");
  if (k == 0) {
    mpi::abort_on_programming_error(communicator.native_handle(),
                                    zero_diagnostic);
  }
  if (!std::in_range<std::size_t>(k)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "local quality metric", capacity_diagnostic);
  }
  return static_cast<std::size_t>(k);
}

[[nodiscard]] auto validated_block_count(
    PartitionID k,
    mpi::communicator_view communicator,
    std::string_view zero_diagnostic,
    std::string_view mismatch_diagnostic,
    std::string_view capacity_diagnostic) noexcept -> std::size_t {
  mpi::require_live_intracommunicator(
      communicator,
      "distributed quality metric requires a live intracommunicator");
  static_assert(sizeof(PartitionID) <= sizeof(std::uint64_t));
  auto const local = static_cast<std::uint64_t>(k);
  auto minimum = std::uint64_t{};
  auto maximum = std::uint64_t{};
  mpi::check_or_abort(MPI_Allreduce(&local, &minimum, 1, MPI_UINT64_T, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(distributed block count minimum)");
  mpi::check_or_abort(MPI_Allreduce(&local, &maximum, 1, MPI_UINT64_T, MPI_MAX,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(distributed block count maximum)");
  if (minimum != maximum) {
    mpi::abort_on_programming_error(communicator.native_handle(),
                                    mismatch_diagnostic);
  }
  if (k == 0) {
    mpi::abort_on_programming_error(communicator.native_handle(),
                                    zero_diagnostic);
  }
  if (!std::in_range<std::size_t>(k)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "distributed quality metric",
                                   capacity_diagnostic);
  }
  return static_cast<std::size_t>(k);
}

void require_collective_condition(bool local_condition,
                                  mpi::communicator_view communicator,
                                  std::string_view diagnostic) noexcept {
  auto const local = local_condition ? 1 : 0;
  auto all_are_valid = 0;
  mpi::check_or_abort(MPI_Allreduce(&local, &all_are_valid, 1, MPI_INT, MPI_MIN,
                                    communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(distributed quality-metric validation)");
  if (all_are_valid == 0) {
    mpi::abort_on_programming_error(communicator.native_handle(), diagnostic);
  }
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr auto checked_add(T& accumulator, T value) noexcept
    -> bool {
  if (value > std::numeric_limits<T>::max() - accumulator) {
    return false;
  }
  accumulator += value;
  return true;
}

void require_collective_capacity(bool local_condition,
                                 mpi::communicator_view communicator,
                                 std::string_view boundary,
                                 std::string_view diagnostic) noexcept {
  auto const local = local_condition ? 1 : 0;
  auto all_are_representable = 0;
  mpi::check_or_abort(
      MPI_Allreduce(&local, &all_are_representable, 1, MPI_INT, MPI_MIN,
                    communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(distributed quality-metric capacity validation)");
  if (all_are_representable == 0) {
    mpi::abort_on_capacity_failure(communicator.native_handle(), boundary,
                                   diagnostic);
  }
}

[[nodiscard]] auto exact_balance_ratio(
    std::span<NodeWeight const> block_weights,
    NodeWeight divisor,
    mpi::communicator_view communicator,
    std::string_view boundary,
    std::string_view total_overflow_diagnostic) noexcept -> double {
  if (block_weights.empty() || divisor == 0) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "balance ratio requires nonempty blocks and a positive divisor");
  }
  auto total_weight = NodeWeight{};
  for (auto const weight : block_weights) {
    if (!checked_add(total_weight, weight)) {
      mpi::abort_on_capacity_failure(communicator.native_handle(), boundary,
                                     total_overflow_diagnostic);
    }
  }
  // Empty graphs and graphs whose modeled load is identically zero are
  // neutrally balanced by convention.
  if (total_weight == 0) {
    return 1.0;
  }
  auto const maximum_weight = std::ranges::max(block_weights);
  auto const ideal_weight =
      total_weight / divisor + (total_weight % divisor != 0 ? 1 : 0);
  return static_cast<double>(maximum_weight) /
         static_cast<double>(ideal_weight);
}
}  // namespace

EdgeWeight distributed_quality_metrics::edge_cut_second(
    parallel_graph_access& G,
    MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  try {
    auto local_cut = EdgeWeight{};
    auto local_cut_is_representable = true;
    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      for (EdgeID edge = G.get_first_edge(node),
                  edge_end = G.get_first_invalid_edge(node);
           edge < edge_end; ++edge) {
        auto const target = G.getEdgeTarget(edge);
        if (G.getSecondPartitionIndex(node) !=
            G.getSecondPartitionIndex(target)) {
          local_cut_is_representable =
              checked_add(local_cut, G.getEdgeWeight(edge)) &&
              local_cut_is_representable;
        }
      }
    }
    require_collective_capacity(
        local_cut_is_representable, communicator_view,
        "distributed second edge cut",
        "local edge-cut sum exceeds EdgeWeight capacity");

    auto const local = std::array{local_cut};
    auto global = std::array<EdgeWeight, 1>{};
    mpi::all_reduce_checked_sum(
        std::span<EdgeWeight const>{local}, std::span{global},
        communicator_view, "MPI_Allreduce(distributed second edge cut)",
        "distributed second edge cut",
        "global edge-cut sum exceeds EdgeWeight capacity");
    return global[0] / 2;
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "distributed second edge-cut computation failed");
  }
}

EdgeWeight distributed_quality_metrics::local_edge_cut(parallel_graph_access& G,
                                                       int* partition_map,
                                                       MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  mpi::require_live_intracommunicator(
      communicator_view, "local edge cut requires a live intracommunicator");
  if (G.number_of_local_nodes() != 0 && partition_map == nullptr) {
    mpi::abort_on_programming_error(
        communicator_view.native_handle(),
        "local edge cut requires a partition map for nonempty local storage");
  }
  try {
    auto local_cut = EdgeWeight{};
    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      for (EdgeID edge = G.get_first_edge(node),
                  edge_end = G.get_first_invalid_edge(node);
           edge < edge_end; ++edge) {
        auto const target = G.getEdgeTarget(edge);
        if (partition_map[node] != partition_map[target] &&
            !checked_add(local_cut, G.getEdgeWeight(edge))) {
          mpi::abort_on_capacity_failure(
              communicator_view.native_handle(), "local edge cut",
              "local edge-cut sum exceeds EdgeWeight capacity");
        }
      }
    }
    return local_cut / 2;
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "local edge-cut computation failed");
  }
}

EdgeWeight distributed_quality_metrics::edge_cut(parallel_graph_access& G,
                                                 MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  try {
    auto local_cut = EdgeWeight{};
    auto local_cut_is_representable = true;
    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      for (EdgeID edge = G.get_first_edge(node),
                  edge_end = G.get_first_invalid_edge(node);
           edge < edge_end; ++edge) {
        auto const target = G.getEdgeTarget(edge);
        if (G.getNodeLabel(node) != G.getNodeLabel(target)) {
          local_cut_is_representable =
              checked_add(local_cut, G.getEdgeWeight(edge)) &&
              local_cut_is_representable;
        }
      }
    }
    require_collective_capacity(
        local_cut_is_representable, communicator_view, "distributed edge cut",
        "local edge-cut sum exceeds EdgeWeight capacity");

    auto const local = std::array{local_cut};
    auto global = std::array<EdgeWeight, 1>{};
    mpi::all_reduce_checked_sum(
        std::span<EdgeWeight const>{local}, std::span{global},
        communicator_view, "MPI_Allreduce(distributed edge cut)",
        "distributed edge cut",
        "global edge-cut sum exceeds EdgeWeight capacity");
    return global[0] / 2;
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "distributed edge-cut computation failed");
  }
}

NodeWeight distributed_quality_metrics::local_max_block_weight(
    PPartitionConfig& config,
    parallel_graph_access& G,
    int* partition_map,
    MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const block_count = validated_local_block_count(
      config.k, communicator_view,
      "local maximum block weight requires k greater than zero",
      "local maximum block count exceeds addressable storage");
  if (G.number_of_local_nodes() != 0 && partition_map == nullptr) {
    mpi::abort_on_programming_error(
        communicator_view.native_handle(),
        "local maximum block weight requires a partition map for nonempty "
        "local storage");
  }
  try {
    auto block_weights = std::vector<NodeWeight>(block_count, 0);
    auto labels_are_valid = true;
    auto sums_are_representable = true;

    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      auto const block = static_cast<PartitionID>(partition_map[node]);
      if (block >= config.k) {
        labels_are_valid = false;
        continue;
      }
      sums_are_representable =
          checked_add(block_weights[static_cast<std::size_t>(block)],
                      G.getNodeWeight(node)) &&
          sums_are_representable;
    }
    if (!labels_are_valid) {
      mpi::abort_on_programming_error(
          communicator_view.native_handle(),
          "local maximum block weight label is outside [0, k)");
    }
    if (!sums_are_representable) {
      mpi::abort_on_capacity_failure(
          communicator_view.native_handle(), "local maximum block weight",
          "local block-weight sum exceeds NodeWeight capacity");
    }
    return std::ranges::max(block_weights);
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "local maximum block-weight computation failed");
  }
}

double distributed_quality_metrics::balance(PPartitionConfig& config,
                                            parallel_graph_access& G,
                                            MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const block_count = validated_block_count(
      config.k, communicator_view,
      "distributed balance requires k greater than zero",
      "distributed balance k differs across communicator",
      "distributed balance block count exceeds addressable storage");
  try {
    auto block_weights = std::vector<NodeWeight>(block_count, 0);
    auto local_graph_vertex_weight = NodeWeight{};
    auto labels_are_valid = true;
    auto sums_are_representable = true;

    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      auto const block = static_cast<PartitionID>(G.getNodeLabel(node));
      auto const weight = G.getNodeWeight(node);
      sums_are_representable = checked_add(local_graph_vertex_weight, weight) &&
                               sums_are_representable;
      if (block >= config.k) {
        labels_are_valid = false;
        continue;
      }
      sums_are_representable =
          checked_add(block_weights[static_cast<std::size_t>(block)], weight) &&
          sums_are_representable;
    }
    require_collective_capacity(
        sums_are_representable, communicator_view, "distributed balance",
        "local vertex-weight sum exceeds NodeWeight capacity");
    require_collective_condition(labels_are_valid, communicator_view,
                                 "distributed balance label is outside [0, k)");

    auto overall_weights = std::vector<NodeWeight>(block_count, 0);
    mpi::all_reduce_checked_sum(
        std::span<NodeWeight const>{block_weights}, std::span{overall_weights},
        communicator_view, "MPI_Allreduce(distributed block weights)",
        "distributed balance",
        "global block-weight sum exceeds NodeWeight capacity");
    return exact_balance_ratio(
        overall_weights, config.k, communicator_view, "distributed balance",
        "global vertex-weight sum exceeds NodeWeight capacity");
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "distributed balance computation failed");
  }
}

double distributed_quality_metrics::balance_second(PPartitionConfig& config,
                                                   parallel_graph_access& G,
                                                   MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const block_count = validated_block_count(
      config.k, communicator_view,
      "distributed second balance requires k greater than zero",
      "distributed second balance k differs across communicator",
      "distributed second balance block count exceeds addressable storage");
  try {
    auto block_weights = std::vector<NodeWeight>(block_count, 0);
    auto local_graph_vertex_weight = NodeWeight{};
    auto labels_are_valid = true;
    auto sums_are_representable = true;

    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      auto const block =
          static_cast<PartitionID>(G.getSecondPartitionIndex(node));
      auto const weight = G.getNodeWeight(node);
      sums_are_representable = checked_add(local_graph_vertex_weight, weight) &&
                               sums_are_representable;
      if (block >= config.k) {
        labels_are_valid = false;
        continue;
      }
      sums_are_representable =
          checked_add(block_weights[static_cast<std::size_t>(block)], weight) &&
          sums_are_representable;
    }
    require_collective_capacity(
        sums_are_representable, communicator_view, "distributed second balance",
        "local vertex-weight sum exceeds NodeWeight capacity");
    require_collective_condition(
        labels_are_valid, communicator_view,
        "distributed second balance label is outside [0, k)");

    auto overall_weights = std::vector<NodeWeight>(block_count, 0);
    mpi::all_reduce_checked_sum(
        std::span<NodeWeight const>{block_weights}, std::span{overall_weights},
        communicator_view, "MPI_Allreduce(distributed second block weights)",
        "distributed second balance",
        "global block-weight sum exceeds NodeWeight capacity");
    return exact_balance_ratio(
        overall_weights, config.k, communicator_view,
        "distributed second balance",
        "global vertex-weight sum exceeds NodeWeight capacity");
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "distributed second-balance computation failed");
  }
}

double distributed_quality_metrics::balance_load(PPartitionConfig& config,
                                                 parallel_graph_access& G,
                                                 MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const block_count = validated_block_count(
      config.k, communicator_view,
      "distributed load balance requires k greater than zero",
      "distributed load balance k differs across communicator",
      "distributed load balance block count exceeds addressable storage");
  try {
    auto block_weights = std::vector<NodeWeight>(block_count, 0);
    auto local_weight = NodeWeight{};
    auto labels_are_valid = true;
    auto sums_are_representable = true;
    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      auto const block = static_cast<PartitionID>(G.getNodeLabel(node));
      auto const node_weight = G.getNodeWeight(node);
      auto const degree = G.getNodeDegree(node);
      auto node_load = node_weight;
      auto const node_load_is_representable =
          std::in_range<NodeWeight>(degree) &&
          checked_add(node_load, static_cast<NodeWeight>(degree));
      sums_are_representable =
          node_load_is_representable && sums_are_representable;
      if (block >= config.k) {
        labels_are_valid = false;
        continue;
      }
      if (!node_load_is_representable) {
        continue;
      }
      sums_are_representable =
          checked_add(block_weights[static_cast<std::size_t>(block)],
                      node_load) &&
          checked_add(local_weight, node_load) && sums_are_representable;
    }
    require_collective_capacity(
        sums_are_representable, communicator_view, "distributed load balance",
        "local node-load sum exceeds NodeWeight capacity");
    require_collective_condition(
        labels_are_valid, communicator_view,
        "distributed load balance label is outside [0, k)");

    auto overall_weights = std::vector<NodeWeight>(block_count, 0);
    mpi::all_reduce_checked_sum(
        std::span<NodeWeight const>{block_weights}, std::span{overall_weights},
        communicator_view, "MPI_Allreduce(distributed load block weights)",
        "distributed load balance",
        "global block-load sum exceeds NodeWeight capacity");
    return exact_balance_ratio(overall_weights, config.k, communicator_view,
                               "distributed load balance",
                               "global total load exceeds NodeWeight capacity");
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "distributed load-balance computation failed");
  }
}

double distributed_quality_metrics::balance_load_dist(PPartitionConfig&,
                                                      parallel_graph_access& G,
                                                      MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const rank = communicator_view.rank();
  auto const size = communicator_view.size();
  try {
    auto rank_weights =
        std::vector<NodeWeight>(static_cast<std::size_t>(size), 0);
    auto local_weight = NodeWeight{};
    auto sums_are_representable = true;
    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      auto node_load = G.getNodeWeight(node);
      auto const degree = G.getNodeDegree(node);
      auto const node_load_is_representable =
          std::in_range<NodeWeight>(degree) &&
          checked_add(node_load, static_cast<NodeWeight>(degree));
      sums_are_representable =
          node_load_is_representable && sums_are_representable;
      if (node_load_is_representable) {
        sums_are_representable =
            checked_add(local_weight, node_load) && sums_are_representable;
      }
    }
    rank_weights[static_cast<std::size_t>(rank)] = local_weight;
    require_collective_capacity(
        sums_are_representable, communicator_view,
        "distributed rank-load balance",
        "local node-load sum exceeds NodeWeight capacity");

    auto overall_weights =
        std::vector<NodeWeight>(static_cast<std::size_t>(size), 0);
    mpi::all_reduce_checked_sum(
        std::span<NodeWeight const>{rank_weights}, std::span{overall_weights},
        communicator_view, "MPI_Allreduce(distributed rank load weights)",
        "distributed rank-load balance",
        "global rank-load sum exceeds NodeWeight capacity");
    return exact_balance_ratio(overall_weights, static_cast<NodeWeight>(size),
                               communicator_view,
                               "distributed rank-load balance",
                               "global total load exceeds NodeWeight capacity");
  } catch (...) {
    mpi::abort_on_exception(communicator_view.native_handle(),
                            "distributed rank-load balance computation failed");
  }
}

// measure the communication volume of the current graph distribution
EdgeWeight distributed_quality_metrics::comm_vol(PPartitionConfig& config,
                                                 parallel_graph_access& G,
                                                 MPI_Comm communicator) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const rank = communicator_view.rank();

  auto const block_count = validated_block_count(
      config.k, communicator_view,
      "distributed communication volume requires k greater than zero",
      "distributed communication volume k differs across communicator",
      "distributed communication-volume block count exceeds addressable "
      "storage");
  try {
    auto block_volume = std::vector<EdgeWeight>(block_count, 0);
    auto labels_are_valid = true;
    auto local_volume_is_representable = true;
    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      std::vector<bool> block_incident(block_count, false);
      auto const block = static_cast<PartitionID>(G.getNodeLabel(node));
      auto const block_is_valid = block < config.k;
      labels_are_valid = labels_are_valid && block_is_valid;
      if (block_is_valid) {
        block_incident[block] = true;
      }
      auto num_incident_blocks = EdgeWeight{};

      for (EdgeID edge = G.get_first_edge(node),
                  edge_end = G.get_first_invalid_edge(node);
           edge < edge_end; ++edge) {
        auto const target = G.getEdgeTarget(edge);
        auto const target_block =
            static_cast<PartitionID>(G.getNodeLabel(target));
        if (target_block >= config.k) {
          labels_are_valid = false;
          continue;
        }
        if (!block_incident[target_block]) {
          block_incident[target_block] = true;
          local_volume_is_representable =
              checked_add(num_incident_blocks, EdgeWeight{1}) &&
              local_volume_is_representable;
        }
      }
      if (block_is_valid) {
        local_volume_is_representable =
            checked_add(block_volume[static_cast<std::size_t>(block)],
                        num_incident_blocks) &&
            local_volume_is_representable;
      }
    }
    require_collective_capacity(
        local_volume_is_representable, communicator_view,
        "distributed communication volume",
        "local communication-volume sum exceeds EdgeWeight capacity");
    require_collective_condition(
        labels_are_valid, communicator_view,
        "distributed communication-volume label is outside [0, k)");

    auto overall_weights = std::vector<EdgeWeight>(block_count, 0);
    mpi::all_reduce_checked_sum(
        std::span<EdgeWeight const>{block_volume}, std::span{overall_weights},
        communicator_view,
        "MPI_Allreduce(distributed communication volume by block)",
        "distributed communication volume",
        "global block communication-volume sum exceeds EdgeWeight capacity");

    auto total_comm_vol = EdgeWeight{};
    for (auto const weight : overall_weights) {
      if (!checked_add(total_comm_vol, weight)) {
        mpi::abort_on_capacity_failure(
            communicator_view.native_handle(),
            "distributed communication volume",
            "total communication volume exceeds EdgeWeight capacity");
      }
    }
    if (rank == ROOT) {
      EdgeWeight max_comm_vol =
          *(std::max_element(overall_weights.begin(), overall_weights.end()));
      EdgeWeight min_comm_vol =
          *(std::min_element(overall_weights.begin(), overall_weights.end()));

      std::cout << "log> total vol part " << total_comm_vol << std::endl;
      std::cout << "log> max vol part " << max_comm_vol << std::endl;
      std::cout << "log> min vol part " << min_comm_vol << std::endl;
      if (min_comm_vol == 0) {
        std::cout << "log> vol part ratio undefined" << std::endl;
      } else {
        std::cout << "log> vol part ratio "
                  << max_comm_vol / static_cast<double>(min_comm_vol)
                  << std::endl;
      }
    }

    return total_comm_vol;
  } catch (...) {
    mpi::abort_on_exception(
        communicator_view.native_handle(),
        "distributed communication-volume computation failed");
  }
}

// measure the communication volume of the current graph distribution
EdgeWeight distributed_quality_metrics::comm_vol_dist(parallel_graph_access& G,
                                                      MPI_Comm communicator) {
  auto local_comm_vol = EdgeWeight{};
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const rank = communicator_view.rank();
  auto const size = communicator_view.size();
  try {
    auto target_owners_are_valid = true;
    auto local_volume_is_representable = true;

    for (NodeID node = 0, node_end = G.number_of_local_nodes(); node < node_end;
         ++node) {
      std::vector<bool> block_incident(static_cast<std::size_t>(size), false);
      block_incident[static_cast<std::size_t>(rank)] = true;
      auto num_incident_blocks = EdgeWeight{};

      for (EdgeID edge = G.get_first_edge(node),
                  edge_end = G.get_first_invalid_edge(node);
           edge < edge_end; ++edge) {
        auto const target = G.getEdgeTarget(edge);
        if (!G.is_local_node(target)) {
          auto const target_owner = G.getTargetPE(target);
          if (!std::in_range<std::size_t>(target_owner) ||
              target_owner >= size) {
            target_owners_are_valid = false;
            continue;
          }
          auto const owner_index = static_cast<std::size_t>(target_owner);
          if (!block_incident[owner_index]) {
            block_incident[owner_index] = true;
            local_volume_is_representable =
                checked_add(num_incident_blocks, EdgeWeight{1}) &&
                local_volume_is_representable;
          }
        }
      }
      local_volume_is_representable =
          checked_add(local_comm_vol, num_incident_blocks) &&
          local_volume_is_representable;
    }
    require_collective_capacity(
        local_volume_is_representable, communicator_view,
        "distributed communication volume by rank",
        "local communication-volume sum exceeds EdgeWeight capacity");
    require_collective_condition(
        target_owners_are_valid, communicator_view,
        "distributed communication-volume target owner is outside "
        "communicator");

    auto const local_volume = std::array{local_comm_vol};
    auto total_volume = std::array<EdgeWeight, 1>{};
    auto maximum_volume = std::array<EdgeWeight, 1>{};
    auto minimum_volume = std::array<EdgeWeight, 1>{};
    mpi::all_reduce_checked_sum(
        std::span<EdgeWeight const>{local_volume}, std::span{total_volume},
        communicator_view,
        "MPI_Allreduce(distributed communication volume sum)",
        "distributed communication volume by rank",
        "global communication-volume sum exceeds EdgeWeight capacity");
    mpi::reduce_bounded(std::span<EdgeWeight const>{local_volume},
                        std::span{maximum_volume}, mpi::reduction_kind::maximum,
                        ROOT, communicator_view,
                        "MPI_Reduce(distributed communication volume maximum)");
    mpi::reduce_bounded(std::span<EdgeWeight const>{local_volume},
                        std::span{minimum_volume}, mpi::reduction_kind::minimum,
                        ROOT, communicator_view,
                        "MPI_Reduce(distributed communication volume minimum)");

    if (rank == ROOT) {
      std::cout << "log> total vol currentdist " << total_volume[0]
                << std::endl;
      std::cout << "log> max vol currentdist " << maximum_volume[0]
                << std::endl;
      std::cout << "log> min vol currentdist " << minimum_volume[0]
                << std::endl;
      if (minimum_volume[0] == 0) {
        std::cout << "log> vol dist currentratio undefined" << std::endl;
      } else {
        std::cout << "log> vol dist currentratio "
                  << maximum_volume[0] / static_cast<double>(minimum_volume[0])
                  << std::endl;
      }
    }

    return local_comm_vol;
  } catch (...) {
    mpi::abort_on_exception(
        communicator_view.native_handle(),
        "distributed communication-volume-by-rank computation failed");
  }
}
}  // namespace parhip
