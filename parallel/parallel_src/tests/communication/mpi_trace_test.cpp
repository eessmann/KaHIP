#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "communication/mpi_trace.h"

TEST_CASE("MPI trace records are canonical and byte comparable",
          "[mpi][trace]") {
  using parhip::mpi::trace::epoch;
  using parhip::mpi::trace::hierarchy_position;
  using parhip::mpi::trace::record;
  auto const input = hierarchy_position{
      .cycle = 0,
      .level = 0,
      .epoch_id = epoch::input,
      .iteration = 0,
      .round = 0};
  auto const contraction = hierarchy_position{
      .cycle = 0,
      .level = 2,
      .epoch_id = epoch::contraction,
      .iteration = 0,
      .round = 0};
  auto const projection = hierarchy_position{
      .cycle = 0,
      .level = 2,
      .epoch_id = epoch::projection,
      .iteration = 0,
      .round = 0};
  auto const final = hierarchy_position{
      .cycle = 0,
      .level = 0,
      .epoch_id = epoch::final_partition,
      .iteration = 0,
      .round = 0};
  auto records = std::vector<record>{
      parhip::mpi::trace::final_partition(final, 9, 1, 1),
      parhip::mpi::trace::projection_reply(projection, 3, 0, 1, 8, 41),
      parhip::mpi::trace::graph_distribution_edge(input, 7, 1, 8, 4),
      parhip::mpi::trace::quotient_edge(contraction, 4, 1, 2, 5),
      parhip::mpi::trace::graph_distribution_node(input, 7, 1, 3),
      parhip::mpi::trace::projection_request(projection, 3, 0, 1, 8),
      parhip::mpi::trace::contraction_label(contraction, 7, 1, 19, 4),
      parhip::mpi::trace::ghost_update(projection, 8, 1, 0, 41),
      parhip::mpi::trace::quotient_node_weight(contraction, 4, 1, 6),
      parhip::mpi::trace::block_propagation(contraction, 4, 1, 1, 1)};

  auto const expected = std::string{
      "kahip-mpi-trace-v3 upstream="
      "5935f349f65f1788a9b68fcf6d853e698d86956d\n"
      "graph-distribution-node cycle=0 level=0 epoch=input iteration=0 round=0 global=7 "
      "owner=1 requester=- receiver=1 key=owner:1 weight=3\n"
      "graph-distribution-edge cycle=0 level=0 epoch=input iteration=0 round=0 global=7 "
      "owner=1 requester=- receiver=1 key=target:8 weight=4\n"
      "contraction-label cycle=0 level=2 epoch=contraction iteration=0 round=0 global=7 "
      "owner=1 requester=- receiver=1 key=label:19 coarse=4\n"
      "quotient-node-weight cycle=0 level=2 epoch=contraction iteration=0 round=0 global=4 "
      "owner=1 requester=- receiver=1 key=node weight=6\n"
      "quotient-edge cycle=0 level=2 epoch=contraction iteration=0 round=0 global=4 "
      "owner=1 requester=- receiver=1 key=target:2 weight=5\n"
      "projection-request cycle=0 level=2 epoch=projection iteration=0 round=0 global=8 "
      "owner=1 requester=0 receiver=1 key=request:3 requester=0 owner=1\n"
      "projection-reply cycle=0 level=2 epoch=projection iteration=0 round=0 global=8 "
      "owner=1 requester=0 receiver=0 key=request:3 requester=0 owner=1 "
      "label=41\n"
      "ghost-update cycle=0 level=2 epoch=projection iteration=0 round=0 global=8 "
      "owner=1 requester=- receiver=0 key=label label=41\n"
      "block-propagation cycle=0 level=2 epoch=contraction iteration=0 round=0 global=4 "
      "owner=1 requester=- receiver=1 key=block block=1\n"
      "final-partition cycle=0 level=0 epoch=final-partition iteration=0 round=0 global=9 "
      "owner=1 requester=- receiver=1 key=partition block=1\n"};

  REQUIRE(parhip::mpi::trace::canonical_text(records) == expected);

  std::ranges::reverse(records);
  REQUIRE(parhip::mpi::trace::canonical_text(records) == expected);
}

TEST_CASE("MPI trace exposes every Task 5 stage schema", "[mpi][trace]") {
  constexpr auto expected = std::array{
      parhip::mpi::trace::stage::graph_distribution_node,
      parhip::mpi::trace::stage::graph_distribution_edge,
      parhip::mpi::trace::stage::contraction_label,
      parhip::mpi::trace::stage::quotient_node_weight,
      parhip::mpi::trace::stage::quotient_edge,
      parhip::mpi::trace::stage::projection_request,
      parhip::mpi::trace::stage::projection_reply,
      parhip::mpi::trace::stage::ghost_update,
      parhip::mpi::trace::stage::block_propagation,
      parhip::mpi::trace::stage::final_partition};

  STATIC_REQUIRE(parhip::mpi::trace::all_stages == expected);
}

TEST_CASE("MPI trace distinguishes hierarchy levels and semantic receivers",
          "[mpi][trace]") {
  using parhip::mpi::trace::epoch;
  using parhip::mpi::trace::hierarchy_position;

  auto const level_one = hierarchy_position{
      .cycle = 1,
      .level = 1,
      .epoch_id = epoch::projection,
      .iteration = 0,
      .round = 0};
  auto const level_two = hierarchy_position{
      .cycle = 1,
      .level = 2,
      .epoch_id = epoch::projection,
      .iteration = 0,
      .round = 0};
  auto const records = std::vector<parhip::mpi::trace::record>{
      parhip::mpi::trace::quotient_node_weight(level_two, 9, 0, 7),
      parhip::mpi::trace::quotient_node_weight(level_one, 9, 0, 7),
      parhip::mpi::trace::ghost_update(level_two, 9, 1, 2, 7),
      parhip::mpi::trace::ghost_update(level_two, 9, 1, 0, 7)};

  auto const expected = std::string{
      "kahip-mpi-trace-v3 upstream="
      "5935f349f65f1788a9b68fcf6d853e698d86956d\n"
      "quotient-node-weight cycle=1 level=1 epoch=projection iteration=0 round=0 "
      "global=9 owner=0 requester=- receiver=0 key=node weight=7\n"
      "quotient-node-weight cycle=1 level=2 epoch=projection iteration=0 round=0 "
      "global=9 owner=0 requester=- receiver=0 key=node weight=7\n"
      "ghost-update cycle=1 level=2 epoch=projection iteration=0 round=0 global=9 "
      "owner=1 requester=- receiver=0 key=label label=7\n"
      "ghost-update cycle=1 level=2 epoch=projection iteration=0 round=0 global=9 "
      "owner=1 requester=- receiver=2 key=label label=7\n"};

  REQUIRE(parhip::mpi::trace::canonical_text(records) == expected);
}

TEST_CASE("MPI trace run IDs make rank filenames collision-resistant",
          "[mpi][trace]") {
  auto const slash =
      parhip::mpi::trace::rank_file_path("trace/output", "job/42", 3);
  auto const question =
      parhip::mpi::trace::rank_file_path("trace/output", "job?42", 3);

  REQUIRE(slash ==
          parhip::mpi::trace::rank_file_path("trace/output", "job/42", 3));
  REQUIRE(slash != question);
  REQUIRE(slash.starts_with("trace/output.run-job_42-"));
  REQUIRE(slash.ends_with(".rank3.trace"));
}
