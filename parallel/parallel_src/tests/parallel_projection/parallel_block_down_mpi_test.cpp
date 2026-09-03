#include <mpi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_all.hpp>

#include "communication/contiguous_owner_layout.h"
#include "communication/ghost_exchange_plan.h"
#include "communication/mpi_adapter.h"
#include "communication/mpi_trace.h"
#include "data_structure/parallel_graph_access.h"
#include "kahip_mpi_capabilities.h"
#include "parallel_contraction_projection/parallel_block_down_propagation.h"

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
inline bool interposer_error = false;
inline int dense_payload_calls = 0;
inline int dense_payload_c_calls = 0;
inline int topology_create_calls = 0;
inline int neighbor_count_calls = 0;
inline int neighbor_payload_calls = 0;
inline int neighbor_payload_c_calls = 0;
inline int point_to_point_calls = 0;
inline int immediate_neighbor_calls = 0;
inline int persistent_calls = 0;
inline int completion_calls = 0;
inline int barrier_calls = 0;
inline int tag11_isend_calls = 0;
inline int tag11_probe_calls = 0;
inline int tag11_recv_calls = 0;
inline fixed_log<MPI_Aint, 8> dense_payload_extents;
inline fixed_log<MPI_Aint, 8> neighbor_payload_extents;
inline fixed_log<int, 32> neighbor_sources;
inline fixed_log<int, 32> neighbor_destinations;
inline fixed_log<unsigned long long, 32> neighbor_send_counts;
inline bool corruption_fired = false;

enum class receive_mutation {
  none,
  dense_unknown_id,
  dense_block_equal_k,
  dense_conflicting_duplicate,
  dense_duplicate_replacing_missing,
  neighbor_unknown_id,
  neighbor_wrong_source,
  neighbor_duplicate_replacing_missing,
  neighbor_missing_extra,
  neighbor_block_equal_k,
};

inline receive_mutation mutation = receive_mutation::none;
inline int mutation_target_rank = 0;
inline parhip::PartitionID mutation_block_domain = 0;
inline parhip::NodeID mutation_replacement_id = 0;

void reset() noexcept {
  interposer_error = false;
  dense_payload_calls = 0;
  dense_payload_c_calls = 0;
  topology_create_calls = 0;
  neighbor_count_calls = 0;
  neighbor_payload_calls = 0;
  neighbor_payload_c_calls = 0;
  point_to_point_calls = 0;
  immediate_neighbor_calls = 0;
  persistent_calls = 0;
  completion_calls = 0;
  barrier_calls = 0;
  tag11_isend_calls = 0;
  tag11_probe_calls = 0;
  tag11_recv_calls = 0;
  dense_payload_extents.clear();
  neighbor_payload_extents.clear();
  neighbor_sources.clear();
  neighbor_destinations.clear();
  neighbor_send_counts.clear();
  corruption_fired = false;
  mutation = receive_mutation::none;
  mutation_target_rank = 0;
  mutation_block_domain = 0;
  mutation_replacement_id = 0;
}

void record_extent(MPI_Datatype datatype,
                   fixed_log<MPI_Aint, 8>& extents) noexcept {
  auto lower_bound = MPI_Aint{0};
  auto extent = MPI_Aint{0};
  if (PMPI_Type_get_extent(datatype, &lower_bound, &extent) != MPI_SUCCESS ||
      lower_bound != 0) {
    interposer_error = true;
    return;
  }
  extents.push_back(extent);
}

void record_tag11(int tag, MPI_Comm communicator, int& counter) noexcept {
  auto size = 0;
  if (PMPI_Comm_size(communicator, &size) != MPI_SUCCESS || size <= 0) {
    interposer_error = true;
    return;
  }
  if (11 * size <= tag && tag < 12 * size) {
    ++counter;
  }
}

class activation final {
 public:
  explicit activation(receive_mutation selected = receive_mutation::none,
                      int target_rank = 0,
                      parhip::PartitionID block_domain = 0,
                      parhip::NodeID replacement_id = 0) noexcept {
    reset();
    mutation = selected;
    mutation_target_rank = target_rank;
    mutation_block_domain = block_domain;
    mutation_replacement_id = replacement_id;
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};

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
    if (!std::in_range<unsigned long long>(counts[index])) {
      interposer_error = true;
      return;
    }
    neighbor_send_counts.push_back(
        static_cast<unsigned long long>(counts[index]));
  }
}

[[nodiscard]] auto is_dense_mutation() noexcept -> bool {
  return mutation == receive_mutation::dense_unknown_id ||
         mutation == receive_mutation::dense_block_equal_k ||
         mutation == receive_mutation::dense_conflicting_duplicate ||
         mutation == receive_mutation::dense_duplicate_replacing_missing;
}

[[nodiscard]] auto is_neighbor_mutation() noexcept -> bool {
  return mutation == receive_mutation::neighbor_unknown_id ||
         mutation == receive_mutation::neighbor_wrong_source ||
         mutation == receive_mutation::neighbor_duplicate_replacing_missing ||
         mutation == receive_mutation::neighbor_missing_extra ||
         mutation == receive_mutation::neighbor_block_equal_k;
}

