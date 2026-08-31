#pragma once

#include <concepts>
#include <cstddef>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace parhip::mpi {
template <typename T>
class segmented_buffer {
public:
  segmented_buffer() = default;

  segmented_buffer(std::vector<T> storage,
                   std::vector<std::size_t> counts,
                   std::vector<std::size_t> offsets)
      : storage_(std::move(storage)),
        counts_(std::move(counts)),
        offsets_(std::move(offsets)) {}

  template <std::ranges::forward_range Segments>
    requires std::ranges::forward_range<std::ranges::range_value_t<Segments>> &&
             std::convertible_to<
                 std::ranges::range_value_t<std::ranges::range_value_t<Segments>>,
                 T>
  [[nodiscard]] static auto from_segments(Segments const& segments)
      -> segmented_buffer {
    std::vector<T> storage;
    std::vector<std::size_t> counts;
    std::vector<std::size_t> offsets;
    counts.reserve(static_cast<std::size_t>(std::ranges::distance(segments)));
    offsets.reserve(counts.capacity());

    for (auto const& segment : segments) {
      offsets.push_back(storage.size());
      counts.push_back(
          static_cast<std::size_t>(std::ranges::distance(segment)));
      storage.insert(storage.end(), std::ranges::begin(segment),
                     std::ranges::end(segment));
    }
    return segmented_buffer{
        std::move(storage), std::move(counts), std::move(offsets)};
  }

  [[nodiscard]] auto storage() noexcept -> std::vector<T>& { return storage_; }
  [[nodiscard]] auto storage() const noexcept -> std::vector<T> const& {
    return storage_;
  }
  [[nodiscard]] auto counts() const noexcept
      -> std::vector<std::size_t> const& {
    return counts_;
  }
  [[nodiscard]] auto offsets() const noexcept
      -> std::vector<std::size_t> const& {
    return offsets_;
  }
  [[nodiscard]] auto segment_count() const noexcept -> std::size_t {
    return counts_.size();
  }

  [[nodiscard]] auto segment(std::size_t index) -> std::span<T> {
    validate_segment(index);
    return std::span<T>{storage_}.subspan(offsets_[index], counts_[index]);
  }
  [[nodiscard]] auto segment(std::size_t index) const -> std::span<T const> {
    validate_segment(index);
    return std::span<T const>{storage_}.subspan(offsets_[index], counts_[index]);
  }

  [[nodiscard]] auto has_canonical_layout(std::size_t expected_segments) const
      noexcept -> bool {
    if (counts_.size() != expected_segments ||
        offsets_.size() != expected_segments) {
      return false;
    }

    std::size_t expected_offset = 0;
    for (std::size_t index = 0; index < expected_segments; ++index) {
      if (offsets_[index] != expected_offset ||
          counts_[index] >
              std::numeric_limits<std::size_t>::max() - expected_offset) {
        return false;
      }
      expected_offset += counts_[index];
    }
    return expected_offset == storage_.size();
  }

private:
  void validate_segment(std::size_t index) const {
    if (index >= counts_.size() || index >= offsets_.size() ||
        offsets_[index] > storage_.size() ||
        counts_[index] > storage_.size() - offsets_[index]) {
      throw std::out_of_range{"invalid segmented buffer segment"};
    }
  }

  std::vector<T> storage_;
  std::vector<std::size_t> counts_;
  std::vector<std::size_t> offsets_;
};
}  // namespace parhip::mpi
