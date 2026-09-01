#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "communication/contiguous_owner_layout.h"
#include "communication/mpi_adapter.h"
#include "communication/mpi_failure.h"
#include "kahip_mpi_capabilities.h"
#include "parhip_interface.h"

namespace test_support {
struct wire_entry {
  std::uint64_t id;
  int owner;
  double weight;

  auto operator==(wire_entry const&) const -> bool = default;
};

struct non_default_wire_entry {
  non_default_wire_entry() = delete;
  constexpr non_default_wire_entry(std::uint64_t entry_id,
                                   int entry_owner) noexcept
      : id(entry_id), owner(entry_owner) {}

  std::uint64_t id;
  int owner;

  auto operator==(non_default_wire_entry const&) const -> bool = default;
};
}  // namespace test_support

template <>
struct parhip::mpi::wire_members<test_support::wire_entry> {
  inline static constexpr auto value =
      boost::hana::make_tuple(&test_support::wire_entry::id,
                              &test_support::wire_entry::owner,
                              &test_support::wire_entry::weight);
};

template <>
struct parhip::mpi::wire_members<test_support::non_default_wire_entry> {
  inline static constexpr auto value =
      boost::hana::make_tuple(&test_support::non_default_wire_entry::id,
                              &test_support::non_default_wire_entry::owner);
};

namespace semantic_error_protocol_probe {
inline bool active = false;
inline int error_string_calls = 0;

class activation final {
 public:
  activation() noexcept {
    error_string_calls = 0;
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace semantic_error_protocol_probe

namespace capacity_protocol_probe {
inline bool active = false;
inline int allreduce_calls = 0;
inline bool signature_matches = true;

class activation final {
 public:
  activation() noexcept {
    allreduce_calls = 0;
    signature_matches = true;
    active = true;
  }
  ~activation() noexcept { active = false; }

  activation(activation const&) = delete;
  auto operator=(activation const&) -> activation& = delete;
};
}  // namespace capacity_protocol_probe

extern "C" int MPI_Error_string(int error_code,
                                char* error_text,
                                int* error_text_length) {
  if (semantic_error_protocol_probe::active) {
    ++semantic_error_protocol_probe::error_string_calls;
  }
  return PMPI_Error_string(error_code, error_text, error_text_length);
}

extern "C" int MPI_Allreduce(void const* send_buffer,
                             void* receive_buffer,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op operation,
                             MPI_Comm communicator) {
  if (capacity_protocol_probe::active) {
    ++capacity_protocol_probe::allreduce_calls;
    capacity_protocol_probe::signature_matches =
        capacity_protocol_probe::signature_matches && send_buffer != nullptr &&
        receive_buffer != nullptr && count == 2 && datatype == MPI_UINT64_T &&
        operation == MPI_BOR;
  }
  return PMPI_Allreduce(send_buffer, receive_buffer, count, datatype, operation,
                        communicator);
}

namespace neighborhood_protocol_probe {
inline bool active = false;
inline int dist_graph_create_calls = 0;
inline int neighbor_count_calls = 0;
inline int neighbor_payload_calls = 0;
inline int neighbor_payload_c_calls = 0;
inline int point_to_point_calls = 0;
inline int maximum_active_send_segments = 0;
inline int maximum_active_receive_segments = 0;
inline int maximum_payload_count = 0;
inline int nonzero_displacement_calls = 0;

void reset() {
  dist_graph_create_calls = 0;
  neighbor_count_calls = 0;
  neighbor_payload_calls = 0;
  neighbor_payload_c_calls = 0;
  point_to_point_calls = 0;
  maximum_active_send_segments = 0;
  maximum_active_receive_segments = 0;
  maximum_payload_count = 0;
  nonzero_displacement_calls = 0;
}
}  // namespace neighborhood_protocol_probe

extern "C" int MPI_Dist_graph_create(MPI_Comm communicator,
                                     int source_count,
                                     int const sources[],
                                     int const degrees[],
                                     int const destinations[],
                                     int const weights[],
                                     MPI_Info info,
                                     int reorder,
                                     MPI_Comm* graph_communicator) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::dist_graph_create_calls;
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
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::neighbor_count_calls;
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
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::neighbor_payload_calls;
    auto indegree = 0;
    auto outdegree = 0;
    auto weighted = 0;
    if (PMPI_Dist_graph_neighbors_count(communicator, &indegree, &outdegree,
                                        &weighted) == MPI_SUCCESS) {
      auto active_sends = 0;
      auto active_receives = 0;
      for (int index = 0; index < outdegree; ++index) {
        active_sends += send_counts[index] != 0 ? 1 : 0;
        neighborhood_protocol_probe::maximum_payload_count =
            std::max(neighborhood_protocol_probe::maximum_payload_count,
                     send_counts[index]);
        neighborhood_protocol_probe::nonzero_displacement_calls +=
            send_displacements[index] != 0 ? 1 : 0;
      }
      for (int index = 0; index < indegree; ++index) {
        active_receives += receive_counts[index] != 0 ? 1 : 0;
        neighborhood_protocol_probe::maximum_payload_count =
            std::max(neighborhood_protocol_probe::maximum_payload_count,
                     receive_counts[index]);
        neighborhood_protocol_probe::nonzero_displacement_calls +=
            receive_displacements[index] != 0 ? 1 : 0;
      }
      neighborhood_protocol_probe::maximum_active_send_segments =
          std::max(neighborhood_protocol_probe::maximum_active_send_segments,
                   active_sends);
      neighborhood_protocol_probe::maximum_active_receive_segments =
          std::max(neighborhood_protocol_probe::maximum_active_receive_segments,
                   active_receives);
    }
  }
  return PMPI_Neighbor_alltoallv(send_buffer, send_counts, send_displacements,
                                 send_datatype, receive_buffer, receive_counts,
                                 receive_displacements, receive_datatype,
                                 communicator);
}

#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C) && \
    KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C
extern "C" int MPI_Neighbor_alltoallv_c(void const* send_buffer,
                                        MPI_Count const send_counts[],
                                        MPI_Aint const send_displacements[],
                                        MPI_Datatype send_datatype,
                                        void* receive_buffer,
                                        MPI_Count const receive_counts[],
                                        MPI_Aint const receive_displacements[],
                                        MPI_Datatype receive_datatype,
                                        MPI_Comm communicator) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::neighbor_payload_c_calls;
  }
  return PMPI_Neighbor_alltoallv_c(send_buffer, send_counts, send_displacements,
                                   send_datatype, receive_buffer,
                                   receive_counts, receive_displacements,
                                   receive_datatype, communicator);
}
#endif

extern "C" int MPI_Isend(void const* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int destination,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Isend(buffer, count, datatype, destination, tag, communicator,
                    request);
}

extern "C" int MPI_Irecv(void* buffer,
                         int count,
                         MPI_Datatype datatype,
                         int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Request* request) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Irecv(buffer, count, datatype, source, tag, communicator,
                    request);
}

extern "C" int MPI_Recv(void* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int source,
                        int tag,
                        MPI_Comm communicator,
                        MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Recv(buffer, count, datatype, source, tag, communicator, status);
}

extern "C" int MPI_Probe(int source,
                         int tag,
                         MPI_Comm communicator,
                         MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Probe(source, tag, communicator, status);
}

extern "C" int MPI_Iprobe(int source,
                          int tag,
                          MPI_Comm communicator,
                          int* flag,
                          MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Iprobe(source, tag, communicator, flag, status);
}

extern "C" int MPI_Mprobe(int source,
                          int tag,
                          MPI_Comm communicator,
                          MPI_Message* message,
                          MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Mprobe(source, tag, communicator, message, status);
}

