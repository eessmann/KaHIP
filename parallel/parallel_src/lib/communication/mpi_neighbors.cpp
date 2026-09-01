#include "communication/mpi_neighbors.h"

#include <algorithm>
#include <utility>

namespace parhip::mpi {
distributed_graph::distributed_graph(communicator_view source,
                                     std::vector<int> outgoing_destinations) {
  auto const rank = source.rank();
  auto const size = source.size();
  auto const local_destination_ranks_are_valid = std::ranges::all_of(
      outgoing_destinations, [size](auto const destination) {
        return destination >= 0 && destination < size;
      });

  std::ranges::sort(outgoing_destinations);
  auto const unique_end = std::ranges::unique(outgoing_destinations);
  outgoing_destinations.erase(unique_end.begin(), unique_end.end());
  auto const local_outdegree_is_representable =
      std::in_range<int>(outgoing_destinations.size());

  auto destination_ranks_are_valid = false;
  auto outdegree_is_representable = false;
  {
    auto validation_communicator = communicator{source};
    auto const collective_communicator = validation_communicator.view();
    destination_ranks_are_valid = detail::collective_predicate(
        local_destination_ranks_are_valid, collective_communicator);
    outdegree_is_representable = detail::collective_predicate(
        local_outdegree_is_representable, collective_communicator);
  }
  // KAHIP_SEMANTIC_EXIT_BEGIN(distributed-graph-rank-domain)
  if (!destination_ranks_are_valid) {
    throw_collectively_agreed_semantic_error(
        source.native_handle(),
        "distributed graph destination validation failed");
  }
  // KAHIP_SEMANTIC_EXIT_END(distributed-graph-rank-domain)
  if (!outdegree_is_representable) {
    throw mpi_error{MPI_ERR_ARG,
                    "distributed graph outdegree exceeds MPI int capacity"};
  }

  {
    auto construction_communicator = communicator{source};
    auto const outdegree = static_cast<int>(outgoing_destinations.size());
    // Although a zero-degree destination array is never dereferenced, some
    // MPI implementations reject a null array argument. Keep the zero-degree
    // topology portable with a valid ignored pointer.
    auto const ignored_destination = rank;
    auto const* destinations = outgoing_destinations.empty()
                                   ? &ignored_destination
                                   : outgoing_destinations.data();
    check_or_abort(
        MPI_Dist_graph_create(construction_communicator.native_handle(), 1,
                              &rank, &outdegree, destinations, MPI_UNWEIGHTED,
                              MPI_INFO_NULL, 0, &communicator_),
        construction_communicator.native_handle(), "MPI_Dist_graph_create");
  }

  auto const handler_result =
      MPI_Comm_set_errhandler(communicator_, MPI_ERRORS_RETURN);
  if (handler_result != MPI_SUCCESS) {
    abort_on_mpi_error(communicator_, handler_result,
                       "MPI_Comm_set_errhandler(distributed graph)");
  }

  try {
    auto topology_kind = MPI_UNDEFINED;
    check_or_abort(MPI_Topo_test(communicator_, &topology_kind), communicator_,
                   "MPI_Topo_test(distributed graph)");
    if (topology_kind != MPI_DIST_GRAPH) {
      abort_on_mpi_error(communicator_, MPI_ERR_TOPOLOGY,
                         "distributed graph topology verification");
    }

    auto indegree = 0;
    auto outdegree = 0;
    auto weighted = 0;
    check_or_abort(MPI_Dist_graph_neighbors_count(communicator_, &indegree,
                                                  &outdegree, &weighted),
                   communicator_, "MPI_Dist_graph_neighbors_count");
    if (indegree < 0 || outdegree < 0) {
      abort_on_mpi_error(communicator_, MPI_ERR_TOPOLOGY,
                         "distributed graph reported a negative degree");
    }

    sources_.resize(static_cast<std::size_t>(indegree));
    destinations_.resize(static_cast<std::size_t>(outdegree));
    check_or_abort(MPI_Dist_graph_neighbors(
                       communicator_, indegree, sources_.data(), MPI_UNWEIGHTED,
                       outdegree, destinations_.data(), MPI_UNWEIGHTED),
                   communicator_, "MPI_Dist_graph_neighbors");
    source_lookup_ = make_lookup(sources_);
    destination_lookup_ = make_lookup(destinations_);
  } catch (...) {
    abort_on_exception(communicator_, "distributed graph local failure");
  }
}

distributed_graph::~distributed_graph() noexcept {
  reset();
}

distributed_graph::distributed_graph(distributed_graph&& other) noexcept
    : communicator_(std::exchange(other.communicator_, MPI_COMM_NULL)),
      sources_(std::move(other.sources_)),
      destinations_(std::move(other.destinations_)),
      source_lookup_(std::move(other.source_lookup_)),
      destination_lookup_(std::move(other.destination_lookup_)) {}

auto distributed_graph::operator=(distributed_graph&& other) noexcept
    -> distributed_graph& {
  if (this != &other) {
    reset();
    communicator_ = std::exchange(other.communicator_, MPI_COMM_NULL);
    sources_ = std::move(other.sources_);
    destinations_ = std::move(other.destinations_);
    source_lookup_ = std::move(other.source_lookup_);
    destination_lookup_ = std::move(other.destination_lookup_);
  }
  return *this;
}

auto distributed_graph::make_lookup(std::span<int const> ranks)
    -> std::vector<rank_index> {
  auto result = std::vector<rank_index>{};
  result.reserve(ranks.size());
  for (std::size_t index = 0; index < ranks.size(); ++index) {
    result.emplace_back(ranks[index], index);
  }
  std::ranges::sort(result, {}, &rank_index::first);
  return result;
}

auto distributed_graph::find_index(std::span<rank_index const> lookup,
                                   int rank) noexcept
    -> std::optional<std::size_t> {
  auto const position =
      std::ranges::lower_bound(lookup, rank, {}, &rank_index::first);
  if (position == lookup.end() || position->first != rank) {
    return std::nullopt;
  }
  return position->second;
}

void distributed_graph::reset() noexcept {
  if (communicator_ != MPI_COMM_NULL && runtime_is_active()) {
    auto const free_result = MPI_Comm_free(&communicator_);
    if (free_result != MPI_SUCCESS) {
      abort_on_mpi_error(MPI_COMM_WORLD, free_result,
                         "MPI_Comm_free(distributed graph)");
    }
  }
  communicator_ = MPI_COMM_NULL;
  sources_.clear();
  destinations_.clear();
  source_lookup_.clear();
  destination_lookup_.clear();
}
}  // namespace parhip::mpi
