#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "communication/mpi_trace.h"

TEST_CASE("MPI trace records are canonical and byte comparable",
          "[mpi][trace]") {
  using parhip::mpi::trace::record;
  auto records = std::vector<record>{
      parhip::mpi::trace::final_partition(9, 1),
      parhip::mpi::trace::projection_reply(3, 1, 0, 8, 41),
      parhip::mpi::trace::graph_distribution_edge(7, 8, 4),
      parhip::mpi::trace::quotient_edge(4, 2, 5),
      parhip::mpi::trace::graph_distribution_node(7, 1, 3),
      parhip::mpi::trace::projection_request(3, 0, 1, 8),
      parhip::mpi::trace::contraction_label(7, 19, 4),
      parhip::mpi::trace::ghost_update(8, 1, 41),
      parhip::mpi::trace::quotient_node_weight(4, 6),
      parhip::mpi::trace::block_propagation(4, 1)};

  auto const expected = std::string{
      "kahip-mpi-trace-v1 upstream="
      "5935f349f65f1788a9b68fcf6d853e698d86956d\n"
      "graph-distribution-node global=7 key=owner:1 weight=3\n"
      "graph-distribution-edge global=7 key=target:8 weight=4\n"
      "contraction-label global=7 key=label:19 coarse=4\n"
      "quotient-node-weight global=4 key=node weight=6\n"
      "quotient-edge global=4 key=target:2 weight=5\n"
      "projection-request global=8 key=request:3 source=0 destination=1\n"
      "projection-reply global=8 key=request:3 source=1 destination=0 label=41\n"
      "ghost-update global=8 key=owner:1 label=41\n"
      "block-propagation global=4 key=block block=1\n"
      "final-partition global=9 key=partition block=1\n"};

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