extern "C" int MPI_Improbe(int source,
                            int tag,
                            MPI_Comm communicator,
                            int* flag,
                            MPI_Message* message,
                            MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Improbe(source, tag, communicator, flag, message, status);
}

extern "C" int MPI_Send(void const* buffer,
                        int count,
                        MPI_Datatype datatype,
                        int destination,
                        int tag,
                        MPI_Comm communicator) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Send(buffer, count, datatype, destination, tag, communicator);
}

extern "C" int MPI_Sendrecv(void const* send_buffer,
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
                            MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Sendrecv(send_buffer, send_count, send_datatype, destination,
                       send_tag, receive_buffer, receive_count,
                       receive_datatype, source, receive_tag, communicator,
                       status);
}

extern "C" int MPI_Sendrecv_replace(void* buffer,
                                     int count,
                                     MPI_Datatype datatype,
                                     int destination,
                                     int send_tag,
                                     int source,
                                     int receive_tag,
                                     MPI_Comm communicator,
                                     MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Sendrecv_replace(buffer, count, datatype, destination, send_tag,
                               source, receive_tag, communicator, status);
}

extern "C" int MPI_Wait(MPI_Request* request, MPI_Status* status) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Wait(request, status);
}

extern "C" int MPI_Waitall(int count,
                           MPI_Request requests[],
                           MPI_Status statuses[]) {
  if (neighborhood_protocol_probe::active) {
    ++neighborhood_protocol_probe::point_to_point_calls;
  }
  return PMPI_Waitall(count, requests, statuses);
}

