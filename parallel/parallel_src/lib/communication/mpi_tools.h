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

  // alltoallv that can send more than int-count elements
  void alltoallv(void *sendbuf, ULONG sendcounts[], ULONG displs[],
                 const MPI_Datatype &sendtype, void *recvbuf,
                 ULONG recvcounts[], ULONG rdispls[],
                 const MPI_Datatype &recvtype) {
    alltoallv(sendbuf, sendcounts, displs, sendtype, recvbuf, recvcounts,
              rdispls, recvtype, MPI_COMM_WORLD);
  };

  void alltoallv(void *sendbuf, ULONG sendcounts[], ULONG displs[],
                 const MPI_Datatype &sendtype, void *recvbuf,
                 ULONG recvcounts[], ULONG rdispls[],
                 const MPI_Datatype &recvtype, MPI_Comm communicator);
};

namespace mpi {

template <typename Elem> struct mpi_packed_message {
  std::vector<Elem> packed_message;
  std::vector<int> offsets;
  std::vector<int> lengths;
};

/**
 * Packs a container of containers into a single flat vector suitable for
 * MPI communication.
 *
 * This function takes a nested container (e.g.,
 * `std::vector<std::vector<T>>`) and flattens it into a single
 * `std::vector<T>`. It also calculates the lengths and offsets necessary to
 * reconstruct the original nested container structure.
 *
 * Transformations:
 * - Flattens the nested container using `std::ranges::views::join`.
 * - Converts the flattened view into a `std::vector`.
 * - Calculates the lengths of the inner containers.
 * - Calculates the offsets for each inner container using exclusive_scan.
 *
 * Requirements:
 * - `Input` must be a container of containers.
 * - The inner containers must support the `value_type` type alias.
 *
 * @param messages The input container of containers to be packed.
 * @return A `mpi_packed_message` object containing:
 *         - `flattened_vector`: A single vector containing all elements of
 * the nested containers.
 *         - `offsets`: A vector of offsets for where each inner container
 * starts in the flattened vector.
 *         - `lengths`: A vector of lengths for each inner container in the
 * original nested container.
 */
template <container Input>
  requires container<typename Input::value_type>
auto pack_messages(Input const &messages)
    -> mpi_packed_message<typename Input::value_type::value_type> {
  using inner = typename Input::value_type;
  using element_type = typename inner::value_type;

  // Flattening the container of containers using join_view
  auto const flattened_view = messages | std::ranges::views::join;
  std::vector<element_type> flattened_vector{};
  // converting view into vector
  for (auto &&elem : flattened_view) {
    flattened_vector.push_back(static_cast<decltype(elem) &&>(elem));
  }

  // Calculating lengths using transform view
  std::vector<int> lengths;
  lengths.reserve(messages.size());
  std::ranges::transform(
      messages, std::back_inserter(lengths),
      [](auto const &elem) { return static_cast<int>(elem.size()); });

  // Calculating offsets using exclusive_scan
  std::vector<int> offsets(messages.size());
  std::exclusive_scan(lengths.begin(), lengths.end(), offsets.begin(), 0);

  return mpi_packed_message<element_type>{flattened_vector, offsets, lengths};
}

/**
 * Unpacks a flat MPI packed message into a nested vector.
 *
 * This function transforms a packed MPI message, represented as an
 * `mpi_packed_message<Elem>`, back into a vector of vectors.
 * The original structure of the nested container is reconstructed.
 *
 * The function deconstructs the `mpi_packed_message` into its constituent
 * parts:
 *  - A flat buffer containing all elements.
 *  - Displacements indicating the start of each inner vector within the
 * flat buffer.
 *  - Counts indicating the lengths of each inner vector.
 *
 * It handles two different approaches depending on whether the `_CRAYC`
 * macro is defined:
 *  - If `_CRAYC` is defined, it uses vector insertion.
 *  - Otherwise, it uses `std::span` to subspan elements.
 *
 * @tparam Elem Type of elements contained in the message.
 * @param packed_message The packed message to be unpacked.
 * @return A nested vector of elements, reconstructing the original
 * structure.
 */
template <typename Elem>
auto unpack_messages(mpi_packed_message<Elem> const &packed_message)
    -> std::vector<std::vector<Elem>> {
  auto const &[recv_buf, recv_displs, recv_counts] = packed_message;
  int num_ranks = static_cast<int>(recv_counts.size());

  std::vector<std::vector<Elem>> result;
  result.reserve(num_ranks);
  if constexpr (_CRAYC) {
    puts("Cray compiler detected");
    for (int i = 0; i < num_ranks; ++i) {
      std::vector<Elem> subvec{};
      subvec.insert(subvec.begin(), recv_buf.begin() + recv_displs[i],
                    recv_buf.begin() + recv_displs[i] + recv_counts[i]);
      result.emplace_back(subvec.begin(), subvec.end());
    }
  } else {
    auto const recv_span = std::span(recv_buf);
    puts("Not using Cray compiler");
    for (int i = 0; i < num_ranks; ++i) {
      auto const subspan =
          recv_span.subspan(recv_displs.at(i), recv_counts.at(i));
      result.emplace_back(subspan.begin(), subspan.end());
    }
  }

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
template <container Input>
  requires container<typename Input::value_type>
auto all_to_all(Input const &sends, MPI_Comm communicator)
    -> std::vector<std::vector<typename Input::value_type::value_type>> {
  using inner = typename Input::value_type;
  using element_type = typename inner::value_type;

  PEID rank, size;
  MPI_Comm_rank(communicator, &rank);
  MPI_Comm_size(communicator, &size);

  // Packing messages into vector and computing offsets and lengths for the sub
  // messages
  auto [send_packed_messages, send_offsets, send_lengths] =
      mpi::pack_messages(sends);

  if (send_offsets.size() != send_lengths.size()) {
    throw(std::runtime_error("MPI_collective_tools::pack_messages()"));
  } else if ((send_offsets.size() != size) || (send_lengths.size() != size)) {
    throw(std::runtime_error("MPI_collective_tools::pack_messages()"));
  }

  // Preparing receive buffers for the node ids, offsets, and lengths
  std::vector<int> num_recv_from_rank(send_lengths.size(),
                                      0); // number of messages from each rank
  MPI_Alltoall(send_lengths.data(), 1, MPI_INT, num_recv_from_rank.data(), 1,
               MPI_INT, communicator);
  auto const recv_buff_size =
      std::reduce(num_recv_from_rank.begin(), num_recv_from_rank.end(), 0);

  auto recv_packed_messages = std::vector<element_type>(recv_buff_size, 0);
  auto recv_offsets = std::vector<int>(size, 0);
  auto const recv_lengths = num_recv_from_rank;

  // Calculating recv offsets
  std::exclusive_scan(recv_lengths.begin(), recv_lengths.end(),
                      recv_offsets.begin(), 0);

  auto const mpi_error = MPI_Alltoallv(
      send_packed_messages.data(), send_lengths.data(), send_offsets.data(),
      get_mpi_datatype<element_type>(), recv_packed_messages.data(),
      recv_lengths.data(), recv_offsets.data(),
      get_mpi_datatype<element_type>(), communicator);
  if (mpi_error != MPI_SUCCESS) {
    throw(std::runtime_error("MPI_collective_tools::all_to_all()"));
  }
  return mpi::unpack_messages<element_type>(
      {recv_packed_messages, recv_offsets, recv_lengths});
}
} // namespace mpi

#endif /* end of include guard: MPI_TOOLS_HMESDXF2 */
