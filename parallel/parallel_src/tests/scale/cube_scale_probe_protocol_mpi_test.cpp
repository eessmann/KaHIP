#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "scale/cube_scale_probe_core.h"
#include "scale/cube_scale_probe_protocol.h"

namespace {
using parhip::scale_probe::capacity_reason;
using parhip::scale_probe::remote_label_reply;
using parhip::scale_probe::remote_label_request;

[[nodiscard]] auto world_rank() -> int {
  auto rank = -1;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
  return rank;
}

[[nodiscard]] auto world_size() -> int {
  auto size = 0;
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  return size;
}

[[nodiscard]] auto boundaries_for(std::uint64_t vertices, int size)
    -> std::vector<std::uint64_t> {
  auto boundaries =
      std::vector<std::uint64_t>(static_cast<std::size_t>(size) + 1);
  REQUIRE(parhip::scale_probe::write_balanced_boundaries(vertices, boundaries));
  return boundaries;
}
}  // namespace

TEST_CASE("remote request validation pins full ordered semantic keys",
          "[cube-scale][protocol][injection]") {
  auto const boundaries = std::array<std::uint64_t, 3>{0, 4, 8};
  auto const valid =
      std::array{remote_label_request{0, 4}, remote_label_request{1, 5},
                 remote_label_request{2, 6}, remote_label_request{3, 7}};
  auto const context = parhip::scale_probe::request_validation_context{
      .side = 2,
      .boundaries = boundaries,
      .sender = 0,
      .receiver = 1,
      .window_first = 0,
      .window_end = 4,
  };
  REQUIRE(parhip::scale_probe::request_segment_is_valid(valid, context));

  auto injected = valid;
  injected[0].source = 4;
  CHECK_FALSE(parhip::scale_probe::request_segment_is_valid(injected, context));
  injected = valid;
  injected[0].target = 3;
  CHECK_FALSE(parhip::scale_probe::request_segment_is_valid(injected, context));
  injected = valid;
  injected[0].target = 6;
  CHECK_FALSE(parhip::scale_probe::request_segment_is_valid(injected, context));
  injected = valid;
  injected[1] = injected[0];
  CHECK_FALSE(parhip::scale_probe::request_segment_is_valid(injected, context));
  injected = valid;
  std::swap(injected[0], injected[1]);
  CHECK_FALSE(parhip::scale_probe::request_segment_is_valid(injected, context));

  auto wrong_sender = context;
  wrong_sender.sender = 1;
  CHECK_FALSE(
      parhip::scale_probe::request_segment_is_valid(valid, wrong_sender));
  auto wrong_window = context;
  wrong_window.window_end = 3;
  CHECK_FALSE(
      parhip::scale_probe::request_segment_is_valid(valid, wrong_window));
}

TEST_CASE("reply validation rejects count key and label injection",
          "[cube-scale][protocol][injection]") {
  auto const requests =
      std::array{remote_label_request{0, 4}, remote_label_request{1, 5},
                 remote_label_request{2, 6}};
  auto const valid =
      std::array{remote_label_reply{0, 4, 1}, remote_label_reply{1, 5, 0},
                 remote_label_reply{2, 6, 1}};
  REQUIRE(parhip::scale_probe::reply_segment_is_valid(requests, valid, 2));
  CHECK_FALSE(parhip::scale_probe::reply_segment_is_valid(
      requests, std::span{valid}.first(2), 2));

  auto injected = valid;
  injected[1].source = 0;
  CHECK_FALSE(
      parhip::scale_probe::reply_segment_is_valid(requests, injected, 2));
  injected = valid;
  injected[1].target = 6;
  CHECK_FALSE(
      parhip::scale_probe::reply_segment_is_valid(requests, injected, 2));
  injected = valid;
  injected[1].label = 2;
  CHECK_FALSE(
      parhip::scale_probe::reply_segment_is_valid(requests, injected, 2));
}

