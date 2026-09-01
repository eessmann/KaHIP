/******************************************************************************
 * parallel_graph_access.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_GRAPH_ACCESS_X6O9MRS8
#define PARALLEL_GRAPH_ACCESS_X6O9MRS8

#include <algorithm>
#include <memory>
#include <mpi.h>
#include <optional>
#include <unordered_map>
#include <iostream>
#include <ostream>
#include <fstream>
#include <vector>

#include "data_structure/balance_management.h"
#include "communication/mpi_trace.h"
#include "definitions.h"
#include "partition_config.h"
#include "tools/timer.h"
namespace parhip {
struct Node {
        EdgeID firstEdge;
};

struct NodeData {
        NodeID     label;
        PartitionID block; // a given partition of the graph (for v-cycles)
        NodeWeight weight; // save a little bit of memory
        bool       is_interface_node; // save a little bit of memory
};

//struct NodeData {
//NodeID     label;
//PartitionID block:15; // a given partition of the graph (for v-cycles)
//NodeWeight weight:47; // save a little bit of memory
//bool       is_interface_node:1; // save a little bit of memory
//};

//struct AdditionalNonLocalNodeData {
//PEID   peID:15; // save a little bit of memory
//NodeID globalID:48;
//};

struct AdditionalNonLocalNodeData {
        PEID   peID; // save a little bit of memory
        NodeID globalID;
};

struct Edge {
        NodeID     local_target;
        EdgeWeight weight;
};

//makros - graph access
#define forall_local_nodes(G,n) { for(NodeID n = 0, end = G.number_of_local_nodes(); n < end; ++n) {
#define forall_ghost_nodes(G,n) { for(NodeID node = G.number_of_local_nodes()+1, end = G.number_of_local_nodes()+1+G.number_of_ghost_nodes(); node < end; ++node) { n = node;
#define forall_local_edges(G,e) { for(EdgeID e = 0, end = G.number_of_local_edges(); e < end; ++e) {
#define forall_out_edges(G,e,n) { for(EdgeID e = G.get_first_edge(n), end = G.get_first_invalid_edge(n); e < end; ++e) {
#define endfor }}


class parallel_graph_access;
class ghost_exchange_plan;

// handle communication of data associated with ghost nodes
class ghost_node_communication {
 public:
  ghost_node_communication(MPI_Comm communicator, PEID rank, PEID size);

  ~ghost_node_communication();

  ghost_node_communication(ghost_node_communication const&) = delete;
  auto operator=(ghost_node_communication const&)
      -> ghost_node_communication& = delete;
  ghost_node_communication(ghost_node_communication&&) = delete;
  auto operator=(ghost_node_communication&&)
      -> ghost_node_communication& = delete;

  void setGraphReference(parallel_graph_access* graph) noexcept;
  void init() noexcept;
  void add_adjacent_processor(PEID pe_id) noexcept;
  void set_skip_limit(ULONG skip_limit) noexcept;
  void set_desired_rounds(ULONG desired_rounds) noexcept;

  void update_ghost_node_data(bool check_iteration_counter);
  void update_ghost_node_data_finish();
  void update_ghost_node_data_global();
  void addLabel(NodeID node, NodeID label);

  [[nodiscard]] bool is_adjacent_PE(PEID pe_id) const noexcept;
  [[nodiscard]] PEID getNumberOfAdjacentPEs() const noexcept;

 private:
  friend class parallel_graph_access;

  [[nodiscard]] bool generation_is_idle() const noexcept;
  void reset_generation() noexcept;

  void receive_messages_of_neighbors();
  void post_pending_round();
  void validate_incremental_protocol(ghost_exchange_plan const& plan);

  struct state;
  std::unique_ptr<state> state_;
};


class parallel_graph_access {
public:

        friend class ghost_node_communication;
        friend auto make_ghost_exchange_plan(
            parallel_graph_access const& graph)
            -> std::unique_ptr<ghost_exchange_plan>;

        parallel_graph_access();

        parallel_graph_access( MPI_Comm communicator );

        virtual ~parallel_graph_access();

        parallel_graph_access(parallel_graph_access const&) = delete;
        auto operator=(parallel_graph_access const&)
            -> parallel_graph_access& = delete;
        parallel_graph_access(parallel_graph_access&&) = delete;
        auto operator=(parallel_graph_access&&)
            -> parallel_graph_access& = delete;

        /* ============================================================= */
        /* build methods */
        /* ============================================================= */
        void start_construction(NodeID n, EdgeID m, NodeID global_n,
                                NodeID global_m,
                                bool update_comm_rounds = true);

        void set_range(NodeID l, NodeID r) {
                from        = l;
                to          = r;
        };

        NodeID get_from_range() {
                return from;
        };

        NodeID get_to_range() {
                return to;
        };

        void set_range_array(std::vector< NodeID > & vertex_dist) {
                m_range_array = vertex_dist;
        };

        std::vector<NodeID> &get_range_array() {
                return m_range_array;
        };

        void set_edge_range_array(const std::vector<EdgeID> &edge_range_array) {
                m_edge_range_array = edge_range_array;
        };

        std::vector<EdgeID> &get_edge_range_array() {
                return m_edge_range_array;
        };

        PEID get_PEID_from_range_array(NodeID node) {
                // TODO optimize with binary search
                for( PEID peID = 1; peID < (PEID)m_range_array.size(); peID++) {
                        if( node < m_range_array[peID] ) {
                                return (peID-1);
                        }
                }
                return -1;
        };

        NodeID new_node() {
                m_cur_degree = 0;
                ASSERT_TRUE(m_building_graph);
                return node++;
        };

        EdgeID new_edge(NodeID source, NodeID target) {
                ASSERT_TRUE(m_building_graph);
                ASSERT_TRUE(e < m_edges.size());

                // build ghost nodes on the fly
                if( from <= target && target <= to) {
                        m_edges[e].local_target = target - from;
                } else {
                        m_nodes_data[source].is_interface_node = true;

                        // check wether this is already a ghost node
                        if(m_global_to_local_id.find(target) != m_global_to_local_id.end()) {
                                // this node is already a ghost node
                                m_edges[e].local_target = m_global_to_local_id[target];
                        } else {
                                // we need to create a new ghost node
                                m_global_to_local_id[target] = m_num_nodes++;
                                m_edges[e].local_target      = m_global_to_local_id[target];

                                //create the ghost node in the array
                                Node dummy;
                                dummy.firstEdge = 0;
                                m_nodes.push_back(dummy);

                                NodeData dummy_data;
                                dummy_data.label             = target;
                                dummy_data.block             = 0;
                                dummy_data.is_interface_node = false;
                                dummy_data.weight            = 1;
                                m_nodes_data.push_back(dummy_data);

                                // add addtional data
                                AdditionalNonLocalNodeData add_data;
                                //has to be changed once we implement better load balancing
                                //add_data.peID     = target / m_divisor;
                                add_data.peID     = get_PEID_from_range_array(target);
                                add_data.globalID = target;

                                m_add_non_local_node_data.push_back(add_data);
                                m_gnc->add_adjacent_processor(add_data.peID);
                        }
                }

                EdgeID e_bar = e;
                ++e;

                ASSERT_TRUE(source+1 < m_nodes.size());
                m_nodes[source+1].firstEdge = e;

                //fill isolated sources at the end
                if ((NodeID)(m_last_source+1) < source) {
                        for (NodeID i = source; i>(NodeID)(m_last_source+1); i--) {
                                m_nodes[i].firstEdge = m_nodes[m_last_source+1].firstEdge;
                        }
                }
                m_last_source = source;
                m_cur_degree++;

                if( m_cur_degree > m_max_node_degree ) {
                        m_max_node_degree = m_cur_degree;
                }
                return e_bar;
        };


        void finish_construction() {
                m_edges.resize(e);
                m_building_graph = false;
                m_graph_construction_complete = true;

                //fill isolated sources at the end
                if ((NodeID)(m_last_source) != node-1) {
                        //in that case at least the last node was an isolated node
                        for (NodeID i = node; i>(NodeID)(m_last_source+1); i--) {
                                m_nodes[i].firstEdge = m_nodes[m_last_source+1].firstEdge;
                        }
                }

                m_gnc->init();
        };

        NodeID get_max_degree() {
                return m_max_node_degree;
        }
        /* ============================================================= */
        /* methods handeling balance */
        /* ============================================================= */
        void init_balance_management( PPartitionConfig & config );
        void update_block_weights();

        // if a ghost node changes its block we update the fuzzy block weight
        void update_non_contained_block_balance( PartitionID from, PartitionID to, NodeWeight node_weight);

        NodeWeight getBlockSize( PartitionID block );
        void setBlockSize( PartitionID block, NodeWeight block_size );

        /* ============================================================= */
        /* parallel graph access methods */
        /* ============================================================= */
        NodeID number_of_local_nodes() {return m_num_local_nodes;};
        NodeID number_of_ghost_nodes() {
                return m_nodes.empty()
                           ? NodeID{0}
                           : static_cast<NodeID>(m_nodes.size()) -
                                 m_num_local_nodes - 1;
        };
        NodeID number_of_global_nodes() {return m_global_n;};
        EdgeID number_of_local_edges() {return m_edges.size();};
        EdgeID number_of_global_edges() {return m_global_m;};
        void set_number_of_global_edges( EdgeID global_edges) {m_global_m = global_edges;};

        void allocate_node_to_cnode() {
                m_nodes_to_cnode.resize( m_nodes.size() );
        }

        [[nodiscard]] auto node_to_cnode_storage_size() const noexcept
            -> std::size_t;
        void replace_node_to_cnode(std::vector<NodeID>&& replacement) noexcept;

        void setCNode( NodeID node, NodeID cnode) {
                m_nodes_to_cnode[ node ] = cnode;
        }

        NodeID getCNode( NodeID node ) {
                return m_nodes_to_cnode[node];
        };

        EdgeID get_first_edge(NodeID node);
        EdgeID get_first_invalid_edge(NodeID node);

        NodeID getNodeLabel(NodeID node);
        void setNodeLabel(NodeID node, NodeID label);


        NodeID getSecondPartitionIndex(NodeID node);
        void setSecondPartitionIndex(NodeID node, NodeID label);

        NodeWeight getNodeWeight(NodeID node);
        void setNodeWeight(NodeID node, NodeWeight weight);

        EdgeID getNodeDegree(NodeID node);
        EdgeID getNodeNumGhostNodes(NodeID node);

        bool is_interface_node(NodeID node);
        bool is_local_node(NodeID node);
        bool is_local_node_from_global_id(NodeID node);

        bool is_adjacent_PE(PEID peID) {
                return m_gnc->is_adjacent_PE(peID);
        };

        PEID getNumberOfAdjacentPEs() {
                return m_gnc->getNumberOfAdjacentPEs();
        }

        MPI_Comm getCommunicator() {
                return m_communicator;
        }

        EdgeWeight getEdgeWeight(EdgeID e);
        void setEdgeWeight(EdgeID e, EdgeWeight weight);

        NodeID getEdgeTarget(EdgeID e);

        //methods for non-local / ghost nodes only
        //these methods are usally called to communicate data
        PEID getTargetPE(NodeID node);

        //input is a global id
        //output is the local id
        NodeID getLocalID(NodeID node) {
                if( from <= node && node <= to ) {
                        return node - from;
                } else {
                        return m_global_to_local_id[node];
                }
        };

        [[nodiscard]] auto find_local_id(NodeID global_id) const noexcept
            -> std::optional<NodeID>;
        [[nodiscard]] auto find_ghost_local_id(NodeID global_id,
                                               PEID expected_owner)
            const noexcept -> std::optional<NodeID>;

        [[nodiscard]] auto ghost_plan() -> ghost_exchange_plan const&;

        //methods for local nodes only
        NodeID getGlobalID(NodeID node);

        // these functions should only be called if the graph completely resides on the current PE
        // they are for the call to KaFFPaE
        int* UNSAFE_metis_style_xadj_array();
        int* UNSAFE_metis_style_adjncy_array();

        int* UNSAFE_metis_style_vwgt_array();
        int* UNSAFE_metis_style_adjwgt_array();

        int build_from_metis(int n, int* xadj, int* adjncy);
        int build_from_metis_weighted(int n, int* xadj, int* adjncy, int * vwgt, int* adjwgt);

        /* ============================================================= */
        /* inter process communication routines  */
        /* ============================================================= */
        void update_ghost_node_data( bool check_iteration_counter = true );
        void update_ghost_node_data_finish();
        void update_ghost_node_data_global();

        static void set_comm_rounds(ULONG comm_rounds);
        static void set_comm_rounds_up(ULONG comm_rounds);

        /* ============================================================= */
        /* info  */
        /* ============================================================= */
        void printMemoryUsage(std::ostream& out) const {
#ifndef NOOUTPUT
                out << "** approx. local memory usage on hard disk per node [MB (bytes per node)] **" << std::endl;

                unsigned int memoryTotal = 0;
                memoryTotal += printMemoryUsage(out, "nodes", (m_nodes.size()-1) * (sizeof(Node)+sizeof(NodeData)+sizeof(NodeID)+sizeof(AdditionalNonLocalNodeData)));
                memoryTotal += printMemoryUsage(out, "edges", (m_edges.size()-1) * sizeof(Edge));

                printMemoryUsage(out, "TOTAL", memoryTotal);
                out << std::endl;
#endif
        }

        /** Prints the memory usage of one particular data structure of this UpdateableGraph. */
        unsigned int printMemoryUsage(std::ostream& out, const std::string descr, const unsigned int mem) const {
#ifndef NOOUTPUT
                unsigned int megaBytes = (unsigned int)((mem / (double)(1024*1024)) + 0.5);
                unsigned int bytesPerNode = (unsigned int)((mem / (double)(m_nodes.size()-1)) + 0.5);
                out << "   " << descr << ": " << megaBytes << " (" << bytesPerNode << ")" << std::endl;
#endif
                return mem;
        }

        void reinit();
        /* ============================================================= */
        /* parallel graph data structure  */
        /* ============================================================= */
