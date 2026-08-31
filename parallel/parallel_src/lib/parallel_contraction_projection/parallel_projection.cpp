/******************************************************************************
 * parallel_projection.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_projection.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "communication/mpi_adapter.h"
#include "communication/mpi_trace.h"
namespace parhip {
parallel_projection::parallel_projection() {
                
}

parallel_projection::~parallel_projection() {
                
}

void parallel_projection::parallel_project( MPI_Comm communicator, parallel_graph_access & finer, parallel_graph_access & coarser ) {
  PEID rank, size;
  MPI_Comm_rank(communicator, &rank);
  MPI_Comm_size(communicator, &size);

  auto const divisor = static_cast<NodeID>(
      ceil(coarser.number_of_global_nodes() / static_cast<double>(size)));
  auto requests_by_destination =
      std::vector<std::vector<projection::request>>(
          static_cast<std::size_t>(size));
  std::map<NodeID, projection::request> request_by_coarse_node;
  std::unordered_map<NodeID, std::vector<NodeID>> nodes_by_request;
  std::unordered_map<NodeID, NodeID> coarse_node_by_request;

  forall_local_nodes(finer, node) {
    auto const cnode = finer.getCNode(node);
    if( coarser.is_local_node_from_global_id(cnode) ) {
      auto const new_label = coarser.getNodeLabel(coarser.getLocalID(cnode));
      finer.setNodeLabel(node, new_label);
    } else {
      auto [position, inserted] = request_by_coarse_node.try_emplace(
          cnode,
          projection::request{finer.getGlobalID(node), cnode});
      auto const request_id = position->second.request_id;
      if (inserted) {
        auto const destination = static_cast<std::size_t>(cnode / divisor);
        requests_by_destination.at(destination).push_back(position->second);
        coarse_node_by_request.emplace(request_id, cnode);
      }
      nodes_by_request[request_id].push_back(node);
    }
  } endfor

  for (std::size_t destination = 0;
       destination < requests_by_destination.size();
       ++destination) {
    auto& destination_requests = requests_by_destination[destination];
    std::ranges::stable_sort(destination_requests, {}, [](auto const& request) {
      return std::tie(request.coarse_global_id, request.request_id);
    });
    for (auto const& request : destination_requests) {
      KAHIP_MPI_TRACE(mpi::trace::projection_request(
          mpi::trace::current_hierarchy(), request.request_id,
          rank,
          static_cast<int>(destination),
          request.coarse_global_id));
    }
  }

  auto incoming_requests = mpi::all_to_all_v(
      mpi::segmented_buffer<projection::request>::from_segments(
          requests_by_destination),
      mpi::communicator_view{communicator});
  auto replies_by_destination = std::vector<std::vector<projection::reply>>(
      static_cast<std::size_t>(size));
  for (std::size_t source = 0; source < incoming_requests.segment_count();
       ++source) {
    auto& replies = replies_by_destination[source];
    for (auto const& request : incoming_requests.segment(source)) {
      if (!coarser.is_local_node_from_global_id(request.coarse_global_id)) {
        throw std::logic_error{"projection request reached the wrong owner"};
      }
      replies.push_back(projection::reply{
          request.request_id,
          request.coarse_global_id,
          coarser.getNodeLabel(
              coarser.getLocalID(request.coarse_global_id))});
    }
    std::ranges::stable_sort(replies, {}, [](auto const& reply) {
      return std::tie(reply.request_id, reply.coarse_global_id);
    });
    for (auto const& reply : replies) {
      KAHIP_MPI_TRACE(mpi::trace::projection_reply(
          mpi::trace::current_hierarchy(), reply.request_id,
          static_cast<int>(source),
          rank,
          reply.coarse_global_id,
          reply.label));
    }
  }

  auto incoming_replies = mpi::all_to_all_v(
      mpi::segmented_buffer<projection::reply>::from_segments(
          replies_by_destination),
      mpi::communicator_view{communicator});
  for (auto const& reply : incoming_replies.storage()) {
    auto const coarse_node = coarse_node_by_request.find(reply.request_id);
    auto const projected_nodes = nodes_by_request.find(reply.request_id);
    if (coarse_node == coarse_node_by_request.end() ||
        projected_nodes == nodes_by_request.end() ||
        coarse_node->second != reply.coarse_global_id) {
      throw std::logic_error{"projection reply has an unknown request ID"};
    }
    for (auto const node : projected_nodes->second) {
      finer.setNodeLabel(node, reply.label);
    }
    nodes_by_request.erase(projected_nodes);
  }
  if (!nodes_by_request.empty()) {
    throw std::logic_error{"projection reply is missing"};
  }

  finer.update_ghost_node_data_global(); // blocking
}

//initial assignment after initial partitioning
void parallel_projection::initial_assignment( parallel_graph_access & G, complete_graph_access & Q) {
  forall_local_nodes(G, node) {
    G.setNodeLabel(node, Q.getNodeLabel(G.getGlobalID(node)));
    if( G.is_interface_node(node) ) {
      forall_out_edges(G, e, node) {
        NodeID target = G.getEdgeTarget(e);
        if( !G.is_local_node( target ) ) {
          G.setNodeLabel(target, Q.getNodeLabel(G.getGlobalID(target)));
        }
      } endfor
}
  } endfor
}
}
