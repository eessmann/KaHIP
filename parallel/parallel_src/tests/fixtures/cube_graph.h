#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>

namespace parhip::testing {
struct cube_dimensions final {
  std::uint64_t nx;
  std::uint64_t ny;
  std::uint64_t nz;
};

class cube_neighbors final {
 public:
  using value_type = std::uint64_t;
  using const_iterator = value_type const*;

  [[nodiscard]] auto begin() const noexcept -> const_iterator {
    return values_.data();
  }
  [[nodiscard]] auto end() const noexcept -> const_iterator {
    return values_.data() + size_;
  }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto values() const noexcept -> std::span<value_type const> {
    return {begin(), end()};
  }

 private:
  friend class cube_graph;

  std::array<value_type, 6> values_{};
  std::size_t size_{};
};

class cube_graph final {
 public:
  using vertex_id = std::uint64_t;

  explicit cube_graph(cube_dimensions dimensions);

  [[nodiscard]] auto dimensions() const noexcept -> cube_dimensions {
    return dimensions_;
  }
  [[nodiscard]] auto vertex_count() const noexcept -> std::uint64_t {
    return vertex_count_;
  }
  [[nodiscard]] auto undirected_edge_count() const noexcept -> std::uint64_t {
    return undirected_edge_count_;
  }
  [[nodiscard]] auto cell_id(std::uint64_t x,
                             std::uint64_t y,
                             std::uint64_t z) const -> vertex_id;
  [[nodiscard]] auto neighbors(vertex_id vertex) const -> cube_neighbors;

  void write_metis(std::ostream& output) const;

 private:
  cube_dimensions dimensions_;
  std::uint64_t xy_plane_;
  std::uint64_t vertex_count_;
  std::uint64_t undirected_edge_count_;
};
}  // namespace parhip::testing