private:
        void reset_graph_generation();

        // the graph representation itself
        // local and ghost nodes in one array,
        // local nodes are stored in the beginning
        // ghost nodes in the end of the array
        std::vector<Node>                       m_nodes;
        std::vector<NodeData>                   m_nodes_data;
        std::vector<Edge>                       m_edges;

        //Ghost Node Stuff
        std::vector<AdditionalNonLocalNodeData> m_add_non_local_node_data;

        // NodeID to CNode for ghost nodes and local nodes
        std::vector<NodeID>                     m_nodes_to_cnode;

        // stores the ranges for which a processor is responsible for
        // m_range_array[i]= starting position of PE i
        std::vector<NodeID>                     m_range_array;
        std::vector<EdgeID>                     m_edge_range_array;

        std::unordered_map<NodeID, NodeID> m_global_to_local_id;

        NodeID m_ghost_adddata_array_offset; // node id of ghost node - offset to get the position in add data
        NodeID m_divisor; // needed to compute the target id of a ghost node
        NodeID m_num_local_nodes; // store the number of local / non-ghost nodes
        NodeID from; // each process stores nodes [from. to]
        NodeID to;

        // construction properties
        bool   m_building_graph;
        bool   m_graph_construction_complete;
        NodeID m_last_source;
        NodeID m_num_ghost_nodes;
        NodeID node; //current node that is constructed
        EdgeID e;    //current edge that is constructed
        NodeID m_num_nodes;

        NodeID m_global_n; // global number of nodes
        NodeID m_global_m; // global number of edges
        static ULONG m_comm_rounds; // global number of edges
        static ULONG m_comm_rounds_up; // global number of edges

        NodeID m_max_node_degree;
        NodeID m_cur_degree;

        PEID size;
        PEID rank;

        ghost_node_communication* m_gnc;
        std::unique_ptr<ghost_exchange_plan> m_ghost_exchange_plan;
        balance_management* m_bm;
        MPI_Comm m_communicator;

};

