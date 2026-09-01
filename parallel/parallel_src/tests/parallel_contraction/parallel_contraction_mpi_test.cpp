//
// Created by Erich Essmann on 16/08/2024.
//
#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include "communication/contiguous_owner_layout.h"
#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_error.h"
#include "communication/mpi_tools.h"
#include "communication/mpi_trace.h"
#include "distributed_partitioning/distributed_partitioner.h"
#include "kahip_mpi_capabilities.h"
#include "parallel_contraction_projection/parallel_contraction.h"

using namespace parhip;

namespace protocol_probe {
template <typename T, std::size_t Capacity>
class fixed_log final {
 public:
  void clear() noexcept {
    size_ = 0;
    overflowed_ = false;
  }

  void push_back(T value) noexcept {
    if (size_ == Capacity) {
      overflowed_ = true;
      return;
    }
    values_[size_++] = value;
  }

  [[nodiscard]] auto begin() const noexcept { return values_.begin(); }
  [[nodiscard]] auto end() const noexcept {
    return values_.begin() + static_cast<std::ptrdiff_t>(size_);
  }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto overflowed() const noexcept -> bool { return overflowed_; }

  friend auto operator==(fixed_log const& lhs, std::vector<T> const& rhs)
      -> bool {
    return std::ranges::equal(lhs, rhs);
  }

 private:
  std::array<T, Capacity> values_{};
  std::size_t size_ = 0;
  bool overflowed_ = false;
};

inline bool active = false;
inline int all_to_all_v_calls = 0;
inline int all_to_all_v_c_calls = 0;
inline int topology_create_calls = 0;
inline int neighbor_count_exchange_calls = 0;
inline int neighbor_payload_calls = 0;
inline int neighbor_payload_c_calls = 0;
inline int point_to_point_calls = 0;
inline int immediate_neighbor_calls = 0;
inline int persistent_calls = 0;
inline int completion_calls = 0;
inline int barrier_calls = 0;
inline int isend_calls = 0;
inline int probe_calls = 0;
inline int recv_calls = 0;
inline bool interposer_error = false;
inline fixed_log<MPI_Aint, 32> payload_extents;
inline fixed_log<int, 128> isend_tags;
inline fixed_log<int, 128> probe_tags;
inline fixed_log<int, 128> recv_tags;
inline fixed_log<int, 32> neighbor_sources;
inline fixed_log<int, 32> neighbor_destinations;
inline fixed_log<unsigned long long, 32> neighbor_send_counts;
inline bool ghost_corruption_fired = false;
inline NodeID ghost_coarse_domain = 0;
inline NodeID ghost_replacement_global_id = 0;

enum class receive_mutation {
  none,
  label_request_wrong_owner,
  label_reply_bad_correlation,
  label_reply_coarse_id_out_of_domain,
  quotient_edge_wrong_owner,
  quotient_edge_target_out_of_domain,
  quotient_edge_sequence_gap,
  quotient_node_weight_wrong_owner,
  ghost_cnode_unknown_id,
  ghost_cnode_wrong_source,
  ghost_cnode_duplicate_replacing_missing,
  ghost_cnode_missing_extra,
  ghost_cnode_coarse_id_out_of_domain,
};

inline receive_mutation mutation = receive_mutation::none;
inline int mutation_payload_ordinal = 0;
inline int mutation_target_rank = 0;

void reset() {
  all_to_all_v_calls = 0;
  all_to_all_v_c_calls = 0;
  topology_create_calls = 0;
  neighbor_count_exchange_calls = 0;
  neighbor_payload_calls = 0;
  neighbor_payload_c_calls = 0;
  point_to_point_calls = 0;
  immediate_neighbor_calls = 0;
  persistent_calls = 0;
  completion_calls = 0;
  barrier_calls = 0;
  isend_calls = 0;
  probe_calls = 0;
  recv_calls = 0;
  interposer_error = false;
  payload_extents.clear();
  isend_tags.clear();
  probe_tags.clear();
  recv_tags.clear();
  neighbor_sources.clear();
  neighbor_destinations.clear();
  neighbor_send_counts.clear();
  ghost_corruption_fired = false;
  ghost_coarse_domain = 0;
  ghost_replacement_global_id = 0;
  mutation = receive_mutation::none;
  mutation_payload_ordinal = 0;
  mutation_target_rank = 0;
}

[[nodiscard]] auto dense_payload_collective_calls() -> int {
  return all_to_all_v_calls + all_to_all_v_c_calls;
}

[[nodiscard]] auto blocking_neighbor_payload_calls() -> int {
  return neighbor_payload_calls + neighbor_payload_c_calls;
}

class activation final {
 public:
  explicit activation(receive_mutation selected = receive_mutation::none,
                      int target_rank = 0,
                      NodeID coarse_domain = 0,
                      NodeID replacement_global_id = 0) {
    reset();
    mutation = selected;
    mutation_target_rank = target_rank;
    ghost_coarse_domain = coarse_domain;
    ghost_replacement_global_id = replacement_global_id;
    active = true;
  }
  ~activation() { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};

void record_payload_extent(MPI_Datatype datatype) noexcept {
  MPI_Aint lower_bound = 0;
  MPI_Aint extent = 0;
  if (PMPI_Type_get_extent(datatype, &lower_bound, &extent) != MPI_SUCCESS ||
      lower_bound != 0) {
    interposer_error = true;
    return;
  }
  payload_extents.push_back(extent);
}

template <typename Tags>
[[nodiscard]] auto calls_in_tag_phase(Tags const& tags, int phase, int size)
    -> std::size_t {
  auto const first = phase * size;
  auto const last = (phase + 1) * size;
  return static_cast<std::size_t>(std::ranges::count_if(tags, [&](int tag) {
    return first <= tag && tag < last;
  }));
}

[[nodiscard]] auto payload_calls_with_extent(MPI_Aint extent) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count(payload_extents, extent));
}

class scoped_receive_mutation {
public:
  scoped_receive_mutation(receive_mutation selected,
                          int payload_ordinal,
                          int target_rank = 0) noexcept {
    mutation = selected;
    mutation_payload_ordinal = payload_ordinal;
    mutation_target_rank = target_rank;
  }

  ~scoped_receive_mutation() noexcept {
    mutation = receive_mutation::none;
    mutation_payload_ordinal = 0;
    mutation_target_rank = 0;
  }

  scoped_receive_mutation(scoped_receive_mutation const&) = delete;
  auto operator=(scoped_receive_mutation const&)
      -> scoped_receive_mutation& = delete;
};

template <typename Count, typename Displacement>
void mutate_received_payload(int payload_ordinal,
                             void* receive_buffer,
                             Count const receive_counts[],
                             Displacement const receive_displacements[],
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) noexcept {
  if (mutation == receive_mutation::none ||
      payload_ordinal != mutation_payload_ordinal) {
    return;
  }

  int rank = 0;
  int size = 0;
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      PMPI_Comm_size(communicator, &size) != MPI_SUCCESS) {
    interposer_error = true;
    return;
  }
  if (rank != mutation_target_rank) {
    return;
  }

  MPI_Aint lower_bound = 0;
  MPI_Aint extent = 0;
  if (PMPI_Type_get_extent(receive_datatype, &lower_bound, &extent) !=
          MPI_SUCCESS ||
      lower_bound != 0) {
    interposer_error = true;
    return;
  }
  for (int source = 0; source < size; ++source) {
    if (receive_counts[source] <= 0) {
      continue;
    }
    auto* record = static_cast<std::byte*>(receive_buffer) +
                   static_cast<MPI_Aint>(receive_displacements[source]) *
                       extent;
    switch (mutation) {
      case receive_mutation::label_request_wrong_owner:
        reinterpret_cast<contraction::label_request*>(record)->old_label =
            NodeID{2};
        break;
      case receive_mutation::quotient_edge_wrong_owner:
        reinterpret_cast<contraction::bundled_edge*>(record)->source =
            NodeID{2};
        break;
      case receive_mutation::quotient_edge_target_out_of_domain:
        reinterpret_cast<contraction::bundled_edge*>(record)->target =
            NodeID{4};
        break;
      case receive_mutation::quotient_node_weight_wrong_owner:
        reinterpret_cast<contraction::node_weight_contribution*>(record)
            ->coarse_global_id = NodeID{2};
        break;
      case receive_mutation::label_reply_bad_correlation:
        // Rank 0 requested label 3 from source 1 in this fixture. Label 2 is
        // still in-domain and owned by source 1, but it is not a key rank 0
        // requested, so only semantic correlation rejects it.
        reinterpret_cast<contraction::label_reply*>(record)->old_label =
            NodeID{2};
        break;
      case receive_mutation::label_reply_coarse_id_out_of_domain:
        // The fixture has exactly three distinct labels, so ID 3 is the
        // first invalid half-open coarse ID and exercises the received bound.
        reinterpret_cast<contraction::label_reply*>(record)
            ->coarse_global_id = NodeID{3};
        break;
      case receive_mutation::quotient_edge_sequence_gap:
        ++reinterpret_cast<contraction::bundled_edge*>(record)
              ->sender_sequence;
        break;
      case receive_mutation::ghost_cnode_unknown_id:
      case receive_mutation::ghost_cnode_wrong_source:
      case receive_mutation::ghost_cnode_duplicate_replacing_missing:
      case receive_mutation::ghost_cnode_missing_extra:
      case receive_mutation::ghost_cnode_coarse_id_out_of_domain:
        interposer_error = true;
        return;
      case receive_mutation::none:
        break;
    }
    return;
  }
  interposer_error = true;
}