namespace {
using parhip::mpi::agree_collectively;
using parhip::mpi::all_to_all_v;
using parhip::mpi::capacity_issue;
using parhip::mpi::capacity_issue_diagnostic;
using parhip::mpi::capacity_issue_mask;
using parhip::mpi::capacity_result;
using parhip::mpi::capacity_route;
using parhip::mpi::capacity_route_for;
using parhip::mpi::collective_options;
using parhip::mpi::communicator;
using parhip::mpi::communicator_view;
using parhip::mpi::contiguous_owner_layout;
using parhip::mpi::distributed_graph;
using parhip::mpi::first_fatal_capacity_issue;
using parhip::mpi::has_bounded_capacity_issue;
using parhip::mpi::has_fatal_capacity_issue;
using parhip::mpi::make_mpi_datatype;
using parhip::mpi::neighbor_all_to_all_v;
using parhip::mpi::resolve_capacity_collectively;
using parhip::mpi::run_with_exception_barrier;
using parhip::mpi::runtime_is_active;
using parhip::mpi::segmented_buffer;
using parhip::mpi::topology;
using parhip::mpi::validate_collectively;
using parhip::mpi::with_bounded_capacity_issue;
using parhip::mpi::with_fatal_capacity_issue;

template <typename Operation>
void require_exact_common_mpi_error(Operation&& operation,
                                    std::string_view expected_context,
                                    communicator_view communicator) {
  auto caught = 0;
  auto exact_dynamic_type = 0;
  auto raw_code_matches = 0;
  auto context_matches = 0;
  auto error_string_calls = 0;
  {
    semantic_error_protocol_probe::activation observation{};
    try {
      std::invoke(std::forward<Operation>(operation));
    } catch (parhip::mpi::mpi_error const& error) {
      caught = 1;
      exact_dynamic_type =
          typeid(error) == typeid(parhip::mpi::mpi_error) ? 1 : 0;
      raw_code_matches = error.error_code() == MPI_ERR_ARG ? 1 : 0;
      context_matches = error.context() == expected_context ? 1 : 0;
    } catch (...) {
      caught = 1;
    }
    error_string_calls = semantic_error_protocol_probe::error_string_calls;
  }

  auto totals = std::array{caught, exact_dynamic_type, raw_code_matches,
                           context_matches, error_string_calls};
  auto global_totals = std::array{0, 0, 0, 0, 0};
  REQUIRE(PMPI_Allreduce(totals.data(), global_totals.data(),
                         static_cast<int>(totals.size()), MPI_INT, MPI_SUM,
                         communicator.native_handle()) == MPI_SUCCESS);
  auto const size = communicator.size();
  REQUIRE(global_totals == std::array{size, size, size, size, 0});
}

template <typename Operation>
void require_collective_semantic_error(Operation&& operation,
                                       std::string_view expected_context,
                                       communicator_view communicator) {
  require_exact_common_mpi_error(std::forward<Operation>(operation),
                                 expected_context, communicator);
}

TEST_CASE("capacity issues have stable bits diagnostics and fatal priority",
          "[unit][mpi][failure-policy][capacity][pure]") {
  STATIC_REQUIRE(std::is_trivially_copyable_v<capacity_result>);
  STATIC_REQUIRE(std::is_standard_layout_v<capacity_result>);
  STATIC_REQUIRE(sizeof(capacity_result) == 2 * sizeof(std::uint64_t));

  STATIC_REQUIRE(
      capacity_issue_mask(capacity_issue::received_count_not_representable) ==
      (std::uint64_t{1} << 0));
  STATIC_REQUIRE(
      capacity_issue_mask(capacity_issue::cumulative_offset_overflow) ==
      (std::uint64_t{1} << 1));
  STATIC_REQUIRE(
      capacity_issue_mask(capacity_issue::storage_byte_size_overflow) ==
      (std::uint64_t{1} << 2));
  STATIC_REQUIRE(
      capacity_issue_mask(capacity_issue::topology_degree_not_representable) ==
      (std::uint64_t{1} << 3));
  STATIC_REQUIRE(capacity_issue_mask(
                     capacity_issue::collective_layout_not_representable) ==
                 (std::uint64_t{1} << 4));
  STATIC_REQUIRE(
      capacity_issue_mask(capacity_issue::direct_backend_not_representable) ==
      (std::uint64_t{1} << 5));
  STATIC_REQUIRE(
      capacity_issue_mask(capacity_issue::bounded_round_arithmetic_overflow) ==
      (std::uint64_t{1} << 6));

  STATIC_REQUIRE(capacity_issue_diagnostic(
                     capacity_issue::received_count_not_representable) ==
                 "received element count exceeds local size_t capacity");
  STATIC_REQUIRE(
      capacity_issue_diagnostic(capacity_issue::cumulative_offset_overflow) ==
      "cumulative element offset exceeds local size_t capacity");
  STATIC_REQUIRE(
      capacity_issue_diagnostic(capacity_issue::storage_byte_size_overflow) ==
      "element storage byte size exceeds local size_t capacity");
  STATIC_REQUIRE(capacity_issue_diagnostic(
                     capacity_issue::topology_degree_not_representable) ==
                 "distributed graph outdegree exceeds MPI int capacity");
  STATIC_REQUIRE(capacity_issue_diagnostic(
                     capacity_issue::collective_layout_not_representable) ==
                 "collective payload layout has no representable MPI backend");
  STATIC_REQUIRE(
      capacity_issue_diagnostic(
          capacity_issue::direct_backend_not_representable) ==
      "direct neighborhood payload has no representable MPI backend");
  STATIC_REQUIRE(
      capacity_issue_diagnostic(
          capacity_issue::bounded_round_arithmetic_overflow) ==
      "bounded MPI-3 chunk arithmetic exceeds local size_t capacity");

  constexpr auto empty = capacity_result{};
  constexpr auto fallback = with_bounded_capacity_issue(
      empty, capacity_issue::collective_layout_not_representable);
  constexpr auto high_fatal = with_fatal_capacity_issue(
      fallback, capacity_issue::direct_backend_not_representable);
  constexpr auto mixed = with_fatal_capacity_issue(
      high_fatal, capacity_issue::received_count_not_representable);

  STATIC_REQUIRE_FALSE(has_fatal_capacity_issue(
      empty, capacity_issue::direct_backend_not_representable));
  STATIC_REQUIRE(has_bounded_capacity_issue(
      fallback, capacity_issue::collective_layout_not_representable));
  STATIC_REQUIRE(has_fatal_capacity_issue(
      mixed, capacity_issue::direct_backend_not_representable));
  STATIC_REQUIRE(first_fatal_capacity_issue(mixed).has_value());
  STATIC_REQUIRE(*first_fatal_capacity_issue(mixed) ==
                 capacity_issue::received_count_not_representable);
  STATIC_REQUIRE(capacity_route_for(empty) == capacity_route::direct);
  STATIC_REQUIRE(capacity_route_for(fallback) == capacity_route::bounded);
  STATIC_REQUIRE_FALSE(capacity_route_for(mixed).has_value());
  STATIC_REQUIRE(noexcept(resolve_capacity_collectively(
      capacity_result{}, MPI_COMM_WORLD, MPI_COMM_WORLD, "capacity test")));
}

TEST_CASE("capacity resolver performs one BOR and agrees direct or bounded",
          "[unit][mpi][failure-policy][capacity][collective]") {
  communicator_view const world{MPI_COMM_WORLD};

  auto require_route = [&](capacity_result local, capacity_route expected) {
    auto route_matches = 0;
    auto allreduce_calls = 0;
    auto signature_matches = 0;
    auto error_string_calls = 0;
    {
      semantic_error_protocol_probe::activation error_string_observation{};
      capacity_protocol_probe::activation collective_observation{};
      auto const route = resolve_capacity_collectively(
          local, world.native_handle(), world.native_handle(),
          "capacity route test");
      route_matches = route == expected ? 1 : 0;
      allreduce_calls = capacity_protocol_probe::allreduce_calls;
      signature_matches = capacity_protocol_probe::signature_matches ? 1 : 0;
      error_string_calls = semantic_error_protocol_probe::error_string_calls;
    }

    auto local_result = std::array{route_matches, allreduce_calls,
                                   signature_matches, error_string_calls};
    auto global_result = std::array{0, 0, 0, 0};
    REQUIRE(PMPI_Allreduce(local_result.data(), global_result.data(),
                           static_cast<int>(local_result.size()), MPI_INT,
                           MPI_SUM, world.native_handle()) == MPI_SUCCESS);
    REQUIRE(global_result ==
            std::array{world.size(), world.size(), world.size(), 0});
  };

  SECTION("no issue selects the direct route") {
    require_route(capacity_result{}, capacity_route::direct);
  }

  SECTION("one rank requesting fallback selects bounded everywhere") {
    auto local = capacity_result{};
    if (world.rank() == 0) {
      local = with_bounded_capacity_issue(
          local, capacity_issue::collective_layout_not_representable);
    }
    require_route(local, capacity_route::bounded);
  }
}

TEST_CASE("dense preflight routes synthetic MPI-4 layout fallback collectively",
          "[unit][mpi][failure-policy][capacity][dense]") {
  constexpr auto count_failure = with_fatal_capacity_issue(
      capacity_result{}, capacity_issue::received_count_not_representable);
  constexpr auto offset_failure = with_fatal_capacity_issue(
      capacity_result{}, capacity_issue::cumulative_offset_overflow);
  constexpr auto all_failures =
      parhip::mpi::detail::dense_capacity_preflight<std::uint64_t>(
          parhip::mpi::detail::combine_capacity_results(count_failure,
                                                        offset_failure),
          std::numeric_limits<std::size_t>::max(), true, false);
  STATIC_REQUIRE(has_fatal_capacity_issue(
      all_failures, capacity_issue::received_count_not_representable));
  STATIC_REQUIRE(has_fatal_capacity_issue(
      all_failures, capacity_issue::cumulative_offset_overflow));
  STATIC_REQUIRE(has_fatal_capacity_issue(
      all_failures, capacity_issue::storage_byte_size_overflow));
  STATIC_REQUIRE(has_bounded_capacity_issue(
      all_failures, capacity_issue::collective_layout_not_representable));
  STATIC_REQUIRE(first_fatal_capacity_issue(all_failures) ==
                 capacity_issue::received_count_not_representable);

  communicator_view const world{MPI_COMM_WORLD};

  auto require_dense_route = [&](bool local_layout_is_representable,
                                 capacity_route expected) {
    auto const local =
        parhip::mpi::detail::dense_capacity_preflight<std::uint64_t>(
            capacity_result{}, std::size_t{0}, true,
            local_layout_is_representable);
    auto allreduce_calls = 0;
    auto signature_matches = 0;
    auto error_string_calls = 0;
    auto route = capacity_route::direct;
    {
      semantic_error_protocol_probe::activation error_observation{};
      capacity_protocol_probe::activation collective_observation{};
      route = resolve_capacity_collectively(
          local, world.native_handle(), world.native_handle(),
          "dense synthetic MPI-4 layout preflight");
      allreduce_calls = capacity_protocol_probe::allreduce_calls;
      signature_matches = capacity_protocol_probe::signature_matches ? 1 : 0;
      error_string_calls = semantic_error_protocol_probe::error_string_calls;
    }

    auto const local_result =
        std::array{route == expected ? 1 : 0, allreduce_calls,
                   signature_matches, error_string_calls};
    auto global_result = std::array{0, 0, 0, 0};
    REQUIRE(PMPI_Allreduce(local_result.data(), global_result.data(),
                           static_cast<int>(local_result.size()), MPI_INT,
                           MPI_SUM, world.native_handle()) == MPI_SUCCESS);
    REQUIRE(global_result ==
            std::array{world.size(), world.size(), world.size(), 0});
  };

  SECTION("representable layout remains direct") {
    require_dense_route(true, capacity_route::direct);
  }

  SECTION("one unrepresentable MPI-4 layout selects bounded") {
    require_dense_route(world.rank() != 0, capacity_route::bounded);
  }
}

TEST_CASE("contiguous ownership uses exact integer boundaries",
          "[unit][mpi][ownership]") {
  using id_type = std::uint64_t;

  SECTION("zero work has no owners") {
    constexpr contiguous_owner_layout<id_type> layout{0, 5};
    STATIC_REQUIRE(layout.chunk_size() == 1);
    STATIC_REQUIRE(layout.boundary(0) == 0);
    STATIC_REQUIRE(layout.boundary(5) == 0);
    STATIC_REQUIRE_FALSE(layout.owner(0).has_value());
  }

  SECTION("more ranks than IDs leaves trailing ranks empty") {
    constexpr contiguous_owner_layout<id_type> layout{2, 5};
    STATIC_REQUIRE(layout.chunk_size() == 1);
    STATIC_REQUIRE(layout.begin(0) == 0);
    STATIC_REQUIRE(layout.end(0) == 1);
    STATIC_REQUIRE(layout.begin(1) == 1);
    STATIC_REQUIRE(layout.end(1) == 2);
    STATIC_REQUIRE(layout.begin(2) == 2);
    STATIC_REQUIRE(layout.end(4) == 2);
    STATIC_REQUIRE(layout.owner(0) == 0);
    STATIC_REQUIRE(layout.owner(1) == 1);
    STATIC_REQUIRE_FALSE(layout.owner(2).has_value());
  }

  SECTION("uneven ownership retains the pinned fixed-chunk partition") {
    constexpr contiguous_owner_layout<id_type> layout{4, 3};
    STATIC_REQUIRE(layout.chunk_size() == 2);
    STATIC_REQUIRE(layout.boundary(0) == 0);
    STATIC_REQUIRE(layout.boundary(1) == 2);
    STATIC_REQUIRE(layout.boundary(2) == 4);
    STATIC_REQUIRE(layout.boundary(3) == 4);
    STATIC_REQUIRE(layout.owner(0) == 0);
    STATIC_REQUIRE(layout.owner(1) == 0);
    STATIC_REQUIRE(layout.owner(2) == 1);
    STATIC_REQUIRE(layout.owner(3) == 1);
  }

  SECTION("values above the exact double integer range stay exact") {
    constexpr auto total = (id_type{1} << 53) + 1;
    constexpr contiguous_owner_layout<id_type> layout{total, 2};
    STATIC_REQUIRE(layout.chunk_size() == (id_type{1} << 52) + 1);
    STATIC_REQUIRE(layout.boundary(1) == (id_type{1} << 52) + 1);
    STATIC_REQUIRE(layout.boundary(2) == total);
    STATIC_REQUIRE(layout.owner(total - 1) == 1);
  }

  SECTION("maximum NodeID never overflows a boundary product") {
    constexpr auto total = std::numeric_limits<id_type>::max();
    constexpr contiguous_owner_layout<id_type> two_ranks{total, 2};
    constexpr contiguous_owner_layout<id_type> three_ranks{total, 3};
    constexpr contiguous_owner_layout<id_type> five_ranks{total, 5};
    STATIC_REQUIRE(two_ranks.boundary(2) == total);
    STATIC_REQUIRE(three_ranks.boundary(3) == total);
    STATIC_REQUIRE(five_ranks.boundary(5) == total);
    STATIC_REQUIRE(two_ranks.owner(total - 1) == 1);
    STATIC_REQUIRE(three_ranks.owner(total - 1) == 2);
    STATIC_REQUIRE(five_ranks.owner(total - 1) == 4);
  }
}

TEST_CASE("native MPI types use the closed MP11 mapping", "[unit][mpi]") {
  STATIC_REQUIRE(parhip::mpi::mpi_native_datatype<int>);
  STATIC_REQUIRE(parhip::mpi::mpi_native_datatype<std::uint64_t>);
  STATIC_REQUIRE(parhip::mpi::mpi_native_datatype<double>);
  STATIC_REQUIRE_FALSE(parhip::mpi::mpi_native_datatype<std::string>);

  REQUIRE(make_mpi_datatype<int>().native_handle() == MPI_INT);
  REQUIRE(make_mpi_datatype<unsigned long>().native_handle() ==
          MPI_UNSIGNED_LONG);
  REQUIRE(make_mpi_datatype<double>().native_handle() == MPI_DOUBLE);
  REQUIRE_FALSE(make_mpi_datatype<int>().owns_handle());
}

TEST_CASE("communicator and topology ownership stays scoped", "[unit][mpi]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<communicator>);
  STATIC_REQUIRE(std::is_move_constructible_v<communicator>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<topology>);
  STATIC_REQUIRE(std::is_move_constructible_v<topology>);

  communicator_view const world{MPI_COMM_WORLD};
  REQUIRE(world.size() >= 1);
  REQUIRE(world.rank() >= 0);

  communicator duplicate{world};
  int comparison = MPI_UNEQUAL;
  REQUIRE(MPI_Comm_compare(world.native_handle(), duplicate.native_handle(),
                           &comparison) == MPI_SUCCESS);
  REQUIRE(comparison == MPI_CONGRUENT);

  MPI_Errhandler handler = MPI_ERRHANDLER_NULL;
  REQUIRE(MPI_Comm_get_errhandler(duplicate.native_handle(), &handler) ==
          MPI_SUCCESS);
  REQUIRE(handler == MPI_ERRORS_RETURN);
  REQUIRE(MPI_Errhandler_free(&handler) == MPI_SUCCESS);

  int dimensions[] = {world.size()};
  int periods[] = {0};
  MPI_Comm cartesian = MPI_COMM_NULL;
  REQUIRE(MPI_Cart_create(world.native_handle(), 1, dimensions, periods, 0,
                          &cartesian) == MPI_SUCCESS);
  {
    topology duplicate_topology{communicator_view{cartesian}};
    REQUIRE(duplicate_topology.view().size() == world.size());
  }
  REQUIRE(MPI_Comm_free(&cartesian) == MPI_SUCCESS);
}

TEST_CASE("exception barrier captures failures without letting them escape",
          "[unit][mpi]") {
  REQUIRE(runtime_is_active());

  std::exception_ptr captured;
  run_with_exception_barrier(
      [] { throw std::runtime_error{"boundary failure"}; },
      [&](std::exception_ptr failure) noexcept { captured = failure; });

  REQUIRE(captured != nullptr);
  REQUIRE_THROWS_WITH(std::rethrow_exception(captured), "boundary failure");
}

TEST_CASE("exported partition boundary is non-throwing", "[unit][mpi]") {
  using partition_function =
      void(idxtype*, idxtype*, idxtype*, idxtype*, idxtype*, int*, double*,
           bool, int, int, int*, idxtype*, MPI_Comm*) noexcept;
  STATIC_REQUIRE(
      std::is_same_v<decltype(&ParHIPPartitionKWay), partition_function*>);
}

TEST_CASE("explicit Hana wire metadata produces array-safe extent",
          "[unit][mpi]") {
  STATIC_REQUIRE(parhip::mpi::mpi_wire_datatype<test_support::wire_entry>);
  STATIC_REQUIRE(std::is_standard_layout_v<test_support::wire_entry>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<test_support::wire_entry>);

  auto datatype = make_mpi_datatype<test_support::wire_entry>();
  REQUIRE(datatype.owns_handle());

  MPI_Aint lower_bound = -1;
  MPI_Aint extent = -1;
  REQUIRE(MPI_Type_get_extent(datatype.native_handle(), &lower_bound,
                              &extent) == MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  REQUIRE(extent == static_cast<MPI_Aint>(sizeof(test_support::wire_entry)));
}

TEST_CASE("wire-record exchange needs no default constructor", "[unit][mpi]") {
  STATIC_REQUIRE(
      parhip::mpi::mpi_wire_datatype<test_support::non_default_wire_entry>);
  STATIC_REQUIRE(
      std::is_standard_layout_v<test_support::non_default_wire_entry>);
  STATIC_REQUIRE(
      std::is_trivially_copyable_v<test_support::non_default_wire_entry>);

  auto datatype = make_mpi_datatype<test_support::non_default_wire_entry>();
  REQUIRE(datatype.owns_handle());

  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<test_support::non_default_wire_entry>> segments(
      static_cast<std::size_t>(world.size()));
  segments[static_cast<std::size_t>(rank)].emplace_back(
      static_cast<std::uint64_t>(rank + 1), rank);

  auto received = all_to_all_v(
      segmented_buffer<test_support::non_default_wire_entry>::from_segments(
          segments),
      world);

  REQUIRE(std::ranges::equal(received.segment(static_cast<std::size_t>(rank)),
                             segments[static_cast<std::size_t>(rank)]));
}

TEST_CASE("segmented buffers expose canonical contiguous spans",
          "[unit][mpi]") {
  auto buffer = segmented_buffer<int>::from_segments(
      std::vector<std::vector<int>>{{1, 2}, {}, {3, 4, 5}});

  REQUIRE(
      std::ranges::equal(buffer.storage(), std::vector<int>{1, 2, 3, 4, 5}));
  REQUIRE(buffer.counts() == std::vector<std::size_t>{2, 0, 3});
  REQUIRE(buffer.offsets() == std::vector<std::size_t>{0, 2, 2});
  REQUIRE(std::ranges::equal(buffer.segment(0), std::array{1, 2}));
  REQUIRE(buffer.segment(1).empty());
  REQUIRE(std::ranges::equal(buffer.segment(2), std::array{3, 4, 5}));
}

TEST_CASE("dense exchange preserves all-empty segments", "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto sends = segmented_buffer<int>::from_segments(
      std::vector<std::vector<int>>(static_cast<std::size_t>(world.size())));

  auto received = all_to_all_v(std::move(sends), world);

  REQUIRE(received.storage().empty());
  REQUIRE(
      received.has_canonical_layout(static_cast<std::size_t>(world.size())));
  REQUIRE(std::ranges::all_of(received.counts(),
                              [](auto count) { return count == 0; }));
}

TEST_CASE("dense exchange preserves self-only wire-record arrays",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<test_support::wire_entry>> segments(
      static_cast<std::size_t>(world.size()));
  segments[static_cast<std::size_t>(rank)] = {
      {static_cast<std::uint64_t>(rank * 10 + 1), rank, 1.25},
      {static_cast<std::uint64_t>(rank * 10 + 2), rank, 2.5}};

  auto received = all_to_all_v(
      segmented_buffer<test_support::wire_entry>::from_segments(segments),
      world);

  REQUIRE(
      received.has_canonical_layout(static_cast<std::size_t>(world.size())));
  for (int source = 0; source < world.size(); ++source) {
    if (source == rank) {
      REQUIRE(
          std::ranges::equal(received.segment(static_cast<std::size_t>(source)),
                             segments[static_cast<std::size_t>(rank)]));
    } else {
      REQUIRE(received.segment(static_cast<std::size_t>(source)).empty());
    }
  }
}

TEST_CASE("dense uneven exchange returns canonical source segments",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<int>> segments(
      static_cast<std::size_t>(world.size()));
  for (int destination = 0; destination < world.size(); ++destination) {
    auto const count = rank + destination;
    for (int index = 0; index < count; ++index) {
      segments[static_cast<std::size_t>(destination)].push_back(
          rank * 10'000 + destination * 100 + index);
    }
  }

  auto received =
      all_to_all_v(segmented_buffer<int>::from_segments(segments), world);

  REQUIRE(
      received.has_canonical_layout(static_cast<std::size_t>(world.size())));
  for (int source = 0; source < world.size(); ++source) {
    std::vector<int> expected;
    for (int index = 0; index < source + rank; ++index) {
      expected.push_back(source * 10'000 + rank * 100 + index);
    }
    REQUIRE(std::ranges::equal(
        received.segment(static_cast<std::size_t>(source)), expected));
  }
}

TEST_CASE("zero-local-work ranks still participate in dense exchange",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<int>> segments(
      static_cast<std::size_t>(world.size()));
  if (rank != 0) {
    segments[0].push_back(rank * 7);
  }

  auto received =
      all_to_all_v(segmented_buffer<int>::from_segments(segments), world);

  if (rank == 0) {
    REQUIRE(received.counts()[0] == 0);
    for (int source = 1; source < world.size(); ++source) {
      REQUIRE(
          std::ranges::equal(received.segment(static_cast<std::size_t>(source)),
                             std::array{source * 7}));
    }
  } else {
    REQUIRE(received.storage().empty());
  }
}

TEST_CASE("dense validation failure propagates to every rank", "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};

  std::vector<std::size_t> counts(static_cast<std::size_t>(world.size()), 0);
  std::vector<std::size_t> offsets(static_cast<std::size_t>(world.size()), 0);
  if (world.rank() == 0) {
    offsets[0] = 1;
  }
  segmented_buffer<int> malformed{{}, std::move(counts), std::move(offsets)};

  require_collective_semantic_error(
      [&] { static_cast<void>(all_to_all_v(std::move(malformed), world)); },
      "all_to_all_v collective input validation failed", world);
}

TEST_CASE("collective validation helper throws one exact common semantic error",
          "[unit][mpi][failure-policy][semantic][validation]") {
  communicator_view const world{MPI_COMM_WORLD};
  require_collective_semantic_error(
      [&] {
        validate_collectively(world.rank() != 0, world,
                              "adapter collective validation failed");
      },
      "adapter collective validation failed", world);
}

TEST_CASE("collective value agreement is exact or succeeds at one rank",
          "[unit][mpi][failure-policy][semantic][agreement]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() == 1) {
    semantic_error_protocol_probe::activation observation{};
    REQUIRE(agree_collectively(17, world, "adapter value agreement failed") ==
            17);
    REQUIRE(semantic_error_protocol_probe::error_string_calls == 0);
    return;
  }

  require_collective_semantic_error(
      [&] {
        static_cast<void>(agree_collectively(world.rank(), world,
                                             "adapter value agreement failed"));
      },
      "adapter value agreement failed", world);
}

TEST_CASE("dense options reject zero ceiling collectively", "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto segments =
      std::vector<std::vector<int>>(static_cast<std::size_t>(world.size()));
  require_collective_semantic_error(
      [&] {
        static_cast<void>(all_to_all_v(
            segmented_buffer<int>::from_segments(segments), world,
            collective_options{.mpi3_round_ceiling = 0, .force_mpi3 = true}));
      },
      "all_to_all_v collective options must match and use a nonzero MPI-3 "
      "ceiling",
      world);
}

TEST_CASE("dense options agree or reject rank skew collectively",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto segments =
      std::vector<std::vector<int>>(static_cast<std::size_t>(world.size()));
  if (world.size() == 1) {
    semantic_error_protocol_probe::activation observation{};
    auto received = all_to_all_v(
        segmented_buffer<int>::from_segments(segments), world,
        collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});
    REQUIRE(received.storage().empty());
    REQUIRE(semantic_error_protocol_probe::error_string_calls == 0);
    return;
  }

  auto const options = collective_options{
      .mpi3_round_ceiling = world.rank() == 0 ? 1U : 2U, .force_mpi3 = true};
  require_collective_semantic_error(
      [&] {
        static_cast<void>(all_to_all_v(
            segmented_buffer<int>::from_segments(segments), world, options));
      },
      "all_to_all_v collective options must match and use a nonzero MPI-3 "
      "ceiling",
      world);
}

TEST_CASE("forced MPI-3 bounded rounds preserve every element", "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<int>> segments(
      static_cast<std::size_t>(world.size()));
  for (int destination = 0; destination < world.size(); ++destination) {
    for (int index = 0; index < 5; ++index) {
      segments[static_cast<std::size_t>(destination)].push_back(
          rank * 10'000 + destination * 100 + index);
    }
  }

  auto received = all_to_all_v(
      segmented_buffer<int>::from_segments(segments), world,
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});

