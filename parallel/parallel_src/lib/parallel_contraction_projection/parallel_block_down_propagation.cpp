/******************************************************************************
 * parallel_block_down_propagation.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_block_down_propagation.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "communication/mpi_adapter.h"
#include "communication/mpi_trace.h"
namespace parhip {
parallel_block_down_propagation::parallel_block_down_propagation() {
                
}

parallel_block_down_propagation::~parallel_block_down_propagation() {
                
}

void parallel_block_down_propagation::propagate_block_down( MPI_Comm communicator, PPartitionConfig & config, 
                                                            parallel_graph_access & G, 
                                                            parallel_graph_access & Q) {


  std::unordered_map< NodeID, NodeID > coarse_block_ids;

  forall_local_nodes(G, node) {
    NodeID cur_cnode = G.getCNode( node );
    coarse_block_ids[cur_cnode] = G.getSecondPartitionIndex( node );
  } endfor

  PEID rank, size;
  MPI_Comm_rank( communicator, &rank);
  MPI_Comm_size( communicator, &size);

  NodeID divisor          = ceil( Q.number_of_global_nodes()/(double)size);

  auto updates_by_destination =
      std::vector<std::vector<block_down::block_update>>(
          static_cast<std::size_t>(size));
  for (auto const& [coarse_global_id, block] : coarse_block_ids) {
    auto const destination =
        static_cast<std::size_t>(coarse_global_id / divisor);
    updates_by_destination.at(destination).push_back(
        {coarse_global_id, block});
  }
  for (auto& updates : updates_by_destination) {
    std::ranges::stable_sort(updates, {}, [](auto const& update) {
      return std::tie(update.coarse_global_id, update.block);
    });
  }

  auto incoming_updates = mpi::all_to_all_v(
      mpi::segmented_buffer<block_down::block_update>::from_segments(
          updates_by_destination),
      mpi::communicator_view{communicator});
  std::map<NodeID, NodeID> blocks_by_coarse_node;
  for (std::size_t source = 0;
       source < incoming_updates.segment_count();
       ++source) {
    for (auto const& update : incoming_updates.segment(source)) {
      if (static_cast<PEID>(update.coarse_global_id / divisor) != rank ||
          !Q.is_local_node_from_global_id(update.coarse_global_id)) {
        throw std::logic_error{"block update reached the wrong owner"};
      }
      auto const [position, inserted] = blocks_by_coarse_node.try_emplace(
          update.coarse_global_id, update.block);
      if (!inserted && position->second != update.block) {
        throw std::logic_error{"conflicting block updates for coarse node"};
      }
    }
  }
  for (auto const& [coarse_global_id, block] : blocks_by_coarse_node) {
    Q.setSecondPartitionIndex(Q.getLocalID(coarse_global_id), block);
  }

  forall_local_nodes(Q, node) {
    KAHIP_MPI_TRACE(mpi::trace::block_propagation(
        mpi::trace::current_hierarchy(), Q.getGlobalID(node), rank, rank,
        Q.getSecondPartitionIndex(node)));
  } endfor

  update_ghost_nodes_blocks( communicator, Q );
}

void parallel_block_down_propagation::update_ghost_nodes_blocks( MPI_Comm communicator, parallel_graph_access & G ) {
  PEID rank, size;
  MPI_Comm_rank( communicator, &rank);
  MPI_Comm_size( communicator, &size);

  m_send_buffers.resize(size);
  std::vector< bool > PE_packed(size, false);
  forall_local_nodes(G, node) {
    forall_out_edges(G, e, node) {
      NodeID target = G.getEdgeTarget(e);
      if( !G.is_local_node(target)  ) {
        PEID peID = G.getTargetPE(target);
        if( !PE_packed[peID] ) { // make sure a node is sent at most once
          m_send_buffers[peID].push_back(G.getGlobalID(node));
          m_send_buffers[peID].push_back(G.getSecondPartitionIndex(node));
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
                m_send_buffers[peID].size(), MPI_UNSIGNED_LONG_LONG, peID, peID+11*size, communicator, &rq);
  }
}

  //receive incomming
  PEID counter = 0;
  while( counter < G.getNumberOfAdjacentPEs()) {
    // wait for incomming message of an adjacent processor
    MPI_Status st;
    MPI_Probe(MPI_ANY_SOURCE, rank+11*size, communicator, &st);

    int message_length;
    MPI_Get_count(&st, MPI_UNSIGNED_LONG_LONG, &message_length);
    std::vector<NodeID> message; message.resize(message_length);

    MPI_Status rst;
    MPI_Recv( &message[0], message_length, MPI_UNSIGNED_LONG_LONG, st.MPI_SOURCE, rank+11*size, communicator, &rst);
    counter++;

    // now integrate the changes
    if(message_length == 1) continue; // nothing to do

    for( int i = 0; i < message_length-1; i+=2) {
      NodeID global_id   = message[i];
      NodeWeight  block  = message[i+1];

      G.setSecondPartitionIndex( G.getLocalID(global_id), block );
      KAHIP_MPI_TRACE(mpi::trace::block_propagation(
          mpi::trace::current_hierarchy(), global_id, st.MPI_SOURCE, rank,
          block));
    }
  }

}
}
