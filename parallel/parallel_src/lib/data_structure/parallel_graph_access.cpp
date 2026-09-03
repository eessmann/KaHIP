/******************************************************************************
 * parallel_graph_access.cpp
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#include "parallel_graph_access.h"
#include "balance_management_coarsening.h"
#include "balance_management_refinement.h"
#include "communication/ghost_exchange_plan.h"
#include "communication/ghost_label_update.h"
#include "communication/mpi_async_neighbors.h"
#include "communication/mpi_failure.h"
#include "communication/mpi_neighbors.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace parhip {
struct ghost_node_communication::state final {
  [[nodiscard]] static auto checked_rank_count(PEID size) -> std::size_t {
    if (size < 0) {
      throw std::invalid_argument{"negative ghost communicator size"};
    }
    return static_cast<std::size_t>(size);
  }

  state(MPI_Comm communicator_value, PEID rank_value, PEID size_value)
      : size(size_value),
        rank(rank_value),
        pe_packed(checked_rank_count(size_value)),
        adjacent_processors(checked_rank_count(size_value)),
        pending_by_rank(checked_rank_count(size_value)),
        communicator(communicator_value) {}

  parallel_graph_access* graph = nullptr;
  PEID size;
  PEID rank;
  NodeID iteration_counter = 0;
  ULONG skip_limit = 0;
  ULONG send_iteration = 1;
  ULONG receive_iteration = 1;
  ULONG desired_rounds = 0;
  bool protocol_validated = false;
  std::vector<bool> pe_packed;
  std::vector<bool> adjacent_processors;
  std::vector<std::vector<ghost_label_update>> pending_by_rank;
  std::optional<mpi::neighbor_exchange_request<ghost_label_update>> in_flight;
  ghost_exchange_plan const* exchange_plan = nullptr;
  MPI_Comm communicator;
};

namespace {
enum class semantic_failure_action {
  throw_transactionally,
  abort_communicator,
};

struct resolved_ghost_updates final {
  std::vector<std::vector<NodeID>> local_ids_by_source;
};

[[nodiscard]] auto resolve_ghost_updates(
    parallel_graph_access& graph,
    ghost_exchange_plan const& plan,
    mpi::segmented_buffer<ghost_label_update> const& received,
    bool require_exact_membership,
    semantic_failure_action failure_action,
    std::string_view context) -> resolved_ghost_updates {
  auto semantic_failure = false;
  auto result = resolved_ghost_updates{};
  try {
    auto local_structure_is_valid =
        received.segment_count() == plan.topology().sources().size();
    result.local_ids_by_source.resize(plan.topology().sources().size());
    for (std::size_t source_index = 0;
         source_index < plan.topology().sources().size(); ++source_index) {
      auto const source = plan.topology().sources()[source_index];
      auto const records = received.segment(source_index);
      auto const expected = plan.expected_ghost_nodes(source_index);
      auto& local_ids = result.local_ids_by_source[source_index];
      local_ids.reserve(records.size());

      auto received_ids = std::vector<NodeID>{};
      if (require_exact_membership) {
        received_ids.reserve(records.size());
      }
      for (auto const& record : records) {
        auto const local_id =
            graph.find_ghost_local_id(record.global_id, source);
        auto const belongs_to_source =
            std::ranges::binary_search(expected, record.global_id);
        local_structure_is_valid = local_structure_is_valid &&
                                   local_id.has_value() && belongs_to_source;
        local_ids.push_back(local_id.value_or(NodeID{0}));
        if (require_exact_membership) {
          received_ids.push_back(record.global_id);
        }
      }

      if (require_exact_membership) {
        std::ranges::sort(received_ids);
        local_structure_is_valid =
            local_structure_is_valid && records.size() == expected.size() &&
            std::ranges::adjacent_find(received_ids) == received_ids.end() &&
            std::ranges::equal(received_ids, expected);
      }
    }

    semantic_failure = !mpi::detail::collective_predicate(
        local_structure_is_valid, plan.topology().view());
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(), context);
  }

  if (semantic_failure) {
    if (failure_action == semantic_failure_action::abort_communicator) {
      mpi::abort_on_programming_error(plan.topology().native_handle(), context);
    }
    mpi::throw_collectively_agreed_semantic_error(
        plan.topology().native_handle(), context);
  }
  return result;
}

void apply_ghost_updates(
    parallel_graph_access& graph,
    ghost_exchange_plan const& plan,
    mpi::segmented_buffer<ghost_label_update> const& received,
    resolved_ghost_updates const& resolved,
    [[maybe_unused]] int receiver,
    std::optional<std::uint32_t> round,
    bool update_non_contained_balance,
    std::string_view context) {
  try {
    for (std::size_t source_index = 0;
         source_index < plan.topology().sources().size(); ++source_index) {
      [[maybe_unused]] auto const source =
          plan.topology().sources()[source_index];
      auto const records = received.segment(source_index);
      auto const& local_ids = resolved.local_ids_by_source[source_index];
      for (std::size_t record_index = 0; record_index < records.size();
           ++record_index) {
        auto const& record = records[record_index];
        auto const local_id = local_ids[record_index];
        if (update_non_contained_balance) {
          graph.update_non_contained_block_balance(
              graph.getNodeLabel(local_id), record.label,
              graph.getNodeWeight(local_id));
        }
        graph.setNodeLabel(local_id, record.label);
        KAHIP_MPI_TRACE(mpi::trace::ghost_update(
            round.has_value() ? mpi::trace::current_hierarchy_with_round(*round)
                              : mpi::trace::current_hierarchy(),
            record.global_id, source, receiver, record.label));
      }
    }
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(), context);
  }
}

[[nodiscard]] auto outgoing_current_labels(parallel_graph_access& graph,
                                           ghost_exchange_plan const& plan)
    -> mpi::segmented_buffer<ghost_label_update> {
  auto outgoing = std::vector<std::vector<ghost_label_update>>(
      plan.topology().destinations().size());
  for (std::size_t destination_index = 0;
       destination_index < plan.topology().destinations().size();
       ++destination_index) {
    auto const local_nodes = plan.outgoing_local_nodes(destination_index);
    auto& records = outgoing[destination_index];
    records.reserve(local_nodes.size());
    std::ranges::transform(
        local_nodes, std::back_inserter(records), [&](NodeID local_node) {
          return ghost_label_update{graph.getGlobalID(local_node),
                                    graph.getNodeLabel(local_node)};
        });
  }
  return mpi::segmented_buffer<ghost_label_update>::from_segments(outgoing);
}
}  // namespace

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

ghost_node_communication::ghost_node_communication(MPI_Comm communicator,
                                                   PEID rank,
                                                   PEID size)
    : state_(std::make_unique<state>(communicator, rank, size)) {}

ghost_node_communication::~ghost_node_communication() = default;

void ghost_node_communication::setGraphReference(
    parallel_graph_access* graph) noexcept {
  state_->graph = graph;
}

void ghost_node_communication::init() noexcept {}

void ghost_node_communication::add_adjacent_processor(PEID pe_id) noexcept {
  if (pe_id < 0 || !std::in_range<std::size_t>(pe_id) ||
      static_cast<std::size_t>(pe_id) >= state_->adjacent_processors.size()) {
    mpi::abort_on_programming_error(
        state_->communicator,
        "ghost communication adjacent rank is out of range");
  }
  state_->adjacent_processors[static_cast<std::size_t>(pe_id)] = true;
}

void ghost_node_communication::set_skip_limit(ULONG skip_limit) noexcept {
  state_->skip_limit = skip_limit;
}

void ghost_node_communication::set_desired_rounds(
    ULONG desired_rounds) noexcept {
  state_->desired_rounds = desired_rounds;
}

bool ghost_node_communication::is_adjacent_PE(PEID pe_id) const noexcept {
  return pe_id >= 0 && std::in_range<std::size_t>(pe_id) &&
         static_cast<std::size_t>(pe_id) < state_->adjacent_processors.size() &&
         state_->adjacent_processors[static_cast<std::size_t>(pe_id)];
}

PEID ghost_node_communication::getNumberOfAdjacentPEs() const noexcept {
  auto const count = std::ranges::count(state_->adjacent_processors, true);
  if (!std::in_range<PEID>(count)) {
    mpi::abort_on_programming_error(
        state_->communicator,
        "ghost communication neighbor count is not representable");
  }
  return static_cast<PEID>(count);
}

bool ghost_node_communication::generation_is_idle() const noexcept {
  auto const has_initial_counters =
      state_->send_iteration == 1 && state_->receive_iteration == 1;
  auto const has_finished_counters =
      state_->send_iteration == 0 && state_->receive_iteration == 0;

  return !state_->in_flight.has_value() && state_->iteration_counter == 0 &&
         std::ranges::none_of(state_->pe_packed, std::identity{}) &&
         (has_initial_counters || has_finished_counters);
}

void ghost_node_communication::reset_generation() noexcept {
  std::ranges::fill(state_->pe_packed, false);
  std::ranges::fill(state_->adjacent_processors, false);
  for (auto& buffer : state_->pending_by_rank) {
    buffer.clear();
  }
  state_->in_flight.reset();
  state_->exchange_plan = nullptr;
  state_->iteration_counter = 0;
  state_->skip_limit = 0;
  state_->send_iteration = 1;
  state_->receive_iteration = 1;
  state_->desired_rounds = 0;
  state_->protocol_validated = false;
}

void ghost_node_communication::addLabel(NodeID node, NodeID label) {
  if (state_->graph == nullptr) {
    mpi::abort_on_programming_error(
        state_->communicator,
        "ghost label buffering requires an attached graph");
  }

  try {
    for (auto edge = state_->graph->get_first_edge(node),
              end = state_->graph->get_first_invalid_edge(node);
         edge < end; ++edge) {
      auto const target = state_->graph->getEdgeTarget(edge);
      if (state_->graph->is_local_node(target)) {
        continue;
      }
      auto const destination = state_->graph->getTargetPE(target);
      if (destination < 0 || !std::in_range<std::size_t>(destination) ||
          static_cast<std::size_t>(destination) >=
              state_->pending_by_rank.size()) {
        mpi::abort_on_programming_error(
            state_->communicator,
            "ghost label destination rank is out of range");
      }
      auto const destination_index = static_cast<std::size_t>(destination);
      if (!state_->pe_packed[destination_index]) {
        state_->pending_by_rank[destination_index].push_back(
            {state_->graph->getGlobalID(node), label});
        state_->pe_packed[destination_index] = true;
      }
    }
    for (auto edge = state_->graph->get_first_edge(node),
              end = state_->graph->get_first_invalid_edge(node);
         edge < end; ++edge) {
      auto const target = state_->graph->getEdgeTarget(edge);
      if (!state_->graph->is_local_node(target)) {
        auto const destination = state_->graph->getTargetPE(target);
        state_->pe_packed[static_cast<std::size_t>(destination)] = false;
      }
    }
  } catch (...) {
    mpi::abort_on_exception(state_->communicator,
                            "ghost label buffering failed");
  }
}

void ghost_node_communication::validate_incremental_protocol(
    ghost_exchange_plan const& plan) {
  if (state_->protocol_validated) {
    return;
  }

  auto const local_protocol = std::array{
      state_->desired_rounds,
      state_->send_iteration,
      state_->receive_iteration,
  };
  auto minimum_protocol = local_protocol;
  auto maximum_protocol = local_protocol;
  auto const communicator = plan.topology().native_handle();
  mpi::check_or_abort(
      MPI_Allreduce(local_protocol.data(), minimum_protocol.data(),
                    static_cast<int>(local_protocol.size()),
                    mpi::get_mpi_datatype<ULONG>(), MPI_MIN, communicator),
      communicator, "MPI_Allreduce(ghost label incremental protocol minimum)");
  mpi::check_or_abort(
      MPI_Allreduce(local_protocol.data(), maximum_protocol.data(),
                    static_cast<int>(local_protocol.size()),
                    mpi::get_mpi_datatype<ULONG>(), MPI_MAX, communicator),
      communicator, "MPI_Allreduce(ghost label incremental protocol maximum)");
  if (minimum_protocol != maximum_protocol) {
    mpi::abort_on_programming_error(
        communicator, "ghost label incremental protocol diverged across ranks");
  }
  state_->protocol_validated = true;
}

void ghost_node_communication::post_pending_round() {
  if (state_->graph == nullptr || state_->in_flight.has_value()) {
    mpi::abort_on_programming_error(
        state_->communicator,
        state_->graph == nullptr
            ? "ghost label post requires an attached graph"
            : "ghost label post requires no active exchange");
  }

  auto const& plan = state_->exchange_plan == nullptr
                         ? state_->graph->ghost_plan()
                         : *state_->exchange_plan;
  state_->exchange_plan = std::addressof(plan);
  validate_incremental_protocol(plan);
  try {
    auto outgoing = std::vector<std::span<ghost_label_update const>>{};
    outgoing.reserve(plan.topology().destinations().size());
    auto destination_is_present =
        std::vector<bool>(state_->pending_by_rank.size(), false);
    for (auto const destination : plan.topology().destinations()) {
      if (destination < 0 || !std::in_range<std::size_t>(destination) ||
          static_cast<std::size_t>(destination) >=
              state_->pending_by_rank.size()) {
        mpi::abort_on_programming_error(
            plan.topology().native_handle(),
            "ghost label topology destination is out of range");
      }
      auto const destination_index = static_cast<std::size_t>(destination);
      destination_is_present[destination_index] = true;
      outgoing.emplace_back(state_->pending_by_rank[destination_index]);
    }
    for (std::size_t rank_index = 0;
         rank_index < state_->pending_by_rank.size(); ++rank_index) {
      if (!state_->pending_by_rank[rank_index].empty() &&
          !destination_is_present[rank_index]) {
        mpi::abort_on_programming_error(
            plan.topology().native_handle(),
            "ghost label buffer targets a rank outside the ghost topology");
      }
    }

    auto sends =
        mpi::segmented_buffer<ghost_label_update>::from_segments(outgoing);
    state_->in_flight.emplace(
        mpi::start_neighbor_all_to_all_v(std::move(sends), plan.topology()));
    for (auto const destination : plan.topology().destinations()) {
      state_->pending_by_rank[static_cast<std::size_t>(destination)].clear();
    }
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "ghost label exchange post failed");
  }
}

void ghost_node_communication::receive_messages_of_neighbors() {
  if (state_->graph == nullptr || !state_->in_flight.has_value()) {
    mpi::abort_on_programming_error(
        state_->communicator,
        state_->graph == nullptr
            ? "ghost label completion requires an attached graph"
            : "ghost label completion requires an active exchange");
  }
  if (state_->receive_iteration == std::numeric_limits<ULONG>::max()) {
    mpi::abort_on_programming_error(
        state_->communicator, "ghost label receive-round counter overflow");
  }
  auto const next_receive_iteration = state_->receive_iteration + ULONG{1};
  if (!std::in_range<std::uint32_t>(next_receive_iteration)) {
    mpi::abort_on_programming_error(
        state_->communicator, "ghost label trace round is not representable");
  }

  auto received = std::move(*state_->in_flight).wait();
  state_->in_flight.reset();
  if (state_->exchange_plan == nullptr) {
    mpi::abort_on_programming_error(
        state_->communicator, "ghost label completion has no cached topology");
  }
  auto const& plan = *state_->exchange_plan;
  auto resolved = resolve_ghost_updates(
      *state_->graph, plan, received, false,
      semantic_failure_action::abort_communicator,
      "incremental ghost label receive validation failed after payload "
      "completion");
  apply_ghost_updates(*state_->graph, plan, received, resolved, state_->rank,
                      static_cast<std::uint32_t>(next_receive_iteration), true,
                      "incremental ghost label application failed");
  state_->receive_iteration = next_receive_iteration;
}

void ghost_node_communication::update_ghost_node_data(
    bool check_iteration_counter) {
  if (check_iteration_counter) {
    if (state_->iteration_counter == std::numeric_limits<NodeID>::max()) {
      mpi::abort_on_programming_error(state_->communicator,
                                      "ghost label skip counter overflow");
    }
    ++state_->iteration_counter;
    if (state_->iteration_counter <= state_->skip_limit || state_->size == 1) {
      return;
    }
  }

  state_->iteration_counter = 0;
  if (state_->send_iteration == std::numeric_limits<ULONG>::max()) {
    mpi::abort_on_programming_error(state_->communicator,
                                    "ghost label send-round counter overflow");
  }
  ++state_->send_iteration;

  if (!state_->in_flight.has_value()) {
    post_pending_round();
    return;
  }

  state_->graph->update_block_weights();
  receive_messages_of_neighbors();
  post_pending_round();
}

void ghost_node_communication::update_ghost_node_data_finish() {
  while (state_->send_iteration < state_->desired_rounds) {
    update_ghost_node_data(false);
  }
  while (state_->receive_iteration < state_->desired_rounds) {
    receive_messages_of_neighbors();
  }
  if (state_->in_flight.has_value()) {
    mpi::abort_on_programming_error(
        state_->communicator,
        "ghost label finish reached the final round with an active exchange");
  }

  update_ghost_node_data(false);
  state_->graph->update_block_weights();
  receive_messages_of_neighbors();

  state_->send_iteration = 0;
  state_->receive_iteration = 0;
  state_->protocol_validated = false;
  state_->iteration_counter = 0;
  for (auto& buffer : state_->pending_by_rank) {
    buffer.clear();
  }
}

void ghost_node_communication::update_ghost_node_data_global() {
  if (state_->graph == nullptr) {
    mpi::abort_on_programming_error(
        state_->communicator,
        "global ghost label exchange requires an attached graph");
  }
  if (!generation_is_idle()) {
    mpi::abort_on_programming_error(
        state_->communicator,
        "global ghost label exchange requires no active incremental "
        "exchange");
  }
  auto const& plan = state_->exchange_plan == nullptr
                         ? state_->graph->ghost_plan()
                         : *state_->exchange_plan;
  state_->exchange_plan = std::addressof(plan);
  auto received = mpi::segmented_buffer<ghost_label_update>{};
  try {
    received = mpi::neighbor_all_to_all_v(
        outgoing_current_labels(*state_->graph, plan), plan.topology());
  } catch (...) {
    mpi::abort_on_exception(plan.topology().native_handle(),
                            "global ghost label exchange failed");
  }

  auto resolved =
      resolve_ghost_updates(*state_->graph, plan, received, true,
                            semantic_failure_action::throw_transactionally,
                            "global ghost label receive validation failed");
  apply_ghost_updates(*state_->graph, plan, received, resolved, state_->rank,
                      std::nullopt, false,
                      "global ghost label application failed");
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
    mpi::throw_collectively_agreed_semantic_error(
        m_communicator,
        "ghost exchange plan requires completed graph construction");
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
