#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>

#include <fmt/base.h>

#include "fixtures/cube_graph.h"

namespace {
[[nodiscard]] auto parse_dimension(std::string_view text,
                                   std::uint64_t& value) noexcept -> bool {
  auto const [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 5) {
    fmt::println(stderr, "usage: {} NX NY NZ OUTPUT.graph",
                 argc > 0 ? argv[0] : "kahip_cube_generator");
    return 64;
  }

  auto dimensions = parhip::testing::cube_dimensions{};
  if (!parse_dimension(argv[1], dimensions.nx) ||
      !parse_dimension(argv[2], dimensions.ny) ||
      !parse_dimension(argv[3], dimensions.nz)) {
    fmt::println(stderr, "cube dimensions must be unsigned integers");
    return 64;
  }

  try {
    auto const graph = parhip::testing::cube_graph{dimensions};
    auto output = std::ofstream{argv[4], std::ios::out | std::ios::trunc};
    if (!output) {
      fmt::println(stderr, "cannot open cube graph output '{}'", argv[4]);
      return 1;
    }
    graph.write_metis(output);
    fmt::println("generated {} vertices and {} undirected edges in {}",
                 graph.vertex_count(), graph.undirected_edge_count(), argv[4]);
  } catch (std::exception const& error) {
    fmt::println(stderr, "cannot generate cube graph: {}", error.what());
    return 1;
  }
}
