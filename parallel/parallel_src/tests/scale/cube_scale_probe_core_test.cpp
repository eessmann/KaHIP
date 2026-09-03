#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "range_owner.h"
#include "scale/cube_scale_probe_core.h"

namespace {
using parhip::scale_probe::cube_counts;
using parhip::scale_probe::digest_lanes;

struct scale_case final {
  std::uint64_t side;
  std::uint32_t ranks;
  cube_counts counts;
  std::uint64_t maximum_local_nodes;
  std::uint64_t bound;
};

constexpr auto cases = std::array{
    scale_case{4, 2, {64, 144, 288}, 32, 32},
    scale_case{10, 5, {1'000, 2'700, 5'400}, 200, 206},
    scale_case{600,
               2'304,
               {216'000'000, 646'920'000, 1'293'840'000},
               93'750,
               96'562},
    scale_case{755,
               4'608,
               {430'368'875, 1'289'396'550, 2'578'793'100},
               93'397,
               96'198},
    scale_case{900,
               7'776,
               {729'000'000, 2'184'570'000, 4'369'140'000},
               93'750,
               96'562},
    scale_case{1'008,
               10'944,
               {1'024'192'512, 3'069'529'344, 6'139'058'688},
               93'585,
               96'392},
};
}  // namespace

TEST_CASE("cube scale arithmetic pins every local and planned tuple",
          "[cube-scale][arithmetic]") {
  for (auto const& test : cases) {
    INFO("side=" << test.side << " ranks=" << test.ranks);
    REQUIRE(parhip::scale_probe::counts_for_side(test.side) == test.counts);
    REQUIRE(parhip::scale_probe::maximum_balanced_slice(
                test.counts.vertices, test.ranks) == test.maximum_local_nodes);
    REQUIRE(parhip::scale_probe::exact_unit_weight_bound(
                test.counts.vertices, test.ranks, 3) == test.bound);
  }
}

TEST_CASE("cube arithmetic rejects empty and overflowing domains",
          "[cube-scale][arithmetic]") {
  CHECK_FALSE(parhip::scale_probe::counts_for_side(0).has_value());
  CHECK_FALSE(
      parhip::scale_probe::counts_for_side(std::uint64_t{1} << 22).has_value());
  CHECK_FALSE(parhip::scale_probe::counts_for_side(
                  std::numeric_limits<std::uint64_t>::max())
                  .has_value());
  CHECK_FALSE(parhip::scale_probe::maximum_balanced_slice(10, 0).has_value());
  CHECK_FALSE(
      parhip::scale_probe::exact_unit_weight_bound(10, 0, 3).has_value());
  CHECK_FALSE(parhip::scale_probe::exact_unit_weight_bound(
                  std::numeric_limits<std::uint64_t>::max(), 1, 3)
                  .has_value());
}

TEST_CASE("independent bound oracle distinguishes accidental two percent",
          "[cube-scale][arithmetic][balance]") {
  CHECK(parhip::scale_probe::exact_unit_weight_bound(216'000'000, 2'304, 2) ==
        95'625);
  CHECK(parhip::scale_probe::exact_unit_weight_bound(216'000'000, 2'304, 3) ==
        96'562);
}

TEST_CASE("balanced boundaries implement overflow-safe floor i N over P",
          "[cube-scale][arithmetic][ownership]") {
  auto boundaries = std::array<std::uint64_t, 5>{};
  REQUIRE(parhip::scale_probe::write_balanced_boundaries(10, boundaries));
  CHECK(boundaries == std::array<std::uint64_t, 5>{0, 2, 5, 7, 10});

  auto sparse = std::array<std::uint64_t, 6>{};
  REQUIRE(parhip::scale_probe::write_balanced_boundaries(3, sparse));
  CHECK(sparse == std::array<std::uint64_t, 6>{0, 0, 1, 1, 2, 3});
  CHECK(kahip::range_owner::from_boundaries(sparse, std::uint64_t{0}) == 1);
  CHECK(kahip::range_owner::from_boundaries(sparse, std::uint64_t{1}) == 3);
  CHECK(kahip::range_owner::from_boundaries(sparse, std::uint64_t{2}) == 4);
  CHECK(kahip::range_owner::from_boundaries(sparse, std::uint64_t{3}) == -1);

  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  CHECK(parhip::scale_probe::balanced_boundary(maximum, 1, 3) ==
        6'148'914'691'236'517'205ULL);
  CHECK(parhip::scale_probe::balanced_boundary(maximum, 2, 3) ==
        12'297'829'382'473'034'410ULL);
  CHECK(parhip::scale_probe::balanced_boundary(maximum, 3, 3) == maximum);
  CHECK(parhip::scale_probe::balanced_boundary(
            maximum, std::numeric_limits<std::uint32_t>::max() - 1,
            std::numeric_limits<std::uint32_t>::max()) ==
        18'446'744'069'414'584'318ULL);
  CHECK_FALSE(parhip::scale_probe::balanced_boundary(10, 0, 0).has_value());
  CHECK_FALSE(parhip::scale_probe::balanced_boundary(10, 5, 4).has_value());
}

TEST_CASE("two-pass local CSR is exact sorted and slice-concatenable",
          "[cube-scale][csr]") {
  auto const require_neighbors = [](std::uint64_t vertex,
                                    auto const& expected) {
    auto const actual = parhip::scale_probe::neighbors_for_vertex(3, vertex);
    REQUIRE(actual.has_value());
    REQUIRE(actual->count == expected.size());
    CHECK(std::ranges::equal(actual->span(), expected));
  };
  require_neighbors(0, std::array<std::uint64_t, 3>{1, 3, 9});
  require_neighbors(1, std::array<std::uint64_t, 4>{0, 2, 4, 10});
  require_neighbors(4, std::array<std::uint64_t, 5>{1, 3, 5, 7, 13});
  require_neighbors(13, std::array<std::uint64_t, 6>{4, 10, 12, 14, 16, 22});

  auto const whole = parhip::scale_probe::build_local_cube_csr(2, 0, 8);
  CHECK(whole.offsets ==
        std::vector<unsigned long long>{0, 3, 6, 9, 12, 15, 18, 21, 24});
  CHECK(whole.targets ==
        std::vector<unsigned long long>{1, 2, 4, 0, 3, 5, 0, 3, 6, 1, 2, 7,
                                        0, 5, 6, 1, 4, 7, 2, 4, 7, 3, 5, 6});

  auto boundaries = std::array<std::uint64_t, 6>{};
  REQUIRE(parhip::scale_probe::write_balanced_boundaries(64, boundaries));
  auto concatenated_offsets = std::vector<unsigned long long>{0};
  auto concatenated_targets = std::vector<unsigned long long>{};
  for (std::size_t rank = 0; rank + 1 < boundaries.size(); ++rank) {
    auto const slice = parhip::scale_probe::build_local_cube_csr(
        4, boundaries[rank], boundaries[rank + 1]);
    for (auto offset : std::span{slice.offsets}.subspan(1)) {
      concatenated_offsets.push_back(
          static_cast<unsigned long long>(concatenated_targets.size()) +
          offset);
    }
    concatenated_targets.insert(concatenated_targets.end(),
                                slice.targets.begin(), slice.targets.end());
  }
  auto const cube4 = parhip::scale_probe::build_local_cube_csr(4, 0, 64);
  REQUIRE(cube4.targets.size() == 288);
  CHECK(concatenated_offsets == cube4.offsets);
  CHECK(concatenated_targets == cube4.targets);
  for (std::size_t vertex = 0; vertex + 1 < cube4.offsets.size(); ++vertex) {
    auto const first = static_cast<std::size_t>(cube4.offsets[vertex]);
    auto const end = static_cast<std::size_t>(cube4.offsets[vertex + 1]);
    CHECK(std::ranges::is_sorted(
        std::span{cube4.targets}.subspan(first, end - first)));
  }
}

TEST_CASE("local CSR rejects invalid and unaddressable slices",
          "[cube-scale][csr]") {
  CHECK_THROWS_AS(parhip::scale_probe::build_local_cube_csr(2, 5, 4),
                  std::invalid_argument);
  CHECK_THROWS_AS(parhip::scale_probe::build_local_cube_csr(2, 0, 9),
                  std::out_of_range);
  CHECK_THROWS_AS(
      parhip::scale_probe::build_local_cube_csr(std::uint64_t{1} << 22, 0, 1),
      std::overflow_error);
}

TEST_CASE("semantic digests have versioned literal lanes and ignore slices",
          "[cube-scale][digest]") {
  auto const expected_graph =
      digest_lanes{{0xef5146193e8c0fefULL, 0x27c7c004c7c7a159ULL,
                    0x2ec68fd76213931eULL, 0x97e4152d3243b55eULL}};
  auto const whole = parhip::scale_probe::graph_digest(2, 0, 8);
  CHECK(whole == expected_graph);
  CHECK((parhip::scale_probe::graph_digest(2, 0, 3) ^
         parhip::scale_probe::graph_digest(2, 3, 8)) == expected_graph);

  constexpr auto partition =
      std::array<std::uint64_t, 8>{0, 0, 0, 0, 1, 1, 1, 1};
  CHECK(parhip::scale_probe::partition_digest(2, 0, partition, 2) ==
        digest_lanes{{0xcfab14550a55a03fULL, 0x6c9ae38680f7894aULL,
                      0x509848c142fe96dbULL, 0xec6f053ec2141019ULL}});
  CHECK((parhip::scale_probe::partition_digest(
             2, 0, std::span{partition}.first(3), 2) ^
         parhip::scale_probe::partition_digest(
             2, 3, std::span{partition}.subspan(3), 2)) ==
        parhip::scale_probe::partition_digest(2, 0, partition, 2));
}