void record_graph_neighbors(MPI_Comm communicator) noexcept {
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS ||
      indegree < 0 || outdegree < 0 || indegree > 32 || outdegree > 32) {
    interposer_error = true;
    return;
  }
  auto sources = std::array<int, 32>{};
  auto destinations = std::array<int, 32>{};
  if (PMPI_Dist_graph_neighbors(communicator, indegree, sources.data(),
                                MPI_UNWEIGHTED, outdegree, destinations.data(),
                                MPI_UNWEIGHTED) != MPI_SUCCESS) {
    interposer_error = true;
    return;
  }
  for (auto index = 0; index < indegree; ++index) {
    neighbor_sources.push_back(sources[static_cast<std::size_t>(index)]);
  }
  for (auto index = 0; index < outdegree; ++index) {
    neighbor_destinations.push_back(
        destinations[static_cast<std::size_t>(index)]);
  }
}

template <typename Count>
void record_neighbor_send_counts(Count const counts[],
                                 MPI_Comm communicator) noexcept {
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS ||
      outdegree < 0 || outdegree > 32 ||
      (outdegree != 0 && counts == nullptr)) {
    interposer_error = true;
    return;
  }
  for (auto index = 0; index < outdegree; ++index) {
    auto const value = counts[index];
    if (!std::in_range<unsigned long long>(value)) {
      interposer_error = true;
      return;
    }
    neighbor_send_counts.push_back(static_cast<unsigned long long>(value));
  }
}

template <typename Count, typename Displacement>
void mutate_neighbor_payload(void* receive_buffer,
                             Count const receive_counts[],
                             Displacement const receive_displacements[],
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) noexcept {
  auto const is_ghost_mutation =
      mutation == receive_mutation::ghost_cnode_unknown_id ||
      mutation == receive_mutation::ghost_cnode_wrong_source ||
      mutation == receive_mutation::ghost_cnode_duplicate_replacing_missing ||
      mutation == receive_mutation::ghost_cnode_missing_extra ||
      mutation == receive_mutation::ghost_cnode_coarse_id_out_of_domain;
  if (!active || !is_ghost_mutation) {
    return;
  }

  auto rank = 0;
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS ||
      indegree < 0 || indegree > 32 ||
      (indegree != 0 &&
       (receive_buffer == nullptr || receive_counts == nullptr ||
        receive_displacements == nullptr))) {
    interposer_error = true;
    return;
  }
  if (rank != mutation_target_rank) {
    return;
  }

  auto lower_bound = MPI_Aint{0};
  auto extent = MPI_Aint{0};
  if (PMPI_Type_get_extent(receive_datatype, &lower_bound, &extent) !=
          MPI_SUCCESS ||
      lower_bound != 0 ||
      extent !=
          static_cast<MPI_Aint>(sizeof(contraction::ghost_cnode_assignment))) {
    interposer_error = true;
    return;
  }

  auto nonempty = std::array<int, 32>{};
  auto nonempty_count = std::size_t{0};
  for (auto index = 0; index < indegree; ++index) {
    if (receive_counts[index] > 0) {
      nonempty[nonempty_count++] = index;
    }
  }
  if (nonempty_count == 0) {
    interposer_error = true;
    return;
  }

  auto* records =
      static_cast<contraction::ghost_cnode_assignment*>(receive_buffer);
  auto const first = nonempty[0];
  if (!std::in_range<std::size_t>(receive_displacements[first])) {
    interposer_error = true;
    return;
  }
  auto const first_offset =
      static_cast<std::size_t>(receive_displacements[first]);
  switch (mutation) {
    case receive_mutation::ghost_cnode_unknown_id:
      records[first_offset].global_id = std::numeric_limits<NodeID>::max();
      ghost_corruption_fired = true;
      break;
    case receive_mutation::ghost_cnode_wrong_source:
      if (nonempty_count < 2 ||
          !std::in_range<std::size_t>(receive_displacements[nonempty[1]])) {
        interposer_error = true;
        return;
      }
      records[first_offset] =
          records[static_cast<std::size_t>(receive_displacements[nonempty[1]])];
      ghost_corruption_fired = true;
      break;
    case receive_mutation::ghost_cnode_duplicate_replacing_missing:
      if (receive_counts[first] < 2) {
        interposer_error = true;
        return;
      }
      records[first_offset + 1] = records[first_offset];
      ghost_corruption_fired = true;
      break;
    case receive_mutation::ghost_cnode_missing_extra:
      records[first_offset].global_id = ghost_replacement_global_id;
      ghost_corruption_fired = true;
      break;
    case receive_mutation::ghost_cnode_coarse_id_out_of_domain:
      records[first_offset].coarse_global_id = ghost_coarse_domain;
      ghost_corruption_fired = true;
      break;
    case receive_mutation::none:
    case receive_mutation::label_request_wrong_owner:
    case receive_mutation::label_reply_bad_correlation:
    case receive_mutation::label_reply_coarse_id_out_of_domain:
    case receive_mutation::quotient_edge_wrong_owner:
    case receive_mutation::quotient_edge_target_out_of_domain:
    case receive_mutation::quotient_edge_sequence_gap:
    case receive_mutation::quotient_node_weight_wrong_owner:
      interposer_error = true;
      break;
  }
}
}  // namespace protocol_probe

extern "C" int MPI_Alltoallv(const void* send_buffer,
                             const int send_counts[],
                             const int send_displacements[],
                             MPI_Datatype send_datatype,
                             void* receive_buffer,
                             const int receive_counts[],
                             const int receive_displacements[],
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::all_to_all_v_calls;
    protocol_probe::record_payload_extent(send_datatype);
  }
  auto const payload_ordinal = protocol_probe::dense_payload_collective_calls();
  auto const result = PMPI_Alltoallv(send_buffer,
                                     send_counts,
                                     send_displacements,
                                     send_datatype,
                                     receive_buffer,
                                     receive_counts,
                                     receive_displacements,
                                     receive_datatype,
                                     communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_received_payload(payload_ordinal,
                                            receive_buffer,
                                            receive_counts,
                                            receive_displacements,
                                            receive_datatype,
                                            communicator);
  }
  return result;
}

#if KAHIP_HAVE_MPI_ALLTOALLV_C
extern "C" int MPI_Alltoallv_c(const void* send_buffer,
                               const MPI_Count send_counts[],
                               const MPI_Aint send_displacements[],
                               MPI_Datatype send_datatype,
                               void* receive_buffer,
                               const MPI_Count receive_counts[],
                               const MPI_Aint receive_displacements[],
                               MPI_Datatype receive_datatype,
                               MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::all_to_all_v_c_calls;
    protocol_probe::record_payload_extent(send_datatype);
  }
  auto const payload_ordinal = protocol_probe::dense_payload_collective_calls();
  auto const result = PMPI_Alltoallv_c(send_buffer,
                                       send_counts,
                                       send_displacements,
                                       send_datatype,
                                       receive_buffer,
                                       receive_counts,
                                       receive_displacements,
                                       receive_datatype,
                                       communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_received_payload(payload_ordinal,
                                            receive_buffer,
                                            receive_counts,
                                            receive_displacements,
                                            receive_datatype,
                                            communicator);
  }
  return result;
}
#endif

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::topology_create_calls;
  }
  auto const result = PMPI_Dist_graph_create(
      communicator, source_count, sources, degrees, destinations, weights, info,
      reorder, graph_communicator);
  if (protocol_probe::active && result == MPI_SUCCESS &&
      graph_communicator != nullptr && *graph_communicator != MPI_COMM_NULL) {
    protocol_probe::record_graph_neighbors(*graph_communicator);
  }
  return result;
}

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_datatype,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_datatype,
                                     MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::neighbor_count_exchange_calls;
  }
  return PMPI_Neighbor_alltoall(send_buffer, send_count, send_datatype,
                                receive_buffer, receive_count, receive_datatype,
                                communicator);
}

