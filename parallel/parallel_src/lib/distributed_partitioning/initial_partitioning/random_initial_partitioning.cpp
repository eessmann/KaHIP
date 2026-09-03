/******************************************************************************
 * random_initial_partitioning.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "random_initial_partitioning.h"

#include <cstdint>
#include <iostream>
#include <string_view>

#include "communication/mpi_collectives.h"
#include "communication/mpi_failure.h"
#include "data_structure/parallel_graph_access.h"
#include "tools/distributed_quality_metrics.h"
#include "tools/random_functions.h"
namespace parhip {
namespace {
void require_collectively(bool local_condition,
                          mpi::communicator_view communicator,
                          std::string_view diagnostic) noexcept {
  if (!mpi::detail::collective_predicate(local_condition, communicator)) {
    mpi::abort_on_programming_error(communicator.native_handle(), diagnostic);
  }
}

void require_compatible_graph_communicator(
    mpi::communicator_view communicator,
    parallel_graph_access& graph) noexcept {
  auto const graph_communicator = graph.getCommunicator();
  require_collectively(
      graph_communicator != MPI_COMM_NULL, communicator,
      "random initial partitioning requires a live graph communicator");

  auto relation = int{MPI_UNEQUAL};
  mpi::check_or_abort(
      MPI_Comm_compare(communicator.native_handle(), graph_communicator,
                       &relation),
      communicator.native_handle(),
      "MPI_Comm_compare(random initial partitioning graph)");
  require_collectively(
      relation == MPI_IDENT || relation == MPI_CONGRUENT, communicator,
      "random initial partitioning graph communicator differs in process or "
      "rank order");
}

void require_valid_block_count(PartitionID block_count,
                               mpi::communicator_view communicator) noexcept {
  static_assert(sizeof(PartitionID) <= sizeof(std::uint64_t));
  auto const local = static_cast<std::uint64_t>(block_count);
  auto minimum = std::uint64_t{};
  auto maximum = std::uint64_t{};
  mpi::check_or_abort(
      MPI_Allreduce(&local, &minimum, 1, MPI_UINT64_T, MPI_MIN,
                    communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(random initial partitioning k minimum)");
  mpi::check_or_abort(
      MPI_Allreduce(&local, &maximum, 1, MPI_UINT64_T, MPI_MAX,
                    communicator.native_handle()),
      communicator.native_handle(),
      "MPI_Allreduce(random initial partitioning k maximum)");
  if (minimum != maximum) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "random initial partitioning k differs across communicator");
  }
  if (block_count == 0) {
    mpi::abort_on_programming_error(
        communicator.native_handle(),
        "random initial partitioning requires k greater than zero");
  }
}
}  // namespace

random_initial_partitioning::random_initial_partitioning() {
                
}

random_initial_partitioning::~random_initial_partitioning() {
                
}


void random_initial_partitioning::perform_partitioning(
    mpi::communicator_view communicator,
    PPartitionConfig& config,
    parallel_graph_access& G) {
  mpi::require_live_intracommunicator(
      communicator,
      "random initial partitioning requires a live intracommunicator");
  auto const rank = communicator.rank();
  auto const size = communicator.size();
  require_collectively(
      size > 0 && rank >= 0 && rank < size, communicator,
      "random initial partitioning received an invalid communicator rank or "
      "size");
  require_compatible_graph_communicator(communicator, G);
  require_valid_block_count(config.k, communicator);

  try {
    forall_local_nodes(G, node) {
      G.setNodeLabel(
          node,
          random_functions::nextInt(
              NodeID{0}, config.k - PartitionID{1}));
    } endfor

    G.update_ghost_node_data_global();

    auto quality = distributed_quality_metrics{};
    auto const edge_cut =
        quality.edge_cut(G, communicator.native_handle());
    auto const balance =
        quality.balance(config, G, communicator.native_handle());

    if (rank == ROOT) {
      std::cout << "log>initial edge edge cut " << edge_cut << '\n';
      std::cout << "log>initial imbalance " << balance << '\n';
    }
  } catch (...) {
    mpi::abort_on_exception(communicator.native_handle(),
                            "random initial partitioning failed");
  }
}
}
