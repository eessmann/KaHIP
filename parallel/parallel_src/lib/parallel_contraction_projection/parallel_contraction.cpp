/******************************************************************************
 * parallel_contraction.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_contraction.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "communication/contiguous_owner_layout.h"
#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_adapter.h"
#include "communication/mpi_trace.h"
#include "data_structure/hashed_graph.h"
#include "tools/helpers.h"
namespace parhip {
void parallel_contraction::contract_to_distributed_quotient( MPI_Comm communicator, PPartitionConfig & config,
                                                             parallel_graph_access & G, 
                                                             parallel_graph_access & Q) {
#if KAHIP_ENABLE_MPI_TRACE
  auto const trace_rank = mpi::communicator_view{communicator}.rank();
#endif

  NodeID number_of_distinct_labels; // equals global number of coarse nodes

  // maps old ids to new ids in interval [0, ...., num_of_distinct_labels
  // and stores this information only for the local nodes
  std::unordered_map< NodeID, NodeID > label_mapping;

  compute_label_mapping( communicator, G, number_of_distinct_labels, label_mapping);

  // Compute and commit the complete local/ghost projection table as one
  // transaction. Trace records are emitted only after the commit succeeds.
  get_nodes_to_cnodes_ghost_nodes(communicator, G, number_of_distinct_labels,
                                  label_mapping);

  //now we can really build the edges of the quotient graph
  hashed_graph hG;
  std::unordered_map< NodeID, NodeWeight > node_weights;

  build_quotient_graph_locally( G, number_of_distinct_labels, hG, node_weights);

  mpi::check_or_abort(
      MPI_Barrier(communicator), communicator, "MPI_Barrier(contraction)");

  m_send_buffers.clear();

  redistribute_hased_graph_and_build_graph_locally( communicator, hG, node_weights, number_of_distinct_labels, Q );
  update_ghost_nodes_weights( communicator, Q );
  forall_local_nodes(Q, node) {
    KAHIP_MPI_TRACE(mpi::trace::quotient_node_weight(
        mpi::trace::current_hierarchy(), Q.getGlobalID(node), trace_rank,
        Q.getNodeWeight(node)));
    forall_out_edges(Q, edge, node) {
      auto const target = Q.getEdgeTarget(edge);
      KAHIP_MPI_TRACE(mpi::trace::quotient_edge(
          mpi::trace::current_hierarchy(), Q.getGlobalID(node), trace_rank,
          Q.getGlobalID(target), Q.getEdgeWeight(edge)));
    } endfor
  } endfor
}

// MPI AlltoAll based implementation
void parallel_contraction::compute_label_mapping(
    MPI_Comm communicator,
    parallel_graph_access& G,
    NodeID& global_num_distinct_ids,
    std::unordered_map<NodeID, NodeID>& label_mapping) {
  auto const communicator_view = mpi::communicator_view{communicator};
  auto const rank = communicator_view.rank();
  auto const size = communicator_view.size();
  auto const rank_index = static_cast<std::size_t>(rank);

  auto const number_of_global_nodes = mpi::agree_collectively(
      G.number_of_global_nodes(),
      communicator_view,
      "label global node count agreement failed");
  auto const ownership = mpi::contiguous_owner_layout<NodeID>{
      number_of_global_nodes, static_cast<std::size_t>(size)};

  auto requests_by_destination =
      std::vector<std::vector<contraction::label_request>>(
          static_cast<std::size_t>(size));
  std::unordered_set<NodeID> requested_labels;

  auto local_requests_are_valid = true;
  forall_local_nodes(G, node) {
    local_requests_are_valid =
        local_requests_are_valid &&
        ownership.owner(G.getNodeLabel(node)).has_value();
  } endfor
  mpi::validate_collectively(
      local_requests_are_valid,
      mpi::communicator_view{communicator},
      "label request local validation failed");

  forall_local_nodes(G, node) {
    auto const old_label = G.getNodeLabel(node);
    auto const destination = ownership.owner(old_label).value();
    requests_by_destination.at(destination).push_back({old_label});
    requested_labels.insert(old_label);
  } endfor

  for (auto& requests : requests_by_destination) {
    std::ranges::stable_sort(requests, {}, [](auto const& request) {
      return request.old_label;
    });
    auto const unique_end = std::ranges::unique(
        requests, {}, [](auto const& request) { return request.old_label; });
    requests.erase(unique_end.begin(), unique_end.end());
  }

  auto incoming_requests = mpi::all_to_all_v(
      mpi::segmented_buffer<contraction::label_request>::from_segments(
          requests_by_destination),
      mpi::communicator_view{communicator});

  auto incoming_requests_are_valid = true;
  for (std::size_t source = 0; source < incoming_requests.segment_count();
       ++source) {
    for (auto const& request : incoming_requests.segment(source)) {
      incoming_requests_are_valid =
          incoming_requests_are_valid &&
          ownership.owner(request.old_label) == rank_index;
    }
  }
  mpi::validate_collectively(
      incoming_requests_are_valid,
      mpi::communicator_view{communicator},
      "label request owner validation failed");

  std::vector<NodeID> local_labels;
  local_labels.reserve(incoming_requests.storage().size());
  for (std::size_t source = 0; source < incoming_requests.segment_count();
       ++source) {
    for (auto const& request : incoming_requests.segment(source)) {
      local_labels.push_back(request.old_label);
    }
  }

  helpers helper;
  helper.filter_duplicates(
      local_labels,
      [](NodeID const& lhs, NodeID const& rhs) -> bool { return (lhs < rhs); },
      [](NodeID const& lhs, NodeID const& rhs) -> bool {
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

  mpi::check_or_abort(MPI_Scan(&local_num_labels,
                               &prefix_sum,
                               1,
                               MPI_UNSIGNED_LONG_LONG,
                               MPI_SUM,
                               communicator),
                      communicator,
                      "MPI_Scan(label prefix)");

  global_num_distinct_ids = prefix_sum;
  // Broadcast global number of ids
  mpi::check_or_abort(MPI_Bcast(&global_num_distinct_ids,
                                1,
                                MPI_UNSIGNED_LONG_LONG,
                                size - 1,
                                communicator),
                      communicator,
                      "MPI_Bcast(global label count)");

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

  auto replies_by_destination =
      std::vector<std::vector<contraction::label_reply>>(
          static_cast<std::size_t>(size));
  for (std::size_t source = 0; source < incoming_requests.segment_count();
       ++source) {
    auto& replies = replies_by_destination[source];
    for (auto const& request : incoming_requests.segment(source)) {
      replies.push_back({request.old_label,
                         label_mapping_to_cnode.at(request.old_label)});
    }
    std::ranges::stable_sort(replies, {}, [](auto const& reply) {
      return std::tie(reply.old_label, reply.coarse_global_id);
    });
  }

  auto incoming_replies = mpi::all_to_all_v(
      mpi::segmented_buffer<contraction::label_reply>::from_segments(
          replies_by_destination),
      mpi::communicator_view{communicator});
  auto incoming_replies_are_valid = true;
  for (std::size_t source = 0; source < incoming_replies.segment_count();
       ++source) {
    for (auto const& reply : incoming_replies.segment(source)) {
      auto const owner = ownership.owner(reply.old_label);
      if (!owner.has_value()) {
        incoming_replies_are_valid = false;
        continue;
      }
      incoming_replies_are_valid =
          incoming_replies_are_valid &&
          *owner == source &&
          reply.coarse_global_id < global_num_distinct_ids &&
          requested_labels.contains(reply.old_label);
      if (requested_labels.erase(reply.old_label) == 0) {
        incoming_replies_are_valid = false;
      }
    }
  }
  incoming_replies_are_valid =
      incoming_replies_are_valid && requested_labels.empty();
  mpi::validate_collectively(
      incoming_replies_are_valid,
      mpi::communicator_view{communicator},
      "label reply validation failed");
  for (auto const& reply : incoming_replies.storage()) {
    label_mapping[reply.old_label] = reply.coarse_global_id;
  }
}

void parallel_contraction::get_nodes_to_cnodes_ghost_nodes(
    MPI_Comm communicator,
    parallel_graph_access& G,
    NodeID number_of_distinct_labels,
    std::unordered_map<NodeID, NodeID> const& label_mapping) {
  auto const graph_communicator = mpi::communicator_view{G.getCommunicator()};

  auto communicator_is_compatible = communicator != MPI_COMM_NULL;
  if (communicator_is_compatible) {
    auto comparison = int{MPI_UNEQUAL};
    mpi::check_or_abort(
        MPI_Comm_compare(communicator, G.getCommunicator(), &comparison),
        G.getCommunicator(), "MPI_Comm_compare(contraction ghost CNodes)");
    communicator_is_compatible =
        comparison == MPI_IDENT || comparison == MPI_CONGRUENT;
  }
  mpi::validate_collectively(
      communicator_is_compatible, graph_communicator,
      "contraction ghost CNode communicator validation failed");

  auto const agreed_coarse_count = mpi::agree_collectively(
      number_of_distinct_labels, graph_communicator,
      "contraction ghost CNode coarse count agreement failed");
  auto const agreed_global_count = mpi::agree_collectively(
      G.number_of_global_nodes(), graph_communicator,
      "contraction ghost CNode global count agreement failed");

  auto staged = std::vector<NodeID>{};
  auto assigned = std::vector<unsigned char>{};
  auto local_is_valid = true;
  try {
    auto const storage_size = G.node_to_cnode_storage_size();
    staged.resize(storage_size);
    assigned.assign(storage_size, static_cast<unsigned char>(0));

    auto const local_count = G.number_of_local_nodes();
    auto const ghost_count = G.number_of_ghost_nodes();
    local_is_valid = std::in_range<std::size_t>(local_count) &&
                     std::in_range<std::size_t>(ghost_count);
    if (local_is_valid) {
      auto const local_size = static_cast<std::size_t>(local_count);
      auto const ghost_size = static_cast<std::size_t>(ghost_count);
      local_is_valid = storage_size > local_size &&
                       storage_size - local_size - std::size_t{1} == ghost_size;
    }

    for (NodeID local = 0; local < local_count; ++local) {
      auto const label = G.getNodeLabel(local);
      auto const mapping = label_mapping.find(label);
      auto const global_id = G.getGlobalID(local);
      auto const roundtrip = G.find_local_id(global_id);
      local_is_valid = local_is_valid && mapping != label_mapping.end() &&
                       global_id < agreed_global_count && roundtrip == local;
      if (mapping == label_mapping.end() ||
          mapping->second >= agreed_coarse_count ||
          !std::in_range<std::size_t>(local)) {
        local_is_valid = false;
        continue;
      }
      auto const index = static_cast<std::size_t>(local);
      if (index >= staged.size() || assigned[index] != 0) {
        local_is_valid = false;
        continue;
      }
      staged[index] = mapping->second;
      assigned[index] = 1;
    }
    if (agreed_coarse_count == 0 && G.number_of_local_nodes() != 0) {
      local_is_valid = false;
    }
  } catch (...) {
    mpi::abort_on_exception(G.getCommunicator(),
                            "contraction ghost CNode local staging");
  }

  mpi::validate_collectively(local_is_valid, graph_communicator,
                             "contraction ghost CNode local validation failed");

  auto const& plan = G.ghost_plan();
  auto semantic_failure = std::string_view{};
  auto semantic_error = std::exception_ptr{};
  try {
    auto outgoing =
        std::vector<std::vector<contraction::ghost_cnode_assignment>>(
            plan.topology().destinations().size());
    auto outgoing_is_valid = true;
    for (std::size_t destination_index = 0;
         destination_index < plan.topology().destinations().size();
         ++destination_index) {
      auto const local_nodes = plan.outgoing_local_nodes(destination_index);
      auto& records = outgoing[destination_index];
      records.reserve(local_nodes.size());
      auto previous = std::optional<NodeID>{};
      for (auto const local : local_nodes) {
        auto const local_is_representable = std::in_range<std::size_t>(local);
        auto const index = local_is_representable
                               ? static_cast<std::size_t>(local)
                               : std::size_t{0};
        outgoing_is_valid = outgoing_is_valid && local_is_representable &&
                            local < G.number_of_local_nodes() &&
                            index < staged.size() && assigned[index] != 0 &&
                            (!previous.has_value() || *previous < local);
        if (!local_is_representable || local >= G.number_of_local_nodes() ||
            index >= staged.size() || assigned[index] == 0) {
          continue;
        }
        auto const global_id = G.getGlobalID(local);
        outgoing_is_valid = outgoing_is_valid &&
                            global_id < agreed_global_count &&
                            G.find_local_id(global_id) == local &&
                            staged[index] < agreed_coarse_count;
        records.push_back({global_id, staged[index]});
        previous = local;
      }
    }
    outgoing_is_valid =
        outgoing_is_valid &&
        (agreed_coarse_count != 0 ||
         std::ranges::all_of(
             outgoing,
             &std::vector<contraction::ghost_cnode_assignment>::empty));
    if (!mpi::detail::collective_predicate(outgoing_is_valid,
                                           plan.topology().view())) {
      semantic_failure = "contraction ghost CNode outgoing validation failed";
    } else {
      auto received = mpi::neighbor_all_to_all_v(
          mpi::segmented_buffer<
              contraction::ghost_cnode_assignment>::from_segments(outgoing),
          plan.topology());

      auto resolved =
          std::vector<std::vector<NodeID>>(plan.topology().sources().size());
      auto structure_is_valid =
          received.segment_count() == plan.topology().sources().size();
      auto const source_limit =
          std::min(received.segment_count(), plan.topology().sources().size());
      for (std::size_t source_index = 0; source_index < source_limit;
           ++source_index) {
        auto const source = plan.topology().sources()[source_index];
        auto const records = received.segment(source_index);
        auto const expected = plan.expected_ghost_nodes(source_index);
        auto& local_ids = resolved[source_index];
        local_ids.reserve(records.size());
        auto received_ids = std::vector<NodeID>{};
        received_ids.reserve(records.size());
        for (auto const& record : records) {
          received_ids.push_back(record.global_id);
          auto const local_id = G.find_ghost_local_id(record.global_id, source);
          structure_is_valid = structure_is_valid &&
                               record.global_id < agreed_global_count &&
                               record.coarse_global_id < agreed_coarse_count &&
                               local_id.has_value();
          local_ids.push_back(local_id.value_or(NodeID{0}));
        }
        std::ranges::sort(received_ids);
        structure_is_valid =
            structure_is_valid && records.size() == expected.size() &&
            std::ranges::adjacent_find(received_ids) == received_ids.end() &&
            std::ranges::equal(received_ids, expected);
      }

      if (!mpi::detail::collective_predicate(structure_is_valid,
                                             plan.topology().view())) {
        semantic_failure = "contraction ghost CNode received validation failed";
      } else {
        auto staging_is_complete = true;
        for (std::size_t source_index = 0;
             source_index < plan.topology().sources().size(); ++source_index) {
          auto const records = received.segment(source_index);
          auto const& local_ids = resolved[source_index];
          staging_is_complete =
              staging_is_complete && records.size() == local_ids.size();
          for (std::size_t record_index = 0; record_index < records.size();
               ++record_index) {
            auto const local_id = local_ids[record_index];
            auto const representable = std::in_range<std::size_t>(local_id);
            auto const index = representable
                                   ? static_cast<std::size_t>(local_id)
                                   : std::size_t{0};
            staging_is_complete = staging_is_complete && representable &&
                                  local_id > G.number_of_local_nodes() &&
                                  index < staged.size() && assigned[index] == 0;
            if (!representable || local_id <= G.number_of_local_nodes() ||
                index >= staged.size() || assigned[index] != 0) {
              continue;
            }
            staged[index] = records[record_index].coarse_global_id;
            assigned[index] = 1;
          }
        }

        auto const local_count =
            static_cast<std::size_t>(G.number_of_local_nodes());
        staging_is_complete = staging_is_complete &&
                              staged.size() == G.node_to_cnode_storage_size() &&
                              local_count < assigned.size() &&
                              assigned[local_count] == 0;
        for (std::size_t index = 0; index < assigned.size(); ++index) {
          if (index != local_count) {
            staging_is_complete = staging_is_complete && assigned[index] != 0;
          }
        }
        if (!mpi::detail::collective_predicate(staging_is_complete,
                                               plan.topology().view())) {
          semantic_failure =
              "contraction ghost CNode staging validation failed";
        } else {
          G.replace_node_to_cnode(std::move(staged));
#if KAHIP_ENABLE_MPI_TRACE
          auto const trace_rank = mpi::communicator_view{communicator}.rank();
#endif
          forall_local_nodes(G, node) {
            KAHIP_MPI_TRACE(mpi::trace::contraction_label(
                mpi::trace::current_hierarchy(), G.getGlobalID(node),
                trace_rank, G.getNodeLabel(node), G.getCNode(node)));
          }
          endfor
        }
      }
    }
    if (!semantic_failure.empty()) {
      semantic_error = std::make_exception_ptr(
          mpi::mpi_error{MPI_ERR_ARG, std::string{semantic_failure}});
    }
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "contraction ghost CNode exchange");
  }

  if (semantic_error != nullptr) {
    std::rethrow_exception(semantic_error);
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



void parallel_contraction::redistribute_hased_graph_and_build_graph_locally( MPI_Comm communicator, hashed_graph &  hG,
                                                                             std::unordered_map< NodeID, NodeWeight > & node_weights,
                                                                             NodeID number_of_cnodes,
                                                                             parallel_graph_access & Q  ) {
        auto const communicator_view = mpi::communicator_view{communicator};
        auto const rank = communicator_view.rank();
        auto const size = communicator_view.size();
        auto const rank_index = static_cast<std::size_t>(rank);
        number_of_cnodes = mpi::agree_collectively(
            number_of_cnodes,
            communicator_view,
            "quotient coarse node count agreement failed");
        auto const ownership = mpi::contiguous_owner_layout<NodeID>{
            number_of_cnodes, static_cast<std::size_t>(size)};

        auto local_edges_are_valid = true;
        for (auto const& [edge, data] : hG) {
                static_cast<void>(data);
                local_edges_are_valid =
                    local_edges_are_valid &&
                    edge.source < number_of_cnodes &&
                    edge.target < number_of_cnodes;
        }
        mpi::validate_collectively(
            local_edges_are_valid,
            mpi::communicator_view{communicator},
            "quotient edge local validation failed");

        auto local_weights_are_valid = true;
        for (auto const& [coarse_global_id, weight] : node_weights) {
                static_cast<void>(weight);
                local_weights_are_valid =
                    local_weights_are_valid &&
                    coarse_global_id < number_of_cnodes;
        }
        mpi::validate_collectively(
            local_weights_are_valid,
            mpi::communicator_view{communicator},
            "quotient node-weight local validation failed");

        auto const from = ownership.begin(rank_index);
        auto const end = ownership.end(rank_index);
        auto const local_num_cnodes = end - from;
        auto const to = local_num_cnodes == 0 ? from : end - 1;
        Q.set_range(from, to);

        auto edges_by_destination =
            std::vector<std::vector<contraction::bundled_edge>>(
                static_cast<std::size_t>(size));
        auto sender_sequences = std::vector<NodeID>(
            static_cast<std::size_t>(size), NodeID{0});
        for (auto const& [edge, data] : hG) {
                auto const source_owner = ownership.owner(edge.source).value();
                auto const target_owner = ownership.owner(edge.target).value();
                edges_by_destination.at(source_owner).push_back(
                    {edge.source,
                     edge.target,
                     data.weight,
                     sender_sequences.at(source_owner)++});
                edges_by_destination.at(target_owner).push_back(
                    {edge.target,
                     edge.source,
                     data.weight,
                     sender_sequences.at(target_owner)++});
        }
        for (auto& edges : edges_by_destination) {
                std::ranges::stable_sort(edges, {}, [](auto const& edge) {
                        return std::tie(
                            edge.source, edge.target, edge.sender_sequence);
                });
        }
        auto incoming_edges = mpi::all_to_all_v(
            mpi::segmented_buffer<contraction::bundled_edge>::from_segments(
                edges_by_destination),
            mpi::communicator_view{communicator});

        auto incoming_edges_are_valid = true;
        for (std::size_t source = 0;
             source < incoming_edges.segment_count();
             ++source) {
                auto source_edges = incoming_edges.segment(source);
                // The pinned upstream build materializes each source rank's
                // unordered hashed_graph iteration order. Preserve that
                // sender-local sequence after the semantic wire sort so the
                // quotient adjacency (and its downstream traversal order)
                // remains exactly compatible with that build.
                std::ranges::sort(
                    source_edges, {}, [](auto const& edge) {
                            return std::tie(edge.sender_sequence,
                                            edge.source,
                                            edge.target);
                    });
                for (std::size_t index = 0; index < source_edges.size();
                     ++index) {
                        auto const& edge = source_edges[index];
                        incoming_edges_are_valid =
                            incoming_edges_are_valid &&
                            edge.sender_sequence ==
                                static_cast<NodeID>(index);
                        if (edge.source >= number_of_cnodes ||
                            edge.target >= number_of_cnodes) {
                                incoming_edges_are_valid = false;
                                continue;
                        }
                        incoming_edges_are_valid =
                            incoming_edges_are_valid &&
                            ownership.owner(edge.source) == rank_index &&
                            Q.is_local_node_from_global_id(edge.source);
                }
        }
        mpi::validate_collectively(
            incoming_edges_are_valid,
            mpi::communicator_view{communicator},
            "quotient edge received validation failed");

        hashed_graph local_graph;
        for (std::size_t source = 0;
             source < incoming_edges.segment_count();
             ++source) {
                for (auto const& edge : incoming_edges.segment(source)) {
                        hashed_edge local_edge;
                        local_edge.k = number_of_cnodes;
                        local_edge.source = edge.source;
                        local_edge.target = edge.target;
                        local_graph[local_edge].weight += edge.weight;
                }
        }

        std::vector < std::vector< std::pair<NodeID, NodeWeight > > > sorted_graph;
        sorted_graph.resize( local_num_cnodes );

        EdgeID edge_counter = 0;
        for(auto & it : local_graph) {
                data_hashed_edge & e = it.second;
                hashed_edge he       = it.first;

                if( from <= he.target && he.target <= to) {
                        std::pair< NodeID, NodeWeight > edge;
                        edge.first  = he.target;
                        edge.second = e.weight/4;

                        std::pair< NodeID, NodeWeight > e_bar;
                        e_bar.first  = he.source;
                        e_bar.second = e.weight/4;

                        sorted_graph[ he.target - from ].push_back( e_bar);
                        sorted_graph[ he.source - from ].push_back( edge );
                        edge_counter+=2;
                } else {
                        std::pair< NodeID, NodeWeight > edge;
                        edge.first  = he.target;
                        edge.second = e.weight/2;
                        sorted_graph[ he.source - from ].push_back( edge );
                        edge_counter++;
                }
        }

        ULONG global_edges = 0;
        mpi::check_or_abort(MPI_Allreduce(&edge_counter,
                                          &global_edges,
                                          1,
                                          MPI_UNSIGNED_LONG_LONG,
                                          MPI_SUM,
                                          communicator),
                            communicator,
                            "MPI_Allreduce(quotient edge count)");

        Q.start_construction(local_num_cnodes, edge_counter, number_of_cnodes, global_edges);
        Q.set_range(from, to);

        std::vector< NodeID > vertex_dist( size+1, 0 );
        for( PEID peID = 0; peID <= size; peID++) {
                vertex_dist[peID] = ownership.boundary(
                    static_cast<std::size_t>(peID));
        }
        //vertex_dist[size] = std::min(to, number_of_cnodes - 1);
        Q.set_range_array(vertex_dist);

        for (NodeID i = 0; i < local_num_cnodes; ++i) {
                NodeID node = Q.new_node();
                NodeID globalID = from+node;
                Q.setNodeWeight(node, 0);
                Q.setNodeLabel(node, globalID);

                for( EdgeID e = 0; e < sorted_graph[node].size(); e++) {
                        NodeID target = sorted_graph[node][e].first;
                        EdgeID e_bar = Q.new_edge(node, target);
                        Q.setEdgeWeight(e_bar, sorted_graph[node][e].second);
                }
        }

        Q.finish_construction();

        auto weights_by_destination =
            std::vector<std::vector<contraction::node_weight_contribution>>(
                static_cast<std::size_t>(size));
        for (auto const& [coarse_global_id, weight] : node_weights) {
                auto const destination =
                    ownership.owner(coarse_global_id).value();
                weights_by_destination.at(destination).push_back(
                    {coarse_global_id, weight});
        }
        for (auto& weights : weights_by_destination) {
                std::ranges::stable_sort(weights, {}, [](auto const& weight) {
                        return std::tie(weight.coarse_global_id, weight.weight);
                });
        }

        auto incoming_weights = mpi::all_to_all_v(
            mpi::segmented_buffer<
                contraction::node_weight_contribution>::from_segments(
                    weights_by_destination),
            mpi::communicator_view{communicator});
        auto incoming_weights_are_valid = true;
        for (std::size_t source = 0;
             source < incoming_weights.segment_count();
             ++source) {
                for (auto const& contribution :
                     incoming_weights.segment(source)) {
                        if (contribution.coarse_global_id >=
                            number_of_cnodes) {
                                incoming_weights_are_valid = false;
                                continue;
                        }
                        incoming_weights_are_valid =
                            incoming_weights_are_valid &&
                            ownership.owner(
                                contribution.coarse_global_id) == rank_index &&
                            Q.is_local_node_from_global_id(
                                contribution.coarse_global_id);
                }
        }
        mpi::validate_collectively(
            incoming_weights_are_valid,
            mpi::communicator_view{communicator},
            "quotient node-weight received validation failed");
        for (std::size_t source = 0;
             source < incoming_weights.segment_count();
             ++source) {
                for (auto const& contribution :
                     incoming_weights.segment(source)) {
                        auto const node =
                            contribution.coarse_global_id - from;
                        Q.setNodeWeight(
                            node,
                            Q.getNodeWeight(node) + contribution.weight);
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
}