extern "C" int MPI_Neighbor_alltoallv(void const* send_buffer,
                                      int const send_counts[],
                                      int const send_displacements[],
                                      MPI_Datatype send_datatype,
                                      void* receive_buffer,
                                      int const receive_counts[],
                                      int const receive_displacements[],
                                      MPI_Datatype receive_datatype,
                                      MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::neighbor_payload_calls;
    protocol_probe::record_payload_extent(send_datatype);
    protocol_probe::record_neighbor_send_counts(send_counts, communicator);
  }
  auto const result = PMPI_Neighbor_alltoallv(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_neighbor_payload(receive_buffer, receive_counts,
                                            receive_displacements,
                                            receive_datatype, communicator);
  }
  return result;
}

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const send_counts[],
                                        MPI_Aint const send_displacements[],
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        MPI_Count const receive_counts[],
                                        MPI_Aint const receive_displacements[],
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::neighbor_payload_c_calls;
    protocol_probe::record_payload_extent(send_datatype);
    protocol_probe::record_neighbor_send_counts(send_counts, communicator);
  }
  auto const result = PMPI_Neighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_neighbor_payload(receive_buffer, receive_counts,
                                            receive_displacements,
                                            receive_datatype, communicator);
  }
  return result;
}
#endif

extern "C" int MPI_Isend(const void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::point_to_point_calls;
    ++protocol_probe::isend_calls;
    protocol_probe::isend_tags.push_back(tag);
  }
  return PMPI_Isend(
      buffer, count, datatype, destination, tag, communicator, request);
}

extern "C" int MPI_Probe(int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Status* status) {
  if (protocol_probe::active) {
    ++protocol_probe::point_to_point_calls;
    ++protocol_probe::probe_calls;
    protocol_probe::probe_tags.push_back(tag);
  }
  return PMPI_Probe(source, tag, communicator, status);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (protocol_probe::active) {
    ++protocol_probe::point_to_point_calls;
    ++protocol_probe::recv_calls;
    protocol_probe::recv_tags.push_back(tag);
  }
  return PMPI_Recv(
      buffer, count, datatype, source, tag, communicator, status);
}

#define KAHIP_CONTRACTION_P2P_WRAPPER(name, signature, arguments) \
  extern "C" int name signature {                                 \
    if (protocol_probe::active) {                                 \
      ++protocol_probe::point_to_point_calls;                     \
    }                                                             \
    return P##name arguments;                                     \
  }

KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Send,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Ssend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Bsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Rsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Issend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Ibsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Irsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Irecv,
    (void* buffer,
     int count,
     MPI_Datatype datatype,
     int source,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, source, tag, communicator, request))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Iprobe,
    (int source, int tag, MPI_Comm communicator, int* flag, MPI_Status* status),
    (source, tag, communicator, flag, status))
KAHIP_CONTRACTION_P2P_WRAPPER(MPI_Sendrecv,
                              (void const* send_buffer,
                               int send_count,
                               MPI_Datatype send_datatype,
                               int destination,
                               int send_tag,
                               void* receive_buffer,
                               int receive_count,
                               MPI_Datatype receive_datatype,
                               int source,
                               int receive_tag,
                               MPI_Comm communicator,
                               MPI_Status* status),
                              (send_buffer,
                               send_count,
                               send_datatype,
                               destination,
                               send_tag,
                               receive_buffer,
                               receive_count,
                               receive_datatype,
                               source,
                               receive_tag,
                               communicator,
                               status))
KAHIP_CONTRACTION_P2P_WRAPPER(MPI_Sendrecv_replace,
                              (void* buffer,
                               int count,
                               MPI_Datatype datatype,
                               int destination,
                               int send_tag,
                               int source,
                               int receive_tag,
                               MPI_Comm communicator,
                               MPI_Status* status),
                              (buffer,
                               count,
                               datatype,
                               destination,
                               send_tag,
                               source,
                               receive_tag,
                               communicator,
                               status))
KAHIP_CONTRACTION_P2P_WRAPPER(MPI_Mprobe,
                              (int source,
                               int tag,
                               MPI_Comm communicator,
                               MPI_Message* message,
                               MPI_Status* status),
                              (source, tag, communicator, message, status))
KAHIP_CONTRACTION_P2P_WRAPPER(
    MPI_Improbe,
    (int source,
     int tag,
     MPI_Comm communicator,
     int* flag,
     MPI_Message* message,
     MPI_Status* status),
    (source, tag, communicator, flag, message, status))
KAHIP_CONTRACTION_P2P_WRAPPER(MPI_Mrecv,
                              (void* buffer,
                               int count,
                               MPI_Datatype datatype,
                               MPI_Message* message,
                               MPI_Status* status),
                              (buffer, count, datatype, message, status))
KAHIP_CONTRACTION_P2P_WRAPPER(MPI_Imrecv,
                              (void* buffer,
                               int count,
                               MPI_Datatype datatype,
                               MPI_Message* message,
                               MPI_Request* request),
                              (buffer, count, datatype, message, request))

#undef KAHIP_CONTRACTION_P2P_WRAPPER

extern "C" int MPI_Ineighbor_alltoallv(void const* send_buffer,
                                       int const send_counts[],
                                       int const send_displacements[],
                                       MPI_Datatype send_datatype,
                                       void* receive_buffer,
                                       int const receive_counts[],
                                       int const receive_displacements[],
                                       MPI_Datatype receive_datatype,
                                       MPI_Comm communicator,
                                       MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::immediate_neighbor_calls;
  }
  return PMPI_Ineighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                  send_datatype, receive_buffer, receive_counts,
                                  receive_displacements, receive_datatype,
                                  communicator, request);
}

#if KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Ineighbor_alltoallv_c(void const* send_buffer,
                                         MPI_Count const send_counts[],
                                         MPI_Aint const send_displacements[],
                                         MPI_Datatype send_datatype,
                                         void* receive_buffer,
                                         MPI_Count const receive_counts[],
                                         MPI_Aint const receive_displacements[],
                                         MPI_Datatype receive_datatype,
                                         MPI_Comm communicator,
                                         MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::immediate_neighbor_calls;
  }
  return PMPI_Ineighbor_alltoallv_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, request);
}
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT
extern "C" int MPI_Neighbor_alltoallv_init(void const* send_buffer,
                                           int const send_counts[],
                                           int const send_displacements[],
                                           MPI_Datatype send_datatype,
                                           void* receive_buffer,
                                           int const receive_counts[],
                                           int const receive_displacements[],
                                           MPI_Datatype receive_datatype,
                                           MPI_Comm communicator,
                                           MPI_Info info,
                                           MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::persistent_calls;
  }
  return PMPI_Neighbor_alltoallv_init(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
}
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C
extern "C" int MPI_Neighbor_alltoallv_init_c(
    void const* send_buffer,
    MPI_Count const send_counts[],
    MPI_Aint const send_displacements[],
    MPI_Datatype send_datatype,
    void* receive_buffer,
    MPI_Count const receive_counts[],
    MPI_Aint const receive_displacements[],
    MPI_Datatype receive_datatype,
    MPI_Comm communicator,
    MPI_Info info,
    MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::persistent_calls;
  }
  return PMPI_Neighbor_alltoallv_init_c(
      send_buffer, send_counts, send_displacements, send_datatype,
      receive_buffer, receive_counts, receive_displacements, receive_datatype,
      communicator, info, request);
}
#endif

extern "C" int MPI_Start(MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::persistent_calls;
  }
  return PMPI_Start(request);
}

extern "C" int MPI_Startall(int count, MPI_Request requests[]) {
  if (protocol_probe::active) {
    ++protocol_probe::persistent_calls;
  }
  return PMPI_Startall(count, requests);
}

#define KAHIP_CONTRACTION_COMPLETION_WRAPPER(name, signature, arguments) \
  extern "C" int name signature {                                        \
    if (protocol_probe::active) {                                        \
      ++protocol_probe::completion_calls;                                \
    }                                                                    \
    return P##name arguments;                                            \
  }

