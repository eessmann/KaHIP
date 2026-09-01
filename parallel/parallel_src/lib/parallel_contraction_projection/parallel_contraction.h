/******************************************************************************
 * parallel_contraction.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_CONTRACTION_64O127GD
#define PARALLEL_CONTRACTION_64O127GD

#include <concepts>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <unordered_map>

#include "communication/mpi_tools.h"
#include "data_structure/hashed_graph.h"
#include "data_structure/parallel_graph_access.h"
#include "partition_config.h"
namespace parhip {
struct parallel_contraction_test_access;

class parallel_contraction {
public:
  void contract_to_distributed_quotient(MPI_Comm communicator,
                                        PPartitionConfig& config,
                                        parallel_graph_access& G,
                                        parallel_graph_access& Q);

private:
  friend struct parallel_contraction_test_access;

  // compute mapping of labels id into contiguous intervall [0,...,num_lables)
  void compute_label_mapping(MPI_Comm communicator,
                             parallel_graph_access& G,
                             NodeID& global_num_distinct_ids,
                             std::unordered_map<NodeID, NodeID>& label_mapping);

  void get_nodes_to_cnodes_ghost_nodes(
      MPI_Comm communicator,
      parallel_graph_access& G,
      NodeID number_of_distinct_labels,
      std::unordered_map<NodeID, NodeID> const& label_mapping);

  void build_quotient_graph_locally(
      MPI_Comm communicator,
      parallel_graph_access& G,
      NodeID number_of_distinct_labels,
      hashed_graph& hG,
      std::unordered_map<NodeID, NodeWeight>& node_weights);

  void redistribute_hased_graph_and_build_graph_locally(
      MPI_Comm communicator,
      hashed_graph& hG,
      std::unordered_map<NodeID, NodeWeight>& node_weights,
      NodeID number_of_cnodes,
      parallel_graph_access& Q);

  void update_ghost_nodes_weights(MPI_Comm communicator,
                                  parallel_graph_access& G);
};

// Comm types
namespace contraction {
struct label_request {
  NodeID old_label;
};

struct label_reply {
  NodeID old_label;
  NodeID coarse_global_id;
};

struct bundled_edge {
  NodeID source;
  NodeID target;
  EdgeWeight weight;
  NodeID sender_sequence;
};

struct node_weight_contribution {
  NodeID coarse_global_id;
  NodeWeight weight;
};

struct ghost_cnode_assignment {
  NodeID global_id;
  NodeID coarse_global_id;
};

struct ghost_node_weight {
  NodeID global_id;
  NodeWeight weight;
};

template <std::unsigned_integral T>
[[nodiscard]] constexpr auto checked_add(T lhs, T rhs) noexcept
    -> std::optional<T> {
  if (rhs > std::numeric_limits<T>::max() - lhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

[[nodiscard]] constexpr auto checked_local_edge_count_increment(
    EdgeID count,
    bool target_is_local) noexcept -> std::optional<EdgeID> {
  return checked_add(count, target_is_local ? EdgeID{2} : EdgeID{1});
}

template <std::unsigned_integral T, std::ranges::input_range Range>
  requires std::
      same_as<std::remove_cvref_t<std::ranges::range_reference_t<Range>>, T>
    [[nodiscard]] constexpr auto checked_sum(Range&& values)
        -> std::optional<T> {
  auto sum = T{0};
  for (auto const value : values) {
    auto const next = checked_add(sum, value);
    if (!next.has_value()) {
      return std::nullopt;
    }
    sum = *next;
  }
  return sum;
}
}  // namespace contraction

static_assert(std::is_standard_layout_v<contraction::label_request>);
static_assert(std::is_trivially_copyable_v<contraction::label_request>);
static_assert(std::is_standard_layout_v<contraction::label_reply>);
static_assert(std::is_trivially_copyable_v<contraction::label_reply>);
static_assert(std::is_standard_layout_v<contraction::bundled_edge>);
static_assert(std::is_trivially_copyable_v<contraction::bundled_edge>);
static_assert(
    std::is_standard_layout_v<contraction::node_weight_contribution>);
static_assert(
    std::is_trivially_copyable_v<contraction::node_weight_contribution>);
static_assert(std::is_standard_layout_v<contraction::ghost_cnode_assignment>);
static_assert(
    std::is_trivially_copyable_v<contraction::ghost_cnode_assignment>);
static_assert(std::is_standard_layout_v<contraction::ghost_node_weight>);
static_assert(std::is_trivially_copyable_v<contraction::ghost_node_weight>);
}

template <>
struct parhip::mpi::wire_members<parhip::contraction::label_request> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &parhip::contraction::label_request::old_label);
};

template <>
struct parhip::mpi::wire_members<parhip::contraction::label_reply> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &parhip::contraction::label_reply::old_label,
      &parhip::contraction::label_reply::coarse_global_id);
};

template <>
struct parhip::mpi::wire_members<parhip::contraction::bundled_edge> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &parhip::contraction::bundled_edge::source,
      &parhip::contraction::bundled_edge::target,
      &parhip::contraction::bundled_edge::weight,
      &parhip::contraction::bundled_edge::sender_sequence);
};

template <>
struct parhip::mpi::wire_members<
    parhip::contraction::node_weight_contribution> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &parhip::contraction::node_weight_contribution::coarse_global_id,
      &parhip::contraction::node_weight_contribution::weight);
};

template <>
struct parhip::mpi::wire_members<parhip::contraction::ghost_cnode_assignment> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &parhip::contraction::ghost_cnode_assignment::global_id,
      &parhip::contraction::ghost_cnode_assignment::coarse_global_id);
};

template <>
struct parhip::mpi::wire_members<parhip::contraction::ghost_node_weight> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &parhip::contraction::ghost_node_weight::global_id,
      &parhip::contraction::ghost_node_weight::weight);
};
#endif /* end of include guard: PARALLEL_CONTRACTION_64O127GD */
