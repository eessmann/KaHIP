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
#include <ranges>
#include <numeric>

#include "data_structure/parallel_graph_access.h"
#include "partition_config.h"
#include "mpi_types.h"

class mpi_tools {
public:
    mpi_tools();

    virtual ~mpi_tools();

    void collect_and_write_labels(MPI_Comm communicator, PPartitionConfig &config,
                                  parallel_graph_access &G);

    void collect_parallel_graph_to_local_graph(MPI_Comm communicator,
                                               PPartitionConfig &config,
                                               parallel_graph_access &G,
                                               complete_graph_access &Q);

    // G is input (only on ROOT)
    // G is output (on every other PE)
    void distribute_local_graph(MPI_Comm communicator, PPartitionConfig &config, complete_graph_access &G);

    // alltoallv that can send more than int-count elements
    void alltoallv(void *sendbuf,
                   ULONG sendcounts[], ULONG displs[],
                   const MPI_Datatype &sendtype, void *recvbuf,
                   ULONG recvcounts[], ULONG rdispls[],
                   const MPI_Datatype &recvtype) {
        alltoallv(sendbuf, sendcounts, displs, sendtype, recvbuf, recvcounts, rdispls, recvtype, MPI_COMM_WORLD);
    };

    void alltoallv(void *sendbuf,
                   ULONG sendcounts[], ULONG displs[],
                   const MPI_Datatype &sendtype, void *recvbuf,
                   ULONG recvcounts[], ULONG rdispls[],
                   const MPI_Datatype &recvtype, MPI_Comm communicator);
};

namespace mpi_collective_tools {
    template<class ContainerType>
    concept Container = requires(ContainerType a, const ContainerType b)
    {
        requires std::regular<ContainerType>;
        requires std::swappable<ContainerType>;
        requires std::destructible<typename ContainerType::value_type>;
        requires std::same_as<typename ContainerType::reference, typename ContainerType::value_type &>;
        requires std::same_as<typename ContainerType::const_reference, const typename ContainerType::value_type &>;
        requires std::forward_iterator<typename ContainerType::iterator>;
        requires std::forward_iterator<typename ContainerType::const_iterator>;
        requires std::signed_integral<typename ContainerType::difference_type>;
        requires std::same_as<typename ContainerType::difference_type, typename std::iterator_traits<typename
            ContainerType::iterator>::difference_type>;
        requires std::same_as<typename ContainerType::difference_type, typename std::iterator_traits<typename
            ContainerType::const_iterator>::difference_type>;
        { a.begin() } -> std::same_as<typename ContainerType::iterator>;
        { a.end() } -> std::same_as<typename ContainerType::iterator>;
        { b.begin() } -> std::same_as<typename ContainerType::const_iterator>;
        { b.end() } -> std::same_as<typename ContainerType::const_iterator>;
        { a.cbegin() } -> std::same_as<typename ContainerType::const_iterator>;
        { a.cend() } -> std::same_as<typename ContainerType::const_iterator>;
        { a.size() } -> std::same_as<typename ContainerType::size_type>;
        { a.max_size() } -> std::same_as<typename ContainerType::size_type>;
        { a.empty() } -> std::same_as<bool>;
    };

    template<typename Elem>
    struct mpi_packed_message {
        std::vector<Elem> packed_message;
        std::vector<int> offsets;
        std::vector<int> lengths;
    };

    // Packs message buffer so that MPI_AlltoAllv can be used
    template<Container Input>
        requires Container<typename Input::value_type>
    auto pack_messages(Input const &messages)
        -> mpi_packed_message<typename Input::value_type::value_type> {
        using inner = typename Input::value_type;
        using element_type = typename inner::value_type;

        // Flattening the vector of vectors using join_view
        auto const flattened_view = messages | std::ranges::views::join;
        std::vector<element_type> flattened_vector;
        // converting view into vector
        for (auto &&elem: flattened_view) {
            flattened_vector.push_back(static_cast<decltype(elem) &&>(elem));
        }

        // Calculating lengths using transform view
        std::vector<int> lengths;
        lengths.reserve(messages.size());
        std::ranges::transform(messages, std::back_inserter(lengths),
                               [](auto const &elem) { return static_cast<int>(elem.size()); });

        // Calculating offsets using exclusive_scan
        std::vector<int> offsets(messages.size());
        std::exclusive_scan(lengths.begin(), lengths.end(), offsets.begin(), 0);

        return mpi_packed_message{flattened_vector, offsets, lengths};
    }

    template<typename Elem>
    auto unpack_messages(mpi_packed_message<Elem> const &packed_message)
        -> std::vector<std::vector<Elem> > {
        auto const &[recv_buf, recv_displs, recv_counts] = packed_message;
        auto const recv_span = std::span(recv_buf);
        int num_ranks = static_cast<int>(recv_counts.size());

        std::vector<std::vector<Elem> > result;
        result.reserve(num_ranks);

        for (int i = 0; i < num_ranks; ++i) {
            auto const subspan = recv_span.subspan(recv_displs.at(i), recv_counts.at(i));
            result.emplace_back(subspan.begin(), subspan.end());
        }

        return result;
    }

    template<Container Input>
        requires Container<typename Input::value_type>
    auto all_to_all(Input const &sends, MPI_Comm communicator) {
        using inner = typename Input::value_type;
        using element_type = typename inner::value_type;

        PEID rank, size;
        MPI_Comm_rank(communicator, &rank);
        MPI_Comm_size(communicator, &size);

        // Packing messages into vector and computing offsets and lengths for the sub messages
        auto [send_packed_messages, send_offsets, send_lengths] = mpi_collective_tools::pack_messages(sends);

        if (send_offsets.size() != send_lengths.size()) {
            throw(std::runtime_error("MPI_collective_tools::pack_messages()"));
        } else if ((send_offsets.size() != size) || (send_lengths.size() != size)) {
            throw(std::runtime_error("MPI_collective_tools::pack_messages()"));
        }

        // Preparing receive buffers for the node ids, offsets, and lengths
        std::vector<int> num_recv_from_rank(send_lengths.size(),0); // number of messeages from each rank
        MPI_Alltoall(send_lengths.data(), 1, MPI_INT, num_recv_from_rank.data(), 1, MPI_INT, communicator);
        auto const recv_buff_size = std::reduce(num_recv_from_rank.begin(), num_recv_from_rank.end(), 0);

        auto recv_packed_messages = std::vector<element_type>(recv_buff_size, 0);
        auto recv_offsets = std::vector<int>(size, 0);
        auto const recv_lengths = num_recv_from_rank;

        // Calculating recv offsets
        std::exclusive_scan(recv_lengths.begin(), recv_lengths.end(), recv_offsets.begin(), 0);

        auto const mpi_error = MPI_Alltoallv(send_packed_messages.data(), send_lengths.data(), send_offsets.data(),
                                       MPI_UNSIGNED_LONG_LONG, recv_packed_messages.data(), recv_lengths.data(),
                                       recv_offsets.data(), MPI_UNSIGNED_LONG_LONG,
                                       communicator);
        if (mpi_error != MPI_SUCCESS) {
            throw(std::runtime_error("MPI_collective_tools::all_to_all()"));
        }
        return mpi_collective_tools::unpack_messages<element_type>({recv_packed_messages, recv_offsets, recv_lengths});
    }
}


#endif /* end of include guard: MPI_TOOLS_HMESDXF2 */