template <typename Count, typename Displacement>
void mutate_dense_payload(void* receive_buffer,
                          Count const receive_counts[],
                          Displacement const receive_displacements[],
                          MPI_Datatype datatype,
                          MPI_Comm communicator) noexcept {
  if (!active || !is_dense_mutation()) {
    return;
  }
  auto rank = 0;
  auto size = 0;
  auto lower_bound = MPI_Aint{0};
  auto extent = MPI_Aint{0};
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      PMPI_Comm_size(communicator, &size) != MPI_SUCCESS || size < 0 ||
      PMPI_Type_get_extent(datatype, &lower_bound, &extent) != MPI_SUCCESS ||
      lower_bound != 0 ||
      extent !=
          static_cast<MPI_Aint>(sizeof(parhip::block_down::block_update))) {
    interposer_error = true;
    return;
  }
  if (rank != mutation_target_rank) {
    return;
  }
  if (size != 0 && (receive_buffer == nullptr || receive_counts == nullptr ||
                    receive_displacements == nullptr)) {
    interposer_error = true;
    return;
  }
  auto* records =
      static_cast<parhip::block_down::block_update*>(receive_buffer);
  auto first_source = -1;
  auto first_index = std::size_t{0};
  for (auto source = 0; source < size; ++source) {
    if (receive_counts[source] <= 0 ||
        !std::in_range<std::size_t>(receive_displacements[source])) {
      continue;
    }
    first_source = source;
    first_index = static_cast<std::size_t>(receive_displacements[source]);
    break;
  }
  if (first_source < 0) {
    interposer_error = true;
    return;
  }
  switch (mutation) {
    case receive_mutation::dense_unknown_id:
      records[first_index].coarse_global_id =
          std::numeric_limits<parhip::NodeID>::max();
      corruption_fired = true;
      return;
    case receive_mutation::dense_block_equal_k:
      records[first_index].block = mutation_block_domain;
      corruption_fired = true;
      return;
    case receive_mutation::dense_conflicting_duplicate:
      for (auto source = first_source; source < size; ++source) {
        if (receive_counts[source] <= 0 ||
            !std::in_range<std::size_t>(receive_displacements[source])) {
          continue;
        }
        auto const offset =
            static_cast<std::size_t>(receive_displacements[source]);
        for (auto index = std::size_t{0};
             index < static_cast<std::size_t>(receive_counts[source]);
             ++index) {
          auto& candidate = records[offset + index];
          if (offset + index != first_index &&
              candidate.coarse_global_id ==
                  records[first_index].coarse_global_id) {
            candidate.block = records[first_index].block == 0
                                  ? parhip::PartitionID{1}
                                  : records[first_index].block - 1;
            corruption_fired = true;
            return;
          }
        }
      }
      interposer_error = true;
      return;
    case receive_mutation::dense_duplicate_replacing_missing:
      for (auto source = first_source; source < size; ++source) {
        if (receive_counts[source] <= 0 ||
            !std::in_range<std::size_t>(receive_displacements[source])) {
          continue;
        }
        auto const offset =
            static_cast<std::size_t>(receive_displacements[source]);
        for (auto index = std::size_t{0};
             index < static_cast<std::size_t>(receive_counts[source]);
             ++index) {
          if (offset + index == first_index) {
            continue;
          }
          records[offset + index].coarse_global_id =
              records[first_index].coarse_global_id;
          records[offset + index].block = records[first_index].block;
          corruption_fired = true;
          return;
        }
      }
      interposer_error = true;
      return;
    default:
      interposer_error = true;
      return;
  }
}

template <typename Count, typename Displacement>
void mutate_neighbor_payload(void* receive_buffer,
                             Count const receive_counts[],
                             Displacement const receive_displacements[],
                             MPI_Datatype datatype,
                             MPI_Comm communicator) noexcept {
  if (!active || !is_neighbor_mutation()) {
    return;
  }
  auto rank = 0;
  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  auto lower_bound = MPI_Aint{0};
  auto extent = MPI_Aint{0};
  if (PMPI_Comm_rank(communicator, &rank) != MPI_SUCCESS ||
      PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                      &weighted) != MPI_SUCCESS ||
      indegree < 0 || indegree > 32 ||
      PMPI_Type_get_extent(datatype, &lower_bound, &extent) != MPI_SUCCESS ||
      lower_bound != 0 ||
      extent !=
          static_cast<MPI_Aint>(sizeof(parhip::block_down::block_update)) ||
      (indegree != 0 &&
       (receive_buffer == nullptr || receive_counts == nullptr ||
        receive_displacements == nullptr))) {
    interposer_error = true;
    return;
  }
  if (rank != mutation_target_rank) {
    return;
  }
  auto nonempty = std::array<int, 32>{};
  auto nonempty_count = std::size_t{0};
  for (auto index = 0; index < indegree; ++index) {
    if (receive_counts[index] > 0) {
      nonempty[nonempty_count++] = index;
    }
  }
  if (nonempty_count == 0 ||
      !std::in_range<std::size_t>(receive_displacements[nonempty[0]])) {
    interposer_error = true;
    return;
  }
  auto* records =
      static_cast<parhip::block_down::block_update*>(receive_buffer);
  auto const first = nonempty[0];
  auto const first_offset =
      static_cast<std::size_t>(receive_displacements[first]);
  switch (mutation) {
    case receive_mutation::neighbor_unknown_id:
      records[first_offset].coarse_global_id =
          std::numeric_limits<parhip::NodeID>::max();
      corruption_fired = true;
      return;
    case receive_mutation::neighbor_wrong_source:
      if (nonempty_count < 2 ||
          !std::in_range<std::size_t>(receive_displacements[nonempty[1]])) {
        interposer_error = true;
        return;
      }
      records[first_offset] =
          records[static_cast<std::size_t>(receive_displacements[nonempty[1]])];
      corruption_fired = true;
      return;
    case receive_mutation::neighbor_duplicate_replacing_missing:
      if (receive_counts[first] < 2) {
        interposer_error = true;
        return;
      }
      records[first_offset + 1] = records[first_offset];
      corruption_fired = true;
      return;
    case receive_mutation::neighbor_missing_extra:
      records[first_offset].coarse_global_id = mutation_replacement_id;
      corruption_fired = true;
      return;
    case receive_mutation::neighbor_block_equal_k:
      records[first_offset].block = mutation_block_domain;
      corruption_fired = true;
      return;
    default:
      interposer_error = true;
      return;
  }
}
}  // namespace protocol_probe

extern "C" int MPI_Alltoallv(void const* send_buffer,
                             int const send_counts[],
                             int const send_displacements[],
                             MPI_Datatype send_datatype,
                             void* receive_buffer,
                             int const receive_counts[],
                             int const receive_displacements[],
                             MPI_Datatype receive_datatype,
                             MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::dense_payload_calls;
    protocol_probe::record_extent(send_datatype,
                                  protocol_probe::dense_payload_extents);
  }
  auto const result =
      PMPI_Alltoallv(send_buffer, send_counts, send_displacements,
                     send_datatype, receive_buffer, receive_counts,
                     receive_displacements, receive_datatype, communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_dense_payload(receive_buffer, receive_counts,
                                         receive_displacements,
                                         receive_datatype, communicator);
  }
  return result;
}

#if KAHIP_HAVE_MPI_ALLTOALLV_C
extern "C" int MPI_Alltoallv_c(void const* send_buffer,
                               MPI_Count const send_counts[],
                               MPI_Aint const send_displacements[],
                               MPI_Datatype send_datatype,
                               void* receive_buffer,
                               MPI_Count const receive_counts[],
                               MPI_Aint const receive_displacements[],
                               MPI_Datatype receive_datatype,
                               MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::dense_payload_c_calls;
    protocol_probe::record_extent(send_datatype,
                                  protocol_probe::dense_payload_extents);
  }
  auto const result =
      PMPI_Alltoallv_c(send_buffer, send_counts, send_displacements,
                       send_datatype, receive_buffer, receive_counts,
                       receive_displacements, receive_datatype, communicator);
  if (protocol_probe::active && result == MPI_SUCCESS) {
    protocol_probe::mutate_dense_payload(receive_buffer, receive_counts,
                                         receive_displacements,
                                         receive_datatype, communicator);
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
    if (reorder != 0) {
      protocol_probe::interposer_error = true;
    }
  }
  return PMPI_Dist_graph_create(communicator, source_count, sources, degrees,
                                destinations, weights, info, reorder,
                                graph_communicator);
}

