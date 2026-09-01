#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string_view>
#include <system_error>

#include <fmt/base.h>
#include <fmt/ranges.h>

#include "fixtures/cube_partition.h"

namespace {
[[nodiscard]] auto parse_unsigned(std::string_view text,
                                  std::uint64_t& value) noexcept -> bool {
  auto const [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 7) {
    fmt::println(stderr,
                 "usage: {} NX NY NZ BLOCKS IMBALANCE_PERCENT PARTITION.txtp",
                 argc > 0 ? argv[0] : "kahip_cube_partition_verify");
    return 64;
  }

  auto dimensions = parhip::testing::cube_dimensions{};
  auto block_count = std::uint64_t{};
  auto imbalance_percent = std::uint64_t{};
  if (!parse_unsigned(argv[1], dimensions.nx) ||
      !parse_unsigned(argv[2], dimensions.ny) ||
      !parse_unsigned(argv[3], dimensions.nz) ||
      !parse_unsigned(argv[4], block_count) ||
      !parse_unsigned(argv[5], imbalance_percent)) {
    fmt::println(stderr,
                 "cube verification arguments must be unsigned integers");
    return 64;
  }

  try {
    auto const graph = parhip::testing::cube_graph{dimensions};
    auto input = std::ifstream{argv[6]};
    if (!input) {
      fmt::println(stderr, "cannot open partition '{}'", argv[6]);
      return 1;
    }
    auto const partition =
        parhip::testing::read_text_partition(input, graph.vertex_count());
    auto const metrics = parhip::testing::evaluate_cube_partition(
        graph, partition, block_count, imbalance_percent);
    fmt::println(
        "verified vertices={} blocks={} maximum-block-weight={} "
        "block-weights=[{}] weighted-cut={}",
        graph.vertex_count(), block_count, metrics.maximum_block_weight,
        fmt::join(metrics.block_weights, ","), metrics.weighted_cut);
  } catch (std::exception const& error) {
    fmt::println(stderr, "partition verification failed: {}", error.what());
    return 1;
  }
}
