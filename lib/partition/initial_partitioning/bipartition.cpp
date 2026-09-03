/******************************************************************************
 * bipartition.cpp 
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include <algorithm>
#include <cstdint>
#include <optional>
#include <queue>
#include <stdexcept>
#include <vector>

#include "bipartition.h"
#include "data_structure/priority_queues/maxNodeHeap.h"
#include "partition/initial_partitioning/bipartition_candidate.h"
#include "quality_metrics.h"
#include "random_functions.h"
#include "timer.h"
#include "uncoarsening/refinement/quotient_graph_refinement/quotient_graph_refinement.h"
#include "uncoarsening/refinement/refinement.h"

// Ensure both blocks of a bipartition are connected by reassigning
// smaller disconnected components of each block to the other block.
static void repair_bipartition_connectivity(graph_access & G) {
        for(PartitionID block = 0; block < 2; block++) {
                // Find connected components within this block
                std::vector<int> component(G.number_of_nodes(), -1);
                int num_components = 0;
                std::vector<NodeID> comp_sizes;

                forall_nodes(G, node) {
                        if(G.getPartitionIndex(node) == block && component[node] == -1) {
                                NodeID comp_size = 0;
                                std::queue<NodeID> q;
                                q.push(node);
                                component[node] = num_components;
                                while(!q.empty()) {
                                        NodeID v = q.front(); q.pop();
                                        comp_size++;
                                        forall_out_edges(G, e, v) {
                                                NodeID u = G.getEdgeTarget(e);
                                                if(G.getPartitionIndex(u) == block && component[u] == -1) {
                                                        component[u] = num_components;
                                                        q.push(u);
                                                }
                                        } endfor
                                }
                                comp_sizes.push_back(comp_size);
                                num_components++;
                        }
                } endfor

                if(num_components <= 1) continue;

                // Find the largest component
                int largest_comp = 0;
                for(int c = 1; c < num_components; c++) {
                        if(comp_sizes[c] > comp_sizes[largest_comp]) {
                                largest_comp = c;
                        }
                }

                // Reassign all non-largest components to the other block
                PartitionID other_block = 1 - block;
                forall_nodes(G, node) {
                        if(G.getPartitionIndex(node) == block && component[node] != largest_comp) {
                                G.setPartitionIndex(node, other_block);
                        }
                } endfor
        }
}

bipartition::bipartition() {

}

bipartition::~bipartition() {

}

void bipartition::initial_partition( const PartitionConfig & config, 
                                     const unsigned int seed, 
                                     graph_access & G, 
                                     int* partition_map) {

        timer t;
        t.restart();

        auto const targets =
                kahip::initial_partitioning::validated_bipartition_targets(
                        config.target_weights);
        if(!targets) {
                throw std::invalid_argument(
                        "bipartition requires two nonnegative target weights");
        }
        if(config.bipartition_tries <= 0) {
                throw std::invalid_argument(
                        "bipartition requires at least one candidate");
        }
        if(config.bipartition_algorithm != BIPARTITION_BFS &&
           config.bipartition_algorithm != BIPARTITION_FM) {
                throw std::invalid_argument(
                        "bipartition requires a valid growth algorithm");
        }

        if(G.number_of_nodes() == 0) {
                G.set_partition_count(2);
                PRINT(std::cout <<  "bipartition took " <<  t.elapsed()  << std::endl;)
                return;
        }

        auto const iterations = static_cast<unsigned>(config.bipartition_tries);
        auto const requires_two_nonempty_blocks = G.number_of_nodes() >= 2;
        std::optional<kahip::initial_partitioning::bipartition_candidate>
                best_candidate;
        std::vector<int> best_partition(G.number_of_nodes());

        for( unsigned i = 0; i < iterations; i++) {
                if(config.bipartition_algorithm == BIPARTITION_BFS)  {
                        grow_regions_bfs(config, G);
                } else {
                        grow_regions_fm(config, G);
                }

                G.set_partition_count(2);

                post_fm(config, G);

                quality_metrics qm;
                EdgeWeight curcut = qm.edge_cut(G); 

                if(curcut < 0) {
                        throw std::logic_error(
                                "bipartition produced a negative edge cut");
                }

                std::uint64_t lhs_block_weight = 0;
                std::uint64_t rhs_block_weight = 0;
                std::uint64_t lhs_vertices = 0;
                std::uint64_t rhs_vertices = 0;
                bool partition_ids_are_valid = true;

                forall_nodes(G, node) {
                        if(G.getPartitionIndex(node) == 0) {
                                lhs_block_weight += G.getNodeWeight(node);
                                ++lhs_vertices;
                        } else if(G.getPartitionIndex(node) == 1) {
                                rhs_block_weight += G.getNodeWeight(node);
                                ++rhs_vertices;
                        } else {
                                partition_ids_are_valid = false;
                        }
                } endfor

                auto const candidate =
                        kahip::initial_partitioning::make_bipartition_candidate(
                                static_cast<std::uint64_t>(curcut),
                                lhs_block_weight,
                                rhs_block_weight,
                                *targets,
                                lhs_vertices,
                                rhs_vertices,
                                partition_ids_are_valid,
                                requires_two_nonempty_blocks,
                                i);
                if(!best_candidate ||
                   kahip::initial_partitioning::is_better_bipartition_candidate(
                           candidate, *best_candidate)) {
                        best_candidate = candidate;
                        forall_nodes(G, n) {
                                best_partition[n] = G.getPartitionIndex(n);
                        } endfor
                }

        }

        if(!best_candidate || !best_candidate->valid_blocks) {
                throw std::runtime_error(
                        "bipartition failed to produce two valid nonempty blocks");
        }
        std::ranges::copy(best_partition, partition_map);
        PRINT(std::cout <<  "bipartition took " <<  t.elapsed()  << std::endl;)
}

void bipartition::initial_partition( const PartitionConfig & config, 
                                     const unsigned int seed,  
                                     graph_access & G, 
                                     int* xadj,
                                     int* adjncy, 
                                     int* vwgt, 
                                     int* adjwgt,
                                     int* partition_map) {

        std::cout <<  "not implemented yet"  << std::endl;

}

void bipartition::post_fm(const PartitionConfig & config, graph_access & G) {
                refinement* refine          = new quotient_graph_refinement();
                complete_boundary* boundary = new complete_boundary(&G);
                boundary->build();

                PartitionConfig initial_cfg                 = config;
                initial_cfg.fm_search_limit                 = config.bipartition_post_fm_limits;
                initial_cfg.refinement_type                 = REFINEMENT_TYPE_FM;
                initial_cfg.refinement_scheduling_algorithm = REFINEMENT_SCHEDULING_ACTIVE_BLOCKS;
                initial_cfg.bank_account_factor             = 5;
                initial_cfg.rebalance                       = true;
                initial_cfg.softrebalance                   = true;
                initial_cfg.upper_bound_partition           = 100000000;
                initial_cfg.initial_bipartitioning          = true;
                refine->perform_refinement(initial_cfg, G, *boundary);

                delete refine;
                delete boundary;

}

NodeID bipartition::find_start_node( const PartitionConfig & config, graph_access & G) {
        NodeID startNode = random_functions::nextInt(0, G.number_of_nodes()-1);
        NodeID lastNode = startNode;

        int counter = G.number_of_nodes();
        while( G.getNodeDegree(startNode) == 0 && --counter > 0) {
                startNode = random_functions::nextInt(0, G.number_of_nodes()-1);
        }

        //now perform a bfs to get a partition
        for( unsigned i = 0; i < 3; i++) {
                std::vector<bool> touched(G.number_of_nodes(), false);
                startNode = lastNode;
                touched[startNode]  = true;

                std::queue<NodeID>* bfsqueue = new std::queue<NodeID>;
                bfsqueue->push(startNode);
                while(!bfsqueue->empty()) {
                        NodeID source = bfsqueue->front();
                        lastNode = source;
                        bfsqueue->pop();

                        forall_out_edges(G, e, source) {
                                NodeID target = G.getEdgeTarget(e);
                                if(!touched[target]) {
                                        touched[target] = true;
                                        bfsqueue->push(target);
                                }
                        } endfor
                }
                delete bfsqueue;

        }
        return lastNode;
}

void bipartition::grow_regions_dual_bfs(const PartitionConfig & config, graph_access & G) {
        if(G.number_of_nodes() == 0) return;

        // Find start node s and farthest node t
        NodeID s = random_functions::nextInt(0, G.number_of_nodes()-1);
        while(G.getNodeDegree(s) == 0 && s < G.number_of_nodes()-1) s++;
        if(config.buffoon) { s = find_start_node(config, G); }

        // BFS from s to find farthest node t
        std::vector<bool> visited(G.number_of_nodes(), false);
        std::queue<NodeID> tmpq;
        tmpq.push(s);
        visited[s] = true;
        NodeID t = s;
        while(!tmpq.empty()) {
                t = tmpq.front(); tmpq.pop();
                forall_out_edges(G, e, t) {
                        NodeID u = G.getEdgeTarget(e);
                        if(!visited[u]) { visited[u] = true; tmpq.push(u); }
                } endfor
        }

        // Initialize all nodes as unassigned (partition 2 as sentinel)
        forall_nodes(G, node) {
                G.setPartitionIndex(node, 2);
        } endfor

        // Dual BFS: grow block 0 from s, block 1 from t
        std::queue<NodeID> q0, q1;
        G.setPartitionIndex(s, 0); q0.push(s);
        G.setPartitionIndex(t, 1); q1.push(t);
        NodeWeight w0 = G.getNodeWeight(s);
        NodeWeight w1 = G.getNodeWeight(t);

        while(!q0.empty() || !q1.empty()) {
                // Grow the lighter block (or whichever has nodes to expand)
                std::queue<NodeID>* grow_q;
                PartitionID grow_block;
                NodeWeight* grow_weight;

                if(q1.empty() || (!q0.empty() && w0 <= w1)) {
                        grow_q = &q0; grow_block = 0; grow_weight = &w0;
                } else {
                        grow_q = &q1; grow_block = 1; grow_weight = &w1;
                }

                if(grow_q->empty()) break;

                NodeID v = grow_q->front(); grow_q->pop();
                forall_out_edges(G, e, v) {
                        NodeID u = G.getEdgeTarget(e);
                        if(G.getPartitionIndex(u) == 2) { // unassigned
                                G.setPartitionIndex(u, grow_block);
                                grow_q->push(u);
                                *grow_weight += G.getNodeWeight(u);
                        }
                } endfor
        }

        // Handle remaining unassigned nodes (disconnected graph components)
        forall_nodes(G, node) {
                if(G.getPartitionIndex(node) == 2) {
                        if(w0 <= w1) {
                                G.setPartitionIndex(node, 0);
                                w0 += G.getNodeWeight(node);
                        } else {
                                G.setPartitionIndex(node, 1);
                                w1 += G.getNodeWeight(node);
                        }
                }
        } endfor
}

void bipartition::grow_regions_bfs(const PartitionConfig & config, graph_access & G) {
        if(G.number_of_nodes() == 0) return;

        NodeID startNode = random_functions::nextInt(0, G.number_of_nodes()-1);
        if(config.buffoon) { startNode = find_start_node(config, G);  } // more likely to produce connected partitions

        std::vector<bool> touched(G.number_of_nodes(), false);
        touched[startNode]              = true;
        NodeWeight cur_partition_weight = 0;

        forall_nodes(G, node) {
                G.setPartitionIndex(node, 1);
        } endfor

        // The queued start vertex has not been assigned yet. Count it until it
        // is removed from the queue so the RHS-preservation guard observes the
        // actual number of unassigned vertices.
        NodeID nodes_left = G.number_of_nodes();

        //now perform a bfs to get a partition
        std::queue<NodeID>* bfsqueue = new std::queue<NodeID>;
        bfsqueue->push(startNode);
        for(;;) {
                if( nodes_left == 1 ) {
                        //only one node left --> we have to break
                        break;
                }

                if(bfsqueue->empty() && nodes_left > 0 && config.connected_blocks) {
                        break;
                }

                if(bfsqueue->empty() && nodes_left > 0) {
                        //disconnected graph -> find a new start node among those that havent been touched
                        NodeID k = random_functions::nextInt(0, nodes_left-1);
                        NodeID start_node = 0;
                        forall_nodes(G, node) {
                                if(!touched[node]) {
                                        if(k == 0) {
                                                if( G.getNodeDegree(node) != 0) {
                                                        start_node = node;
                                                        break;
                                                } else {
                                                        G.setPartitionIndex(node, 0);
                                                        nodes_left--;
                                                        cur_partition_weight += G.getNodeWeight(node);
                                                        touched[node] = true;

                                                        if(cur_partition_weight >= (NodeWeight) config.grow_target) break;
                                                }
                                        } else {
                                                k--;
                                        }      
                                }
                        } endfor
                        
                        if(cur_partition_weight >= (NodeWeight) config.grow_target) break;
                        
                        bfsqueue->push(start_node);
                        touched[start_node] = true;
                } else if (bfsqueue->empty() && nodes_left == 0) {
                        break;
                }

                NodeID source = bfsqueue->front();
                bfsqueue->pop();
                G.setPartitionIndex(source, 0);

                nodes_left--;
                cur_partition_weight += G.getNodeWeight(source);

                if(cur_partition_weight >= (NodeWeight) config.grow_target) break;

                forall_out_edges(G, e, source) {
                        NodeID target = G.getEdgeTarget(e);
                        if(!touched[target]) {
                                touched[target] = true;
                                bfsqueue->push(target);
                        }
                } endfor
        }
        delete bfsqueue;
}


void bipartition::grow_regions_fm(const PartitionConfig & config, graph_access & G) {
        if(G.number_of_nodes() == 0) return;

        //NodeID startNode        = random_functions::nextInt(0, G.number_of_nodes()-1);
        NodeID startNode        = find_start_node(config, G);

        std::vector<bool> touched(G.number_of_nodes(), false);
        touched[startNode]              = true;
        NodeWeight cur_partition_weight = 0;

        forall_nodes(G, node) {
                G.setPartitionIndex(node, 1);
        } endfor

        // The queued start vertex has not been assigned yet; see
        // grow_regions_bfs.
        NodeID nodes_left = G.number_of_nodes();

        //now perform a pseudo dijkstra to get a partition
        maxNodeHeap* queue = new maxNodeHeap();
        queue->insert(startNode, 0); // in this case the gain doesn't really matter

        for(;;) {
                if( nodes_left == 1 ) {
                        //only one node left --> we have to break
                        break;
                }

                if(queue->empty() && nodes_left > 0 && config.connected_blocks) {
                        break;
                }

                if(queue->empty() && nodes_left > 0) {
                        //disconnected graph -> find a new start node among those that havent been touched
                        NodeID k = random_functions::nextInt(0, nodes_left-1);
                        NodeID start_node = 0;
                        forall_nodes(G, node) {
                                if(!touched[node]) {
                                        if(k == 0) {
                                                start_node = node;
                                                break;
                                        } else {
                                                k--;
                                        }
                                }
                        } endfor

                        queue->insert(start_node, 0);
                        touched[start_node] = true;
                } else if (queue->empty() && nodes_left == 0) {
                        break;
                }

                NodeID source = queue->deleteMax();
                G.setPartitionIndex(source, 0);

                nodes_left--;
                cur_partition_weight += G.getNodeWeight(source);

                if(cur_partition_weight >= (NodeWeight)config.grow_target) break;

                forall_out_edges(G, e, source) {
                        NodeID target = G.getEdgeTarget(e);
                        if(G.getPartitionIndex(target) == 1) { //then we might need to update the gain!
                                Gain gain = compute_gain(G, target, 0);
                                touched[target] = true;

                                if(queue->contains(target)) {
                                        //change the gain
                                        queue->changeKey(target, gain);
                                } else {
                                        //insert
                                        queue->insert(target, gain);
                                }

                        }
                } endfor
        }
        delete queue;
}