typedef parallel_graph_access complete_graph_access; // this is just a naming convention for a graph that is completely local

inline
NodeWeight parallel_graph_access::getBlockSize( PartitionID block ) {
        return m_bm->getBlockSize(block);
}

inline 
void parallel_graph_access::setBlockSize( PartitionID block, NodeWeight block_size ) {
        m_bm->setBlockSize(block, block_size);
}

inline EdgeID parallel_graph_access::get_first_edge(NodeID node) {
#ifdef NDEBUG
        return m_nodes[node].firstEdge;
#else
        return m_nodes.at(node).firstEdge;
#endif
}

inline EdgeID parallel_graph_access::get_first_invalid_edge(NodeID node) {
        return m_nodes[node+1].firstEdge;
}

inline EdgeID parallel_graph_access::getNodeLabel(NodeID node) {
#ifdef NDEBUG
        return m_nodes_data[node].label;
#else
        return m_nodes_data.at(node).label;
#endif
}

inline void parallel_graph_access::setNodeLabel(NodeID node, NodeID label) {
        if( m_nodes_data[node].label != label && is_interface_node(node)) {
                m_gnc->addLabel(node, label);
        }
#ifdef NDEBUG
        m_nodes_data[node].label = label;
#else
        m_nodes_data.at(node).label = label;
#endif
}

