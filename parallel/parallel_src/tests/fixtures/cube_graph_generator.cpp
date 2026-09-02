#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <string_view>
#include <system_error>

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
    std::cerr << "usage: "
              << (argc > 0 ? argv[0] : "kahip_cube_generator")
              << " NX NY NZ OUTPUT.graph\n";
    return 64;
  }

  auto dimensions = parhip::testing::cube_dimensions{};
  if (!parse_dimension(argv[1], dimensions.nx) ||
      !parse_dimension(argv[2], dimensions.ny) ||
      !parse_dimension(argv[3], dimensions.nz)) {
    std::cerr << "cube dimensions must be unsigned integers\n";
    return 64;
  }

  try {
    auto const graph = parhip::testing::cube_graph{dimensions};
    auto output = std::ofstream{argv[4], std::ios::out | std::ios::trunc};
    if (!output) {
      std::cerr << "cannot open cube graph output '" << argv[4] << "'\n";
      return 1;
    }
    graph.write_metis(output);
    std::cout << "generated " << graph.vertex_count() << " vertices and "
              << graph.undirected_edge_count() << " undirected edges in "
              << argv[4] << '\n';
  } catch (std::exception const& error) {
    std::cerr << "cannot generate cube graph: " << error.what() << '\n';
    return 1;
  }
}