  for (int source = 0; source < world.size(); ++source) {
    std::array expected{
        source * 10'000 + rank * 100, source * 10'000 + rank * 100 + 1,
        source * 10'000 + rank * 100 + 2, source * 10'000 + rank * 100 + 3,
        source * 10'000 + rank * 100 + 4};
    REQUIRE(std::ranges::equal(
        received.segment(static_cast<std::size_t>(source)), expected));
  }
}

TEST_CASE("MPI-3 dense phase pairing is exact at the int rank limit",
          "[unit][mpi][mpi3][bounded]") {
  constexpr auto size =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  constexpr auto last = size - 1;

  constexpr auto zero_phase =
      parhip::mpi::detail::dense_phase_peers(last, size, 0);
  STATIC_REQUIRE(zero_phase.destination == last);
  STATIC_REQUIRE(zero_phase.source == last);

  constexpr auto first_phase =
      parhip::mpi::detail::dense_phase_peers(last, size, 1);
  STATIC_REQUIRE(first_phase.destination == 0);
  STATIC_REQUIRE(first_phase.source == size - 2);

  constexpr auto last_phase =
      parhip::mpi::detail::dense_phase_peers(last, size, last);
  STATIC_REQUIRE(last_phase.destination == size - 2);
  STATIC_REQUIRE(last_phase.source == 0);

  constexpr auto rank_zero_last_phase =
      parhip::mpi::detail::dense_phase_peers(0, size, last);
  STATIC_REQUIRE(rank_zero_last_phase.destination == last);
  STATIC_REQUIRE(rank_zero_last_phase.source == 1);
}

TEST_CASE("forced MPI-3 rounds preserve self-only source segments",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<int>> segments(
      static_cast<std::size_t>(world.size()));
  for (int index = 0; index < 5; ++index) {
    segments[static_cast<std::size_t>(rank)].push_back(rank * 100 + index);
  }

  auto received = all_to_all_v(
      segmented_buffer<int>::from_segments(segments), world,
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});