inline
NodeID parallel_graph_access::getSecondPartitionIndex(NodeID node) {
#ifdef NDEBUG
        return m_nodes_data[node].block;
#else
        return m_nodes_data.at(node).block;
#endif
}

inline
void parallel_graph_access::setSecondPartitionIndex(NodeID node, NodeID block) {
#ifdef NDEBUG
        m_nodes_data[node].block = block;
#else
        m_nodes_data.at(node).block = block;
#endif
}

inline void parallel_graph_access::setNodeWeight(NodeID node, NodeWeight weight) {
#ifdef NDEBUG
        m_nodes_data[node].weight = weight;
#else
        m_nodes_data.at(node).weight = weight;
#endif
}

inline NodeWeight parallel_graph_access::getNodeWeight(NodeID node) {
#ifdef NDEBUG
        return m_nodes_data[node].weight;
#else
        return m_nodes_data.at(node).weight;
#endif
}
inline EdgeID parallel_graph_access::getNodeDegree(NodeID node) {
        return m_nodes[node+1].firstEdge-m_nodes[node].firstEdge;
}

inline EdgeID parallel_graph_access::getNodeNumGhostNodes(NodeID node) {
        return m_nodes[node+1].firstEdge-m_nodes[node].firstEdge;
}
inline bool parallel_graph_access::is_interface_node(NodeID node) {
#ifdef NDEBUG
        return m_nodes_data[node].is_interface_node;
#else
        return m_nodes_data.at(node).is_interface_node;
#endif
}

