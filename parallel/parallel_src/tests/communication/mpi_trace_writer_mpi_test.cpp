#include <mpi.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "communication/mpi_trace.h"

namespace {
constexpr auto run_id = std::string_view{"writer-fixture"};
constexpr auto path_mismatch_error =
    std::string_view{"MPI trace path differs across communicator ranks"};

[[nodiscard]] auto rank() -> int {
  int result = 0;
  REQUIRE(MPI_Comm_rank(MPI_COMM_WORLD, &result) == MPI_SUCCESS);
  return result;
}

void require_two_ranks() {
  int size = 0;
  REQUIRE(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
  REQUIRE(size == 2);
}

[[nodiscard]] auto test_base(std::string_view suffix) -> std::string {
  auto const* root = std::getenv("KAHIP_TRACE_WRITER_TEST_BASE");
  REQUIRE(root != nullptr);
  return std::string{root} + "-" + std::string{suffix};
}

[[nodiscard]] auto rank_files(std::vector<std::string> const& bases)
    -> std::vector<std::string> {
  auto files = std::vector<std::string>{};
  for (auto const& base : bases) {
    for (auto target_rank = 0; target_rank < 2; ++target_rank) {
      files.push_back(parhip::mpi::trace::rank_file_path(
          base, run_id, target_rank));
    }
  }
  return files;
}

void remove_files(std::vector<std::string> const& files, int local_rank) {
  if (local_rank == 0) {
    for (auto const& file : files) {
      static_cast<void>(std::filesystem::remove(file));
    }
  }
  REQUIRE(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
}

void set_common_run_id() {
  REQUIRE(setenv("KAHIP_MPI_TRACE_RUN_ID", run_id.data(), 1) == 0);
}

[[nodiscard]] auto write_and_capture_error() -> std::string {
  parhip::mpi::trace::reset();
  try {
    parhip::mpi::trace::write_rank_file_if_requested(MPI_COMM_WORLD);
  } catch (std::runtime_error const& error) {
    return error.what();
  }
  return {};
}

void check_no_files(std::vector<std::string> const& files) {
  for (auto const& file : files) {
    CHECK_FALSE(std::filesystem::exists(file));
  }
}
}  // namespace

TEST_CASE("trace writer rejects rank-local enablement mismatch",
          "[mpi][trace][writer][path]") {
  require_two_ranks();
  auto const local_rank = rank();
  auto const base = test_base("presence-mismatch");
  auto const files = rank_files({base});
  remove_files(files, local_rank);
  set_common_run_id();
  if (local_rank == 0) {
    REQUIRE(unsetenv("KAHIP_MPI_TRACE_PATH") == 0);
  } else {
    REQUIRE(setenv("KAHIP_MPI_TRACE_PATH", base.c_str(), 1) == 0);
  }

  auto const error = write_and_capture_error();
  CHECK(error == path_mismatch_error);
  REQUIRE(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
  check_no_files(files);
  remove_files(files, local_rank);
}

TEST_CASE("trace writer rejects different rank-local base paths",
          "[mpi][trace][writer][path]") {
  require_two_ranks();
  auto const local_rank = rank();
  auto const first = test_base("path-a");
  auto const second = test_base("path-b");
  auto const files = rank_files({first, second});
  remove_files(files, local_rank);
  set_common_run_id();
  auto const& local_path = local_rank == 0 ? first : second;
  REQUIRE(setenv("KAHIP_MPI_TRACE_PATH", local_path.c_str(), 1) == 0);

  auto const error = write_and_capture_error();
  CHECK(error == path_mismatch_error);
  REQUIRE(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
  check_no_files(files);
  remove_files(files, local_rank);
}

TEST_CASE("trace writer returns collectively when tracing is unset",
          "[mpi][trace][writer][path]") {
  require_two_ranks();
  auto const local_rank = rank();
  auto const base = test_base("all-unset");
  auto const files = rank_files({base});
  remove_files(files, local_rank);
  set_common_run_id();
  REQUIRE(unsetenv("KAHIP_MPI_TRACE_PATH") == 0);

  CHECK(write_and_capture_error().empty());
  REQUIRE(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
  check_no_files(files);
  remove_files(files, local_rank);
}

TEST_CASE("trace writer uses one agreed base path",
          "[mpi][trace][writer][path]") {
  require_two_ranks();
  auto const local_rank = rank();
  auto const base = test_base("common");
  auto const files = rank_files({base});
  remove_files(files, local_rank);
  set_common_run_id();
  REQUIRE(setenv("KAHIP_MPI_TRACE_PATH", base.c_str(), 1) == 0);

  CHECK(write_and_capture_error().empty());
  REQUIRE(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
  auto const expected =
      std::string{"kahip-mpi-trace-v3 upstream="
                  "5935f349f65f1788a9b68fcf6d853e698d86956d\n"};
  for (auto const& file : files) {
    REQUIRE(std::filesystem::exists(file));
    auto input = std::ifstream{file, std::ios::binary};
    auto contents = std::string{std::istreambuf_iterator<char>{input}, {}};
    CHECK(contents == expected);
  }
  remove_files(files, local_rank);
}