TEST_CASE("protocol capacity accounts for every count address and byte bound",
          "[cube-scale][protocol][capacity]") {
  auto const native =
      parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944);
  REQUIRE(native.safe());
  CHECK(native.maximum_send == 196'608);
  CHECK(native.maximum_receive == 280'755);

  auto limits = parhip::scale_probe::protocol_capacity_limits{};
  struct boundary_case final {
    std::uint64_t parhip::scale_probe::protocol_capacity_limits::* member;
    std::uint64_t exact;
    capacity_reason reason;
  };
  auto const cases = std::array{
      boundary_case{&parhip::scale_probe::protocol_capacity_limits::int_max,
                    280'755, capacity_reason::int_count},
      boundary_case{&parhip::scale_probe::protocol_capacity_limits::size_max,
                    280'755, capacity_reason::size_count},
      boundary_case{
          &parhip::scale_probe::protocol_capacity_limits::mpi_aint_max, 280'755,
          capacity_reason::mpi_aint_offset},
      boundary_case{
          &parhip::scale_probe::protocol_capacity_limits::request_elements,
          280'755, capacity_reason::request_vector},
      boundary_case{
          &parhip::scale_probe::protocol_capacity_limits::reply_elements,
          280'755, capacity_reason::reply_vector},
      boundary_case{
          &parhip::scale_probe::protocol_capacity_limits::request_bytes,
          280'755 * sizeof(remote_label_request),
          capacity_reason::request_bytes},
      boundary_case{&parhip::scale_probe::protocol_capacity_limits::reply_bytes,
                    280'755 * sizeof(remote_label_reply),
                    capacity_reason::reply_bytes},
  };
  for (auto const& test : cases) {
    limits = parhip::scale_probe::protocol_capacity_limits{};
    limits.*test.member = test.exact;
    CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
              .safe());
    limits.*test.member = test.exact - 1;
    CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
              .reason == test.reason);
  }
  CHECK(parhip::scale_probe::protocol_capacity(0, 1, 1).reason ==
        capacity_reason::zero_window);
  CHECK(parhip::scale_probe::protocol_capacity(
            std::numeric_limits<std::uint64_t>::max(), 1, 1)
            .reason == capacity_reason::count_overflow);

  limits = parhip::scale_probe::protocol_capacity_limits{};
  limits.metadata_elements = 10'944;
  CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
            .safe());
  limits.metadata_elements = 10'943;
  CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
            .reason == capacity_reason::metadata_vector);
  limits = parhip::scale_probe::protocol_capacity_limits{};
  limits.boundary_elements = 10'945;
  CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
            .safe());
  limits.boundary_elements = 10'944;
  CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
            .reason == capacity_reason::boundary_vector);

  auto const metadata_bytes = std::max(
      {std::uint64_t{10'945} * sizeof(std::uint64_t),
       std::uint64_t{10'944} * sizeof(std::size_t),
       std::uint64_t{10'944} * sizeof(std::uint64_t),
       std::uint64_t{10'944} * sizeof(int),
       std::uint64_t{10'944} * sizeof(MPI_Count),
       std::uint64_t{10'944} * sizeof(MPI_Aint),
       std::uint64_t{10'944} * sizeof(std::vector<remote_label_request>),
       std::uint64_t{10'944} * sizeof(std::vector<remote_label_reply>)});
  limits = parhip::scale_probe::protocol_capacity_limits{};
  limits.metadata_bytes = metadata_bytes;
  CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
            .safe());
  limits.metadata_bytes = metadata_bytes - 1;
  CHECK(parhip::scale_probe::protocol_capacity(65'536, 93'585, 10'944, limits)
            .reason == capacity_reason::metadata_bytes);

  limits = parhip::scale_probe::protocol_capacity_limits{};
  limits.int_max = 2;
  CHECK(parhip::scale_probe::protocol_capacity(1, 0, 3, limits).reason ==
        capacity_reason::int_count);
}

TEST_CASE("one-rank protocol keeps every oriented edge local",
          "[cube-scale][protocol][mpi]") {
  if (world_size() != 1) {
    return;
  }
  auto const boundaries = boundaries_for(64, 1);
  auto const graph = parhip::scale_probe::build_local_cube_csr(4, 0, 64);
  auto const partition = std::vector<unsigned long long>(64, 0);
  auto const result = parhip::scale_probe::independent_cut(
      4, graph, partition, boundaries, 1, MPI_COMM_WORLD);
  CHECK(result.cut == 0);
  CHECK(result.rounds == 1);
  CHECK(result.maximum_send == 0);
  CHECK(result.maximum_receive == 0);
}

TEST_CASE(
    "windowed remote labels preserve the exact cube cut through MPI-3 chunks",
    "[cube-scale][protocol][mpi][chunking]") {
  auto const rank = world_rank();
  auto const size = world_size();
  auto const boundaries = boundaries_for(64, size);
  auto const first = boundaries[static_cast<std::size_t>(rank)];
  auto const end = boundaries[static_cast<std::size_t>(rank) + 1];
  auto const graph = parhip::scale_probe::build_local_cube_csr(4, first, end);
  auto partition =
      std::vector<unsigned long long>(static_cast<std::size_t>(end - first));
  for (auto local = std::size_t{0}; local < partition.size(); ++local) {
    partition[local] = (first + local) % 2;
  }
  auto const options = parhip::scale_probe::cut_exchange_options{
      .source_window = 3,
      .mpi3_round_ceiling = 1,
      .force_mpi3 = true,
  };
  auto const result = parhip::scale_probe::independent_cut(
      4, graph, partition, boundaries, 2, MPI_COMM_WORLD, options);
  CHECK(result.cut == 48);
  CHECK(result.rounds ==
        static_cast<std::uint64_t>((64 + size - 1) / size + 2) / 3);
  CHECK(result.maximum_send <= 9);
  CHECK(result.maximum_receive <=
        3 * static_cast<std::uint64_t>(partition.size()));
}

TEST_CASE("more ranks than cube vertices keep empty ranks in lockstep",
          "[cube-scale][protocol][mpi][zero-work]") {
  if (world_size() != 3) {
    return;
  }
  auto const rank = world_rank();
  auto const boundaries = boundaries_for(1, 3);
  REQUIRE(boundaries == std::vector<std::uint64_t>{0, 0, 0, 1});
  auto const first = boundaries[static_cast<std::size_t>(rank)];
  auto const end = boundaries[static_cast<std::size_t>(rank) + 1];
  auto const graph = parhip::scale_probe::build_local_cube_csr(1, first, end);
  auto const partition =
      std::vector<unsigned long long>(static_cast<std::size_t>(end - first), 0);
  auto const result = parhip::scale_probe::independent_cut(
      1, graph, partition, boundaries, 3, MPI_COMM_WORLD,
      parhip::scale_probe::cut_exchange_options{
          .source_window = 1,
          .mpi3_round_ceiling = 1,
          .force_mpi3 = true,
      });
  CHECK(result.cut == 0);
  CHECK(result.rounds == 1);
  CHECK(result.maximum_send == 0);
  CHECK(result.maximum_receive == 0);
  if (rank < 2) {
    CHECK(graph.offsets == std::vector<unsigned long long>{0});
    CHECK(graph.targets.empty());
    CHECK(partition.empty());
  }
}

TEST_CASE("rank-skewed window fails collectively before cut payload",
          "[cube-scale][protocol][mpi][injection]") {
  auto const rank = world_rank();
  auto const size = world_size();
  if (size < 2) {
    return;
  }
  auto const boundaries = boundaries_for(64, size);
  auto const first = boundaries[static_cast<std::size_t>(rank)];
  auto const end = boundaries[static_cast<std::size_t>(rank) + 1];
  auto const graph = parhip::scale_probe::build_local_cube_csr(4, first, end);
  auto const partition =
      std::vector<unsigned long long>(static_cast<std::size_t>(end - first), 0);
  auto caught = 0;
  auto exact_context = 0;
  try {
    static_cast<void>(parhip::scale_probe::independent_cut(
        4, graph, partition, boundaries, 1, MPI_COMM_WORLD,
        parhip::scale_probe::cut_exchange_options{
            .source_window = static_cast<std::uint64_t>(rank + 1),
            .mpi3_round_ceiling = 1,
            .force_mpi3 = true,
        }));
  } catch (parhip::mpi::mpi_error const& error) {
    caught = 1;
    exact_context = error.error_code() == MPI_ERR_ARG &&
                            error.context() ==
                                "cube cut exchange semantic inputs differ "
                                "across communicator"
                        ? 1
                        : 0;
  } catch (...) {
    caught = 1;
  }
  auto local = std::array{caught, exact_context};
  auto global = std::array{0, 0};
  REQUIRE(MPI_Allreduce(local.data(), global.data(),
                        static_cast<int>(local.size()), MPI_INT, MPI_SUM,
                        MPI_COMM_WORLD) == MPI_SUCCESS);
  CHECK(global == std::array{size, size});
}