  for (int source = 0; source < world.size(); ++source) {
    auto const expected = source == rank
                              ? segments[static_cast<std::size_t>(rank)]
                              : std::vector<int>{};
    REQUIRE(std::ranges::equal(
        received.segment(static_cast<std::size_t>(source)), expected));
  }
}

TEST_CASE("forced MPI-3 rounds preserve uneven asymmetric segments",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<int>> segments(
      static_cast<std::size_t>(world.size()));
  for (int destination = 0; destination < world.size(); ++destination) {
    auto const count = 3 + rank + 2 * destination;
    for (int index = 0; index < count; ++index) {
      segments[static_cast<std::size_t>(destination)].push_back(
          rank * 10'000 + destination * 100 + index);
    }
  }

  auto received = all_to_all_v(
      segmented_buffer<int>::from_segments(segments), world,
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});

  for (int source = 0; source < world.size(); ++source) {
    std::vector<int> expected;
    auto const count = 3 + source + 2 * rank;
    for (int index = 0; index < count; ++index) {
      expected.push_back(source * 10'000 + rank * 100 + index);
    }
    REQUIRE(std::ranges::equal(
        received.segment(static_cast<std::size_t>(source)), expected));
  }
}

TEST_CASE("forced MPI-3 rounds retain zero-work rank participation",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  std::vector<std::vector<int>> segments(
      static_cast<std::size_t>(world.size()));
  if (rank != 0) {
    for (int destination = 1; destination < world.size(); ++destination) {
      for (int index = 0; index < 5; ++index) {
        segments[static_cast<std::size_t>(destination)].push_back(
            rank * 10'000 + destination * 100 + index);
      }
    }
  }

  auto received = all_to_all_v(
      segmented_buffer<int>::from_segments(segments), world,
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});

  if (rank == 0) {
    REQUIRE(received.storage().empty());
  } else {
    REQUIRE(received.segment(0).empty());
    for (int source = 1; source < world.size(); ++source) {
      std::array expected{
          source * 10'000 + rank * 100, source * 10'000 + rank * 100 + 1,
          source * 10'000 + rank * 100 + 2, source * 10'000 + rank * 100 + 3,
          source * 10'000 + rank * 100 + 4};
      REQUIRE(std::ranges::equal(
          received.segment(static_cast<std::size_t>(source)), expected));
    }
  }
}

TEST_CASE("distributed graph preserves zero-degree ranks",
          "[unit][mpi][neighbor][topology][zero]") {
  communicator_view const world{MPI_COMM_WORLD};

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  distributed_graph graph{world, {}};
  neighborhood_protocol_probe::active = false;

  REQUIRE(graph.sources().empty());
  REQUIRE(graph.destinations().empty());
  REQUIRE_FALSE(graph.source_index(world.rank()).has_value());
  REQUIRE_FALSE(graph.destination_index(world.rank()).has_value());
  REQUIRE(neighborhood_protocol_probe::dist_graph_create_calls == 1);

  int topology_kind = MPI_UNDEFINED;
  REQUIRE(MPI_Topo_test(graph.native_handle(), &topology_kind) == MPI_SUCCESS);
  REQUIRE(topology_kind == MPI_DIST_GRAPH);

  MPI_Errhandler handler = MPI_ERRHANDLER_NULL;
  REQUIRE(MPI_Comm_get_errhandler(graph.native_handle(), &handler) ==
          MPI_SUCCESS);
  REQUIRE(handler == MPI_ERRORS_RETURN);
  REQUIRE(MPI_Errhandler_free(&handler) == MPI_SUCCESS);
}

TEST_CASE("distributed graph does not alter its caller error handler",
          "[unit][mpi][neighbor][topology][handler]") {
  communicator_view const world{MPI_COMM_WORLD};
  communicator caller{world};
  REQUIRE(MPI_Comm_set_errhandler(caller.native_handle(),
                                  MPI_ERRORS_ARE_FATAL) == MPI_SUCCESS);

  distributed_graph graph{caller.view(), {world.rank()}};

  MPI_Errhandler caller_handler = MPI_ERRHANDLER_NULL;
  REQUIRE(MPI_Comm_get_errhandler(caller.native_handle(), &caller_handler) ==
          MPI_SUCCESS);
  REQUIRE(caller_handler == MPI_ERRORS_ARE_FATAL);
  REQUIRE(MPI_Errhandler_free(&caller_handler) == MPI_SUCCESS);

  MPI_Errhandler graph_handler = MPI_ERRHANDLER_NULL;
  REQUIRE(MPI_Comm_get_errhandler(graph.native_handle(), &graph_handler) ==
          MPI_SUCCESS);
  REQUIRE(graph_handler == MPI_ERRORS_RETURN);
  REQUIRE(MPI_Errhandler_free(&graph_handler) == MPI_SUCCESS);
}

TEST_CASE("distributed graph normalizes unsorted duplicate destinations",
          "[unit][mpi][neighbor][topology][normalization]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  auto const next = (rank + 1) % world.size();
  distributed_graph graph{world, {next, rank, next, rank}};

  auto actual_destinations = std::vector<int>(graph.destinations().begin(),
                                              graph.destinations().end());
  std::ranges::sort(actual_destinations);
  auto expected_destinations = std::vector<int>{rank};
  if (next != rank) {
    expected_destinations.push_back(next);
    std::ranges::sort(expected_destinations);
  }
  REQUIRE(actual_destinations == expected_destinations);

  auto const previous = (rank - 1 + world.size()) % world.size();
  auto actual_sources =
      std::vector<int>(graph.sources().begin(), graph.sources().end());
  std::ranges::sort(actual_sources);
  auto expected_sources = std::vector<int>{rank};
  if (previous != rank) {
    expected_sources.push_back(previous);
    std::ranges::sort(expected_sources);
  }
  REQUIRE(actual_sources == expected_sources);

  for (std::size_t index = 0; index < graph.destinations().size(); ++index) {
    REQUIRE(graph.destination_index(graph.destinations()[index]) == index);
  }
  for (std::size_t index = 0; index < graph.sources().size(); ++index) {
    REQUIRE(graph.source_index(graph.sources()[index]) == index);
  }
  REQUIRE_FALSE(graph.source_index(world.size()).has_value());
  REQUIRE_FALSE(graph.destination_index(world.size()).has_value());

  auto indegree = 0;
  auto outdegree = 0;
  auto weighted = 0;
  REQUIRE(MPI_Dist_graph_neighbors_count(graph.native_handle(), &indegree,
                                         &outdegree, &weighted) == MPI_SUCCESS);
  auto queried_sources = std::vector<int>(static_cast<std::size_t>(indegree));
  auto queried_destinations =
      std::vector<int>(static_cast<std::size_t>(outdegree));
  REQUIRE(MPI_Dist_graph_neighbors(graph.native_handle(), indegree,
                                   queried_sources.data(), MPI_UNWEIGHTED,
                                   outdegree, queried_destinations.data(),
                                   MPI_UNWEIGHTED) == MPI_SUCCESS);
  REQUIRE(std::ranges::equal(graph.sources(), queried_sources));
  REQUIRE(std::ranges::equal(graph.destinations(), queried_destinations));
}

TEST_CASE(
    "distributed graph represents asymmetric source destination and isolated "
    "ranks",
    "[unit][mpi][neighbor][topology][asymmetric]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() < 3) {
    return;
  }

  auto outgoing = std::vector<int>{};
  if (world.rank() == 0) {
    for (int destination = 1; destination < world.size() - 1; ++destination) {
      outgoing.push_back(destination);
    }
  }
  distributed_graph graph{world, std::move(outgoing)};

  if (world.rank() == 0) {
    REQUIRE(graph.sources().empty());
    REQUIRE(graph.destinations().size() ==
            static_cast<std::size_t>(world.size() - 2));
  } else if (world.rank() == world.size() - 1) {
    REQUIRE(graph.sources().empty());
    REQUIRE(graph.destinations().empty());
  } else {
    REQUIRE(std::ranges::equal(graph.sources(), std::array{0}));
    REQUIRE(graph.destinations().empty());
  }
}

TEST_CASE("distributed graph owns one movable communicator",
          "[unit][mpi][neighbor][topology][ownership]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<distributed_graph>);
  STATIC_REQUIRE(std::is_move_constructible_v<distributed_graph>);
  STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<distributed_graph>);
  STATIC_REQUIRE(std::is_move_assignable_v<distributed_graph>);

  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph original{world, {world.rank()}};
  distributed_graph moved{std::move(original)};
  REQUIRE(original.native_handle() == MPI_COMM_NULL);
  REQUIRE(moved.native_handle() != MPI_COMM_NULL);

  distributed_graph replacement{world, {}};
  replacement = std::move(moved);
  REQUIRE(moved.native_handle() == MPI_COMM_NULL);
  REQUIRE(
      std::ranges::equal(replacement.destinations(), std::array{world.rank()}));
}

