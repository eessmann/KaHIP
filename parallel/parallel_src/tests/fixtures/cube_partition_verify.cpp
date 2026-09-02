#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string_view>
#include <system_error>

#include "fixtures/cube_partition.h"

namespace {
template <std::ranges::input_range Range>
void write_joined(std::ostream& output,
                  Range const& values,
                  std::string_view separator) {
  auto first = true;
  for (auto const& value : values) {
    if (!first) {
      output << separator;
    }
    output << value;
    first = false;
  }
}

[[nodiscard]] auto parse_unsigned(std::string_view text,
                                  std::uint64_t& value) noexcept -> bool {
  auto const [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 7) {
    std::cerr << "usage: "
              << (argc > 0 ? argv[0] : "kahip_cube_partition_verify")
              << " NX NY NZ BLOCKS IMBALANCE_PERCENT PARTITION.txtp\n";
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
    std::cerr << "cube verification arguments must be unsigned integers\n";
    return 64;
  }

  try {
    auto const graph = parhip::testing::cube_graph{dimensions};
    auto input = std::ifstream{argv[6]};
    if (!input) {
      std::cerr << "cannot open partition '" << argv[6] << "'\n";
      return 1;
    }
    auto const partition =
        parhip::testing::read_text_partition(input, graph.vertex_count());
    auto const metrics = parhip::testing::evaluate_cube_partition(
        graph, partition, block_count, imbalance_percent);
    std::cout << "verified vertices=" << graph.vertex_count()
              << " blocks=" << block_count
              << " maximum-block-weight=" << metrics.maximum_block_weight
              << " block-weights=[";
    write_joined(std::cout, metrics.block_weights, ",");
    std::cout << "] weighted-cut=" << metrics.weighted_cut << '\n';
  } catch (std::exception const& error) {
    std::cerr << "partition verification failed: " << error.what() << '\n';
    return 1;
  }
}
