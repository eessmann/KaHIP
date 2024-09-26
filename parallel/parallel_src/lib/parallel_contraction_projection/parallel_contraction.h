/******************************************************************************
 * parallel_contraction.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_CONTRACTION_64O127GD
#define PARALLEL_CONTRACTION_64O127GD

#include "data_structure/hashed_graph.h"
#include "data_structure/parallel_graph_access.h"
#include "partition_config.h"
#include "communication/mpi_tools.h"

class parallel_contraction {
 public:
	void contract_to_distributed_quotient(MPI_Comm communicator,
																				PPartitionConfig& config,
																				parallel_graph_access& G,
																				parallel_graph_access& Q);

 private:
	// compute mapping of labels id into contiguous intervall [0,...,num_lables)
	void compute_label_mapping(MPI_Comm communicator,
														 parallel_graph_access& G,
														 NodeID& global_num_distinct_ids,
														 std::unordered_map<NodeID, NodeID>& label_mapping);

	void get_nodes_to_cnodes_ghost_nodes(MPI_Comm communicator,
																			 parallel_graph_access& G);

	void build_quotient_graph_locally(
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

	// some send buffers
	std::vector<std::vector<NodeID>> m_messages;
	std::vector<std::vector<NodeID>> m_out_messages;
	std::vector<std::vector<NodeID>> m_send_buffers;  // buffers to send messages
};

// Comm types
namespace contraction {
struct bundled_edge {
	NodeID source;
	NodeID target;
	NodeWeight weight;
};

struct bundled_node_weight {
	NodeID node;
	NodeWeight weight;
};
}  // namespace contraction

// Specialize mpi_data_kind_trait for bundled_edge
namespace mpi::details {
template <>
struct mpi_data_kind_trait<contraction::bundled_edge> {
	static constexpr mpi_data_kinds kind = mpi_data_kinds::composite;
};

template <>
struct mpi_datatype_trait<contraction::bundled_edge> {
	static MPI_Datatype get_mpi_type() {
		static MPI_Datatype mpi_type = MPI_DATATYPE_NULL;
		if (mpi_type == MPI_DATATYPE_NULL) {
			int block_lengths[3] = {1, 1, 1};
			MPI_Datatype types[3] = {
					get_mpi_datatype<decltype(contraction::bundled_edge::source)>(),
					get_mpi_datatype<decltype(contraction::bundled_edge::target)>(),
					get_mpi_datatype<decltype(contraction::bundled_edge::weight)>()};
			MPI_Aint offsets[3];

			offsets[0] = offsetof(contraction::bundled_edge, source);
			offsets[1] = offsetof(contraction::bundled_edge, target);
			offsets[2] = offsetof(contraction::bundled_edge, weight);

			MPI_Type_create_struct(3, block_lengths, offsets, types, &mpi_type);
			MPI_Type_commit(&mpi_type);
		}
		return mpi_type;
	}
};
}  // namespace mpi::details

// Specialize mpi_data_kind_trait for node_weight
namespace mpi::details {
template <>
struct mpi_data_kind_trait<contraction::bundled_node_weight> {
	static constexpr mpi_data_kinds kind = mpi_data_kinds::composite;
};

template <>
struct mpi_datatype_trait<contraction::bundled_node_weight> {
	static MPI_Datatype get_mpi_type() {
		static MPI_Datatype mpi_type = MPI_DATATYPE_NULL;
		if (mpi_type == MPI_DATATYPE_NULL) {
			int block_lengths[2] = { 1, 1};
			MPI_Datatype types[2] = {
				get_mpi_datatype<decltype(contraction::bundled_node_weight::node)>(),
				get_mpi_datatype<decltype(contraction::bundled_node_weight::weight)>()};
			MPI_Aint offsets[3];

			offsets[0] = offsetof(contraction::bundled_node_weight, node);
			offsets[1] = offsetof(contraction::bundled_node_weight, weight);

			MPI_Type_create_struct(3, block_lengths, offsets, types, &mpi_type);
			MPI_Type_commit(&mpi_type);
		}
		return mpi_type;
	}
};
}  // namespace mpi::details
#endif /* end of include guard: PARALLEL_CONTRACTION_64O127GD */