TEST_CASE(
    "distributed graph rejects an invalid destination collectively before "
    "creation",
    "[unit][mpi][neighbor][topology][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto outgoing = std::vector<int>{};
  if (world.rank() == 0) {
    outgoing.push_back(world.size());
  }

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        distributed_graph invalid{world, outgoing};
        static_cast<void>(invalid);
      },
      "distributed graph destination validation failed", world);
  neighborhood_protocol_probe::active = false;
  REQUIRE(neighborhood_protocol_probe::dist_graph_create_calls == 0);
}

TEST_CASE(
    "neighborhood exchange preserves an all-zero topology without point to "
    "point calls",
    "[unit][mpi][neighbor][exchange][zero]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {}};
  auto sends =
      segmented_buffer<int>::from_segments(std::vector<std::vector<int>>{});

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  auto received = neighbor_all_to_all_v(std::move(sends), graph);
  neighborhood_protocol_probe::active = false;

  REQUIRE(received.storage().empty());
  REQUIRE(received.segment_count() == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_count_calls == 1);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_calls +
              neighborhood_protocol_probe::neighbor_payload_c_calls ==
          1);
  REQUIRE(neighborhood_protocol_probe::point_to_point_calls == 0);
}

TEST_CASE("neighborhood exchange preserves self-only typed arrays",
          "[unit][mpi][neighbor][exchange][self]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  distributed_graph graph{world, {rank}};
  auto const expected = std::vector<test_support::wire_entry>{
      {static_cast<std::uint64_t>(rank * 10 + 1), rank, 1.25},
      {static_cast<std::uint64_t>(rank * 10 + 2), rank, 2.5}};
  auto sends = segmented_buffer<test_support::wire_entry>::from_segments(
      std::vector<std::vector<test_support::wire_entry>>{expected});

  auto received = neighbor_all_to_all_v(std::move(sends), graph);

  REQUIRE(std::ranges::equal(graph.sources(), std::array{rank}));
  REQUIRE(received.segment_count() == 1);
  REQUIRE(std::ranges::equal(received.segment(0), expected));
}