inline bool parallel_graph_access::is_local_node_from_global_id(NodeID node) {
        return from <= node && node <= to;
}

inline bool parallel_graph_access::is_local_node(NodeID node) {
        return (node < m_num_local_nodes);
}

inline EdgeWeight parallel_graph_access::getEdgeWeight(EdgeID e) {
#ifdef NDEBUG
        return m_edges[e].weight;
#else
        return m_edges.at(e).weight;
#endif
}

inline void parallel_graph_access::setEdgeWeight(EdgeID e, EdgeWeight weight) {
#ifdef NDEBUG
        m_edges[e].weight = weight;
#else
        m_edges.at(e).weight = weight;
#endif
}

inline NodeID parallel_graph_access::getEdgeTarget(EdgeID e){
#ifdef NDEBUG
        return m_edges[e].local_target;        
#else
        return m_edges.at(e).local_target;        
#endif
}

//function should only be called for ghost nodes
inline PEID parallel_graph_access::getTargetPE(NodeID node) {
#ifdef NDEBUG
        return m_add_non_local_node_data[node-m_ghost_adddata_array_offset].peID;
#else
        ASSERT_GEQ(node, m_ghost_adddata_array_offset);
        return m_add_non_local_node_data.at(node-m_ghost_adddata_array_offset).peID;
#endif
}

//function should only be called for local nodes
inline NodeID parallel_graph_access::getGlobalID(NodeID node) {
        if( is_local_node(node) ) {
                return from + node;
        } else {
#ifdef NDEBUG
                return m_add_non_local_node_data[node-m_ghost_adddata_array_offset].globalID;
#else
                ASSERT_GEQ(node, m_ghost_adddata_array_offset);
                return m_add_non_local_node_data.at(node-m_ghost_adddata_array_offset).globalID;
#endif
        }
}



