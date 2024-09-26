/******************************************************************************
 * parallel_contraction.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_contraction.h"
#include "data_structure/hashed_graph.h"
#include "tools/helpers.h"

void parallel_contraction::contract_to_distributed_quotient( MPI_Comm communicator, PPartitionConfig & config, 
                                                             parallel_graph_access & G, 
                                                             parallel_graph_access & Q) {

        NodeID number_of_distinct_labels; // equals global number of coarse nodes

        // maps old ids to new ids in interval [0, ...., num_of_distinct_labels
        // and stores this information only for the local nodes 
        std::unordered_map< NodeID, NodeID > label_mapping;

        compute_label_mapping( communicator, G, number_of_distinct_labels, label_mapping);
        
        // compute the projection table
        G.allocate_node_to_cnode();
        forall_local_nodes(G, node) {
                G.setCNode( node, label_mapping[ G.getNodeLabel( node )]);
        } endfor

        get_nodes_to_cnodes_ghost_nodes( communicator, G );   

        //now we can really build the edges of the quotient graph
        hashed_graph hG;
        std::unordered_map< NodeID, NodeWeight > node_weights;

        build_quotient_graph_locally( G, number_of_distinct_labels, hG, node_weights);
        
        MPI_Barrier(communicator);

        m_messages.clear();
        m_out_messages.clear();
        m_send_buffers.clear();

        redistribute_hased_graph_and_build_graph_locally( communicator, hG, node_weights, number_of_distinct_labels, Q );
        update_ghost_nodes_weights( communicator, Q );
}

// MPI AlltoAll based implementation
void parallel_contraction::compute_label_mapping(
		MPI_Comm communicator,
		parallel_graph_access& G,
		NodeID& global_num_distinct_ids,
		std::unordered_map<NodeID, NodeID>& label_mapping) {
	PEID rank, size;
	MPI_Comm_rank(communicator, &rank);
	MPI_Comm_size(communicator, &size);

	NodeID divisor = ceil(G.number_of_global_nodes() / static_cast<double>(size));

	helpers helper;
	m_messages.clear();
	m_messages.resize(size);

	std::vector<std::unordered_map<NodeID, bool> > filter;
	filter.resize(size);

	forall_local_nodes(G, node) {
		PEID peID = G.getNodeLabel(node) / divisor;
		filter[peID][G.getNodeLabel(node)] = true;
	} endfor

    for (PEID peID = 0; peID < size; peID++) {
		std::unordered_map<NodeID, bool>::iterator it;
		for (it = filter.at(peID).begin(); it != filter.at(peID).end(); ++it) {
			m_messages.at(peID).push_back(it->first);
		}
	}

	auto const local_labels_byPE = mpi::all_to_all(m_messages, communicator);

	std::vector<NodeID> local_labels;
	for (PEID peID = 0; peID < size; peID++) {
		for (ULONG i = 0; i < local_labels_byPE.at(peID).size(); i++) {
			local_labels.push_back(local_labels_byPE.at(peID).at(i));
		}
	}

	// filter duplicates locally
	helper.filter_duplicates(
			local_labels,
			[](const NodeID& lhs, const NodeID& rhs) -> bool { return (lhs < rhs); },
			[](const NodeID& lhs, const NodeID& rhs) -> bool {
				return (lhs == rhs);
			});
	// afterward they are sorted!

	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	// %%%%%%%%%%%%%%%%%%%%%%%Labels are unique on all PEs%%%%%%%%%%%%%%%%%%%%
	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	// now counting

	NodeID local_num_labels = local_labels.size();
	NodeID prefix_sum = 0;

	MPI_Scan(&local_num_labels, &prefix_sum, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM,
					 communicator);

	global_num_distinct_ids = prefix_sum;
	// Broadcast global number of ids
	MPI_Bcast(&global_num_distinct_ids, 1, MPI_UNSIGNED_LONG_LONG, size - 1,
						communicator);

	NodeID num_smaller_ids = prefix_sum - local_num_labels;

	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	// %%%%%Now Build the mapping and send information back to PEs%%%%%%%%%%%%
	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
	// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

	// build the mapping locally
	std::unordered_map<NodeID, NodeID> label_mapping_to_cnode;
	NodeID cur_id = num_smaller_ids;
	for (ULONG i = 0; i < local_labels.size(); i++) {
		label_mapping_to_cnode[local_labels[i]] = cur_id++;
	}

	// now send the processes the mapping back
	// std::vector< std::vector< NodeID > >  m_out_messages;
	m_out_messages.clear();
	m_out_messages.resize(size);

	for (PEID peID = 0; peID < size; peID++) {
		if (peID == rank)
			continue;

		if (local_labels_byPE.at(peID).empty()) {
			m_out_messages.at(peID) = {};
			continue;
		}

		for (ULONG i = 0; i < local_labels_byPE[peID].size(); i++) {
			m_out_messages.at(peID).push_back(
					label_mapping_to_cnode.at(local_labels_byPE.at(peID).at(i)));
		}
	}

	auto recv_mapping = mpi::all_to_all(m_out_messages, communicator);

	// first the local labels
	for (ULONG i = 0; i < m_messages.at(rank).size(); i++) {
		label_mapping[m_messages.at(rank).at(i)] =
				label_mapping_to_cnode.at(m_messages.at(rank).at(i));
	}

	for (PEID peID = 0; peID < size; peID++) {
		for (ULONG i = 0; i < recv_mapping.at(peID).size(); i++) {
			label_mapping[m_messages.at(peID).at(i)] = recv_mapping.at(peID).at(i);
		}
	}
}

void parallel_contraction::get_nodes_to_cnodes_ghost_nodes( MPI_Comm communicator, parallel_graph_access & G ) {
        PEID rank, size;
        MPI_Comm_rank( communicator, &rank);
        MPI_Comm_size( communicator, &size);
        
        std::vector< bool > PE_packed( size, false );
		m_send_buffers.clear();
        m_send_buffers.resize( size );

        forall_local_nodes(G, node) {
                if(G.is_interface_node(node)) {
                        forall_out_edges(G, e, node) {
                                NodeID target = G.getEdgeTarget(e);
                                if( !G.is_local_node(target)  ) {
                                        PEID peID = G.getTargetPE(target);
                                        if( !PE_packed[peID] ) { // make sure a node is sent at most once
                                                m_send_buffers[peID].push_back(G.getGlobalID(node));
                                                m_send_buffers[peID].push_back(G.getCNode(node));
                                                PE_packed[peID] = true;
                                        }
                                }
                        } endfor
                        forall_out_edges(G, e, node) {
                                NodeID target = G.getEdgeTarget(e);
                                if( !G.is_local_node(target)  ) {
                                        PE_packed[G.getTargetPE(target)] = false;
                                }
                        } endfor
                }
        } endfor

        //send all neighbors their packages using Isends
        //a neighbor that does not receive something gets a specific token
        for( PEID peID = 0; peID < (PEID)m_send_buffers.size(); peID++) {
                if( G.is_adjacent_PE(peID) ) {
                        //now we have to send a message
                        if( m_send_buffers[peID].size() == 0 ){
                                // length 1 encode no message
                                m_send_buffers[peID].push_back(0);
                        }

                        MPI_Request rq;
                        MPI_Isend( &m_send_buffers[peID][0], 
                                    m_send_buffers[peID].size(), MPI_UNSIGNED_LONG_LONG, peID, peID+6*size, communicator, &rq);
                }
        }

        ////receive incomming
        PEID num_adjacent = G.getNumberOfAdjacentPEs();
        PEID counter = 0;
        while( counter < num_adjacent ) {
                // wait for incomming message of an adjacent processor
                MPI_Status st;
                MPI_Probe(MPI_ANY_SOURCE, rank+6*size, communicator, &st);
                
                int message_length;
                MPI_Get_count(&st, MPI_UNSIGNED_LONG_LONG, &message_length);
                std::vector<NodeID> message; message.resize(message_length);

                MPI_Status rst;
                MPI_Recv( &message[0], message_length, MPI_UNSIGNED_LONG_LONG, st.MPI_SOURCE, rank+6*size, communicator, &rst); 
                counter++;

                // now integrate the changes
                if(message_length == 1) continue; // nothing to do

                for( int i = 0; i < message_length-1; i+=2) {
                        NodeID global_id = message[i];
                        NodeID cnode     = message[i+1];

                        G.setCNode( G.getLocalID(global_id), cnode);
                }
        }
}


void parallel_contraction::build_quotient_graph_locally( parallel_graph_access & G, 
                                                         NodeID number_of_distinct_labels, 
                                                         hashed_graph & hG, 
                                                         std::unordered_map< NodeID, NodeWeight > & node_weights) {
        forall_local_nodes(G, node) {
                NodeID cur_cnode = G.getCNode( node );
                if( node_weights.find(cur_cnode) == node_weights.end()) {
                        node_weights[cur_cnode] = 0;
                }

                node_weights[cur_cnode] += G.getNodeWeight( node );

                forall_out_edges(G, e, node) {
                        NodeID target       = G.getEdgeTarget(e);
                        NodeID target_cnode = G.getCNode(target);
                        if( cur_cnode != target_cnode ) {
                                // update the edge
                                hashed_edge he;
                                he.k            = number_of_distinct_labels;
                                he.source       = cur_cnode;
                                he.target       = target_cnode;

                                hG[he].weight  += G.getEdgeWeight(e);
                        }
                } endfor
        } endfor
}

void parallel_contraction::redistribute_hased_graph_and_build_graph_locally(
		MPI_Comm communicator,
		hashed_graph& hG,
		std::unordered_map<NodeID, NodeWeight>& node_weights,
		NodeID number_of_cnodes,
		parallel_graph_access& Q) {
	PEID rank, size;
	MPI_Comm_rank(communicator, &rank);
	MPI_Comm_size(communicator, &size);

	NodeID divisor = ceil(number_of_cnodes / (double)size);

	std::vector<std::vector<contraction::bundled_edge>> messages(size);
	m_messages.clear();
	m_messages.resize(size);

	// build messages
	hashed_graph::iterator it;
	for (it = hG.begin(); it != hG.end(); it++) {
		data_hashed_edge& e = it->second;
		hashed_edge he = it->first;
		PEID peID = he.source / divisor;
		messages[peID].emplace_back(he.source, he.target, e.weight);
		peID = he.target / divisor;
		messages[peID].emplace_back(he.target, he.source, e.weight);
	}

	// build the local part of the graph
	//
	auto const local_msg_byPE = mpi::all_to_all(messages, communicator);

	hashed_graph local_graph;
	for (PEID peID = 0; peID < size; peID++) {
		if (!local_msg_byPE[peID].empty()) {
			for (auto packed_edge : local_msg_byPE[peID]) {
				hashed_edge he;
				he.k = number_of_cnodes;
				he.source = packed_edge.source;
				he.target = packed_edge.target;

				local_graph[he].weight += packed_edge.weight;
			}
		}
	}

	ULONG from = rank * ceil(number_of_cnodes / (double)size);
	ULONG to = (rank + 1) * ceil(number_of_cnodes / (double)size) - 1;
	// handle the case where we dont have local edges
	from = std::min(from, number_of_cnodes);
	to = std::min(to, number_of_cnodes - 1);
	ULONG local_num_cnodes = to - from + 1;

	std::vector<std::vector<std::pair<NodeID, NodeWeight>>> sorted_graph;
	sorted_graph.resize(local_num_cnodes);

	EdgeID edge_counter = 0;
	for (it = local_graph.begin(); it != local_graph.end(); it++) {
		data_hashed_edge& e = it->second;
		hashed_edge he = it->first;

		if (from <= he.target && he.target <= to) {
			std::pair<NodeID, NodeWeight> edge;
			edge.first = he.target;
			edge.second = e.weight / 4;

			std::pair<NodeID, NodeWeight> e_bar;
			e_bar.first = he.source;
			e_bar.second = e.weight / 4;

			sorted_graph[he.target - from].push_back(e_bar);
			sorted_graph[he.source - from].push_back(edge);
			edge_counter += 2;
		} else {
			std::pair<NodeID, NodeWeight> edge;
			edge.first = he.target;
			edge.second = e.weight / 2;
			sorted_graph[he.source - from].push_back(edge);
			edge_counter++;
		}
	}

	ULONG global_edges = 0;
	MPI_Allreduce(&edge_counter, &global_edges, 1, MPI_UNSIGNED_LONG_LONG,
								MPI_SUM, communicator);

	Q.start_construction(local_num_cnodes, edge_counter, number_of_cnodes,
											 global_edges);
	Q.set_range(from, to);

	std::vector<NodeID> vertex_dist(size + 1, 0);
	for (PEID peID = 0; peID <= size; peID++) {
		vertex_dist[peID] = std::min(
				number_of_cnodes,
				(NodeID)(peID *
								 ceil(number_of_cnodes / (double)size)));  // from positions
	}
	// vertex_dist[size] = std::min(to, number_of_cnodes - 1);
	Q.set_range_array(vertex_dist);

	for (NodeID i = 0; i < local_num_cnodes; ++i) {
		NodeID node = Q.new_node();
		NodeID globalID = from + node;
		Q.setNodeWeight(node, 0);
		Q.setNodeLabel(node, globalID);

		for (EdgeID e = 0; e < sorted_graph[node].size(); e++) {
			NodeID target = sorted_graph[node][e].first;
			EdgeID e_bar = Q.new_edge(node, target);
			Q.setEdgeWeight(e_bar, sorted_graph[node][e].second);
		}
	}

	Q.finish_construction();

	for (PEID peID = 0; peID < size; peID++) {
		m_messages[peID].clear();
	}
	// now distribute the node weights
	// pack messages
	std::vector<std::vector<contraction::bundled_node_weight>> weight_messages(
			size);
	std::unordered_map<NodeID, NodeWeight>::iterator wit;
	for (wit = node_weights.begin(); wit != node_weights.end(); wit++) {
		NodeID node = wit->first;
		NodeWeight weight = wit->second;
		PEID peID = node / divisor;
		weight_messages[peID].emplace_back(node, weight);
	}

	auto const node_weights_byPE = mpi::all_to_all(weight_messages, communicator);

	for (auto& message_byPE : node_weights_byPE) {
		if (message_byPE.empty()) {
			for (auto& [globalID, weight] : message_byPE) {
				NodeID node = globalID - from;
				Q.setNodeWeight(node, Q.getNodeWeight(node) + weight);
			}
		}
	}
}


void parallel_contraction::update_ghost_nodes_weights( MPI_Comm communicator, parallel_graph_access & G ) {
        PEID rank, size;
        MPI_Comm_rank( communicator, &rank);
        MPI_Comm_size( communicator, &size);
        
               //std::vector< std::vector< NodeID > > send_buffers; // buffers to send messages
        m_send_buffers.resize(size);
        std::vector< bool > PE_packed(size, false);
        forall_local_nodes(G, node) {
                forall_out_edges(G, e, node) {
                        NodeID target = G.getEdgeTarget(e);
                        if( !G.is_local_node(target)  ) {
                                PEID peID = G.getTargetPE(target);
                                if( !PE_packed[peID] ) { // make sure a node is sent at most once
                                        m_send_buffers[peID].push_back(G.getGlobalID(node));
                                        m_send_buffers[peID].push_back(G.getNodeWeight(node));
                                        PE_packed[peID] = true;
                                }
                        }
                } endfor
                forall_out_edges(G, e, node) {
                        NodeID target = G.getEdgeTarget(e);
                        if( !G.is_local_node(target)  ) {
                                PE_packed[G.getTargetPE(target)] = false;
                        }
                } endfor
        } endfor

        //send all neighbors their packages using Isends
        //a neighbor that does not receive something gets a specific token
        for( PEID peID = 0; peID < (PEID)m_send_buffers.size(); peID++) {
                if( G.is_adjacent_PE(peID) ) {
                        //now we have to send a message
                        if( m_send_buffers[peID].size() == 0 ){
                                // length 1 encode no message
                                m_send_buffers[peID].push_back(0);
                        }

                        MPI_Request rq; 
                        MPI_Isend( &m_send_buffers[peID][0], 
                                    m_send_buffers[peID].size(), MPI_UNSIGNED_LONG_LONG, peID, peID+9*size, communicator, &rq);
                }
        }

        //receive incomming
        PEID counter = 0;
        while( counter < G.getNumberOfAdjacentPEs()) {
                // wait for incomming message of an adjacent processor
                MPI_Status st;
                MPI_Probe(MPI_ANY_SOURCE, rank+9*size, communicator, &st);
                
                int message_length;
                MPI_Get_count(&st, MPI_UNSIGNED_LONG_LONG, &message_length);
                std::vector<NodeID> message; message.resize(message_length);

                MPI_Status rst;
                MPI_Recv( &message[0], message_length, MPI_UNSIGNED_LONG_LONG, st.MPI_SOURCE, rank+9*size, communicator, &rst); 
                counter++;

                // now integrate the changes
                if(message_length == 1) continue; // nothing to do

                for( int i = 0; i < message_length-1; i+=2) {
                        NodeID global_id   = message[i];
                        NodeWeight  weight = message[i+1];

                        G.setNodeWeight( G.getLocalID(global_id), weight);
                }
        }

}