TEST_CASE("neighborhood exchange follows authoritative queried neighbor order",
          "[unit][mpi][neighbor][exchange][order][uneven]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const rank = world.rank();
  auto const next = (rank + 1) % world.size();
  distributed_graph graph{world, {next, rank}};

  auto segments = std::vector<std::vector<int>>{};
  segments.reserve(graph.destinations().size());
  for (auto const destination : graph.destinations()) {
    auto values = std::vector<int>{};
    auto const count = (rank + destination) % 3;
    for (int index = 0; index < count; ++index) {
      values.push_back(rank * 10'000 + destination * 100 + index);
    }
    segments.push_back(std::move(values));
  }

  auto received = neighbor_all_to_all_v(
      segmented_buffer<int>::from_segments(segments), graph);

  REQUIRE(received.segment_count() == graph.sources().size());
  for (std::size_t index = 0; index < graph.sources().size(); ++index) {
    auto const source = graph.sources()[index];
    auto expected = std::vector<int>{};
    auto const count = (source + rank) % 3;
    for (int element = 0; element < count; ++element) {
      expected.push_back(source * 10'000 + rank * 100 + element);
    }
    REQUIRE(std::ranges::equal(received.segment(index), expected));
  }
}

TEST_CASE("neighborhood exchange supports an asymmetric star",
          "[unit][mpi][neighbor][exchange][asymmetric]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() < 3) {
    return;
  }
  auto outgoing = std::vector<int>{};
  if (world.rank() == 0) {
    for (int destination = 1; destination < world.size() - 1; ++destination) {
      outgoing.push_back(destination);
    }
  }
  distributed_graph graph{world, std::move(outgoing)};

  auto segments = std::vector<std::vector<int>>{};
  for (auto const destination : graph.destinations()) {
    auto values = std::vector<int>{};
    for (int index = 0; index < destination; ++index) {
      values.push_back(destination * 100 + index);
    }
    segments.push_back(std::move(values));
  }
  auto received = neighbor_all_to_all_v(
      segmented_buffer<int>::from_segments(segments), graph);

  if (world.rank() == 0 || world.rank() == world.size() - 1) {
    REQUIRE(received.storage().empty());
  } else {
    REQUIRE(std::ranges::equal(graph.sources(), std::array{0}));
    auto expected = std::vector<int>{};
    for (int index = 0; index < world.rank(); ++index) {
      expected.push_back(world.rank() * 100 + index);
    }
    REQUIRE(std::ranges::equal(received.segment(0), expected));
  }
}

TEST_CASE("neighborhood exchange supports a reversed asymmetric star",
          "[unit][mpi][neighbor][exchange][asymmetric]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() < 3) {
    return;
  }
  auto outgoing = world.rank() == 0 ? std::vector<int>{} : std::vector<int>{0};
  distributed_graph graph{world, std::move(outgoing)};

  auto segments = std::vector<std::vector<int>>{};
  for (auto const destination : graph.destinations()) {
    segments.push_back({world.rank() * 100 + destination});
  }
  auto received = neighbor_all_to_all_v(
      segmented_buffer<int>::from_segments(segments), graph);

  if (world.rank() == 0) {
    REQUIRE(graph.destinations().empty());
    REQUIRE(received.segment_count() ==
            static_cast<std::size_t>(world.size() - 1));
    for (std::size_t index = 0; index < graph.sources().size(); ++index) {
      REQUIRE(std::ranges::equal(received.segment(index),
                                 std::array{graph.sources()[index] * 100}));
    }
  } else {
    REQUIRE(graph.sources().empty());
    REQUIRE(received.storage().empty());
  }
}

