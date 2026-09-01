/******************************************************************************
 * balance_management_refinement.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "balance_management_refinement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "communication/mpi_adapter.h"
#include "communication/mpi_fixed_reduction.h"
#include "parallel_graph_access.h"

namespace parhip {
namespace {
[[nodiscard]] constexpr auto checked_add(NodeWeight& accumulator,
                                         NodeWeight value) noexcept -> bool {
  if (value > std::numeric_limits<NodeWeight>::max() - accumulator) {
    return false;
  }
  accumulator += value;
  return true;
}

[[nodiscard]] auto validated_block_count(
    PartitionID local_count,
    mpi::communicator_view communicator) noexcept -> std::size_t {
  static_assert(sizeof(PartitionID) <= sizeof(std::uint64_t));
  auto const encoded = static_cast<std::uint64_t>(local_count);
  auto minimum = std::uint64_t{};
  auto maximum = std::uint64_t{};
  mpi::check_or_abort(MPI_Allreduce(&encoded, &minimum, 1, MPI_UINT64_T,
                                    MPI_MIN, communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(refinement block count minimum)");
  mpi::check_or_abort(MPI_Allreduce(&encoded, &maximum, 1, MPI_UINT64_T,
                                    MPI_MAX, communicator.native_handle()),
                      communicator.native_handle(),
                      "MPI_Allreduce(refinement block count maximum)");
  if (minimum != maximum) {
    mpi::abort_on_programming_error(communicator.native_handle(),
                                    "refinement balance-management block count "
                                    "differs across communicator");
  }
  if (local_count == 0) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "refinement balance management requires at least one block");
  }
  if (!std::in_range<std::size_t>(local_count)) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "refinement balance management",
                                   "block count exceeds addressable storage");
  }
  return static_cast<std::size_t>(local_count);
}

void require_collective_condition(bool local_condition,
                                  mpi::communicator_view communicator,
                                  std::string_view diagnostic) noexcept {
  auto const local = local_condition ? 1 : 0;
  auto all_are_valid = 0;
  mpi::check_or_abort(
      MPI_Allreduce(&local, &all_are_valid, 1, MPI_INT, MPI_MIN,
                    communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(refinement balance-management validation)");
  if (all_are_valid == 0) {
    mpi::abort_on_programming_error(communicator.native_handle(), diagnostic);
  }
}

void require_collective_capacity(bool local_condition,
                                 mpi::communicator_view communicator,
                                 std::string_view diagnostic) noexcept {
  auto const local = local_condition ? 1 : 0;
  auto all_are_representable = 0;
  mpi::check_or_abort(
      MPI_Allreduce(&local, &all_are_representable, 1, MPI_INT, MPI_MIN,
                    communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(refinement balance-management capacity validation)");
  if (all_are_representable == 0) {
    mpi::abort_on_capacity_failure(communicator.native_handle(),
                                   "refinement balance management", diagnostic);
  }
}
}  // namespace

balance_management_refinement::balance_management_refinement(
    parallel_graph_access* graph,
    PartitionID total_num_labels)
    : balance_management(graph, total_num_labels) {
  if (graph == nullptr) {
    mpi::abort_on_programming_error(
        MPI_COMM_NULL,
        "refinement balance management requires a graph instance");
  }
  auto operation_communicator =
      mpi::communicator{mpi::communicator_view{graph->getCommunicator()}};
  auto const communicator = operation_communicator.view();
  try {
    auto const block_count =
        validated_block_count(total_num_labels, communicator);
    m_total_block_weights.assign(block_count, 0);
    m_local_block_weights.assign(block_count, 0);
    init(communicator);
  } catch (...) {
    mpi::abort_on_exception(
        communicator.native_handle(),
        "refinement balance-management construction failed");
  }
}

balance_management_refinement::~balance_management_refinement() = default;

void balance_management_refinement::init() {
  auto operation_communicator =
      mpi::communicator{mpi::communicator_view{m_G->getCommunicator()}};
  auto const communicator = operation_communicator.view();
  try {
    init(communicator);
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "refinement balance-management init failed");
  }
}

void balance_management_refinement::init(mpi::communicator_view communicator) {
  std::ranges::fill(m_local_block_weights, NodeWeight{});
  auto labels_are_valid = true;
  auto sums_are_representable = true;
  for (NodeID node = 0, node_end = m_G->number_of_local_nodes();
       node < node_end; ++node) {
    auto const label = static_cast<PartitionID>(m_G->getNodeLabel(node));
    if (!std::in_range<std::size_t>(label) ||
        static_cast<std::size_t>(label) >= m_local_block_weights.size()) {
      labels_are_valid = false;
      continue;
    }
    sums_are_representable =
        checked_add(m_local_block_weights[static_cast<std::size_t>(label)],
                    m_G->getNodeWeight(node)) &&
        sums_are_representable;
  }
  require_collective_capacity(
      sums_are_representable, communicator,
      "local block-weight sum exceeds NodeWeight capacity");
  require_collective_condition(
      labels_are_valid, communicator,
      "refinement balance-management label is outside [0, k)");
  update(communicator);
}

void balance_management_refinement::update() {
  auto operation_communicator =
      mpi::communicator{mpi::communicator_view{m_G->getCommunicator()}};
  auto const communicator = operation_communicator.view();
  try {
    update(communicator);
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "refinement balance-management update failed");
  }
}

void balance_management_refinement::update(
    mpi::communicator_view communicator) {
  mpi::all_reduce_checked_sum(
      std::span<NodeWeight const>{m_local_block_weights},
      std::span<NodeWeight>{m_total_block_weights}, communicator,
      "MPI_Allreduce(refinement block weights)",
      "refinement balance management",
      "global block-weight sum exceeds NodeWeight capacity");
}

void balance_management_refinement::setBlockSize(PartitionID block,
                                                 NodeWeight block_size) {
  if (!std::in_range<std::size_t>(block) ||
      static_cast<std::size_t>(block) >= m_total_block_weights.size()) {
    mpi::abort_on_programming_error(
        m_G->getCommunicator(),
        "refinement balance-management block is outside [0, k)");
  }
  auto const index = static_cast<std::size_t>(block);
  auto const previous_total = m_total_block_weights[index];
  auto updated_local = m_local_block_weights[index];
  if (block_size >= previous_total) {
    auto const increase = block_size - previous_total;
    if (!checked_add(updated_local, increase)) {
      mpi::abort_on_capacity_failure(
          m_G->getCommunicator(), "refinement balance management",
          "local block-weight update exceeds NodeWeight capacity");
    }
  } else {
    auto const decrease = previous_total - block_size;
    if (decrease > updated_local) {
      mpi::abort_on_programming_error(
          m_G->getCommunicator(),
          "refinement balance-management local block weight is stale");
    }
    updated_local -= decrease;
  }
  m_local_block_weights[index] = updated_local;
  m_total_block_weights[index] = block_size;
}

auto balance_management_refinement::getBlockSize(PartitionID block)
    -> NodeWeight {
  if (!std::in_range<std::size_t>(block) ||
      static_cast<std::size_t>(block) >= m_total_block_weights.size()) {
    mpi::abort_on_programming_error(
        m_G->getCommunicator(),
        "refinement balance-management block is outside [0, k)");
  }
  return m_total_block_weights[static_cast<std::size_t>(block)];
}
}  // namespace parhip