KAHIP_CONTRACTION_COMPLETION_WRAPPER(MPI_Test,
                                     (MPI_Request * request,
                                      int* complete,
                                      MPI_Status* status),
                                     (request, complete, status))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(MPI_Wait,
                                     (MPI_Request * request,
                                      MPI_Status* status),
                                     (request, status))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(MPI_Waitall,
                                     (int count,
                                      MPI_Request requests[],
                                      MPI_Status statuses[]),
                                     (count, requests, statuses))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(
    MPI_Testall,
    (int count, MPI_Request requests[], int* complete, MPI_Status statuses[]),
    (count, requests, complete, statuses))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(MPI_Testany,
                                     (int count,
                                      MPI_Request requests[],
                                      int* index,
                                      int* complete,
                                      MPI_Status* status),
                                     (count, requests, index, complete, status))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(
    MPI_Testsome,
    (int count,
     MPI_Request requests[],
     int* completed,
     int indices[],
     MPI_Status statuses[]),
    (count, requests, completed, indices, statuses))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(
    MPI_Waitany,
    (int count, MPI_Request requests[], int* index, MPI_Status* status),
    (count, requests, index, status))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(
    MPI_Waitsome,
    (int count,
     MPI_Request requests[],
     int* completed,
     int indices[],
     MPI_Status statuses[]),
    (count, requests, completed, indices, statuses))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(MPI_Request_free,
                                     (MPI_Request * request),
                                     (request))
KAHIP_CONTRACTION_COMPLETION_WRAPPER(MPI_Cancel,
                                     (MPI_Request * request),
                                     (request))

#undef KAHIP_CONTRACTION_COMPLETION_WRAPPER

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::barrier_calls;
  }
  return PMPI_Barrier(communicator);
}

namespace parhip {
struct parallel_contraction_test_access {
  static void assign_nodes_to_cnodes(
      MPI_Comm communicator,
      parallel_graph_access& graph,
      NodeID number_of_distinct_labels,
      std::unordered_map<NodeID, NodeID> const& label_mapping) {
    parallel_contraction contraction;
    contraction.get_nodes_to_cnodes_ghost_nodes(
        communicator, graph, number_of_distinct_labels, label_mapping);
  }

  [[nodiscard]] static auto compute_label_mapping(
      MPI_Comm communicator,
      parallel_graph_access& graph)
      -> std::pair<NodeID, std::unordered_map<NodeID, NodeID>> {
    NodeID global_num_distinct_ids = 0;
    std::unordered_map<NodeID, NodeID> label_mapping;
    parallel_contraction contraction;
    contraction.compute_label_mapping(
        communicator, graph, global_num_distinct_ids, label_mapping);
    return {global_num_distinct_ids, std::move(label_mapping)};
  }

  static void redistribute_quotient(
      MPI_Comm communicator,
      hashed_graph& graph,
      std::unordered_map<NodeID, NodeWeight>& node_weights,
      NodeID number_of_cnodes,
      parallel_graph_access& quotient) {
    parallel_contraction contraction;
    contraction.redistribute_hased_graph_and_build_graph_locally(
        communicator, graph, node_weights, number_of_cnodes, quotient);
  }
};
}  // namespace parhip

namespace {
void build_label_fixture(parallel_graph_access& graph, int rank, int size) {
  constexpr auto global_nodes = NodeID{4};
  constexpr auto labels = std::array<NodeID, 4>{3, 1, 3, 0};
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1);
  for (int pe = 0; pe <= size; ++pe) {
    ranges[static_cast<std::size_t>(pe)] =
        (static_cast<NodeID>(pe) * global_nodes) /
        static_cast<NodeID>(size);
  }
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  auto const local_nodes = end - first;
  graph.start_construction(local_nodes, 0, global_nodes, 0, false);
  graph.set_range(first, local_nodes == 0 ? first : end - 1);
  graph.set_range_array(ranges);
  for (NodeID global = first; global < end; ++global) {
    auto const node = graph.new_node();
    graph.setNodeWeight(node, 1);
    graph.setNodeLabel(node, labels[static_cast<std::size_t>(global)]);
  }
  graph.finish_construction();
}

void build_empty_label_fixture(parallel_graph_access& graph, int size) {
  graph.start_construction(0, 0, 0, 0, false);
  graph.set_range(0, 0);
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 0);
  graph.set_range_array(ranges);
  graph.finish_construction();
}

void build_empty_label_fixture_with_global_count(
    parallel_graph_access& graph,
    NodeID global_nodes,
    int size) {
  graph.start_construction(0, 0, global_nodes, 0, false);
  graph.set_range(0, 0);
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 0);
  graph.set_range_array(ranges);
  graph.finish_construction();
}

struct cnode_fixture {
  std::vector<NodeID> ranges;
  std::vector<std::vector<NodeID>> adjacency;
};

[[nodiscard]] auto contraction_cnode_fixture(int size) -> cnode_fixture {
  if (size == 1) {
    return {{0, 0}, {}};
  }
  if (size == 2) {
    return {{0, 1, 2}, {{1, 1}, {0, 0}}};
  }
  if (size == 3) {
    return {{0, 1, 2, 2}, {{1, 1}, {0, 0}}};
  }
  if (size == 4) {
    return {{0, 1, 2, 3, 4}, {{3, 1}, {0, 2}, {1, 3}, {2, 0}}};
  }
  return {{0, 2, 3, 5, 6, 7}, {{1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4}, {}}};
}

void build_cnode_fixture(parallel_graph_access& graph,
                         cnode_fixture const& fixture,
                         int rank,
                         std::optional<NodeID> global_count = std::nullopt) {
  auto const first = fixture.ranges.at(static_cast<std::size_t>(rank));
  auto const end = fixture.ranges.at(static_cast<std::size_t>(rank + 1));
  auto local_edges = std::size_t{0};
  for (auto global = first; global < end; ++global) {
    local_edges +=
        fixture.adjacency.at(static_cast<std::size_t>(global)).size();
  }
  auto const global_edges = std::ranges::fold_left(
      fixture.adjacency | std::views::transform(&std::vector<NodeID>::size),
      std::size_t{0}, std::plus<>{});
  graph.start_construction(
      end - first, static_cast<EdgeID>(local_edges),
      global_count.value_or(static_cast<NodeID>(fixture.adjacency.size())),
      static_cast<EdgeID>(global_edges), false);
  graph.set_range(first, first == end ? first : end - 1);
  auto ranges = fixture.ranges;
  graph.set_range_array(ranges);
  for (auto global = first; global < end; ++global) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, global);
    for (auto const target :
         fixture.adjacency.at(static_cast<std::size_t>(global))) {
      auto const edge = graph.new_edge(local, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
}

[[nodiscard]] auto paired_label_mapping(cnode_fixture const& fixture)
    -> std::unordered_map<NodeID, NodeID> {
  auto result = std::unordered_map<NodeID, NodeID>{};
  for (auto global = NodeID{0}; global < fixture.adjacency.size(); ++global) {
    result.emplace(global, global / NodeID{2});
  }
  return result;
}

[[nodiscard]] auto paired_coarse_count(cnode_fixture const& fixture) -> NodeID {
  auto const node_count = static_cast<NodeID>(fixture.adjacency.size());
  return node_count == 0 ? NodeID{0}
                         : (node_count - NodeID{1}) / NodeID{2} + NodeID{1};
}

[[nodiscard]] auto snapshot_cnodes(parallel_graph_access& graph)
    -> std::vector<NodeID> {
  auto result = std::vector<NodeID>(graph.node_to_cnode_storage_size());
  for (auto index = std::size_t{0}; index < result.size(); ++index) {
    result[index] = graph.getCNode(static_cast<NodeID>(index));
  }
  return result;
}

void seed_cnodes(parallel_graph_access& graph, NodeID first = NodeID{900}) {
  auto values = std::vector<NodeID>(graph.node_to_cnode_storage_size());
  std::ranges::iota(values, first);
  graph.replace_node_to_cnode(std::move(values));
}

[[nodiscard]] auto contraction_validation_fixture(int size) -> cnode_fixture {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 6);
  ranges[0] = 0;
  ranges[1] = 2;
  ranges[2] = 4;
  ranges[3] = 6;
  return {std::move(ranges), {{2}, {3}, {0, 4}, {1, 5}, {2}, {3}}};
}

[[nodiscard]] auto asymmetric_cnode_fixture(int size) -> cnode_fixture {
  auto ranges = std::vector<NodeID>(static_cast<std::size_t>(size) + 1, 2);
  ranges[0] = 0;
  ranges[1] = 1;
  ranges[2] = 2;
  return {std::move(ranges), {{1}, {}}};
}