TEST_CASE("forced MPI-3 neighborhood rounds preserve asymmetric payloads",
          "[unit][mpi][neighbor][exchange][mpi3][bounded]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto outgoing = std::vector<int>{};
  if (world.size() == 1) {
    outgoing.push_back(0);
  } else if (world.rank() == 0) {
    for (int destination = 1; destination < world.size(); ++destination) {
      outgoing.push_back(destination);
    }
  }
  distributed_graph graph{world, std::move(outgoing)};

  auto segments = std::vector<std::vector<int>>{};
  for (auto const destination : graph.destinations()) {
    auto values = std::vector<int>{};
    for (int index = 0; index < 5; ++index) {
      values.push_back(world.rank() * 10'000 + destination * 100 + index);
    }
    segments.push_back(std::move(values));
  }

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  auto received = neighbor_all_to_all_v(
      segmented_buffer<int>::from_segments(segments), graph,
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});
  neighborhood_protocol_probe::active = false;

  if (world.size() == 1 || world.rank() != 0) {
    REQUIRE(received.segment_count() == 1);
    constexpr auto source = 0;
    auto expected = std::vector<int>{};
    for (int index = 0; index < 5; ++index) {
      expected.push_back(source * 10'000 + world.rank() * 100 + index);
    }
    REQUIRE(std::ranges::equal(received.segment(0), expected));
  } else {
    REQUIRE(received.storage().empty());
  }
  REQUIRE(neighborhood_protocol_probe::neighbor_count_calls == 1);
  auto const expected_payload_calls =
      3 * (world.size() == 1 ? 1 : world.size() - 1);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_calls ==
          expected_payload_calls);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_c_calls == 0);
  REQUIRE(neighborhood_protocol_probe::maximum_active_send_segments <= 1);
  REQUIRE(neighborhood_protocol_probe::maximum_active_receive_segments <= 1);
  REQUIRE(neighborhood_protocol_probe::maximum_payload_count <= 2);
  REQUIRE(neighborhood_protocol_probe::nonzero_displacement_calls == 0);
  REQUIRE(neighborhood_protocol_probe::point_to_point_calls == 0);
}

TEST_CASE(
    "neighborhood exchange rejects a malformed segment count collectively",
    "[unit][mpi][neighbor][exchange][failure]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};
  auto segments = world.rank() == 0
                      ? std::vector<std::vector<int>>{}
                      : std::vector<std::vector<int>>{{world.rank()}};

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        static_cast<void>(neighbor_all_to_all_v(
            segmented_buffer<int>::from_segments(segments), graph));
      },
      "neighbor_all_to_all_v collective input validation failed", world);
  neighborhood_protocol_probe::active = false;
  REQUIRE(neighborhood_protocol_probe::neighbor_count_calls == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_calls == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_c_calls == 0);
}

TEST_CASE("neighborhood exchange rejects mismatched MPI-3 options collectively",
          "[unit][mpi][neighbor][exchange][failure][options]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() == 1) {
    distributed_graph graph{world, {world.rank()}};
    semantic_error_protocol_probe::activation observation{};
    auto received = neighbor_all_to_all_v(
        segmented_buffer<int>::from_segments(
            std::vector<std::vector<int>>{{world.rank()}}),
        graph, collective_options{.mpi3_round_ceiling = 1, .force_mpi3 = true});
    REQUIRE(std::ranges::equal(received.segment(0), std::array{world.rank()}));
    REQUIRE(semantic_error_protocol_probe::error_string_calls == 0);
    return;
  }
  distributed_graph graph{world, {world.rank()}};
  auto options = collective_options{
      .mpi3_round_ceiling = world.rank() == 0 ? 1U : 2U, .force_mpi3 = true};

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        static_cast<void>(neighbor_all_to_all_v(
            segmented_buffer<int>::from_segments(
                std::vector<std::vector<int>>{{world.rank()}}),
            graph, options));
      },
      "neighbor_all_to_all_v collective options must match and use a nonzero "
      "MPI-3 ceiling",
      world);
  neighborhood_protocol_probe::active = false;
  REQUIRE(neighborhood_protocol_probe::neighbor_count_calls == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_calls == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_c_calls == 0);
}

TEST_CASE("neighborhood exchange rejects a zero MPI-3 ceiling before counts",
          "[unit][mpi][neighbor][exchange][failure][options]") {
  communicator_view const world{MPI_COMM_WORLD};
  distributed_graph graph{world, {world.rank()}};

  neighborhood_protocol_probe::reset();
  neighborhood_protocol_probe::active = true;
  require_collective_semantic_error(
      [&] {
        static_cast<void>(neighbor_all_to_all_v(
            segmented_buffer<int>::from_segments(
                std::vector<std::vector<int>>{{world.rank()}}),
            graph,
            collective_options{.mpi3_round_ceiling = 0, .force_mpi3 = true}));
      },
      "neighbor_all_to_all_v collective options must match and use a nonzero "
      "MPI-3 ceiling",
      world);
  neighborhood_protocol_probe::active = false;
  REQUIRE(neighborhood_protocol_probe::neighbor_count_calls == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_calls == 0);
  REQUIRE(neighborhood_protocol_probe::neighbor_payload_c_calls == 0);
}

TEST_CASE("neighbor large-count capability reflects the generated probe",
          "[unit][mpi][neighbor][capability]") {
#if defined(KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C)
  STATIC_REQUIRE(parhip::mpi::capabilities::has_neighbor_alltoallv_c ==
                 (KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C != 0));
#else
  FAIL("generated MPI neighborhood capability macro is missing");
#endif
}

TEST_CASE("collectively agreed semantic errors are MPI-free and rank-symmetric",
          "[unit][mpi][failure-policy][semantic]") {
  communicator_view const world{MPI_COMM_WORLD};
  auto const local_is_valid = world.rank() != 0 ? 1 : 0;
  auto all_are_valid = 0;
  REQUIRE(PMPI_Allreduce(&local_is_valid, &all_are_valid, 1, MPI_INT, MPI_LAND,
                         world.native_handle()) == MPI_SUCCESS);
  REQUIRE(all_are_valid == 0);

  constexpr auto context = std::string_view{"semantic helper symmetry"};
  auto caught = 0;
  auto raw_code_matches = 0;
  auto context_matches = 0;
  auto location_is_retained = 0;
  auto const expected_location = std::source_location::current();
  {
    semantic_error_protocol_probe::activation observation{};
    try {
      parhip::mpi::throw_collectively_agreed_semantic_error(
          world.native_handle(), context, expected_location);
    } catch (parhip::mpi::mpi_error const& error) {
      caught = 1;
      raw_code_matches = error.error_code() == MPI_ERR_ARG ? 1 : 0;
      context_matches = error.context() == context ? 1 : 0;
      auto const actual_location = error.location();
      location_is_retained =
          actual_location.line() == expected_location.line() &&
                  actual_location.column() == expected_location.column() &&
                  std::string_view{actual_location.file_name()} ==
                      expected_location.file_name() &&
                  std::string_view{actual_location.function_name()} ==
                      expected_location.function_name()
              ? 1
              : 0;
    }
  }

  auto local = std::array{caught, raw_code_matches, context_matches,
                          location_is_retained,
                          semantic_error_protocol_probe::error_string_calls};
  auto global = std::array{0, 0, 0, 0, 0};
  REQUIRE(PMPI_Allreduce(local.data(), global.data(),
                         static_cast<int>(local.size()), MPI_INT, MPI_SUM,
                         world.native_handle()) == MPI_SUCCESS);
  REQUIRE(global == std::array{world.size(), world.size(), world.size(),
                               world.size(), 0});
}
}  // namespace
