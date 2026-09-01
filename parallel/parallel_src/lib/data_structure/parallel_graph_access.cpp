/******************************************************************************
 * parallel_graph_access.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "balance_management_coarsening.h"
#include "balance_management_refinement.h"
#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_failure.h"
#include "parallel_graph_access.h"

#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace parhip {
ULONG parallel_graph_access::m_comm_rounds = 128;
ULONG parallel_graph_access::m_comm_rounds_up = 128; 

parallel_graph_access::parallel_graph_access()
    : parallel_graph_access(MPI_COMM_WORLD) {}

parallel_graph_access::parallel_graph_access(MPI_Comm communicator) {
  m_communicator = communicator;
  mpi::check_or_abort(MPI_Comm_rank(m_communicator, &rank), m_communicator,
                      "MPI_Comm_rank(parallel graph)");
  mpi::check_or_abort(MPI_Comm_size(m_communicator, &size), m_communicator,
                      "MPI_Comm_size(parallel graph)");

  try {
    m_gnc = new ghost_node_communication(m_communicator, rank, size);
  } catch (...) {
    mpi::abort_on_exception(
        m_communicator,
        "parallel graph ghost communication allocation failed");
  }
  m_gnc->setGraphReference(this);
  m_bm = nullptr;
  reset_graph_generation();
}

parallel_graph_access::~parallel_graph_access() {
  if (m_gnc != nullptr && !m_gnc->generation_is_idle()) {
    if (mpi::runtime_is_active()) {
      mpi::abort_on_programming_error(
          m_communicator,
          "parallel graph destroyed with an active ghost generation");
    }
    mpi::abort_on_inactive_mpi_ownership(
        "parallel graph destroyed with an active ghost generation");
  }
  if (m_ghost_exchange_plan != nullptr && !mpi::runtime_is_active()) {
    mpi::abort_on_inactive_mpi_ownership(
        "parallel graph cached ghost plan destruction");
  }
  m_ghost_exchange_plan.reset();
  m_comm_rounds = std::min(m_comm_rounds, m_comm_rounds_up);
  delete m_gnc;
  if ( m_bm ) delete m_bm;
}

bool ghost_node_communication::generation_is_idle() const noexcept {
  auto const buffers_are_empty = [](auto const& buffers) {
    return std::ranges::all_of(buffers, &std::vector<NodeID>::empty);
  };
  auto const base_tag =
      static_cast<ULONG>(100) * static_cast<ULONG>(m_size);
  auto const has_initial_counters =
      m_send_iteration == 1 && m_recv_iteration == 1 &&
      m_send_tag == base_tag && m_recv_tag == base_tag;
  auto const has_finished_counters =
      base_tag > 0 && m_send_iteration == 0 && m_recv_iteration == 0 &&
      m_send_tag == base_tag - 1 && m_recv_tag == base_tag - 1;

  return m_send_buffers_ptr == &m_send_buffers_A && m_first_send &&
         m_isend_requests.empty() && m_iteration_counter == 0 &&
         std::ranges::none_of(m_PE_packed, std::identity{}) &&
         buffers_are_empty(m_send_buffers_B) &&
         (has_initial_counters || has_finished_counters);
}

void ghost_node_communication::reset_generation() noexcept {
  std::ranges::fill(m_PE_packed, false);
  std::ranges::fill(m_adjacent_processors, false);
  for (auto& buffer : m_send_buffers_A) {
    buffer.clear();
  }
  for (auto& buffer : m_send_buffers_B) {
    buffer.clear();
  }
  m_isend_requests.clear();
  m_send_buffers_ptr = &m_send_buffers_A;
  m_iteration_counter = 0;
  m_skip_limit = 0;
  m_first_send = true;
  m_send_iteration = 1;
  m_recv_iteration = 1;
  auto const base_tag =
      static_cast<ULONG>(100) * static_cast<ULONG>(m_size);
  m_send_tag = base_tag;
  m_recv_tag = base_tag;
  m_desired_rounds = 0;
  m_num_adjacent = 0;
}

void parallel_graph_access::reset_graph_generation() {
  if (!m_gnc->generation_is_idle()) {
    mpi::abort_on_programming_error(
        m_communicator,
        "parallel graph reset requires idle ghost communication");
  }
  if (m_ghost_exchange_plan != nullptr) {
    if (!mpi::runtime_is_active()) {
      mpi::abort_on_inactive_mpi_ownership(
          "parallel graph cached ghost plan reset");
    }
    m_ghost_exchange_plan.reset();
  }
  if (m_bm != nullptr) {
    delete m_bm;
    m_bm = nullptr;
  }
  m_gnc->reset_generation();

  m_nodes.clear();
  m_nodes_data.clear();
  m_edges.clear();
  m_add_non_local_node_data.clear();
  m_nodes_to_cnode.clear();
  m_range_array.clear();
  m_edge_range_array.clear();
  m_global_to_local_id.clear();

  m_ghost_adddata_array_offset = 0;
  m_divisor = 0;
  m_num_local_nodes = 0;
  from = 0;
  to = 0;
  m_building_graph = false;
  m_graph_construction_complete = false;
  m_last_source = std::numeric_limits<NodeID>::max();
  m_num_ghost_nodes = 0;
  node = 0;
  e = 0;
  m_num_nodes = 0;
  m_global_n = 0;
  m_global_m = 0;
  m_max_node_degree = 0;
  m_cur_degree = 0;
}

void parallel_graph_access::start_construction(NodeID n,
                                               EdgeID m,
                                               NodeID global_n,
                                               NodeID global_m,
                                               bool update_comm_rounds) {
  reset_graph_generation();
  if (n == std::numeric_limits<NodeID>::max()) {
    mpi::abort_on_programming_error(
        m_communicator,
        "parallel graph node count cannot represent its sentinel");
  }
  auto const stored_node_count = n + 1;
  if (!std::in_range<std::size_t>(stored_node_count) ||
      !std::in_range<std::size_t>(m)) {
    mpi::abort_on_programming_error(
        m_communicator,
        "parallel graph storage size is not representable");
  }

  try {
    m_building_graph = true;
    m_graph_construction_complete = false;
    m_num_nodes = stored_node_count;
    m_num_local_nodes = n;
    m_global_n = global_n;
    m_global_m = global_m;
    m_ghost_adddata_array_offset = stored_node_count;

    m_nodes.resize(static_cast<std::size_t>(stored_node_count));
    m_nodes_data.resize(static_cast<std::size_t>(stored_node_count));
    m_edges.resize(static_cast<std::size_t>(m));
    m_nodes[0].firstEdge = 0;
    m_divisor = static_cast<NodeID>(std::ceil(global_n / (double)size));
  } catch (...) {
    mpi::abort_on_exception(
        m_communicator,
        "parallel graph storage allocation failed");
  }

  if (update_comm_rounds) {
    m_comm_rounds = std::max(m_comm_rounds, 8ULL);
    m_gnc->set_desired_rounds(m_comm_rounds);
    m_gnc->set_skip_limit(
        static_cast<ULONG>(std::ceil(n / (double)m_comm_rounds)));
  }
}

void parallel_graph_access::reinit() { reset_graph_generation(); }

auto parallel_graph_access::node_to_cnode_storage_size() const noexcept
    -> std::size_t {
  return m_nodes.size();
}

void parallel_graph_access::replace_node_to_cnode(
    std::vector<NodeID>&& replacement) noexcept {
  if (replacement.size() != m_nodes.size()) {
    mpi::abort_on_programming_error(
        m_communicator, "parallel graph CNode replacement size mismatch");
  }
  m_nodes_to_cnode.swap(replacement);
}

auto parallel_graph_access::find_local_id(NodeID global_id) const noexcept
    -> std::optional<NodeID> {
  if (global_id < from) {
    return std::nullopt;
  }
  auto const offset = global_id - from;
  if (offset >= m_num_local_nodes) {
    return std::nullopt;
  }
  return offset;
}

auto parallel_graph_access::find_ghost_local_id(
    NodeID global_id, PEID expected_owner) const noexcept
    -> std::optional<NodeID> {
  auto const mapping = m_global_to_local_id.find(global_id);
  if (mapping == m_global_to_local_id.end()) {
    return std::nullopt;
  }
  auto const local_id = mapping->second;
  if (local_id < m_ghost_adddata_array_offset ||
      !std::in_range<std::size_t>(local_id) ||
      static_cast<std::size_t>(local_id) >= m_nodes.size() ||
      static_cast<std::size_t>(local_id) >= m_nodes_data.size()) {
    return std::nullopt;
  }
  auto const ghost_index = local_id - m_ghost_adddata_array_offset;
  if (!std::in_range<std::size_t>(ghost_index) ||
      static_cast<std::size_t>(ghost_index) >=
          m_add_non_local_node_data.size()) {
    return std::nullopt;
  }
  auto const& metadata =
      m_add_non_local_node_data[static_cast<std::size_t>(ghost_index)];
  if (metadata.globalID != global_id || metadata.peID != expected_owner) {
    return std::nullopt;
  }
  return local_id;
}

auto parallel_graph_access::ghost_plan() -> ghost_exchange_plan const& {
  auto const view = mpi::communicator_view{m_communicator};
  if (!mpi::detail::collective_predicate(
          m_graph_construction_complete && !m_building_graph, view)) {
    throw mpi::mpi_error{
        MPI_ERR_ARG,
        "ghost exchange plan requires completed graph construction"};
  }

  auto const local_cache = m_ghost_exchange_plan == nullptr ? 0 : 1;
  auto minimum_cache = 0;
  auto maximum_cache = 0;
  mpi::check_or_abort(MPI_Allreduce(&local_cache, &minimum_cache, 1, MPI_INT,
                                    MPI_MIN, m_communicator),
                      m_communicator,
                      "MPI_Allreduce(ghost plan cache minimum)");
  mpi::check_or_abort(MPI_Allreduce(&local_cache, &maximum_cache, 1, MPI_INT,
                                    MPI_MAX, m_communicator),
                      m_communicator,
                      "MPI_Allreduce(ghost plan cache maximum)");
  if (minimum_cache != maximum_cache) {
    mpi::abort_on_programming_error(
        m_communicator, "ghost exchange plan cache state diverged across ranks");
  }
  if (minimum_cache != 0) {
    return *m_ghost_exchange_plan;
  }

  auto candidate = make_ghost_exchange_plan(*this);
  if (candidate == nullptr) {
    mpi::abort_on_programming_error(
        m_communicator, "ghost exchange plan factory returned no plan");
  }
  m_ghost_exchange_plan = std::move(candidate);
  return *m_ghost_exchange_plan;
}

void parallel_graph_access::init_balance_management( PPartitionConfig & config ) {
  if( m_bm != NULL ) {
    delete m_bm;
  }

  if( config.total_num_labels != config.k ) {
    m_bm = new balance_management_coarsening( this, config.total_num_labels );
  } else {
    m_bm = new balance_management_refinement( this, config.total_num_labels );
  }
}

void parallel_graph_access::update_non_contained_block_balance( PartitionID from, PartitionID to, NodeWeight node_weight) {
  m_bm->update_non_contained_block_balance( from, to, node_weight);
}
void parallel_graph_access::update_block_weights() {
  m_bm->update();
}

void parallel_graph_access::update_ghost_node_data( bool check_iteration_counter ) {
  m_gnc->update_ghost_node_data( check_iteration_counter );
}
void parallel_graph_access::update_ghost_node_data_global() {
  m_gnc->update_ghost_node_data_global();
}

void parallel_graph_access::update_ghost_node_data_finish() {
  m_gnc->update_ghost_node_data_finish();
}

void parallel_graph_access::set_comm_rounds(ULONG comm_rounds) {
  m_comm_rounds = comm_rounds;
  set_comm_rounds_up(comm_rounds);
}

void parallel_graph_access::set_comm_rounds_up(ULONG comm_rounds) {
  m_comm_rounds_up = comm_rounds;
}
}