void require_common_probe_result(bool local_condition) {
  auto const local = local_condition ? 1 : 0;
  auto common = 0;
  REQUIRE(PMPI_Allreduce(&local, &common, 1, MPI_INT, MPI_MIN,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(common == 1);
}

template <typename Operation>
void require_collective_validation_failure(Operation&& operation,
                                           std::string_view expected_context,
                                           int size) {
  auto caught = 0;
  auto structured = 0;
  auto context_matches = 0;
  try {
    std::invoke(std::forward<Operation>(operation));
  } catch (mpi::mpi_error const& error) {
    caught = 1;
    structured = 1;
    context_matches = error.context().find(expected_context) !=
                              std::string_view::npos
                          ? 1
                          : 0;
  } catch (std::exception const&) {
    caught = 1;
  }

  auto caught_by_all = 0;
  auto structured_by_all = 0;
  auto context_matches_all = 0;
  REQUIRE(MPI_Allreduce(
              &caught, &caught_by_all, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD) ==
          MPI_SUCCESS);
  REQUIRE(MPI_Allreduce(&structured,
                        &structured_by_all,
                        1,
                        MPI_INT,
                        MPI_SUM,
                        MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(MPI_Allreduce(&context_matches,
                        &context_matches_all,
                        1,
                        MPI_INT,
                        MPI_SUM,
                        MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(caught_by_all == size);
  REQUIRE(structured_by_all == size);
  REQUIRE(context_matches_all == size);
}
}  // namespace

TEST_CASE("ghost CNode assignment wire datatype has exact extent",
          "[unit][mpi][contraction][ghost-cnode][datatype]") {
  STATIC_REQUIRE(
      std::is_standard_layout_v<contraction::ghost_cnode_assignment>);
  STATIC_REQUIRE(
      std::is_trivially_copyable_v<contraction::ghost_cnode_assignment>);
  auto datatype = mpi::make_mpi_datatype<contraction::ghost_cnode_assignment>();
  auto lower_bound = MPI_Aint{0};
  auto extent = MPI_Aint{0};
  REQUIRE(MPI_Type_get_extent(datatype.native_handle(), &lower_bound,
                              &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  REQUIRE(extent ==
          static_cast<MPI_Aint>(sizeof(contraction::ghost_cnode_assignment)));
}

TEST_CASE("graph CNode storage replacement is exact and transactional",
          "[unit][mpi][contraction][ghost-cnode][storage]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  auto const fixture = contraction_cnode_fixture(size);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);

  auto replacement = std::vector<NodeID>(graph.node_to_cnode_storage_size());
  std::ranges::iota(replacement, NodeID{17});
  auto const expected = replacement;
  graph.replace_node_to_cnode(std::move(replacement));

  auto local_is_exact = true;
  for (auto node = NodeID{0}; node < static_cast<NodeID>(expected.size());
       ++node) {
    local_is_exact = local_is_exact && graph.getCNode(node) == expected[node];
  }
  require_common_probe_result(local_is_exact);
}

TEST_CASE("ghost CNode assignment uses one blocking neighborhood exchange",
          "[unit][mpi][contraction][ghost-cnode][protocol]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  auto const fixture = contraction_cnode_fixture(size);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  auto const mapping = paired_label_mapping(fixture);
  auto const coarse_count = paired_coarse_count(fixture);

  mpi::trace::reset();
  mpi::trace::set_active(true);
  KAHIP_MPI_TRACE_SET_HIERARCHY(11, 7, mpi::trace::epoch::contraction);

  {
    auto probe = protocol_probe::activation{};
    parallel_contraction_test_access::assign_nodes_to_cnodes(
        MPI_COMM_WORLD, graph, coarse_count, mapping);

    auto local_is_valid = true;
    for (auto local = NodeID{0}; local < graph.number_of_local_nodes();
         ++local) {
      local_is_valid = local_is_valid &&
                       graph.getCNode(local) == graph.getGlobalID(local) / 2;
    }
    for (auto local = graph.number_of_local_nodes() + NodeID{1};
         local < graph.number_of_local_nodes() + NodeID{1} +
                     graph.number_of_ghost_nodes();
         ++local) {
      local_is_valid = local_is_valid &&
                       graph.getCNode(local) == graph.getGlobalID(local) / 2;
    }

    auto const& plan = graph.ghost_plan();
    local_is_valid = local_is_valid && !protocol_probe::interposer_error &&
                     !protocol_probe::payload_extents.overflowed() &&
                     !protocol_probe::neighbor_sources.overflowed() &&
                     !protocol_probe::neighbor_destinations.overflowed() &&
                     !protocol_probe::neighbor_send_counts.overflowed() &&
                     std::ranges::equal(protocol_probe::neighbor_sources,
                                        plan.topology().sources()) &&
                     std::ranges::equal(protocol_probe::neighbor_destinations,
                                        plan.topology().destinations()) &&
                     protocol_probe::neighbor_send_counts.size() ==
                         plan.topology().destinations().size();
    if (protocol_probe::neighbor_send_counts.size() ==
        plan.topology().destinations().size()) {
      for (std::size_t index = 0; index < plan.topology().destinations().size();
           ++index) {
        local_is_valid =
            local_is_valid &&
            *std::next(protocol_probe::neighbor_send_counts.begin(),
                       static_cast<std::ptrdiff_t>(index)) ==
                plan.outgoing_local_nodes(index).size();
      }
    }

    auto const trace = mpi::trace::snapshot();
#if KAHIP_ENABLE_MPI_TRACE
    auto const trace_size_is_exact =
        trace.size() == static_cast<std::size_t>(graph.number_of_local_nodes());
    local_is_valid = local_is_valid && trace_size_is_exact;
    if (trace_size_is_exact) {
      for (auto local = NodeID{0}; local < graph.number_of_local_nodes();
           ++local) {
        auto const expected = mpi::trace::contraction_label(
            mpi::trace::current_hierarchy(), graph.getGlobalID(local), rank,
            graph.getNodeLabel(local), graph.getGlobalID(local) / 2);
        local_is_valid = local_is_valid &&
                         trace[static_cast<std::size_t>(local)] == expected;
      }
    }
#else
    local_is_valid = local_is_valid && trace.empty();
#endif

#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
    auto const payload_path_is_exact =
        protocol_probe::neighbor_payload_calls == 0 &&
        protocol_probe::neighbor_payload_c_calls == 1;
#else
    auto const payload_path_is_exact =
        protocol_probe::neighbor_payload_calls == 1 &&
        protocol_probe::neighbor_payload_c_calls == 0;
#endif
    local_is_valid =
        local_is_valid && protocol_probe::topology_create_calls == 1 &&
        protocol_probe::neighbor_count_exchange_calls == 1 &&
        protocol_probe::blocking_neighbor_payload_calls() == 1 &&
        protocol_probe::payload_extents.size() == 1 &&
        *protocol_probe::payload_extents.begin() ==
            static_cast<MPI_Aint>(
                sizeof(contraction::ghost_cnode_assignment)) &&
        payload_path_is_exact && protocol_probe::point_to_point_calls == 0 &&
        protocol_probe::isend_calls == 0 && protocol_probe::probe_calls == 0 &&
        protocol_probe::recv_calls == 0 &&
        protocol_probe::immediate_neighbor_calls == 0 &&
        protocol_probe::persistent_calls == 0 &&
        protocol_probe::completion_calls == 0 &&
        protocol_probe::barrier_calls == 0 &&
        protocol_probe::dense_payload_collective_calls() == 0;
    require_common_probe_result(local_is_valid);
  }
  mpi::trace::set_active(false);
  mpi::trace::reset();
}

TEST_CASE(
    "ghost CNode receive failures preserve the complete mapping and trace",
    "[unit][mpi][contraction][ghost-cnode][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const modes = std::array{
      protocol_probe::receive_mutation::ghost_cnode_unknown_id,
      protocol_probe::receive_mutation::ghost_cnode_wrong_source,
      protocol_probe::receive_mutation::ghost_cnode_duplicate_replacing_missing,
      protocol_probe::receive_mutation::ghost_cnode_missing_extra,
      protocol_probe::receive_mutation::ghost_cnode_coarse_id_out_of_domain};

  for (auto const mode : modes) {
    auto const fixture = contraction_validation_fixture(size);
    auto const mapping = paired_label_mapping(fixture);
    auto const coarse_count = paired_coarse_count(fixture);
    parallel_graph_access graph{MPI_COMM_WORLD};
    build_cnode_fixture(graph, fixture, rank);
    seed_cnodes(graph);
    auto const before_cnodes = snapshot_cnodes(graph);

    mpi::trace::reset();
    mpi::trace::set_active(true);
#if KAHIP_ENABLE_MPI_TRACE
    mpi::trace::append(mpi::trace::contraction_label(
        mpi::trace::current_hierarchy(), 777, rank, 19, 23));
#endif
    auto const before_trace = mpi::trace::snapshot();

    auto probe = protocol_probe::activation{mode, 1, coarse_count, NodeID{2}};
    require_collective_validation_failure(
        [&] {
          parallel_contraction_test_access::assign_nodes_to_cnodes(
              MPI_COMM_WORLD, graph, coarse_count, mapping);
        },
        "contraction ghost CNode received validation failed", size);

    auto fired = protocol_probe::ghost_corruption_fired ? 1 : 0;
    auto fired_total = 0;
    REQUIRE(PMPI_Allreduce(&fired, &fired_total, 1, MPI_INT, MPI_SUM,
                           MPI_COMM_WORLD) == MPI_SUCCESS);
    auto local_failure_is_transactional =
        fired_total == 1 && !protocol_probe::interposer_error &&
        snapshot_cnodes(graph) == before_cnodes &&
        mpi::trace::snapshot() == before_trace &&
        protocol_probe::topology_create_calls == 1 &&
        protocol_probe::neighbor_count_exchange_calls == 1 &&
        protocol_probe::blocking_neighbor_payload_calls() == 1 &&
        protocol_probe::point_to_point_calls == 0 &&
        protocol_probe::immediate_neighbor_calls == 0 &&
        protocol_probe::persistent_calls == 0 &&
        protocol_probe::completion_calls == 0;
    require_common_probe_result(local_failure_is_transactional);

    protocol_probe::mutation = protocol_probe::receive_mutation::none;
    protocol_probe::ghost_corruption_fired = false;
    parallel_contraction_test_access::assign_nodes_to_cnodes(
        MPI_COMM_WORLD, graph, coarse_count, mapping);

    auto local_retry_is_exact =
        protocol_probe::topology_create_calls == 1 &&
        protocol_probe::neighbor_count_exchange_calls == 2 &&
        protocol_probe::blocking_neighbor_payload_calls() == 2 &&
        protocol_probe::point_to_point_calls == 0;
    for (auto local = NodeID{0}; local < graph.number_of_local_nodes();
         ++local) {
      local_retry_is_exact =
          local_retry_is_exact &&
          graph.getCNode(local) == graph.getGlobalID(local) / NodeID{2};
    }
    for (auto local = graph.number_of_local_nodes() + NodeID{1};
         local < graph.number_of_local_nodes() + NodeID{1} +
                     graph.number_of_ghost_nodes();
         ++local) {
      local_retry_is_exact =
          local_retry_is_exact &&
          graph.getCNode(local) == graph.getGlobalID(local) / NodeID{2};
    }
#if KAHIP_ENABLE_MPI_TRACE
    local_retry_is_exact =
        local_retry_is_exact &&
        mpi::trace::snapshot().size() ==
            before_trace.size() +
                static_cast<std::size_t>(graph.number_of_local_nodes());
#else
    local_retry_is_exact =
        local_retry_is_exact && mpi::trace::snapshot().empty();
#endif
    require_common_probe_result(local_retry_is_exact);
    mpi::trace::set_active(false);
    mpi::trace::reset();
  }
}

TEST_CASE("zero coarse domain with local work fails before sparse payload",
          "[unit][mpi][contraction][ghost-cnode][failure][zero-domain]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const fixture = contraction_cnode_fixture(size);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  seed_cnodes(graph);
  auto const before = snapshot_cnodes(graph);
  auto zero_mapping = std::unordered_map<NodeID, NodeID>{};
  for (auto global = NodeID{0}; global < fixture.adjacency.size(); ++global) {
    zero_mapping.emplace(global, NodeID{0});
  }
  mpi::trace::reset();
  mpi::trace::set_active(true);
#if KAHIP_ENABLE_MPI_TRACE
  mpi::trace::append(mpi::trace::contraction_label(
      mpi::trace::current_hierarchy(), 778, rank, 20, 24));
#endif
  auto const before_trace = mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::assign_nodes_to_cnodes(
            MPI_COMM_WORLD, graph, 0, zero_mapping);
      },
      "contraction ghost CNode local validation failed", size);
  require_common_probe_result(
      snapshot_cnodes(graph) == before &&
      mpi::trace::snapshot() == before_trace &&
      protocol_probe::topology_create_calls == 0 &&
      protocol_probe::neighbor_count_exchange_calls == 0 &&
      protocol_probe::blocking_neighbor_payload_calls() == 0 &&
      protocol_probe::point_to_point_calls == 0);
  mpi::trace::set_active(false);
  mpi::trace::reset();
}

TEST_CASE("ghost CNode local mapping failure converges before topology",
          "[unit][mpi][contraction][ghost-cnode][failure][mapping]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const fixture = contraction_cnode_fixture(size);
  auto mapping = paired_label_mapping(fixture);
  if (rank == 0) {
    mapping.erase(NodeID{0});
  }
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  seed_cnodes(graph);
  auto const before = snapshot_cnodes(graph);
  auto probe = protocol_probe::activation{};
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::assign_nodes_to_cnodes(
            MPI_COMM_WORLD, graph, paired_coarse_count(fixture), mapping);
      },
      "contraction ghost CNode local validation failed", size);
  require_common_probe_result(
      snapshot_cnodes(graph) == before &&
      protocol_probe::topology_create_calls == 0 &&
      protocol_probe::neighbor_count_exchange_calls == 0 &&
      protocol_probe::blocking_neighbor_payload_calls() == 0);
}

TEST_CASE("ghost CNode count agreements precede topology creation",
          "[unit][mpi][contraction][ghost-cnode][failure][agreement]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const fixture = contraction_cnode_fixture(size);
  auto const mapping = paired_label_mapping(fixture);
  SECTION("coarse count") {
    parallel_graph_access graph{MPI_COMM_WORLD};
    build_cnode_fixture(graph, fixture, rank);
    auto probe = protocol_probe::activation{};
    require_collective_validation_failure(
        [&] {
          parallel_contraction_test_access::assign_nodes_to_cnodes(
              MPI_COMM_WORLD, graph,
              paired_coarse_count(fixture) + (rank == 0 ? NodeID{1} : 0),
              mapping);
        },
        "contraction ghost CNode coarse count agreement failed", size);
    require_common_probe_result(
        protocol_probe::topology_create_calls == 0 &&
        protocol_probe::neighbor_count_exchange_calls == 0 &&
        protocol_probe::blocking_neighbor_payload_calls() == 0);
  }
  SECTION("graph global count") {
    parallel_graph_access graph{MPI_COMM_WORLD};
    build_cnode_fixture(graph, fixture, rank,
                        static_cast<NodeID>(fixture.adjacency.size()) +
                            (rank == 0 ? NodeID{1} : NodeID{0}));
    auto probe = protocol_probe::activation{};
    require_collective_validation_failure(
        [&] {
          parallel_contraction_test_access::assign_nodes_to_cnodes(
              MPI_COMM_WORLD, graph, paired_coarse_count(fixture), mapping);
        },
        "contraction ghost CNode global count agreement failed", size);
    require_common_probe_result(
        protocol_probe::topology_create_calls == 0 &&
        protocol_probe::neighbor_count_exchange_calls == 0 &&
        protocol_probe::blocking_neighbor_payload_calls() == 0);
  }
}

TEST_CASE("ghost CNode exchange accepts congruent communicators",
          "[unit][mpi][contraction][ghost-cnode][communicator]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  auto const fixture = contraction_cnode_fixture(size);
  auto const mapping = paired_label_mapping(fixture);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  auto congruent = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_dup(MPI_COMM_WORLD, &congruent) == MPI_SUCCESS);
  {
    auto probe = protocol_probe::activation{};
    parallel_contraction_test_access::assign_nodes_to_cnodes(
        congruent, graph, paired_coarse_count(fixture), mapping);
    require_common_probe_result(
        protocol_probe::topology_create_calls == 1 &&
        protocol_probe::neighbor_count_exchange_calls == 1 &&
        protocol_probe::blocking_neighbor_payload_calls() == 1);
  }
  REQUIRE(MPI_Comm_free(&congruent) == MPI_SUCCESS);
}

TEST_CASE("ghost CNode exchange reuses a consistency-initialized plan",
          "[unit][mpi][contraction][ghost-cnode][reuse]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  auto const fixture = contraction_cnode_fixture(size);
  auto const mapping = paired_label_mapping(fixture);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  for (auto local = graph.number_of_local_nodes() + NodeID{1};
       local < graph.number_of_local_nodes() + NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    graph.setNodeLabel(local, graph.getGlobalID(local));
  }
  auto partitioner = distributed_partitioner{};
  auto config = PPartitionConfig{};
  {
    auto consistency_probe = protocol_probe::activation{};
    partitioner.check_labels(MPI_COMM_WORLD, config, graph);
    require_common_probe_result(
        protocol_probe::topology_create_calls == 1 &&
        protocol_probe::neighbor_count_exchange_calls == 1 &&
        protocol_probe::blocking_neighbor_payload_calls() == 1 &&
        protocol_probe::point_to_point_calls == 0);
  }

  auto probe = protocol_probe::activation{};
  parallel_contraction_test_access::assign_nodes_to_cnodes(
      MPI_COMM_WORLD, graph, paired_coarse_count(fixture), mapping);
  require_common_probe_result(
      protocol_probe::topology_create_calls == 0 &&
      protocol_probe::neighbor_count_exchange_calls == 1 &&
      protocol_probe::blocking_neighbor_payload_calls() == 1 &&
      protocol_probe::point_to_point_calls == 0 &&
      protocol_probe::immediate_neighbor_calls == 0 &&
      protocol_probe::persistent_calls == 0 &&
      protocol_probe::completion_calls == 0);
}

TEST_CASE("ghost CNode exchange rejects similar communicators before topology",
          "[unit][mpi][contraction][ghost-cnode][communicator][failure]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size < 2) {
    return;
  }
  auto const fixture = contraction_cnode_fixture(size);
  auto const mapping = paired_label_mapping(fixture);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  seed_cnodes(graph);
  auto const before = snapshot_cnodes(graph);
  auto similar = MPI_COMM_NULL;
  REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, size - rank, &similar) ==
          MPI_SUCCESS);
  {
    auto probe = protocol_probe::activation{};
    require_collective_validation_failure(
        [&] {
          parallel_contraction_test_access::assign_nodes_to_cnodes(
              similar, graph, paired_coarse_count(fixture), mapping);
        },
        "contraction ghost CNode communicator validation failed", size);
    require_common_probe_result(
        snapshot_cnodes(graph) == before &&
        protocol_probe::topology_create_calls == 0 &&
        protocol_probe::neighbor_count_exchange_calls == 0 &&
        protocol_probe::blocking_neighbor_payload_calls() == 0);
  }
  REQUIRE(MPI_Comm_free(&similar) == MPI_SUCCESS);
}

