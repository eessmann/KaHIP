/******************************************************************************
 * mpi_tools.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/


#ifndef MPI_TOOLS_HMESDXF2
#define MPI_TOOLS_HMESDXF2

#include "data_structure/parallel_graph_access.h"
#include "partition_config.h"

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

namespace collective_tools {
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
    auto pack_messages(
        Input const& messages) -> mpi_packed_message<typename Input::value_type::value_type> {
        using inner = typename Input::value_type;
        using element_type = typename inner::value_type;

        std::vector<element_type> flattened_vector{};
        std::vector<int> offsets{};
        std::vector<int> lengths{};

        int current_offset = 0;

        for (auto const &subvector: messages) {
            offsets.push_back(current_offset);
            lengths.push_back(static_cast<int>(subvector.size()));

            flattened_vector.insert(flattened_vector.end(), subvector.begin(), subvector.end());

            current_offset += static_cast<int>(subvector.size());
        }

        return mpi_packed_message{flattened_vector, offsets, lengths};
    }

    // Unpacks received message buffer received from alltoall
    template<typename Elem>
    auto unpack_messages(mpi_packed_message<Elem> const& packed_message) -> std::vector<std::vector<Elem>> {
        auto const& [recv_buf, recv_displs, recv_counts] = packed_message;
        int num_ranks = static_cast<int>(recv_counts.size());
        std::vector<std::vector<NodeID> > result(num_ranks);

        for (int i = 0; i < num_ranks; ++i) {
            int const count = recv_counts.at(i);
            int const offset = recv_displs.at(i);

            // Extract the subvector corresponding to rank `i`
            std::vector subvector(recv_buf.begin() + offset, recv_buf.begin() + offset + count);

            // Store it in the result vector at index `i`
            result[i] = std::move(subvector);
        }

        return result;
    }
}


#endif /* end of include guard: MPI_TOOLS_HMESDXF2 */
