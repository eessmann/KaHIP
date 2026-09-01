#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "communication/mpi_neighbors.h"
#include "definitions.h"

namespace parhip {
class parallel_graph_access;

class ghost_exchange_plan final {
 public:
  ghost_exchange_plan(ghost_exchange_plan const&) = delete;
  auto operator=(ghost_exchange_plan const&) -> ghost_exchange_plan& = delete;
  ghost_exchange_plan(ghost_exchange_plan&&) = delete;
  auto operator=(ghost_exchange_plan&&) -> ghost_exchange_plan& = delete;

  [[nodiscard]] auto topology() const noexcept
      -> mpi::distributed_graph const& {
    return topology_;
  }
  [[nodiscard]] auto outgoing_local_nodes(
      std::size_t destination_index) const noexcept -> std::span<NodeID const>;
  [[nodiscard]] auto expected_ghost_nodes(
      std::size_t source_index) const noexcept -> std::span<NodeID const>;

 private:
  friend class parallel_graph_access;
  friend auto make_ghost_exchange_plan(parallel_graph_access const& graph)
      -> std::unique_ptr<ghost_exchange_plan>;

  ghost_exchange_plan(mpi::distributed_graph topology,
                      std::vector<NodeID> outgoing_nodes,
                      std::vector<std::size_t> outgoing_offsets,
                      std::vector<std::size_t> outgoing_counts,
                      std::vector<NodeID> expected_ghosts,
                      std::vector<std::size_t> expected_offsets,
                      std::vector<std::size_t> expected_counts) noexcept;

  mpi::distributed_graph topology_;
  std::vector<NodeID> outgoing_nodes_;
  std::vector<std::size_t> outgoing_offsets_;
  std::vector<std::size_t> outgoing_counts_;
  std::vector<NodeID> expected_ghosts_;
  std::vector<std::size_t> expected_offsets_;
  std::vector<std::size_t> expected_counts_;
};

[[nodiscard]] auto make_ghost_exchange_plan(parallel_graph_access const& graph)
    -> std::unique_ptr<ghost_exchange_plan>;
}  // namespace parhip