TEST_CASE("asymmetric ghost topology fails commonly before payload",
          "[unit][mpi][contraction][ghost-cnode][failure][asymmetric]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }
  auto const fixture = asymmetric_cnode_fixture(size);
  auto const mapping = paired_label_mapping(fixture);
  parallel_graph_access graph{MPI_COMM_WORLD};
  build_cnode_fixture(graph, fixture, rank);
  seed_cnodes(graph);
  auto const before = snapshot_cnodes(graph);
  auto probe = protocol_probe::activation{};
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::assign_nodes_to_cnodes(
            MPI_COMM_WORLD, graph, paired_coarse_count(fixture), mapping);
      },
      "ghost exchange plan semantic validation failed", size);
  require_common_probe_result(
      snapshot_cnodes(graph) == before &&
      protocol_probe::topology_create_calls == 1 &&
      protocol_probe::neighbor_count_exchange_calls == 0 &&
      protocol_probe::blocking_neighbor_payload_calls() == 0 &&
      protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("label mapping uses explicit semantic replies and preserves contiguous IDs",
          "[unit][mpi][contraction][label-mapping]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);

  protocol_probe::reset();
  protocol_probe::active = true;
  auto [global_num_distinct_ids, mapping] =
      parallel_contraction_test_access::compute_label_mapping(
          MPI_COMM_WORLD, graph);
  protocol_probe::active = false;

  REQUIRE(global_num_distinct_ids == 3);
  forall_local_nodes(graph, node) {
    auto const old_label = graph.getNodeLabel(node);
    REQUIRE(mapping.contains(old_label));
    auto const expected = old_label == 0 ? NodeID{0}
                         : old_label == 1 ? NodeID{1}
                                          : NodeID{2};
    REQUIRE(mapping.at(old_label) == expected);
  } endfor

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_calls,
          protocol_probe::probe_calls,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(protocol_probe::payload_extents ==
          std::vector<MPI_Aint>{static_cast<MPI_Aint>(sizeof(NodeID)),
                                static_cast<MPI_Aint>(2 * sizeof(NodeID))});
  REQUIRE(protocol_probe::isend_calls == 0);
  REQUIRE(protocol_probe::probe_calls == 0);
  REQUIRE(protocol_probe::recv_calls == 0);
}

TEST_CASE("global-zero label mapping keeps empty keyed exchanges exact",
          "[unit][mpi][contraction][label-mapping][zero]") {
  int size = 0;
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_empty_label_fixture(graph, size);

  protocol_probe::reset();
  protocol_probe::active = true;
  auto [global_num_distinct_ids, mapping] =
      parallel_contraction_test_access::compute_label_mapping(
          MPI_COMM_WORLD, graph);
  protocol_probe::active = false;

  REQUIRE(global_num_distinct_ids == 0);
  REQUIRE(mapping.empty());
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
  REQUIRE(protocol_probe::payload_extents ==
          std::vector<MPI_Aint>{static_cast<MPI_Aint>(sizeof(NodeID)),
                                static_cast<MPI_Aint>(2 * sizeof(NodeID))});
  REQUIRE(protocol_probe::isend_calls == 0);
  REQUIRE(protocol_probe::probe_calls == 0);
  REQUIRE(protocol_probe::recv_calls == 0);
}

TEST_CASE("label mapping rejects an out-of-domain local label collectively",
          "[unit][mpi][contraction][label-mapping][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);
  if (rank == 0) {
    graph.setNodeLabel(0, graph.number_of_global_nodes());
  }

  require_collective_validation_failure(
      [&] {
        static_cast<void>(
            parallel_contraction_test_access::compute_label_mapping(
                MPI_COMM_WORLD, graph));
      },
      "label request local validation",
      size);
}

TEST_CASE("label mapping rejects an empty-payload global-count mismatch collectively",
          "[unit][mpi][contraction][label-mapping][failure][domain]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_empty_label_fixture_with_global_count(
      graph, rank == 0 ? NodeID{2} : NodeID{3}, size);

  require_collective_validation_failure(
      [&] {
        static_cast<void>(
            parallel_contraction_test_access::compute_label_mapping(
                MPI_COMM_WORLD, graph));
      },
      "label global node count agreement failed",
      size);
}

