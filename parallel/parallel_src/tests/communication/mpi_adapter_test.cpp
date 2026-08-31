#include <mpi.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "communication/mpi_adapter.h"

namespace test_support {
struct wire_entry {
  std::uint64_t id;
  int owner;
  double weight;

  auto operator==(wire_entry const&) const -> bool = default;
};

struct non_default_wire_entry {
  non_default_wire_entry() = delete;

  std::uint64_t id;
  int owner;
};
}  // namespace test_support

template <>
struct parhip::mpi::wire_members<test_support::wire_entry> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &test_support::wire_entry::id,
      &test_support::wire_entry::owner,
      &test_support::wire_entry::weight);
};

template <>
struct parhip::mpi::wire_members<test_support::non_default_wire_entry> {
  inline static constexpr auto value = boost::hana::make_tuple(
      &test_support::non_default_wire_entry::id,
      &test_support::non_default_wire_entry::owner);
};

namespace {
using parhip::mpi::communicator;
using parhip::mpi::communicator_view;
using parhip::mpi::all_to_all_v;
using parhip::mpi::collective_options;
using parhip::mpi::make_mpi_datatype;
using parhip::mpi::segmented_buffer;
using parhip::mpi::topology;

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

TEST_CASE("explicit Hana wire metadata produces array-safe extent",
          "[unit][mpi]") {
  STATIC_REQUIRE(parhip::mpi::mpi_wire_datatype<test_support::wire_entry>);
  STATIC_REQUIRE(std::is_standard_layout_v<test_support::wire_entry>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<test_support::wire_entry>);

  auto datatype = make_mpi_datatype<test_support::wire_entry>();
  REQUIRE(datatype.owns_handle());

  MPI_Aint lower_bound = -1;
  MPI_Aint extent = -1;
  REQUIRE(MPI_Type_get_extent(datatype.native_handle(), &lower_bound, &extent) ==
          MPI_SUCCESS);
  REQUIRE(lower_bound == 0);
  REQUIRE(extent == static_cast<MPI_Aint>(sizeof(test_support::wire_entry)));
}

TEST_CASE("wire records need no default constructor", "[unit][mpi]") {
  STATIC_REQUIRE(
      parhip::mpi::mpi_wire_datatype<test_support::non_default_wire_entry>);
  STATIC_REQUIRE(
      std::is_standard_layout_v<test_support::non_default_wire_entry>);
  STATIC_REQUIRE(
      std::is_trivially_copyable_v<test_support::non_default_wire_entry>);

  auto datatype = make_mpi_datatype<test_support::non_default_wire_entry>();
  REQUIRE(datatype.owns_handle());
}

TEST_CASE("segmented buffers expose canonical contiguous spans",
          "[unit][mpi]") {
  auto buffer = segmented_buffer<int>::from_segments(
      std::vector<std::vector<int>>{{1, 2}, {}, {3, 4, 5}});

  REQUIRE(buffer.storage() == std::vector<int>{1, 2, 3, 4, 5});
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
  REQUIRE(received.has_canonical_layout(
      static_cast<std::size_t>(world.size())));
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

  REQUIRE(received.has_canonical_layout(
      static_cast<std::size_t>(world.size())));
  for (int source = 0; source < world.size(); ++source) {
    if (source == rank) {
      REQUIRE(std::ranges::equal(
          received.segment(static_cast<std::size_t>(source)),
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

  auto received = all_to_all_v(
      segmented_buffer<int>::from_segments(segments), world);

  REQUIRE(received.has_canonical_layout(
      static_cast<std::size_t>(world.size())));
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

  auto received = all_to_all_v(
      segmented_buffer<int>::from_segments(segments), world);

  if (rank == 0) {
    REQUIRE(received.counts()[0] == 0);
    for (int source = 1; source < world.size(); ++source) {
      REQUIRE(std::ranges::equal(
          received.segment(static_cast<std::size_t>(source)),
          std::array{source * 7}));
    }
  } else {
    REQUIRE(received.storage().empty());
  }
}

TEST_CASE("dense validation failure propagates to every rank",
          "[unit][mpi]") {
  communicator_view const world{MPI_COMM_WORLD};
  if (world.size() == 1) {
    return;
  }

  std::vector<std::size_t> counts(static_cast<std::size_t>(world.size()), 0);
  std::vector<std::size_t> offsets(static_cast<std::size_t>(world.size()), 0);
  if (world.rank() == 0) {
    offsets[0] = 1;
  }
  segmented_buffer<int> malformed{{}, std::move(counts), std::move(offsets)};

  bool threw = false;
  try {
    static_cast<void>(all_to_all_v(std::move(malformed), world));
  } catch (parhip::mpi::mpi_error const& error) {
    threw = true;
    REQUIRE(error.error_code() == MPI_ERR_ARG);
    REQUIRE(error.context().find("collective input validation") !=
            std::string_view::npos);
    REQUIRE(error.location().line() > 0);
  }
  REQUIRE(threw);
}

TEST_CASE("forced MPI-3 bounded rounds preserve every element",
          "[unit][mpi]") {
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
      segmented_buffer<int>::from_segments(segments),
      world,
      collective_options{.mpi3_round_ceiling = 2, .force_mpi3 = true});

  for (int source = 0; source < world.size(); ++source) {
    std::array expected{source * 10'000 + rank * 100,
                        source * 10'000 + rank * 100 + 1,
                        source * 10'000 + rank * 100 + 2,
                        source * 10'000 + rank * 100 + 3,
                        source * 10'000 + rank * 100 + 4};
    REQUIRE(std::ranges::equal(
        received.segment(static_cast<std::size_t>(source)), expected));
  }
}
}  // namespace
