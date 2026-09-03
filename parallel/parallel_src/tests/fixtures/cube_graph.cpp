#include "fixtures/cube_graph.h"

#include <algorithm>
#include <limits>
#include <ostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace parhip::testing {
namespace {
using count_type = std::uint64_t;

[[nodiscard]] auto checked_multiply(count_type left,
                                    count_type right,
                                    std::string_view quantity) -> count_type {
  if (left != 0 && right > std::numeric_limits<count_type>::max() / left) {
    throw std::overflow_error{std::string{quantity}};
  }
  return left * right;
}

[[nodiscard]] auto checked_add(count_type left,
                               count_type right,
                               std::string_view quantity) -> count_type {
  if (right > std::numeric_limits<count_type>::max() - left) {
    throw std::overflow_error{std::string{quantity}};
  }
  return left + right;
}

[[nodiscard]] auto axis_edge_count(count_type length,
                                   count_type other_first,
                                   count_type other_second) -> count_type {
  return checked_multiply(
      checked_multiply(length - 1, other_first, "cube edge count overflow"),
      other_second, "cube edge count overflow");
}
}  // namespace

cube_graph::cube_graph(cube_dimensions dimensions)
    : dimensions_(dimensions),
      xy_plane_(0),
      vertex_count_(0),
      undirected_edge_count_(0) {
  if (dimensions.nx == 0 || dimensions.ny == 0 || dimensions.nz == 0) {
    throw std::invalid_argument{"cube dimensions must all be positive"};
  }

  xy_plane_ = checked_multiply(dimensions.nx, dimensions.ny,
                               "cube vertex count overflow");
  vertex_count_ =
      checked_multiply(xy_plane_, dimensions.nz, "cube vertex count overflow");
  undirected_edge_count_ = checked_add(
      checked_add(axis_edge_count(dimensions.nx, dimensions.ny, dimensions.nz),
                  axis_edge_count(dimensions.ny, dimensions.nx, dimensions.nz),
                  "cube edge count overflow"),
      axis_edge_count(dimensions.nz, dimensions.nx, dimensions.ny),
      "cube edge count overflow");
}

auto cube_graph::cell_id(std::uint64_t x,
                         std::uint64_t y,
                         std::uint64_t z) const -> vertex_id {
  if (x >= dimensions_.nx || y >= dimensions_.ny || z >= dimensions_.nz) {
    throw std::out_of_range{"cube cell coordinate is outside the graph"};
  }
  return x + dimensions_.nx * (y + dimensions_.ny * z);
}

auto cube_graph::neighbors(vertex_id vertex) const -> cube_neighbors {
  if (vertex >= vertex_count_) {
    throw std::out_of_range{"cube vertex is outside the graph"};
  }

  auto result = cube_neighbors{};
  auto const z = vertex / xy_plane_;
  auto const within_plane = vertex % xy_plane_;
  auto const y = within_plane / dimensions_.nx;
  auto const x = within_plane % dimensions_.nx;
  auto append = [&result](vertex_id neighbor) {
    result.values_[result.size_++] = neighbor;
  };

  if (x > 0) {
    append(vertex - 1);
  }
  if (x + 1 < dimensions_.nx) {
    append(vertex + 1);
  }
  if (y > 0) {
    append(vertex - dimensions_.nx);
  }
  if (y + 1 < dimensions_.ny) {
    append(vertex + dimensions_.nx);
  }
  if (z > 0) {
    append(vertex - xy_plane_);
  }
  if (z + 1 < dimensions_.nz) {
    append(vertex + xy_plane_);
  }

  std::ranges::sort(std::span{result.values_.data(), result.size_});
  return result;
}

void cube_graph::write_metis(std::ostream& output) const {
  output << vertex_count_ << ' ' << undirected_edge_count_ << '\n';
  for (auto vertex = vertex_id{0}; vertex < vertex_count_; ++vertex) {
    auto const adjacent = neighbors(vertex);
    auto separator = std::string_view{};
    for (auto const neighbor : adjacent) {
      output << separator << neighbor + 1;
      separator = " ";
    }
    output << '\n';
  }
  if (!output) {
    throw std::runtime_error{"failed to write cube graph"};
  }
}
}  // namespace parhip::testing
