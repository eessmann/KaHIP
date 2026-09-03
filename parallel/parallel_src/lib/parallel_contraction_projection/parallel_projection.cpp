/******************************************************************************
 * parallel_projection.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_projection.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "communication/mpi_adapter.h"
#include "communication/contiguous_owner_layout.h"
#include "communication/mpi_trace.h"
namespace parhip {
parallel_projection::parallel_projection() {
                
}

parallel_projection::~parallel_projection() {
                
}

void parallel_projection::parallel_project( MPI_Comm communicator, parallel_graph_access & finer, parallel_graph_access & coarser ) {
  struct pending_label_update {
    NodeID node;
    NodeID label;
  };

  auto const communicator_view = mpi::communicator_view{communicator};
  auto const rank = communicator_view.rank();
  auto const size = communicator_view.size();
  auto const rank_index = static_cast<std::size_t>(rank);
  auto const number_of_coarse_nodes = mpi::agree_collectively(
      coarser.number_of_global_nodes(),
      communicator_view,
      "projection coarse node count agreement failed");
  auto const ownership = mpi::contiguous_owner_layout<NodeID>{
      number_of_coarse_nodes, static_cast<std::size_t>(size)};
  auto const coarse_begin = ownership.begin(rank_index);
  auto const coarse_end = ownership.end(rank_index);
  auto const expected_local_coarse_nodes = coarse_end - coarse_begin;
  auto const expected_last_coarse_node =
      expected_local_coarse_nodes == 0 ? coarse_begin : coarse_end - 1;

  auto local_coarse_nodes_are_valid =
      coarser.number_of_local_nodes() == expected_local_coarse_nodes &&
      coarser.get_from_range() == coarse_begin &&
      coarser.get_to_range() == expected_last_coarse_node;
  forall_local_nodes(finer, node) {
    auto const coarse_global_id = finer.getCNode(node);
    auto const owner = ownership.owner(coarse_global_id);
    if (!owner.has_value()) {
      local_coarse_nodes_are_valid = false;
      continue;
    }
    if (*owner == rank_index &&
        !coarser.is_local_node_from_global_id(coarse_global_id)) {
      local_coarse_nodes_are_valid = false;
    }
  } endfor
  mpi::validate_collectively(
      local_coarse_nodes_are_valid,
      communicator_view,
      "projection local coarse-node validation failed");

  std::vector<pending_label_update> pending_updates;
  pending_updates.reserve(
      static_cast<std::size_t>(finer.number_of_local_nodes()));
  auto requests_by_destination =
      std::vector<std::vector<projection::request>>(
          static_cast<std::size_t>(size));
  std::map<NodeID, projection::request> request_by_coarse_node;
  std::unordered_map<NodeID, std::vector<NodeID>> nodes_by_request;
  std::unordered_map<NodeID, NodeID> coarse_node_by_request;

  forall_local_nodes(finer, node) {
    auto const cnode = finer.getCNode(node);
    auto const owner = ownership.owner(cnode).value();
    if (owner == rank_index) {
      auto const new_label = coarser.getNodeLabel(coarser.getLocalID(cnode));
      pending_updates.push_back({node, new_label});
    } else {
      auto [position, inserted] = request_by_coarse_node.try_emplace(
          cnode,
          projection::request{finer.getGlobalID(node), cnode});
      auto const request_id = position->second.request_id;
      if (inserted) {
        requests_by_destination.at(owner).push_back(position->second);
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
  }

  auto incoming_requests = mpi::all_to_all_v(
      mpi::segmented_buffer<projection::request>::from_segments(
          requests_by_destination),
      mpi::communicator_view{communicator});
  auto replies_by_destination = std::vector<std::vector<projection::reply>>(
      static_cast<std::size_t>(size));
  auto incoming_requests_are_valid = true;
  for (std::size_t source = 0; source < incoming_requests.segment_count();
       ++source) {
    auto seen_request_ids = std::unordered_set<NodeID>{};
    seen_request_ids.reserve(incoming_requests.segment(source).size());
    auto seen_coarse_ids = std::unordered_set<NodeID>{};
    seen_coarse_ids.reserve(incoming_requests.segment(source).size());
    for (auto const& request : incoming_requests.segment(source)) {
      auto const owner = ownership.owner(request.coarse_global_id);
      if (!owner.has_value()) {
        incoming_requests_are_valid = false;
        continue;
      }
      if (*owner != rank_index ||
          !coarser.is_local_node_from_global_id(request.coarse_global_id) ||
          !seen_request_ids.insert(request.request_id).second ||
          !seen_coarse_ids.insert(request.coarse_global_id).second) {
        incoming_requests_are_valid = false;
      }
    }
  }
  mpi::validate_collectively(
      incoming_requests_are_valid,
      communicator_view,
      "projection request received validation failed");

  for (std::size_t source = 0; source < incoming_requests.segment_count();
       ++source) {
    auto& replies = replies_by_destination[source];
    for (auto const& request : incoming_requests.segment(source)) {
      replies.push_back(projection::reply{
          request.request_id,
          request.coarse_global_id,
          coarser.getNodeLabel(
              coarser.getLocalID(request.coarse_global_id))});
    }
    std::ranges::stable_sort(replies, {}, [](auto const& reply) {
      return std::tie(reply.request_id, reply.coarse_global_id);
    });
  }

  auto incoming_replies = mpi::all_to_all_v(
      mpi::segmented_buffer<projection::reply>::from_segments(
          replies_by_destination),
      mpi::communicator_view{communicator});

  auto incoming_replies_are_valid = true;
  auto received_request_ids = std::unordered_set<NodeID>{};
  received_request_ids.reserve(coarse_node_by_request.size());
  for (std::size_t source = 0; source < incoming_replies.segment_count();
       ++source) {
    for (auto const& reply : incoming_replies.segment(source)) {
      auto const owner = ownership.owner(reply.coarse_global_id);
      auto const coarse_node = coarse_node_by_request.find(reply.request_id);
      auto const projected_nodes = nodes_by_request.find(reply.request_id);
      if (!owner.has_value() || *owner != source ||
          coarse_node == coarse_node_by_request.end() ||
          projected_nodes == nodes_by_request.end() ||
          coarse_node->second != reply.coarse_global_id ||
          !received_request_ids.insert(reply.request_id).second) {
        incoming_replies_are_valid = false;
      }
    }
  }
  incoming_replies_are_valid =
      incoming_replies_are_valid &&
      received_request_ids.size() == coarse_node_by_request.size() &&
      std::ranges::all_of(coarse_node_by_request, [&](auto const& entry) {
        return received_request_ids.contains(entry.first);
      });
  mpi::validate_collectively(
      incoming_replies_are_valid,
      communicator_view,
      "projection reply received validation failed");

  for (std::size_t destination = 0;
       destination < requests_by_destination.size();
       ++destination) {
    for (auto const& request : requests_by_destination[destination]) {
      KAHIP_MPI_TRACE(mpi::trace::projection_request(
          mpi::trace::current_hierarchy(), request.request_id,
          rank,
          static_cast<int>(destination),
          request.coarse_global_id));
    }
  }

  for (std::size_t source = 0; source < replies_by_destination.size();
       ++source) {
    for (auto const& reply : replies_by_destination[source]) {
      KAHIP_MPI_TRACE(mpi::trace::projection_reply(
          mpi::trace::current_hierarchy(), reply.request_id,
          static_cast<int>(source),
          rank,
          reply.coarse_global_id,
          reply.label));
    }
  }

  for (auto const& reply : incoming_replies.storage()) {
    auto const& projected_nodes = nodes_by_request.at(reply.request_id);
    for (auto const node : projected_nodes) {
      pending_updates.push_back({node, reply.label});
    }
  }
  for (auto const& [node, label] : pending_updates) {
    finer.setNodeLabel(node, label);
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