extern "C" int MPI_Neighbor_alltoall(void const* send_buffer,
                                     int send_count,
                                     MPI_Datatype send_datatype,
                                     void* receive_buffer,
                                     int receive_count,
                                     MPI_Datatype receive_datatype,
                                     MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::neighbor_count_calls;
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
    protocol_probe::record_extent(send_datatype,
                                  protocol_probe::neighbor_payload_extents);
    protocol_probe::record_graph_neighbors(communicator);
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
    protocol_probe::record_extent(send_datatype,
                                  protocol_probe::neighbor_payload_extents);
    protocol_probe::record_graph_neighbors(communicator);
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

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (protocol_probe::active) {
    ++protocol_probe::point_to_point_calls;
    protocol_probe::record_tag11(tag, communicator,
                                 protocol_probe::tag11_isend_calls);
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Probe(int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Status* status) {
  if (protocol_probe::active) {
    ++protocol_probe::point_to_point_calls;
    protocol_probe::record_tag11(tag, communicator,
                                 protocol_probe::tag11_probe_calls);
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
    protocol_probe::record_tag11(tag, communicator,
                                 protocol_probe::tag11_recv_calls);
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

#define KAHIP_BLOCK_DOWN_P2P_WRAPPER(name, signature, arguments) \
  extern "C" int name signature {                                \
    if (protocol_probe::active) {                                \
      ++protocol_probe::point_to_point_calls;                    \
    }                                                            \
    return P##name arguments;                                    \
  }

KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Send,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Ssend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Bsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Rsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator),
    (buffer, count, datatype, destination, tag, communicator))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Issend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Ibsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Irsend,
    (void const* buffer,
     int count,
     MPI_Datatype datatype,
     int destination,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, destination, tag, communicator, request))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Irecv,
    (void* buffer,
     int count,
     MPI_Datatype datatype,
     int source,
     int tag,
     MPI_Comm communicator,
     MPI_Request* request),
    (buffer, count, datatype, source, tag, communicator, request))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(
    MPI_Iprobe,
    (int source, int tag, MPI_Comm communicator, int* flag, MPI_Status* status),
    (source, tag, communicator, flag, status))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(MPI_Sendrecv,
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
KAHIP_BLOCK_DOWN_P2P_WRAPPER(MPI_Sendrecv_replace,
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
KAHIP_BLOCK_DOWN_P2P_WRAPPER(MPI_Mprobe,
                             (int source,
                              int tag,
                              MPI_Comm communicator,
                              MPI_Message* message,
                              MPI_Status* status),
                             (source, tag, communicator, message, status))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(MPI_Improbe,
                             (int source,
                              int tag,
                              MPI_Comm communicator,
                              int* flag,
                              MPI_Message* message,
                              MPI_Status* status),
                             (source, tag, communicator, flag, message, status))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(MPI_Mrecv,
                             (void* buffer,
                              int count,
                              MPI_Datatype datatype,
                              MPI_Message* message,
                              MPI_Status* status),
                             (buffer, count, datatype, message, status))
KAHIP_BLOCK_DOWN_P2P_WRAPPER(MPI_Imrecv,
                             (void* buffer,
                              int count,
                              MPI_Datatype datatype,
                              MPI_Message* message,
                              MPI_Request* request),
                             (buffer, count, datatype, message, request))

#undef KAHIP_BLOCK_DOWN_P2P_WRAPPER

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

#define KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(name, signature, arguments) \
  extern "C" int name signature {                                       \
    if (protocol_probe::active) {                                       \
      ++protocol_probe::completion_calls;                               \
    }                                                                   \
    return P##name arguments;                                           \
  }

KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(MPI_Test,
                                    (MPI_Request * request,
                                     int* complete,
                                     MPI_Status* status),
                                    (request, complete, status))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(MPI_Wait,
                                    (MPI_Request * request, MPI_Status* status),
                                    (request, status))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(MPI_Waitall,
                                    (int count,
                                     MPI_Request requests[],
                                     MPI_Status statuses[]),
                                    (count, requests, statuses))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(
    MPI_Testall,
    (int count, MPI_Request requests[], int* complete, MPI_Status statuses[]),
    (count, requests, complete, statuses))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(MPI_Testany,
                                    (int count,
                                     MPI_Request requests[],
                                     int* index,
                                     int* complete,
                                     MPI_Status* status),
                                    (count, requests, index, complete, status))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(
    MPI_Testsome,
    (int count,
     MPI_Request requests[],
     int* completed,
     int indices[],
     MPI_Status statuses[]),
    (count, requests, completed, indices, statuses))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(
    MPI_Waitany,
    (int count, MPI_Request requests[], int* index, MPI_Status* status),
    (count, requests, index, status))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(
    MPI_Waitsome,
    (int count,
     MPI_Request requests[],
     int* completed,
     int indices[],
     MPI_Status statuses[]),
    (count, requests, completed, indices, statuses))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(MPI_Request_free,
                                    (MPI_Request * request),
                                    (request))
KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER(MPI_Cancel,
                                    (MPI_Request * request),
                                    (request))

#undef KAHIP_BLOCK_DOWN_COMPLETION_WRAPPER

extern "C" int MPI_Barrier(MPI_Comm communicator) {
  if (protocol_probe::active) {
    ++protocol_probe::barrier_calls;
  }
  return PMPI_Barrier(communicator);
}

namespace {
void require_common(bool local_condition) {
  auto const local = local_condition ? 1 : 0;
  auto common = 0;
  REQUIRE(PMPI_Allreduce(&local, &common, 1, MPI_INT, MPI_MIN,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(common == 1);
}

void build_cross_rank_coarse(parhip::parallel_graph_access& graph, int rank) {
  constexpr auto global_nodes = parhip::NodeID{2};
  constexpr auto global_edges = parhip::EdgeID{4};
  graph.start_construction(1, 2, global_nodes, global_edges, false);
  graph.set_range(static_cast<parhip::NodeID>(rank),
                  static_cast<parhip::NodeID>(rank));
  auto ranges = std::vector<parhip::NodeID>{0, 1, 2};
  graph.set_range_array(ranges);
  auto const local = graph.new_node();
  graph.setNodeWeight(local, 1);
  graph.setNodeLabel(local, static_cast<parhip::NodeID>(rank));
  graph.setSecondPartitionIndex(local, 2);
  for (auto duplicate = 0; duplicate < 2; ++duplicate) {
    auto const edge =
        graph.new_edge(local, static_cast<parhip::NodeID>(1 - rank));
    graph.setEdgeWeight(edge, 1);
  }
  graph.finish_construction();
  auto const storage_size = graph.number_of_local_nodes() + parhip::NodeID{1} +
                            graph.number_of_ghost_nodes();
  for (auto local_id = parhip::NodeID{0}; local_id < storage_size; ++local_id) {
    graph.setSecondPartitionIndex(
        local_id,
        parhip::PartitionID{2} + static_cast<parhip::PartitionID>(local_id));
  }
}

void build_cross_rank_finer(parhip::parallel_graph_access& graph, int rank) {
  graph.start_construction(2, 0, 4, 0, false);
  auto const first = static_cast<parhip::NodeID>(2 * rank);
  graph.set_range(first, first + parhip::NodeID{1});
  auto ranges = std::vector<parhip::NodeID>{0, 2, 4};
  graph.set_range_array(ranges);
  for (auto coarse = parhip::NodeID{0}; coarse < parhip::NodeID{2}; ++coarse) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, first + local);
    graph.setSecondPartitionIndex(
        local,
        parhip::PartitionID{10} + static_cast<parhip::PartitionID>(coarse));
  }
  graph.finish_construction();
  graph.allocate_node_to_cnode();
  graph.setCNode(0, 0);
  graph.setCNode(1, 1);
}

[[nodiscard]] auto expected_trace(int rank) -> std::string {
  auto text = std::string{
      "kahip-mpi-trace-v3 upstream="
      "5935f349f65f1788a9b68fcf6d853e698d86956d\n"};
  for (auto global = 0; global < 2; ++global) {
    text +=
        "block-propagation cycle=3 level=2 epoch=contraction iteration=0 "
        "round=0 global=" +
        std::to_string(global) + " owner=" + std::to_string(global) +
        " requester=- receiver=" + std::to_string(rank) +
        " key=block block=" + std::to_string(10 + global) + "\n";
  }
  return text;
}

struct distributed_fixture {
  std::vector<std::vector<parhip::NodeID>> adjacency;
};

[[nodiscard]] auto fixture_for_size(int size) -> distributed_fixture {
  switch (size) {
    case 1:
      return {{{}}};
    case 2:
      return {{{1, 1}, {0, 0}}};
    case 3:
      return {{{2, 3}, {2, 3}, {0, 1}, {0, 1}}};
    case 4:
      return {{{1, 3}, {0, 2}, {1, 3}, {0, 2}}};
    case 5:
      return {{{1}, {0, 2}, {1, 3}, {2}, {}}};
    default:
      throw std::invalid_argument{"block-down fixture requires ranks 1-5"};
  }
}

[[nodiscard]] auto block_for(parhip::NodeID global_id,
                             parhip::PartitionID epoch = 0)
    -> parhip::PartitionID {
  constexpr auto domain = parhip::PartitionID{14};
  return (parhip::PartitionID{10} +
          static_cast<parhip::PartitionID>(global_id) + epoch) %
         domain;
}

[[nodiscard]] auto fixture_ranges(parhip::NodeID global_nodes, int size)
    -> std::vector<parhip::NodeID> {
  auto const ownership = parhip::mpi::contiguous_owner_layout<parhip::NodeID>{
      global_nodes, static_cast<std::size_t>(size)};
  auto ranges = std::vector<parhip::NodeID>(static_cast<std::size_t>(size) +
                                            std::size_t{1});
  for (auto rank = std::size_t{0}; rank < ranges.size(); ++rank) {
    ranges[rank] = ownership.boundary(rank);
  }
  return ranges;
}

void build_coarse_fixture(
    parhip::parallel_graph_access& graph,
    distributed_fixture const& fixture,
    int rank,
    int size,
    std::optional<parhip::NodeID> global_override = std::nullopt) {
  auto const global_nodes =
      static_cast<parhip::NodeID>(fixture.adjacency.size());
  auto const ranges = fixture_ranges(global_nodes, size);
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  auto local_edges = std::size_t{0};
  for (auto global = first; global < end; ++global) {
    local_edges += fixture.adjacency[static_cast<std::size_t>(global)].size();
  }
  auto global_edges = std::size_t{0};
  for (auto const& adjacency : fixture.adjacency) {
    global_edges += adjacency.size();
  }
  graph.start_construction(end - first,
                           static_cast<parhip::EdgeID>(local_edges),
                           global_override.value_or(global_nodes),
                           static_cast<parhip::EdgeID>(global_edges), false);
  graph.set_range(first, first == end ? first : end - parhip::NodeID{1});
  auto mutable_ranges = ranges;
  graph.set_range_array(mutable_ranges);
  for (auto global = first; global < end; ++global) {
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, global);
    graph.setSecondPartitionIndex(local, parhip::PartitionID{2} + local);
    for (auto const target :
         fixture.adjacency[static_cast<std::size_t>(global)]) {
      auto const edge = graph.new_edge(local, target);
      graph.setEdgeWeight(edge, 1);
    }
  }
  graph.finish_construction();
  auto const storage_size = graph.number_of_local_nodes() + parhip::NodeID{1} +
                            graph.number_of_ghost_nodes();
  for (auto local = parhip::NodeID{0}; local < storage_size; ++local) {
    graph.setSecondPartitionIndex(
        local,
        parhip::PartitionID{2} + static_cast<parhip::PartitionID>(local));
  }
}

void build_finer_fixture(parhip::parallel_graph_access& graph,
                         parhip::NodeID coarse_nodes,
                         int rank,
                         int size,
                         parhip::PartitionID epoch = 0) {
  auto const ranges = fixture_ranges(coarse_nodes, size);
  auto const first = ranges[static_cast<std::size_t>(rank)];
  auto const end = ranges[static_cast<std::size_t>(rank + 1)];
  graph.start_construction(end - first, 0, coarse_nodes, 0, false);
  graph.set_range(first, first == end ? first : end - parhip::NodeID{1});
  auto mutable_ranges = ranges;
  graph.set_range_array(mutable_ranges);
  for (auto global = first; global < end; ++global) {
    auto const coarse = coarse_nodes == 0
                            ? parhip::NodeID{0}
                            : (global + parhip::NodeID{1}) % coarse_nodes;
    auto const local = graph.new_node();
    graph.setNodeWeight(local, 1);
    graph.setNodeLabel(local, global);
    graph.setSecondPartitionIndex(local, block_for(coarse, epoch));
  }
  graph.finish_construction();
  graph.allocate_node_to_cnode();
  for (auto global = first; global < end; ++global) {
    auto const coarse = coarse_nodes == 0
                            ? parhip::NodeID{0}
                            : (global + parhip::NodeID{1}) % coarse_nodes;
    graph.setCNode(global - first, coarse);
  }
}

[[nodiscard]] auto snapshot_blocks(parhip::parallel_graph_access& graph)
    -> std::vector<parhip::PartitionID> {
  auto values = std::vector<parhip::PartitionID>{};
  auto const storage_size = graph.number_of_local_nodes() + parhip::NodeID{1} +
                            graph.number_of_ghost_nodes();
  values.reserve(static_cast<std::size_t>(storage_size));
  for (auto local = parhip::NodeID{0}; local < storage_size; ++local) {
    values.push_back(
        static_cast<parhip::PartitionID>(graph.getSecondPartitionIndex(local)));
  }
  return values;
}

[[nodiscard]] auto blocks_match(parhip::parallel_graph_access& graph,
                                parhip::PartitionID epoch = 0) -> bool {
  auto valid = true;
  for (auto local = parhip::NodeID{0}; local < graph.number_of_local_nodes();
       ++local) {
    valid = valid && graph.getSecondPartitionIndex(local) ==
                         block_for(graph.getGlobalID(local), epoch);
  }
  auto const sentinel = graph.number_of_local_nodes();
  valid = valid && graph.getSecondPartitionIndex(sentinel) ==
                       parhip::PartitionID{2} +
                           static_cast<parhip::PartitionID>(sentinel);
  for (auto local = graph.number_of_local_nodes() + parhip::NodeID{1};
       local < graph.number_of_local_nodes() + parhip::NodeID{1} +
                   graph.number_of_ghost_nodes();
       ++local) {
    valid = valid && graph.getSecondPartitionIndex(local) ==
                         block_for(graph.getGlobalID(local), epoch);
  }
  return valid;
}

[[nodiscard]] auto exact_protocol(parhip::parallel_graph_access& graph,
                                  int topology_creations) -> bool {
  auto const& plan = graph.ghost_plan();
#if KAHIP_HAVE_MPI_ALLTOALLV_C
  auto const dense_path_is_exact = protocol_probe::dense_payload_calls == 0 &&
                                   protocol_probe::dense_payload_c_calls == 1;
#else
  auto const dense_path_is_exact = protocol_probe::dense_payload_calls == 1 &&
                                   protocol_probe::dense_payload_c_calls == 0;
#endif
#if KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
  auto const neighbor_path_is_exact =
      protocol_probe::neighbor_payload_calls == 0 &&
      protocol_probe::neighbor_payload_c_calls == 1;
#else
  auto const neighbor_path_is_exact =
      protocol_probe::neighbor_payload_calls == 1 &&
      protocol_probe::neighbor_payload_c_calls == 0;
#endif
  auto valid =
      !protocol_probe::interposer_error &&
      !protocol_probe::dense_payload_extents.overflowed() &&
      !protocol_probe::neighbor_payload_extents.overflowed() &&
      !protocol_probe::neighbor_sources.overflowed() &&
      !protocol_probe::neighbor_destinations.overflowed() &&
      !protocol_probe::neighbor_send_counts.overflowed() &&
      protocol_probe::dense_payload_extents.size() == 1 &&
      *protocol_probe::dense_payload_extents.begin() ==
          static_cast<MPI_Aint>(sizeof(parhip::block_down::block_update)) &&
      protocol_probe::neighbor_payload_extents.size() == 1 &&
      *protocol_probe::neighbor_payload_extents.begin() ==
          static_cast<MPI_Aint>(sizeof(parhip::block_down::block_update)) &&
      dense_path_is_exact && neighbor_path_is_exact &&
      protocol_probe::topology_create_calls == topology_creations &&
      protocol_probe::neighbor_count_calls == 1 &&
      protocol_probe::point_to_point_calls == 0 &&
      protocol_probe::immediate_neighbor_calls == 0 &&
      protocol_probe::persistent_calls == 0 &&
      protocol_probe::completion_calls == 0 &&
      protocol_probe::barrier_calls == 0 &&
      protocol_probe::tag11_isend_calls == 0 &&
      protocol_probe::tag11_probe_calls == 0 &&
      protocol_probe::tag11_recv_calls == 0 &&
      std::ranges::equal(protocol_probe::neighbor_sources,
                         plan.topology().sources()) &&
      std::ranges::equal(protocol_probe::neighbor_destinations,
                         plan.topology().destinations()) &&
      protocol_probe::neighbor_send_counts.size() ==
          plan.topology().destinations().size();
  if (protocol_probe::neighbor_send_counts.size() ==
      plan.topology().destinations().size()) {
    for (auto index = std::size_t{0};
         index < plan.topology().destinations().size(); ++index) {
      valid = valid && *std::next(protocol_probe::neighbor_send_counts.begin(),
                                  static_cast<std::ptrdiff_t>(index)) ==
                           plan.outgoing_local_nodes(index).size();
    }
  }
  return valid;
}

template <typename Operation>
void require_collective_failure(Operation&& operation,
                                std::string_view expected_context,
                                int size) {
  auto caught = 0;
  auto structured = 0;
  auto context_matches = 0;
  try {
    std::forward<Operation>(operation)();
  } catch (parhip::mpi::mpi_error const& error) {
    caught = 1;
    structured = error.error_code() == MPI_ERR_ARG ? 1 : 0;
    context_matches = std::string_view{error.what()}.find(expected_context) !=
                              std::string_view::npos
                          ? 1
                          : 0;
  } catch (...) {
    caught = 1;
  }
  auto caught_total = 0;
  auto structured_total = 0;
  auto context_total = 0;
  REQUIRE(PMPI_Allreduce(&caught, &caught_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(PMPI_Allreduce(&structured, &structured_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(PMPI_Allreduce(&context_matches, &context_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  REQUIRE(caught_total == size);
  REQUIRE(structured_total == size);
  REQUIRE(context_total == size);
}

class trace_session final {
 public:
  trace_session() {
    parhip::mpi::trace::reset();
    parhip::mpi::trace::set_active(true);
  }
  ~trace_session() {
    parhip::mpi::trace::set_active(false);
    parhip::mpi::trace::reset();
  }

  trace_session(trace_session const&) = delete;
  auto operator=(trace_session const&) -> trace_session& = delete;
};
}  // namespace

TEST_CASE(
    "block-down uses one typed dense and one blocking neighborhood transaction",
    "[mpi][block-down][protocol][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_cross_rank_finer(finer, rank);
  build_cross_rank_coarse(coarser, rank);
  auto config = parhip::PPartitionConfig{};
  config.k = 14;

  auto trace = trace_session{};
  KAHIP_MPI_TRACE_SET_HIERARCHY(3, 2, parhip::mpi::trace::epoch::contraction);
  auto probe = protocol_probe::activation{};
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);

  auto const ghost = coarser.number_of_local_nodes() + parhip::NodeID{1};
#if KAHIP_ENABLE_MPI_TRACE
  auto const trace_is_exact =
      parhip::mpi::trace::canonical_text(parhip::mpi::trace::snapshot()) ==
      expected_trace(rank);
#else
  auto const trace_is_exact = parhip::mpi::trace::snapshot().empty();
#endif
  auto const state_and_trace_are_exact =
      coarser.getSecondPartitionIndex(0) ==
          parhip::PartitionID{10} + static_cast<parhip::PartitionID>(rank) &&
      coarser.getSecondPartitionIndex(ghost) ==
          parhip::PartitionID{10} +
              static_cast<parhip::PartitionID>(1 - rank) &&
      trace_is_exact;
  require_common(state_and_trace_are_exact);

  auto const dense_calls = protocol_probe::dense_payload_calls +
                           protocol_probe::dense_payload_c_calls;
  auto const neighbor_calls = protocol_probe::neighbor_payload_calls +
                              protocol_probe::neighbor_payload_c_calls;
  CAPTURE(rank, dense_calls, protocol_probe::topology_create_calls,
          protocol_probe::neighbor_count_calls, neighbor_calls,
          protocol_probe::point_to_point_calls,
          protocol_probe::tag11_isend_calls, protocol_probe::tag11_probe_calls,
          protocol_probe::tag11_recv_calls);
  require_common(exact_protocol(coarser, 1));
}

TEST_CASE("block-down wire datatype has exact semantic extent",
          "[unit][mpi][block-down][datatype]") {
  STATIC_REQUIRE(std::is_standard_layout_v<parhip::block_down::block_update>);
  STATIC_REQUIRE(
      std::is_trivially_copyable_v<parhip::block_down::block_update>);
  auto datatype =
      parhip::mpi::make_mpi_datatype<parhip::block_down::block_update>();
  auto lower_bound = MPI_Aint{0};
  auto extent = MPI_Aint{0};
  REQUIRE(MPI_Type_get_extent(datatype.native_handle(), &lower_bound,
                              &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  REQUIRE(extent ==
          static_cast<MPI_Aint>(sizeof(parhip::block_down::block_update)));
}

TEST_CASE("block-down covers distributed rank-one through rank-five shapes",
          "[mpi][block-down][matrix][protocol]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size >= 1);
  REQUIRE(size <= 5);

  auto const fixture = fixture_for_size(size);
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_finer_fixture(
      finer, static_cast<parhip::NodeID>(fixture.adjacency.size()), rank, size);
  build_coarse_fixture(coarser, fixture, rank, size);
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto trace = trace_session{};
  KAHIP_MPI_TRACE_SET_HIERARCHY(4, 1, parhip::mpi::trace::epoch::contraction);
  auto probe = protocol_probe::activation{};
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);

  auto local_is_exact = blocks_match(coarser) && exact_protocol(coarser, 1);
#if KAHIP_ENABLE_MPI_TRACE
  local_is_exact = local_is_exact && parhip::mpi::trace::snapshot().size() ==
                                         static_cast<std::size_t>(
                                             coarser.number_of_local_nodes() +
                                             coarser.number_of_ghost_nodes());
#else
  local_is_exact = local_is_exact && parhip::mpi::trace::snapshot().empty();
#endif
  require_common(local_is_exact);
}

TEST_CASE("globally empty block-down participates without sentinels",
          "[mpi][block-down][empty][protocol]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 1) {
    return;
  }

  auto const fixture = distributed_fixture{{}};
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_finer_fixture(finer, 0, rank, size);
  build_coarse_fixture(coarser, fixture, rank, size);
  auto config = parhip::PPartitionConfig{};
  config.k = 1;
  auto trace = trace_session{};
  auto probe = protocol_probe::activation{};
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);
  require_common(blocks_match(coarser) && exact_protocol(coarser, 1) &&
                 parhip::mpi::trace::snapshot().empty());
}

TEST_CASE("block-down reuses a warm topology and refreshes staged blocks",
          "[mpi][block-down][reuse][protocol]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  auto const fixture = fixture_for_size(size);
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_finer_fixture(finer, 2, rank, size);
  build_coarse_fixture(coarser, fixture, rank, size);
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto trace = trace_session{};
  auto probe = protocol_probe::activation{};
  static_cast<void>(coarser.ghost_plan());
  require_common(protocol_probe::topology_create_calls == 1);

  protocol_probe::reset();
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);
  require_common(blocks_match(coarser) && exact_protocol(coarser, 0));

  for (auto local = parhip::NodeID{0}; local < finer.number_of_local_nodes();
       ++local) {
    finer.setSecondPartitionIndex(local, block_for(finer.getCNode(local), 3));
  }
  protocol_probe::reset();
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);
  require_common(blocks_match(coarser, 3) && exact_protocol(coarser, 0));
}

TEST_CASE("dense block-down receive failures preserve state and retry",
          "[mpi][block-down][dense][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  constexpr auto modes =
      std::array{protocol_probe::receive_mutation::dense_unknown_id,
                 protocol_probe::receive_mutation::dense_block_equal_k,
                 protocol_probe::receive_mutation::dense_conflicting_duplicate};
  for (auto const mode : modes) {
    auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
    auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
    build_cross_rank_finer(finer, rank);
    build_cross_rank_coarse(coarser, rank);
    auto config = parhip::PPartitionConfig{};
    config.k = 14;
    auto const before_blocks = snapshot_blocks(coarser);
    auto trace = trace_session{};
#if KAHIP_ENABLE_MPI_TRACE
    parhip::mpi::trace::append(parhip::mpi::trace::block_propagation(
        parhip::mpi::trace::current_hierarchy(), 777, rank, rank, 3));
#endif
    auto const before_trace = parhip::mpi::trace::snapshot();
    auto probe = protocol_probe::activation{mode, 0, 14};
    require_collective_failure(
        [&] {
          parhip::parallel_block_down_propagation{}.propagate_block_down(
              MPI_COMM_WORLD, config, finer, coarser);
        },
        "block-down dense received validation failed", size);
    auto const fired = protocol_probe::corruption_fired ? 1 : 0;
    auto fired_total = 0;
    REQUIRE(PMPI_Allreduce(&fired, &fired_total, 1, MPI_INT, MPI_SUM,
                           MPI_COMM_WORLD) == MPI_SUCCESS);
    require_common(fired_total == 1 && !protocol_probe::interposer_error &&
                   snapshot_blocks(coarser) == before_blocks &&
                   parhip::mpi::trace::snapshot() == before_trace &&
                   protocol_probe::topology_create_calls == 1 &&
                   protocol_probe::dense_payload_calls +
                           protocol_probe::dense_payload_c_calls ==
                       1 &&
                   protocol_probe::neighbor_count_calls == 0 &&
                   protocol_probe::neighbor_payload_calls +
                           protocol_probe::neighbor_payload_c_calls ==
                       0 &&
                   protocol_probe::point_to_point_calls == 0);

    protocol_probe::reset();
    parhip::parallel_block_down_propagation{}.propagate_block_down(
        MPI_COMM_WORLD, config, finer, coarser);
    require_common(blocks_match(coarser) && exact_protocol(coarser, 0));
  }
}

TEST_CASE("dense block-down exact coverage rejects a replaced owner record",
          "[mpi][block-down][dense][coverage][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }

  auto const fixture = fixture_for_size(size);
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_finer_fixture(finer, 4, rank, size);
  build_coarse_fixture(coarser, fixture, rank, size);
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
#if KAHIP_ENABLE_MPI_TRACE
  parhip::mpi::trace::append(parhip::mpi::trace::block_propagation(
      parhip::mpi::trace::current_hierarchy(), 777, rank, rank, 3));
#endif
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{
      protocol_probe::receive_mutation::dense_duplicate_replacing_missing, 0,
      14};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block-down dense received validation failed", size);
  auto const fired = protocol_probe::corruption_fired ? 1 : 0;
  auto fired_total = 0;
  REQUIRE(PMPI_Allreduce(&fired, &fired_total, 1, MPI_INT, MPI_SUM,
                         MPI_COMM_WORLD) == MPI_SUCCESS);
  auto const blocks_preserved = snapshot_blocks(coarser) == before_blocks;
  auto const trace_preserved = parhip::mpi::trace::snapshot() == before_trace;
  CAPTURE(rank, fired_total, protocol_probe::interposer_error, blocks_preserved,
          trace_preserved, protocol_probe::topology_create_calls,
          protocol_probe::dense_payload_calls,
          protocol_probe::dense_payload_c_calls);
  require_common(fired_total == 1 && !protocol_probe::interposer_error &&
                 blocks_preserved && trace_preserved);
  protocol_probe::reset();
  parhip::parallel_block_down_propagation{}.propagate_block_down(
      MPI_COMM_WORLD, config, finer, coarser);
  require_common(blocks_match(coarser) && exact_protocol(coarser, 0));
}

TEST_CASE("neighborhood block-down failures preserve state and retry",
          "[mpi][block-down][neighbor][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size < 2 || size > 4) {
    return;
  }

  auto modes = std::vector<protocol_probe::receive_mutation>{};
  auto target_rank = 0;
  auto replacement_id = parhip::NodeID{0};
  if (size == 2) {
    modes = {protocol_probe::receive_mutation::neighbor_unknown_id,
             protocol_probe::receive_mutation::neighbor_missing_extra,
             protocol_probe::receive_mutation::neighbor_block_equal_k};
  } else if (size == 3) {
    modes = {
        protocol_probe::receive_mutation::neighbor_duplicate_replacing_missing};
    target_rank = 1;
    replacement_id = 3;
  } else {
    modes = {protocol_probe::receive_mutation::neighbor_wrong_source};
  }

  for (auto const mode : modes) {
    auto const fixture = fixture_for_size(size);
    auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
    auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
    build_finer_fixture(finer,
                        static_cast<parhip::NodeID>(fixture.adjacency.size()),
                        rank, size);
    build_coarse_fixture(coarser, fixture, rank, size);
    auto config = parhip::PPartitionConfig{};
    config.k = 14;
    auto const before_blocks = snapshot_blocks(coarser);
    auto trace = trace_session{};
#if KAHIP_ENABLE_MPI_TRACE
    parhip::mpi::trace::append(parhip::mpi::trace::block_propagation(
        parhip::mpi::trace::current_hierarchy(), 777, rank, rank, 3));
#endif
    auto const before_trace = parhip::mpi::trace::snapshot();
    auto probe =
        protocol_probe::activation{mode, target_rank, 14, replacement_id};
    require_collective_failure(
        [&] {
          parhip::parallel_block_down_propagation{}.propagate_block_down(
              MPI_COMM_WORLD, config, finer, coarser);
        },
        "block-down neighbor received validation failed", size);
    auto const fired = protocol_probe::corruption_fired ? 1 : 0;
    auto fired_total = 0;
    REQUIRE(PMPI_Allreduce(&fired, &fired_total, 1, MPI_INT, MPI_SUM,
                           MPI_COMM_WORLD) == MPI_SUCCESS);
    require_common(fired_total == 1 && !protocol_probe::interposer_error &&
                   snapshot_blocks(coarser) == before_blocks &&
                   parhip::mpi::trace::snapshot() == before_trace &&
                   protocol_probe::topology_create_calls == 1 &&
                   protocol_probe::dense_payload_calls +
                           protocol_probe::dense_payload_c_calls ==
                       1 &&
                   protocol_probe::neighbor_count_calls == 1 &&
                   protocol_probe::neighbor_payload_calls +
                           protocol_probe::neighbor_payload_c_calls ==
                       1 &&
                   protocol_probe::point_to_point_calls == 0 &&
                   protocol_probe::immediate_neighbor_calls == 0 &&
                   protocol_probe::persistent_calls == 0 &&
                   protocol_probe::completion_calls == 0 &&
                   protocol_probe::barrier_calls == 0);

    protocol_probe::reset();
    parhip::parallel_block_down_propagation{}.propagate_block_down(
        MPI_COMM_WORLD, config, finer, coarser);
    require_common(blocks_match(coarser) && exact_protocol(coarser, 0));
  }
}

TEST_CASE("block-down rejects a rank-local block equal to k before topology",
          "[mpi][block-down][domain][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_cross_rank_finer(finer, rank);
  build_cross_rank_coarse(coarser, rank);
  if (rank == 0) {
    finer.setSecondPartitionIndex(0, 14);
  }
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block-down local update validation failed", size);
  require_common(snapshot_blocks(coarser) == before_blocks &&
                 parhip::mpi::trace::snapshot() == before_trace &&
                 protocol_probe::topology_create_calls == 0 &&
                 protocol_probe::dense_payload_calls +
                         protocol_probe::dense_payload_c_calls ==
                     0 &&
                 protocol_probe::neighbor_count_calls == 0 &&
                 protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("block-down rejects rank-skewed k before topology",
          "[mpi][block-down][k][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_cross_rank_finer(finer, rank);
  build_cross_rank_coarse(coarser, rank);
  auto config = parhip::PPartitionConfig{};
  config.k = rank == 0 ? parhip::PartitionID{14} : parhip::PartitionID{13};
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block-down block-count agreement failed", size);
  require_common(snapshot_blocks(coarser) == before_blocks &&
                 parhip::mpi::trace::snapshot() == before_trace &&
                 protocol_probe::topology_create_calls == 0 &&
                 protocol_probe::dense_payload_calls +
                         protocol_probe::dense_payload_c_calls ==
                     0);
}

TEST_CASE("block-down rejects zero k before topology",
          "[mpi][block-down][k][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_cross_rank_finer(finer, rank);
  build_cross_rank_coarse(coarser, rank);
  auto config = parhip::PPartitionConfig{};
  config.k = 0;
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block-down requires a positive block count", size);
  require_common(snapshot_blocks(coarser) == before_blocks &&
                 parhip::mpi::trace::snapshot() == before_trace &&
                 protocol_probe::topology_create_calls == 0 &&
                 protocol_probe::dense_payload_calls +
                         protocol_probe::dense_payload_c_calls ==
                     0);
}

TEST_CASE("block-down accepts congruent and rejects similar communicators",
          "[mpi][block-down][communicator]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }

  SECTION("congruent") {
    auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
    auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
    build_cross_rank_finer(finer, rank);
    build_cross_rank_coarse(coarser, rank);
    auto duplicate = MPI_COMM_NULL;
    REQUIRE(MPI_Comm_dup(MPI_COMM_WORLD, &duplicate) == MPI_SUCCESS);
    auto config = parhip::PPartitionConfig{};
    config.k = 14;
    {
      auto probe = protocol_probe::activation{};
      parhip::parallel_block_down_propagation{}.propagate_block_down(
          duplicate, config, finer, coarser);
      require_common(blocks_match(coarser) && exact_protocol(coarser, 1));
    }
    REQUIRE(MPI_Comm_free(&duplicate) == MPI_SUCCESS);
  }

  SECTION("similar") {
    auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
    auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
    build_cross_rank_finer(finer, rank);
    build_cross_rank_coarse(coarser, rank);
    auto similar = MPI_COMM_NULL;
    REQUIRE(MPI_Comm_split(MPI_COMM_WORLD, 0, size - rank, &similar) ==
            MPI_SUCCESS);
    auto config = parhip::PPartitionConfig{};
    config.k = 14;
    auto const before_blocks = snapshot_blocks(coarser);
    {
      auto probe = protocol_probe::activation{};
      require_collective_failure(
          [&] {
            parhip::parallel_block_down_propagation{}.propagate_block_down(
                similar, config, finer, coarser);
          },
          "block-down communicator validation failed", size);
      require_common(snapshot_blocks(coarser) == before_blocks &&
                     protocol_probe::topology_create_calls == 0 &&
                     protocol_probe::dense_payload_calls +
                             protocol_probe::dense_payload_c_calls ==
                         0);
    }
    REQUIRE(MPI_Comm_free(&similar) == MPI_SUCCESS);
  }
}

TEST_CASE("block-down rejects skewed coarse domains before topology",
          "[mpi][block-down][coarse-domain][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 3) {
    return;
  }
  auto const fixture = fixture_for_size(size);
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_finer_fixture(finer, 4, rank, size);
  build_coarse_fixture(coarser, fixture, rank, size,
                       rank == 0 ? std::optional<parhip::NodeID>{5}
                                 : std::optional<parhip::NodeID>{4});
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block-down coarse-node count agreement failed", size);
  require_common(snapshot_blocks(coarser) == before_blocks &&
                 parhip::mpi::trace::snapshot() == before_trace &&
                 protocol_probe::topology_create_calls == 0 &&
                 protocol_probe::dense_payload_calls +
                         protocol_probe::dense_payload_c_calls ==
                     0);
}

TEST_CASE("block-down rejects skewed ownership metadata before topology",
          "[mpi][block-down][ownership][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_cross_rank_finer(finer, rank);
  build_cross_rank_coarse(coarser, rank);
  if (rank == 0) {
    coarser.get_range_array()[1] = coarser.number_of_global_nodes();
  }
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "block-down quotient ownership metadata validation failed", size);
  require_common(snapshot_blocks(coarser) == before_blocks &&
                 parhip::mpi::trace::snapshot() == before_trace &&
                 protocol_probe::topology_create_calls == 0 &&
                 protocol_probe::dense_payload_calls +
                         protocol_probe::dense_payload_c_calls ==
                     0);
}

TEST_CASE("block-down rejects asymmetric ghost topology before dense payload",
          "[mpi][block-down][asymmetric][failure][transaction]") {
  auto rank = 0;
  auto size = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  if (size != 2) {
    return;
  }
  auto const fixture = distributed_fixture{{{1}, {}}};
  auto finer = parhip::parallel_graph_access{MPI_COMM_WORLD};
  auto coarser = parhip::parallel_graph_access{MPI_COMM_WORLD};
  build_finer_fixture(finer, 2, rank, size);
  build_coarse_fixture(coarser, fixture, rank, size);
  auto config = parhip::PPartitionConfig{};
  config.k = 14;
  auto const before_blocks = snapshot_blocks(coarser);
  auto trace = trace_session{};
  auto const before_trace = parhip::mpi::trace::snapshot();
  auto probe = protocol_probe::activation{};
  require_collective_failure(
      [&] {
        parhip::parallel_block_down_propagation{}.propagate_block_down(
            MPI_COMM_WORLD, config, finer, coarser);
      },
      "ghost exchange plan semantic validation failed", size);
  require_common(snapshot_blocks(coarser) == before_blocks &&
                 parhip::mpi::trace::snapshot() == before_trace &&
                 protocol_probe::topology_create_calls == 1 &&
                 protocol_probe::dense_payload_calls +
                         protocol_probe::dense_payload_c_calls ==
                     0 &&
                 protocol_probe::neighbor_count_calls == 0 &&
                 protocol_probe::point_to_point_calls == 0);
}