TEST_CASE("label request receive validation rejects a valid wrong-owner record collectively",
          "[unit][mpi][contraction][label-mapping][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::label_request_wrong_owner, 1};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        static_cast<void>(
            parallel_contraction_test_access::compute_label_mapping(
                MPI_COMM_WORLD, graph));
      },
      "label request owner validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("label reply receive validation rejects bad keyed correlation collectively",
          "[unit][mpi][contraction][label-mapping][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::label_reply_bad_correlation, 2};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        static_cast<void>(
            parallel_contraction_test_access::compute_label_mapping(
                MPI_COMM_WORLD, graph));
      },
      "label reply validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("label reply receive validation rejects an out-of-domain coarse ID collectively",
          "[unit][mpi][contraction][label-mapping][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  parallel_graph_access graph{MPI_COMM_WORLD};
  build_label_fixture(graph, rank, size);
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::label_reply_coarse_id_out_of_domain,
      2};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        static_cast<void>(
            parallel_contraction_test_access::compute_label_mapping(
                MPI_COMM_WORLD, graph));
      },
      "label reply validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("quotient edges use one dense keyed exchange and aggregate exactly",
          "[unit][mpi][contraction][quotient-edges]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  local_edges[hashed_edge{coarse_nodes, 0, 2}].weight +=
      4 * static_cast<NodeWeight>(rank + 1);
  if (rank % 2 == 0) {
    local_edges[hashed_edge{coarse_nodes, 1, 2}].weight += 8;
  }
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  protocol_probe::reset();
  protocol_probe::active = true;
  parallel_contraction_test_access::redistribute_quotient(
      MPI_COMM_WORLD,
      local_edges,
      no_node_weights,
      coarse_nodes,
      quotient);
  protocol_probe::active = false;

  auto actual = std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{};
  forall_local_nodes(quotient, node) {
    auto const source = quotient.getGlobalID(node);
    forall_out_edges(quotient, edge, node) {
      actual.emplace_back(source,
                          quotient.getGlobalID(quotient.getEdgeTarget(edge)),
                          quotient.getEdgeWeight(edge));
    } endfor
  } endfor
  auto const expected_legacy_order = rank == 0
      ? std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{
            {0, 2, static_cast<EdgeWeight>(size * (size + 1))},
            {1, 2, static_cast<EdgeWeight>(4 * ((size + 1) / 2))}}
      : rank == 1
            ? std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{
                  {2, 0, static_cast<EdgeWeight>(size * (size + 1))},
                  {2, 1, static_cast<EdgeWeight>(4 * ((size + 1) / 2))}}
            : std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{};
  if (size == 3) {
    REQUIRE(actual == expected_legacy_order);
  }
  std::ranges::sort(actual);

  auto const ownership = mpi::contiguous_owner_layout<NodeID>{
      coarse_nodes, static_cast<std::size_t>(size)};
  auto const first = ownership.begin(static_cast<std::size_t>(rank));
  auto const end = ownership.end(static_cast<std::size_t>(rank));
  auto expected = std::vector<std::tuple<NodeID, NodeID, EdgeWeight>>{};
  auto const edge_0_2_weight = static_cast<EdgeWeight>(
      size * (size + 1));
  auto const even_contributors = static_cast<EdgeWeight>((size + 1) / 2);
  auto const edge_1_2_weight = EdgeWeight{4} * even_contributors;
  for (auto source = first; source < end; ++source) {
    if (source == 0) {
      expected.emplace_back(0, 2, edge_0_2_weight);
    } else if (source == 1) {
      expected.emplace_back(1, 2, edge_1_2_weight);
    } else if (source == 2) {
      expected.emplace_back(2, 0, edge_0_2_weight);
      expected.emplace_back(2, 1, edge_1_2_weight);
    }
  }
  std::ranges::sort(expected);
  REQUIRE(actual == expected);

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_tags,
          protocol_probe::probe_tags,
          protocol_probe::recv_tags,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::payload_calls_with_extent(
              static_cast<MPI_Aint>(4 * sizeof(NodeID))) == 1);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::isend_tags, 7, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::probe_tags, 7, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::recv_tags, 7, size) == 0);
}

