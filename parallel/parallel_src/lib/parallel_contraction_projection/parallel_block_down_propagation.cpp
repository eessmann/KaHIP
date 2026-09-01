/******************************************************************************
 * parallel_block_down_propagation.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_block_down_propagation.h"

#include <algorithm>
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


  std::vector<block_down::block_update> local_updates;
  local_updates.reserve(static_cast<std::size_t>(G.number_of_local_nodes()));
  forall_local_nodes(G, node) {
    local_updates.push_back(
        {G.getCNode(node), G.getSecondPartitionIndex(node)});
  } endfor

  PEID rank, size;
  MPI_Comm_rank( communicator, &rank);
  MPI_Comm_size( communicator, &size);

  std::ranges::stable_sort(local_updates, {}, [](auto const& update) {
    return std::tie(update.coarse_global_id, update.block);
  });
  auto const number_of_coarse_nodes = Q.number_of_global_nodes();
  auto local_updates_are_valid = true;
  for (auto const& update : local_updates) {
    if (update.coarse_global_id >= number_of_coarse_nodes) {
      local_updates_are_valid = false;
    }
  }
  for (std::size_t index = 1; index < local_updates.size(); ++index) {
    auto const& previous = local_updates[index - 1];
    auto const& current = local_updates[index];
    if (previous.coarse_global_id == current.coarse_global_id &&
        previous.block != current.block) {
      local_updates_are_valid = false;
    }
  }
  mpi::validate_collectively(
      local_updates_are_valid,
      mpi::communicator_view{communicator},
      "block update local validation failed");

  NodeID divisor = number_of_coarse_nodes == 0
                       ? NodeID{1}
                       : ceil(number_of_coarse_nodes / (double)size);

  auto updates_by_destination =
      std::vector<std::vector<block_down::block_update>>(
          static_cast<std::size_t>(size));
  for (auto const& [coarse_global_id, block] : local_updates) {
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
  auto incoming_updates_are_valid = true;
  for (std::size_t source = 0;
       source < incoming_updates.segment_count();
       ++source) {
    for (auto const& update : incoming_updates.segment(source)) {
      if (update.coarse_global_id >= number_of_coarse_nodes) {
        incoming_updates_are_valid = false;
        continue;
      }
      if (static_cast<PEID>(update.coarse_global_id / divisor) != rank ||
          !Q.is_local_node_from_global_id(update.coarse_global_id)) {
        incoming_updates_are_valid = false;
      }
    }
  }
  auto incoming_storage = incoming_updates.storage();
  std::ranges::sort(incoming_storage, {}, [](auto const& update) {
    return std::tie(update.coarse_global_id, update.block);
  });
  for (std::size_t index = 1; index < incoming_storage.size(); ++index) {
    auto const& previous = incoming_storage[index - 1];
    auto const& current = incoming_storage[index];
    if (previous.coarse_global_id == current.coarse_global_id &&
        previous.block != current.block) {
      incoming_updates_are_valid = false;
    }
  }
  auto update_index = std::size_t{0};
  forall_local_nodes(Q, node) {
    auto const coarse_global_id = Q.getGlobalID(node);
    while (update_index < incoming_storage.size() &&
           incoming_storage[update_index].coarse_global_id <
               coarse_global_id) {
      ++update_index;
    }
    if (update_index == incoming_storage.size() ||
        incoming_storage[update_index].coarse_global_id !=
            coarse_global_id) {
      incoming_updates_are_valid = false;
    }
  } endfor
  mpi::validate_collectively(
      incoming_updates_are_valid,
      mpi::communicator_view{communicator},
      "block update received validation failed");

  auto first_update_for_node = true;
  auto previous_coarse_global_id = NodeID{0};
  for (auto const& update : incoming_storage) {
    if (first_update_for_node ||
        update.coarse_global_id != previous_coarse_global_id) {
      Q.setSecondPartitionIndex(
          Q.getLocalID(update.coarse_global_id), update.block);
      previous_coarse_global_id = update.coarse_global_id;
      first_update_for_node = false;
    }
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
