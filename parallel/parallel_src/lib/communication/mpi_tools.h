/******************************************************************************
 * mpi_tools.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef MPI_TOOLS_HMESDXF2
#define MPI_TOOLS_HMESDXF2

#include <algorithm>
#include <functional>
#include <numeric>
#include <ranges>
#include <string>

#include "data_structure/parallel_graph_access.h"
#include "communication/mpi_adapter.h"
#include "partition_config.h"
namespace parhip {
class mpi_tools {
public:
  void collect_and_write_labels(MPI_Comm communicator,
                                PPartitionConfig& config,
                                parallel_graph_access& G);

  void collect_parallel_graph_to_local_graph(MPI_Comm communicator,
                                             PPartitionConfig& config,
                                             parallel_graph_access& G,
                                             complete_graph_access& Q);

  // G is input (only on ROOT)
  // G is output (on every other PE)
  void distribute_local_graph(MPI_Comm communicator,
                              PPartitionConfig& config,
                              complete_graph_access& G);
};

namespace mpi {

template <typename Elem>
struct mpi_packed_message {
  std::vector<Elem> packed_message;
  std::vector<std::size_t> offsets;
  std::vector<std::size_t> lengths;
};

template <std::ranges::forward_range Input>
  requires std::ranges::forward_range<std::ranges::range_value_t<Input>>
auto pack_messages(const Input& messages) -> mpi_packed_message<
    std::ranges::range_value_t<std::ranges::range_value_t<Input>>> {
  using InnerRange = std::ranges::range_value_t<Input>;
  using ElementType = std::ranges::range_value_t<InnerRange>;

  // Flattening the container of containers using views::join
  auto flattened_view = messages | std::ranges::views::join;
  std::vector<ElementType> flattened_vector{flattened_view.begin(),
                                            flattened_view.end()};

  // Calculating lengths of the inner ranges
  std::vector<std::size_t> lengths;
  lengths.reserve(std::ranges::distance(messages));
  for (const auto& inner : messages) {
    lengths.push_back(
        static_cast<std::size_t>(std::ranges::distance(inner)));
  }

  // Calculating offsets using exclusive_scan
  std::vector<std::size_t> offsets(lengths.size());
  std::exclusive_scan(lengths.begin(), lengths.end(), offsets.begin(),
                      std::size_t{0});

  return mpi_packed_message<ElementType>{flattened_vector, offsets, lengths};
}

template <typename Elem>
auto unpack_messages(const mpi_packed_message<Elem>& packed_message)
    -> std::vector<std::vector<Elem>> {
  const auto& [recv_buf, recv_displs, recv_counts] = packed_message;
  std::size_t num_ranks = recv_counts.size();

  // Ensure recv_displs and recv_counts have the same size
  assert(recv_displs.size() == num_ranks);

  std::vector<std::vector<Elem>> result;
  result.reserve(num_ranks);

  // Use std::transform to construct the sub-vectors
  std::transform(recv_displs.begin(), recv_displs.end(), recv_counts.begin(),
                 std::back_inserter(result),
                 [&recv_buf](std::size_t displ, std::size_t count) {
                   auto const start = recv_buf.begin() + displ;
                   auto const end = start + count;
                   return std::vector<Elem>(start, end);
                 });

  return result;
}

template <typename Input>
concept mpi_nested_range = requires(Input) {
  requires std::ranges::forward_range<Input>;
  requires std::ranges::forward_range<std::ranges::range_value_t<Input>>;
  requires mpi_datatype<
      std::ranges::range_value_t<std::ranges::range_value_t<Input>>>;
};

template <mpi_nested_range Input>
using mpi_alltoall_t = std::vector<
    std::vector<std::ranges::range_value_t<std::ranges::range_value_t<Input>>>>;

/**
 * @brief Performs an MPI all-to-all communication operation, distributing
 * data from all processes to all processes.
 *
 * This function packs messages from the input data structure, performs an
 * MPI all-to-all communication, and then unpacks the received messages.
 *
 * @param sends A structure containing the data to be sent from each
 * process.
 * @param communicator The MPI communicator used for the all-to-all
 * operation.
 * @return A vector of vectors, where each inner vector contains the data
 * received by a process from other processes.
 * @throws std::runtime_error if there's an inconsistency in the send
 * offsets/lengths or if the MPI operation fails.
 */
template <mpi_nested_range Input>
auto all_to_all(Input const& sends, MPI_Comm communicator)
    -> mpi_alltoall_t<Input> {
  using InnerRange = std::ranges::range_value_t<Input>;
  using ElementType = std::ranges::range_value_t<InnerRange>;

  auto received = all_to_all_v(
      segmented_buffer<ElementType>::from_segments(sends),
      communicator_view{communicator});
  std::vector<std::vector<ElementType>> result;
  result.reserve(received.segment_count());
  for (std::size_t source = 0; source < received.segment_count(); ++source) {
    auto const segment = received.segment(source);
    result.emplace_back(segment.begin(), segment.end());
  }
  return result;
}
}  // namespace mpi
}
#endif /* end of include guard: MPI_TOOLS_HMESDXF2 */
