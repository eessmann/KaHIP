/******************************************************************************
 * mpi_tools.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef MPI_TOOLS_HMESDXF2
#define MPI_TOOLS_HMESDXF2

#ifndef _CRAYC
#define _CRAYC 0
#endif

#include <algorithm>
#include <functional>
#include <numeric>
#include <ranges>

#include "data_structure/parallel_graph_access.h"
#include "mpi_types.h"
#include "partition_config.h"

class mpi_tools {
public:
  void collect_and_write_labels(MPI_Comm communicator, PPartitionConfig &config,
                                parallel_graph_access &G);

  void collect_parallel_graph_to_local_graph(MPI_Comm communicator,
                                             PPartitionConfig &config,
                                             parallel_graph_access &G,
                                             complete_graph_access &Q);

  // G is input (only on ROOT)
  // G is output (on every other PE)
  void distribute_local_graph(MPI_Comm communicator, PPartitionConfig &config,
                              complete_graph_access &G);

};

namespace mpi {

template <typename Elem> struct mpi_packed_message {
  std::vector<Elem> packed_message;
  std::vector<int> offsets;
  std::vector<int> lengths;
};

auto exchange_num_messages(std::vector<int> const& num_sent_per_rank,
													 MPI_Comm communicator) -> std::vector<int>;


template <std::ranges::forward_range Input>
  requires std::ranges::forward_range<std::ranges::range_value_t<Input>>
auto pack_messages(const Input& messages)
    -> mpi_packed_message<std::ranges::range_value_t<std::ranges::range_value_t<Input>>> {
  using InnerRange = std::ranges::range_value_t<Input>;
  using ElementType = std::ranges::range_value_t<InnerRange>;

  // Flattening the container of containers using views::join
  auto flattened_view = messages | std::ranges::views::join;
  std::vector<ElementType> flattened_vector{flattened_view.begin(), flattened_view.end()};

  // Calculating lengths of the inner ranges
  std::vector<int> lengths;
  lengths.reserve(std::ranges::distance(messages));
  for (const auto& inner : messages) {
    lengths.push_back(static_cast<int>(std::ranges::distance(inner)));
  }

  // Calculating offsets using exclusive_scan
  std::vector<int> offsets(lengths.size());
  std::exclusive_scan(lengths.begin(), lengths.end(), offsets.begin(), 0);

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
  std::transform(
      recv_displs.begin(), recv_displs.end(), recv_counts.begin(),
      std::back_inserter(result),
      [&recv_buf](int displ, int count) {
          auto const start = recv_buf.begin() + displ;
          auto const end = start + count;
          return std::vector<Elem>(start, end);
      });

  return result;
}

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
template <std::ranges::forward_range Input>
  requires std::ranges::forward_range<std::ranges::range_value_t<Input>>
auto all_to_all(const Input& sends, MPI_Comm communicator)
    -> std::vector<std::vector<std::ranges::range_value_t<std::ranges::range_value_t<Input>>>> {
  using InnerRange = std::ranges::range_value_t<Input>;
  using ElementType = std::ranges::range_value_t<InnerRange>;

  PEID rank, size;
  MPI_Comm_rank(communicator, &rank);
  MPI_Comm_size(communicator, &size);

  // Packing messages
  auto [send_packed_messages, send_offsets, send_lengths] = mpi::pack_messages(sends);

  if (send_offsets.size() != send_lengths.size()) {
    throw std::runtime_error("mpi::pack_messages(): send_offsets.size() (" + std::to_string(send_offsets.size()) +
                             ") != send_lengths.size() (" + std::to_string(send_lengths.size()) + ")");
  } else if ((send_offsets.size() != static_cast<std::size_t>(size)) || (send_lengths.size() != static_cast<std::size_t>(size))) {
    throw std::runtime_error("mpi::pack_messages(): send_offsets.size() (" + std::to_string(send_offsets.size()) +
                             ") != mpi size (" + std::to_string(size) + ")");
  }

  // Exchanging message sizes
  auto const recv_lengths = exchange_num_messages(send_lengths, communicator);

  // Calculating total receive buffer size
  auto const recv_buff_size = std::accumulate(recv_lengths.begin(), recv_lengths.end(), 0);

  // Preparing receive buffers
  std::vector<ElementType> recv_packed_messages(recv_buff_size);
  std::vector<int> recv_offsets(size, 0);

  // Calculating receive offsets
  std::exclusive_scan(recv_lengths.begin(), recv_lengths.end(), recv_offsets.begin(), 0);

  // Performing MPI communication
  auto mpi_error = MPI_Alltoallv(
      send_packed_messages.data(), send_lengths.data(), send_offsets.data(),
      get_mpi_datatype<ElementType>(), recv_packed_messages.data(),
      recv_lengths.data(), recv_offsets.data(),
      get_mpi_datatype<ElementType>(), communicator);

  if (mpi_error != MPI_SUCCESS) {
    char error_string[MPI_MAX_ERROR_STRING];
    int length_of_error_string;
    MPI_Error_string(mpi_error, error_string, &length_of_error_string);
    throw std::runtime_error(std::string("mpi::all_to_all() failed with error: ") + error_string);
  }

  // Unpacking messages
  return mpi::unpack_messages<ElementType>({recv_packed_messages, recv_offsets, recv_lengths});
}
} // namespace mpi

#endif /* end of include guard: MPI_TOOLS_HMESDXF2 */