// this function should only be called if the graph is completely stored on the root PE
inline int* parallel_graph_access::UNSAFE_metis_style_xadj_array() {
        int * xadj      = new int[number_of_local_nodes()+1];
        forall_local_nodes((*this), node) {
                xadj[node] = m_nodes[node].firstEdge;
        } endfor

        xadj[number_of_local_nodes()] = m_nodes[number_of_local_nodes()].firstEdge;

        return xadj;
}


// this function should only be called if the graph is completely stored on the root PE
inline int* parallel_graph_access::UNSAFE_metis_style_adjncy_array() {
        int * adjncy    = new int[number_of_local_edges()];
        forall_local_edges((*this), e) {
                adjncy[e] = m_edges[e].local_target;
        } endfor 

        return adjncy;
}


inline int* parallel_graph_access::UNSAFE_metis_style_vwgt_array() {
        int * vwgt      = new int[number_of_local_nodes()];
        forall_local_nodes((*this), node) {
                vwgt[node] = m_nodes_data[node].weight;
        } endfor

        return vwgt;
}

inline int* parallel_graph_access::UNSAFE_metis_style_adjwgt_array() {
        int * adjwgt    = new int[number_of_local_edges()];
        forall_local_edges((*this), e) {
                adjwgt[e] = m_edges[e].weight;
        } endfor 

        return adjwgt;
}

inline int parallel_graph_access::build_from_metis(int n, int* xadj, int* adjncy) {
        start_construction(n, xadj[n], n, xadj[n]);
        set_range(0,n);

        for( unsigned i = 0; i < (unsigned)n; i++) {
                NodeID node = new_node();
                setNodeWeight(node, 1);
                setNodeLabel(node, 0);

                for( unsigned e = xadj[i]; e < (unsigned)xadj[i+1]; e++) {
                        EdgeID e_bar = new_edge(node, adjncy[e]);
                        setEdgeWeight(e_bar, 1);
                }

        }

        finish_construction();
        return 0;
}

inline int parallel_graph_access::build_from_metis_weighted(int n, int* xadj, int* adjncy, int * vwgt, int* adjwgt) {
        start_construction(n, xadj[n], n, xadj[n]);
        set_range(0,n);

        for( unsigned i = 0; i < (unsigned)n; i++) {
                NodeID node = new_node();
                setNodeWeight(node, vwgt[i]);
                setNodeLabel(node, 0);

                for( unsigned e = xadj[i]; e < (unsigned)xadj[i+1]; e++) {
                        EdgeID e_bar = new_edge(node, adjncy[e]);
                        setEdgeWeight(e_bar, adjwgt[e]);
                }
        }

        finish_construction();
        return 0;
}


// Modern and safe graph traversal functions
// Function to iterate over all local nodes
template <typename Func>
void for_all_local_nodes(parallel_graph_access& G, Func func) {
        for (NodeID n = 0; n < G.number_of_local_nodes(); ++n) {
                func(n);
        }
}

// Function to iterate over all ghost nodes
template <typename Func>
void for_all_ghost_nodes(parallel_graph_access& G, Func func) {
        for (NodeID node = G.number_of_local_nodes()+1, end = G.number_of_local_nodes()+1+G.number_of_ghost_nodes(); node < end; ++node) {
                func(node);
        }
}

// Function to iterate over all local edges
template <typename Func>
void for_all_local_edges(parallel_graph_access& G, Func func) {
        for (EdgeID e = 0; e < G.number_of_local_edges(); ++e) {
                func(e);
        }
}

// Function to iterate over all outgoing edges of a node
template <typename Func>
void for_all_out_edges(parallel_graph_access& G, NodeID n, Func func) {
        for (EdgeID e = G.get_first_edge(n); e < G.get_first_invalid_edge(n); ++e) {
                func(e);
        }
}

}
#endif /* end of include guard: PARALLEL_GRAPH_ACCESS_X6O9MRS8 */