TEST_CASE("quotient node weights use one dense keyed exchange and sum exactly",
          "[unit][mpi][contraction][quotient-node-weights]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> local_weights;
  local_weights[0] = static_cast<NodeWeight>(rank + 1);
  local_weights[2] = 2 * static_cast<NodeWeight>(rank + 1);
  if (rank % 2 == 0) {
    local_weights[1] = 5;
  }
  parallel_graph_access quotient{MPI_COMM_WORLD};

  protocol_probe::reset();
  protocol_probe::active = true;
  parallel_contraction_test_access::redistribute_quotient(
      MPI_COMM_WORLD,
      no_edges,
      local_weights,
      coarse_nodes,
      quotient);
  protocol_probe::active = false;

  auto const triangular = static_cast<NodeWeight>(size * (size + 1) / 2);
  auto const expected_weights = std::array<NodeWeight, 4>{
      triangular,
      static_cast<NodeWeight>(5 * ((size + 1) / 2)),
      2 * triangular,
      0};
  forall_local_nodes(quotient, node) {
    auto const global = quotient.getGlobalID(node);
    REQUIRE(quotient.getNodeWeight(node) ==
            expected_weights.at(static_cast<std::size_t>(global)));
  } endfor

  CAPTURE(protocol_probe::all_to_all_v_calls,
          protocol_probe::all_to_all_v_c_calls,
          protocol_probe::isend_tags,
          protocol_probe::probe_tags,
          protocol_probe::recv_tags,
          protocol_probe::payload_extents);
  REQUIRE(protocol_probe::payload_calls_with_extent(
              static_cast<MPI_Aint>(2 * sizeof(NodeID))) == 1);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::isend_tags, 8, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::probe_tags, 8, size) == 0);
  REQUIRE(protocol_probe::calls_in_tag_phase(
              protocol_probe::recv_tags, 8, size) == 0);
}

TEST_CASE("quotient edge receive validation rejects a valid wrong-owner source collectively",
          "[unit][mpi][contraction][quotient-edges][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  local_edges[hashed_edge{coarse_nodes, 0, 2}].weight = 4;
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::quotient_edge_wrong_owner, 1};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge received validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("quotient edge receive validation rejects an out-of-domain target collectively",
          "[unit][mpi][contraction][quotient-edges][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  local_edges[hashed_edge{coarse_nodes, 0, 2}].weight = 4;
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::quotient_edge_target_out_of_domain,
      1};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge received validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("quotient edge receive validation rejects a sender-sequence gap collectively",
          "[unit][mpi][contraction][quotient-edges][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  local_edges[hashed_edge{coarse_nodes, 0, 2}].weight = 4;
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::quotient_edge_sequence_gap, 1};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge received validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("quotient node-weight receive validation rejects a valid wrong-owner ID collectively",
          "[unit][mpi][contraction][quotient-node-weights][failure][receive]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> local_weights{{0, 1}};
  parallel_graph_access quotient{MPI_COMM_WORLD};
  protocol_probe::reset();
  protocol_probe::scoped_receive_mutation mutation{
      protocol_probe::receive_mutation::quotient_node_weight_wrong_owner, 2};
  protocol_probe::active = true;
  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            no_edges,
            local_weights,
            coarse_nodes,
            quotient);
      },
      "quotient node-weight received validation failed",
      size);
  protocol_probe::active = false;
}

TEST_CASE("zero coarse-node redistribution remains an empty dense exchange",
          "[unit][mpi][contraction][quotient][zero]") {
  int size = 0;
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  protocol_probe::reset();
  protocol_probe::active = true;
  parallel_contraction_test_access::redistribute_quotient(
      MPI_COMM_WORLD, no_edges, no_node_weights, 0, quotient);
  protocol_probe::active = false;

  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
  REQUIRE(quotient.get_from_range() == 0);
  REQUIRE(quotient.get_to_range() == 0);
  REQUIRE(protocol_probe::dense_payload_collective_calls() == 2);
}

TEST_CASE("quotient redistribution rejects an empty-payload coarse-count mismatch collectively",
          "[unit][mpi][contraction][quotient][failure][domain]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            no_edges,
            no_node_weights,
            rank == 0 ? NodeID{2} : NodeID{3},
            quotient);
      },
      "quotient coarse node count agreement failed",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
}

TEST_CASE("quotient redistribution rejects a tail-padding edge source collectively",
          "[unit][mpi][contraction][quotient-edges][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  if (rank == 0) {
    local_edges[hashed_edge{coarse_nodes, coarse_nodes, 0}].weight = 4;
  }
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge local validation",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
}

TEST_CASE("quotient redistribution rejects a tail-padding edge target collectively",
          "[unit][mpi][contraction][quotient-edges][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph local_edges;
  if (rank == 0) {
    local_edges[hashed_edge{coarse_nodes, 0, coarse_nodes}].weight = 4;
  }
  std::unordered_map<NodeID, NodeWeight> no_node_weights;
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            local_edges,
            no_node_weights,
            coarse_nodes,
            quotient);
      },
      "quotient edge local validation",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
}

TEST_CASE("quotient redistribution rejects a tail-padding node weight collectively",
          "[unit][mpi][contraction][quotient-node-weights][failure]") {
  int rank = 0;
  int size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  constexpr auto coarse_nodes = NodeID{4};
  hashed_graph no_edges;
  std::unordered_map<NodeID, NodeWeight> local_weights;
  if (rank == 0) {
    local_weights[coarse_nodes] = 7;
  }
  parallel_graph_access quotient{MPI_COMM_WORLD};

  require_collective_validation_failure(
      [&] {
        parallel_contraction_test_access::redistribute_quotient(
            MPI_COMM_WORLD,
            no_edges,
            local_weights,
            coarse_nodes,
            quotient);
      },
      "quotient node-weight local validation",
      size);
  REQUIRE(quotient.number_of_local_nodes() == 0);
  REQUIRE(quotient.number_of_local_edges() == 0);
}

TEST_CASE("all to all vector of vectors", "[unit][mpi]") {
	SECTION("empty cases") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<NodeID>> v_empty(
				static_cast<std::size_t>(size), std::vector<NodeID>{1, 2, 3});
		auto vec = mpi::all_to_all(v_empty, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		REQUIRE(v_empty == vec);
	}
	SECTION("complex case") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		auto v_empty = std::vector<std::vector<unsigned short>>(
				static_cast<std::size_t>(size));
		for (int destination = 0; destination < size; ++destination) {
			v_empty[static_cast<std::size_t>(destination)].assign(
					static_cast<std::size_t>(destination),
					static_cast<unsigned short>(destination));
		}
		auto vec = mpi::all_to_all(v_empty, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		fmt::print("rank: {} -> {}\n", rank, vec);
		REQUIRE(v_empty.size() == vec.size());
	}

	SECTION("custom types") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<contraction::bundled_edge>> empty_edges(
				static_cast<std::size_t>(size),
				std::vector<contraction::bundled_edge>{{0, 0, 0, 0}});
		const std::vector<std::vector<contraction::node_weight_contribution>> empty_weights(
				static_cast<std::size_t>(size),
				std::vector<contraction::node_weight_contribution>{{}});
		const auto empty_meta = empty_weights;
		auto vec_1 = mpi::all_to_all(empty_edges, MPI_COMM_WORLD);
		auto vec_2 = mpi::all_to_all(empty_weights, MPI_COMM_WORLD);
		auto vec_3 = mpi::all_to_all(empty_meta, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		REQUIRE(empty_edges.size() == vec_1.size());
		REQUIRE(empty_weights.size() == vec_2.size());
		REQUIRE(empty_meta.size() == vec_3.size());
	}
}
